/* sharedraw.c — shader access to a D3D11 -> D3D12 shared texture, which sharetest.c/sharepat.c do not cover
 * (they only clear, copy and UpdateSubresource). This is what MSFS does with its video frames: Media Foundation's
 * TransferVideoFrame clears the shared texture and DRAWS the frame into it on the D3D11 device; the game SAMPLES
 * it in D3D12 shaders.
 *
 *   sharedraw.exe [width height]   (default 1920 1080)
 *
 * D3D11 (MISC_SHARED, KMT handle, B8G8R8A8_UNORM, RT|SRV) clears to red, then draws a full-screen triangle whose
 * pixel shader writes a per-pixel pattern; Flush. D3D12 opens the handle and reads it three ways:
 *   copy    CopyTextureRegion to a readback buffer
 *   load    compute shader Texture2D.Load -> RWStructuredBuffer
 *   sample  compute shader Texture2D.SampleLevel with a point sampler
 * and, as a control, load/sample again after copying it into a D3D12 OPTIMAL texture of its own (opt-load,
 * opt-sample). The compute-shader sampling uses a static sampler; ps-sample / ps-opt sample the same two textures
 * from a pixel shader with a sampler-heap sampler, drawing into a D3D12 render target. Each is compared per pixel. Shaders are compiled at run time with d3dcompiler_47.dll.
 * Output: stdout and sharedraw-out.txt. Build: build-sharedraw.sh.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

static FILE *out;
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); if (out) { fprintf(out, __VA_ARGS__); fflush(out); } } while (0)
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { LOG("FAIL %s = %#lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

static UINT W = 1920, H = 1080;
static pD3DCompile compile;

/* B8G8R8A8 dword of the pattern at (x, y): R = x, G = y, B = x ^ y (each & 255), A = 255 */
static UINT32 want(UINT x, UINT y)
{
    return ((x ^ y) & 255) | ((y & 255) << 8) | ((x & 255) << 16) | 0xff000000u;
}

static int compare(const unsigned char *base, size_t pitch, const char *who)
{
    size_t bad = 0; long first = -1; UINT32 got_first = 0;
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++)
        {
            UINT32 v; memcpy(&v, base + y * pitch + x * 4, 4);
            if (v != want(x, y)) { if (first < 0) { first = (long)y * W + x; got_first = v; } bad++; }
        }
    LOG("%-10s %ux%u: px(0,0) %08x px(%u,%u) %08x; matching %zu / %u -> %s", who, W, H, *(const UINT32 *)base, W - 1, H - 1,
        *(const UINT32 *)(base + (H - 1) * pitch + (W - 1) * 4), (size_t)W * H - bad, W * H, bad ? "WRONG" : "OK");
    if (first >= 0) LOG(" (first bad %ld,%ld = %08x, want %08x)", first % W, first / W, got_first, want(first % W, first / W));
    LOG("\n");
    return bad ? 1 : 0;
}

static ID3DBlob *build(const char *src, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = compile(src, strlen(src), NULL, NULL, NULL, "main", target, 0, 0, &code, &err);
    if (FAILED(hr)) LOG("D3DCompile(%s) = %#lx: %s\n", target, (unsigned long)hr, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
    return code;
}

static const char vs_src[] =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n";
static const char ps_src[] =
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  uint2 p = uint2(pos.xy);\n"
    "  return float4((p.x & 255) / 255.0, (p.y & 255) / 255.0, ((p.x ^ p.y) & 255) / 255.0, 1.0); }\n";
static char cs_src[1024];

static int wait_queue(ID3D12Device *dev, ID3D12CommandQueue *q)
{
    ID3D12Fence *f; HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f));
    CK(ID3D12CommandQueue_Signal(q, f, 1));
    CK(ID3D12Fence_SetEventOnCompletion(f, 1, ev));
    if (WaitForSingleObject(ev, 10000)) LOG("fence wait timed out\n");
    ID3D12Fence_Release(f); CloseHandle(ev);
    return 0;
}

/* draw a full-screen triangle into a D3D12 B8G8R8A8 render target, sampling 'srctex' with a point sampler from a
 * sampler descriptor heap (no static sampler), then read the render target back and compare */
static int ps_sample(ID3D12Device *dev, ID3D12CommandQueue *q, ID3D12Resource *srctex, const char *who)
{
    static char ps_samp[512];
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rbk = {D3D12_HEAP_TYPE_READBACK};
    D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_HEAP_DESC smh = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_HEAP_DESC rth = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    D3D12_DESCRIPTOR_RANGE r0 = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, r1 = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
    D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 0, NULL, 0};
    D3D12_SAMPLER_DESC sd = {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1, D3D12_COMPARISON_FUNC_NEVER, {0}, 0, D3D12_FLOAT32_MAX};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET}, bd = {0};
    D3D12_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1}; D3D12_RECT sc = {0, 0, W, H};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; D3D12_TEXTURE_COPY_LOCATION dst, src; D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv; D3D12_CPU_DESCRIPTOR_HANDLE c0, c1; D3D12_GPU_DESCRIPTOR_HANDLE g0, g1;
    ID3D12DescriptorHeap *hs, *hsm, *hrt; ID3D12RootSignature *rs; ID3D12PipelineState *pso; ID3D12Resource *rt, *rb;
    ID3D12CommandAllocator *ca; ID3D12GraphicsCommandList *cl; ID3DBlob *rsb, *rse = NULL, *vsb, *psb; UINT64 total; unsigned char *map;

    snprintf(ps_samp, sizeof(ps_samp), "Texture2D<float4> t : register(t0); SamplerState s : register(s0);\n"
             "float4 main(float4 pos : SV_Position) : SV_Target { return t.Sample(s, pos.xy / float2(%u, %u)); }\n", W, H);
    if (!(vsb = build(vs_src, "vs_5_0")) || !(psb = build(ps_samp, "ps_5_0"))) return 1;
    params[0].DescriptorTable.NumDescriptorRanges = 1; params[0].DescriptorTable.pDescriptorRanges = &r0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &r1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    CK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse));
    CK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs));
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffff;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count = 1;
    CK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
    CK(ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    ID3D12Device_GetCopyableFootprints(dev, &rd, 0, 1, 0, &fp, NULL, NULL, &total);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev, &rbk, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    CK(ID3D12Device_CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&hs));
    CK(ID3D12Device_CreateDescriptorHeap(dev, &smh, &IID_ID3D12DescriptorHeap, (void **)&hsm));
    CK(ID3D12Device_CreateDescriptorHeap(dev, &rth, &IID_ID3D12DescriptorHeap, (void **)&hrt));
    hs->lpVtbl->GetCPUDescriptorHandleForHeapStart(hs, &c0); hs->lpVtbl->GetGPUDescriptorHandleForHeapStart(hs, &g0);
    hsm->lpVtbl->GetCPUDescriptorHandleForHeapStart(hsm, &c1); hsm->lpVtbl->GetGPUDescriptorHandleForHeapStart(hsm, &g1);
    hrt->lpVtbl->GetCPUDescriptorHandleForHeapStart(hrt, &rtv);
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, srctex, &srv, c0);
    ID3D12Device_CreateSampler(dev, &sd, c1);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    CK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca));
    CK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, pso, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    {
        ID3D12DescriptorHeap *heaps[2] = {hs, hsm};
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
    }
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, g0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, g1);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    b.Transition.pResource = rt; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    if (wait_queue(dev, q)) return 1;
    CK(ID3D12Resource_Map(rb, 0, NULL, (void **)&map));
    return compare(map + fp.Offset, fp.Footprint.RowPitch, who);
}

static int d3d12_side(HANDLE h)
{
    D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT}, rbk = {D3D12_HEAP_TYPE_READBACK};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER param = {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE};
    D3D12_STATIC_SAMPLER_DESC samp = {D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                      D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1, D3D12_COMPARISON_FUNC_NEVER, 0, 0, 0, 0, 0, D3D12_SHADER_VISIBILITY_ALL};
    D3D12_ROOT_SIGNATURE_DESC rsd = {1, &param, 1, &samp, 0};
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {0};
    D3D12_RESOURCE_DESC bd = {0}, td;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {0};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    ID3D12Device *dev; ID3D12Resource *tex = NULL, *rb1, *ub, *rb2, *opt, *ub2, *rb3;
    D3D12_RESOURCE_DESC od; ID3D12CommandQueue *q; ID3D12CommandAllocator *ca;
    ID3D12GraphicsCommandList *cl; ID3D12DescriptorHeap *heap; ID3D12RootSignature *rs; ID3D12PipelineState *pso;
    ID3DBlob *rsblob, *rserr = NULL, *cs; UINT64 total; UINT inc; unsigned char *map; HRESULT hr; int bad = 0;

    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    hr = ID3D12Device_OpenSharedHandle(dev, h, &IID_ID3D12Resource, (void **)&tex);
    LOG("ID3D12Device::OpenSharedHandle -> %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 3;
    tex->lpVtbl->GetDesc(tex, &td);
    CK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca));
    CK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));

    /* copy */
    ID3D12Device_GetCopyableFootprints(dev, &td, 0, 1, 0, &fp, NULL, NULL, &total);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev, &rbk, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb1));
    dst.pResource = rb1; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);

    /* control: an OPTIMAL copy of the shared texture */
    od = td; od.Flags = D3D12_RESOURCE_FLAG_NONE; od.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    CK(ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &od, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&opt));
    dst.pResource = opt; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    {
        D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition.pResource = opt; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
    }

    /* load + sample through a compute shader */
    snprintf(cs_src, sizeof(cs_src),
        "Texture2D<float4> t : register(t0); RWStructuredBuffer<uint> o : register(u0); SamplerState s : register(s0);\n"
        "uint pack(float4 c) { uint4 u = uint4(round(saturate(c) * 255.0)); return u.b | (u.g << 8) | (u.r << 16) | (u.a << 24); }\n"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
        "  if (id.x >= %u || id.y >= %u) return;\n"
        "  o[id.y * %u + id.x] = pack(t.Load(int3(id.xy, 0)));\n"
        "  o[%u + id.y * %u + id.x] = pack(t.SampleLevel(s, (float2(id.xy) + 0.5) / float2(%u, %u), 0)); }\n",
        W, H, W, W * H, W, W, H);
    if (!(cs = build(cs_src, "cs_5_0"))) return 1;
    param.DescriptorTable.NumDescriptorRanges = 2; param.DescriptorTable.pDescriptorRanges = ranges;
    CK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsblob, &rserr));
    CK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsblob), ID3D10Blob_GetBufferSize(rsblob), &IID_ID3D12RootSignature, (void **)&rs));
    pd.pRootSignature = rs; pd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs); pd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    CK(ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
    bd.Width = (UINT64)W * H * 8; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CK(ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&ub));
    bd.Flags = 0;
    CK(ID3D12Device_CreateCommittedResource(dev, &rbk, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb2));
    CK(ID3D12Device_CreateCommittedResource(dev, &rbk, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb3));
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CK(ID3D12Device_CreateCommittedResource(dev, &def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&ub2));
    bd.Flags = 0;
    CK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &cpu);
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &gpu);
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, tex, &srv, cpu);
    cpu.ptr += inc;
    uav.Format = DXGI_FORMAT_UNKNOWN; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = W * H * 2; uav.Buffer.StructureByteStride = 4;
    ID3D12Device_CreateUnorderedAccessView(dev, ub, NULL, &uav, cpu);
    cpu.ptr += inc;
    ID3D12Device_CreateShaderResourceView(dev, opt, &srv, cpu);
    cpu.ptr += inc;
    ID3D12Device_CreateUnorderedAccessView(dev, ub2, NULL, &uav, cpu);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &heap);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(cl, (W + 7) / 8, (H + 7) / 8, 1);
    gpu.ptr += 2 * inc;
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(cl, (W + 7) / 8, (H + 7) / 8, 1);
    {
        D3D12_RESOURCE_BARRIER b[2] = {{D3D12_RESOURCE_BARRIER_TYPE_TRANSITION}, {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION}};
        b[0].Transition.pResource = ub; b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b[1] = b[0]; b[1].Transition.pResource = ub2;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 2, b);
    }
    ID3D12GraphicsCommandList_CopyResource(cl, rb2, ub);
    ID3D12GraphicsCommandList_CopyResource(cl, rb3, ub2);
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    if (wait_queue(dev, q)) return 1;

    CK(ID3D12Resource_Map(rb1, 0, NULL, (void **)&map));
    bad += compare(map + fp.Offset, fp.Footprint.RowPitch, "copy");
    CK(ID3D12Resource_Map(rb2, 0, NULL, (void **)&map));
    bad += compare(map, (size_t)W * 4, "load");
    bad += compare(map + (size_t)W * H * 4, (size_t)W * 4, "sample");
    CK(ID3D12Resource_Map(rb3, 0, NULL, (void **)&map));
    bad += compare(map, (size_t)W * 4, "opt-load");
    bad += compare(map + (size_t)W * H * 4, (size_t)W * 4, "opt-sample");
    bad += ps_sample(dev, q, tex, "ps-sample");
    bad += ps_sample(dev, q, opt, "ps-opt");
    return bad ? 2 : 0;
}

int main(int argc, char **argv)
{
    D3D11_TEXTURE2D_DESC desc = {0, 0, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
    static const float red[4] = {1, 0, 0, 1};
    ID3D11Device *dev11; ID3D11DeviceContext *ctx; ID3D11Texture2D *tex; ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3DBlob *vsb, *psb; IDXGIResource *dr;
    D3D11_VIEWPORT vp = {0}; D3D_FEATURE_LEVEL fl; HANDLE h = NULL; HMODULE dc; int r;

    if (argc > 2) { W = atoi(argv[1]); H = atoi(argv[2]); }
    out = fopen("sharedraw-out.txt", "w");
    LOG("sharedraw %ux%u: D3D11 clears red, draws a pattern; D3D12 copies, loads and samples\n", W, H);
    if (!(dc = LoadLibraryA("d3dcompiler_47.dll")) || !(compile = (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile")))
    { LOG("no D3DCompile\n"); return 1; }
    if (!(vsb = build(vs_src, "vs_5_0")) || !(psb = build(ps_src, "ps_5_0"))) return 1;

    desc.Width = W; desc.Height = H;
    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
    CK(ID3D11Device_CreateTexture2D(dev11, &desc, NULL, &tex));
    CK(ID3D11Device_CreateRenderTargetView(dev11, (ID3D11Resource *)tex, NULL, &rtv));
    CK(ID3D11Device_CreateVertexShader(dev11, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs));
    CK(ID3D11Device_CreatePixelShader(dev11, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps));
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, red);
    vp.Width = (float)W; vp.Height = (float)H; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_Flush(ctx);
    CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&dr));
    CK(IDXGIResource_GetSharedHandle(dr, &h));
    LOG("D3D11 drew into shared texture, KMT handle %p\n", h);
    Sleep(200);
    r = d3d12_side(h);
    LOG("RESULT %s (code %d)\n", r == 0 ? "PASS" : "FAIL", r);
    return r;
}
