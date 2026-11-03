/* d3dbench.c — CPU cost of the D3D11/D3D12 -> Vulkan translation (DXVK, vkd3d-proton), for comparing an x86-64
 * build of the DLLs (runs inside FEX together with the program) with an ARM64EC build (runs natively).
 *
 * No window, no swap chain: everything draws into a 64x64 offscreen target, so the GPU has almost nothing to do and
 * the times are the CPU side. Each test checks its result (read back the render target) so a fast but wrong build
 * does not pass.
 *
 *   d3d12-draw    per frame: reset, set state, DRAWS x (SetGraphicsRoot32BitConstants + DrawInstanced), execute, wait
 *   d3d12-scene   like d3d12-draw, closer to a game: 4 PSOs, descriptor table + root CBV + root constants + VB/IB +
 *                 DrawIndexedInstanced per draw, a texture barrier pair every 100 draws
 *   d3d12-desc    CreateShaderResourceView / CreateConstantBufferView / CopyDescriptorsSimple, DESC times each
 *   d3d12-pso     CreateGraphicsPipelineState with PSOS different pixel shaders (DXBC -> SPIR-V -> MSL), then draw each
 *   d3d12-pso-mt  8 threads create PSOS pipelines together; wall and process CPU time
 *   d3d11-draw    per frame: DRAWS x (Map DISCARD constant buffer + PSSetConstantBuffers + Draw), flush, wait
 *   d3d11-shader  CreatePixelShader with PSOS different shaders, then one draw each (forces DXVK's pipeline compile)
 *
 * Usage: d3dbench.exe [frames=N] [draws=N] [desc=N] [psos=N] [settle=ms] [test...]   (no test = all)
 *   settle: sleep between creating the PSOs/shaders and the first draws, to see what is left over in the background
 * Output: one "BENCH <test> ..." line per test plus "BENCH <test>-total" (wall and process CPU time of the whole test), "RESULT PASS|FAIL" at the end.
 * Shader sources get a per-run salt so neither vkd3d's disk cache nor the Metal shader cache can make a run faster.
 * Built for Windows x86-64 with brew's mingw-w64 (build.sh); d3dcompiler_47 is loaded at run time and shaders are
 * compiled before the timed part.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { LOG("FAIL %s = %#lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

enum { RT = 64 };
static UINT frames = 100, warm = 10, draws = 2000, ndesc = 100000, npsos = 48, settle = 0;
static unsigned salt;
static pD3DCompile compile;
static LARGE_INTEGER qpf;

static double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / qpf.QuadPart; }
/* user + kernel time of all threads of this process (not the Metal compiler service, which is another process) */
static double cpu_ms(void)
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double median(double *v, UINT n) { qsort(v, n, sizeof(*v), cmp_d); return n ? (n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2) : 0; }

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
static const char ps_cb_src[] = /* color from b0 (root constants in D3D12, constant buffer in D3D11) */
    "cbuffer c : register(b0) { float4 col; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target { return col; }\n";
/* variant k returns a known constant color. col is all zero at run time, so the loop adds exactly 0, but the compiler
 * can't fold it (or the salt) away: every variant and every run is different bytecode with some real math in it. */
static void ps_variant(char *buf, size_t n, UINT k)
{
    snprintf(buf, n,
        "cbuffer c : register(b0) { float4 col; };\n"
        "float4 main(float4 pos : SV_Position) : SV_Target {\n"
        "  float4 a = col * float4(%u, %u, %u, %u);\n"
        "  [unroll] for (int i = 0; i < 8; i++) a = a * 0.5 + sin(a + i) * col.x;\n"
        "  return float4(%u / 255.0, %u / 255.0, 1.0, 1.0) + a; }\n",
        salt, k + 1, k + 2, k + 3, k & 255, (k * 7) & 255);
}
static UINT32 variant_rgba(UINT k) { return (k & 255) | (((k * 7) & 255) << 8) | 0xffff0000u; }

/* RGBA8 pixel of float4 c, with D3D's round-to-nearest */
static UINT32 rgba(const float *c)
{
    UINT32 v = 0;
    for (int i = 0; i < 4; i++) v |= (UINT32)(c[i] * 255.0f + 0.5f) << (8 * i);
    return v;
}
static void frame_color(UINT f, float *c) { c[0] = ((f * 37) & 255) / 255.0f; c[1] = ((f * 11) & 255) / 255.0f; c[2] = ((f * 5) & 255) / 255.0f; c[3] = 1.0f; }

static int check(const char *who, const unsigned char *p, UINT pitch, UINT32 want)
{
    UINT bad = 0; UINT32 first = 0;
    for (UINT y = 0; y < RT; y++)
        for (UINT x = 0; x < RT; x++)
        {
            UINT32 v; memcpy(&v, p + y * pitch + x * 4, 4);
            if (v != want && !bad++) first = v;
        }
    if (bad) LOG("%s: %u / %u pixels wrong (first %08x, want %08x)\n", who, bad, RT * RT, first, want);
    return bad ? 1 : 0;
}

/* ---------------------------------------------------------------- D3D12 */

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;
static ID3D12RootSignature *rs;
static ID3D12Resource *rt12, *rb12;
static D3D12_CPU_DESCRIPTOR_HANDLE rtv12;
static ID3DBlob *vsb, *psb;
static UINT rb_pitch = (RT * 4 + 255) & ~255u;

static void wait12(void)
{
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    if (ID3D12Fence_GetCompletedValue(fence) < fence_value)
    {
        ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }
}

static void barrier12(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static void begin_pass12(ID3D12PipelineState *pso)
{
    D3D12_VIEWPORT vp = {0, 0, RT, RT, 0, 1};
    D3D12_RECT sc = {0, 0, RT, RT};
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv12, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

static void submit12(void)
{
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    wait12();
}

/* copy the render target into the readback buffer and compare */
static int readback12(const char *who, UINT32 want)
{
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    void *p; int r;
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    barrier12(rt12, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    src.pResource = rt12; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb12; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = RT; dst.PlacedFootprint.Footprint.Height = RT; dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rb_pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    barrier12(rt12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    submit12();
    if (FAILED(ID3D12Resource_Map(rb12, 0, NULL, &p))) { LOG("%s: Map readback failed\n", who); return 1; }
    r = check(who, p, rb_pitch, want);
    ID3D12Resource_Unmap(rb12, 0, NULL);
    return r;
}

static ID3D12PipelineState *make_pso12(ID3DBlob *ps)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    ID3D12PipelineState *pso = NULL;
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso))) return NULL;
    return pso;
}

static int init12(void)
{
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_ROOT_PARAMETER rp = {0};
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    ID3DBlob *rsb, *rse;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = {0};
    D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_R8G8B8A8_UNORM};
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    ID3D12DescriptorHeap *rtvh;

    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    CK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    CK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_Close(list);
    CK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp.Constants.Num32BitValues = 4; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rsd.NumParameters = 1; rsd.pParameters = &rp;
    CK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse));
    CK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs));

    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = RT; td.Height = RT; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&rt12));
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    memset(&td, 0, sizeof(td));
    td.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; td.Width = rb_pitch * RT; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb12));
    CK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvh));
    rtvh->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvh, &rtv12);
    ID3D12Device_CreateRenderTargetView(dev, rt12, NULL, rtv12);
    return 0;
}

static int bench_d3d12_draw(void)
{
    ID3D12PipelineState *pso = make_pso12(psb);
    double *rec = malloc(frames * sizeof(double)), *tot = malloc(frames * sizeof(double)), t0, t1;
    float c[4];
    if (!pso) { LOG("FAIL d3d12-draw: CreateGraphicsPipelineState\n"); return 1; }
    for (UINT f = 0; f < warm + frames; f++)
    {
        t0 = now_ms();
        begin_pass12(pso);
        for (UINT i = 0; i < draws; i++)
        {
            frame_color(f + i, c);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, c, 0);
            ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        }
        ID3D12GraphicsCommandList_Close(list);
        t1 = now_ms();
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        wait12();
        if (f >= warm) { rec[f - warm] = t1 - t0; tot[f - warm] = now_ms() - t0; }
    }
    frame_color(warm + frames - 1 + draws - 1, c);
    int bad = readback12("d3d12-draw", rgba(c));
    double mr = median(rec, frames), mt = median(tot, frames);
    LOG("BENCH d3d12-draw frames=%u draws=%u record_ms=%.3f frame_ms=%.3f ns_per_draw=%.1f check=%s\n",
        frames, draws, mr, mt, mr * 1e6 / draws, bad ? "WRONG" : "OK");
    ID3D12PipelineState_Release(pso); free(rec); free(tot);
    return bad;
}

/* closer to a game frame: 4 PSOs (switch every 50 draws), a shader-visible heap, and per draw a descriptor table,
 * a root CBV, root constants, vertex + index buffer and an indexed draw; a texture barrier every 100 draws */
static const char ps_scene_src[] =
    "cbuffer c : register(b0) { float4 col; };\n"
    "cbuffer d : register(b1) { float4 add; };\n"
    "Buffer<float4> t : register(t0);\n"
    "float4 main(float4 pos : SV_Position) : SV_Target { return col + add + t.Load(0); }\n";

static int bench_d3d12_scene(void)
{
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER rp[3] = {0};
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    D3D12_DESCRIPTOR_HEAP_DESC hv = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd = {0}, td = {0};
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    D3D12_VIEWPORT vp = {0, 0, RT, RT, 0, 1};
    D3D12_RECT sc = {0, 0, RT, RT};
    D3D12_VERTEX_BUFFER_VIEW vbv;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ID3D12RootSignature *srs; ID3DBlob *rsb, *rse, *ps;
    ID3D12DescriptorHeap *heap;
    ID3D12Resource *buf, *tex;
    ID3D12PipelineState *pso[4];
    D3D12_CPU_DESCRIPTOR_HANDLE c0, h; D3D12_GPU_DESCRIPTOR_HANDLE g0, g;
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    double *rec = malloc(frames * sizeof(double)), *tot = malloc(frames * sizeof(double)), t0, t1;
    unsigned char *p;
    float c[4];

    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[1].Descriptor.ShaderRegister = 1;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[2].DescriptorTable.NumDescriptorRanges = 1; rp[2].DescriptorTable.pDescriptorRanges = &range;
    for (int i = 0; i < 3; i++) rp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rsd.NumParameters = 3; rsd.pParameters = rp;
    CK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse));
    CK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&srs));
    if (!(ps = build(ps_scene_src, "ps_5_0"))) return 1;

    /* one zeroed upload buffer: 0..16K constants, 16K.. typed SRV data, 32K index buffer */
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&buf));
    CK(ID3D12Resource_Map(buf, 0, NULL, (void **)&p));
    memset(p, 0, 65536);
    { UINT16 idx[3] = {0, 1, 2}; memcpy(p + 32768, idx, sizeof(idx)); }
    ID3D12Resource_Unmap(buf, 0, NULL);
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = 4; td.Height = 4; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&tex));

    CK(ID3D12Device_CreateDescriptorHeap(dev, &hv, &IID_ID3D12DescriptorHeap, (void **)&heap));
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &c0);
    heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(heap, &g0);
    sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Buffer.FirstElement = 1024; sv.Buffer.NumElements = 64;
    for (UINT i = 0; i < 256; i++) { h.ptr = c0.ptr + i * inc; ID3D12Device_CreateShaderResourceView(dev, buf, &sv, h); }

    pd.pRootSignature = srs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = ~0u; pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    for (int k = 0; k < 4; k++) /* same output, different PSOs */
    {
        pd.RasterizerState.FrontCounterClockwise = k & 1; pd.RasterizerState.DepthClipEnable = !(k & 2);
        CK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso[k]));
    }
    vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + 16384; vbv.SizeInBytes = 4096; vbv.StrideInBytes = 16;
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + 32768; ibv.SizeInBytes = 6; ibv.Format = DXGI_FORMAT_R16_UINT;

    for (UINT f = 0; f < warm + frames; f++)
    {
        D3D12_GPU_VIRTUAL_ADDRESS cb0 = ID3D12Resource_GetGPUVirtualAddress(buf);
        t0 = now_ms();
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, pso[0]);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, srs);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv12, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT i = 0; i < draws; i++)
        {
            if (i % 50 == 0) ID3D12GraphicsCommandList_SetPipelineState(list, pso[(i / 50) & 3]);
            if (i % 100 == 99)
            {
                barrier12(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                barrier12(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            }
            g.ptr = g0.ptr + (i & 255) * inc;
            frame_color(f + i, c);
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, g);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 1, cb0 + (i & 63) * 256);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, c, 0);
            ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
            ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
            ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 3, 1, 0, 0, 0);
        }
        ID3D12GraphicsCommandList_Close(list);
        t1 = now_ms();
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        wait12();
        if (f >= warm) { rec[f - warm] = t1 - t0; tot[f - warm] = now_ms() - t0; }
    }
    frame_color(warm + frames - 1 + draws - 1, c);
    int bad = readback12("d3d12-scene", rgba(c));
    double mr = median(rec, frames), mt = median(tot, frames);
    LOG("BENCH d3d12-scene frames=%u draws=%u record_ms=%.3f frame_ms=%.3f ns_per_draw=%.1f check=%s\n",
        frames, draws, mr, mt, mr * 1e6 / draws, bad ? "WRONG" : "OK");
    for (int k = 0; k < 4; k++) ID3D12PipelineState_Release(pso[k]);
    ID3D12DescriptorHeap_Release(heap); ID3D12Resource_Release(buf); ID3D12Resource_Release(tex);
    ID3D12RootSignature_Release(srs); free(rec); free(tot);
    return bad;
}

static int bench_d3d12_desc(void)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024}, hv = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12DescriptorHeap *cpu_heap, *gpu_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE c0, g0, h, d;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC bd = {0};
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {0};
    ID3D12Resource *buf;
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    double t_srv, t_cbv, t_copy, t;

    CK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
    CK(ID3D12Device_CreateDescriptorHeap(dev, &hv, &IID_ID3D12DescriptorHeap, (void **)&gpu_heap));
    cpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(cpu_heap, &c0);
    gpu_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(gpu_heap, &g0);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 1 << 20; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&buf));

    /* texture SRV of the render target, buffer CBV with a moving offset, copy into the shader-visible heap */
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    for (int pass = 0; pass < 2; pass++) /* pass 0 warms up */
    {
        UINT n = pass ? ndesc : ndesc / 10;
        t = now_ms();
        for (UINT i = 0; i < n; i++) { h.ptr = c0.ptr + (i & 1023) * inc; ID3D12Device_CreateShaderResourceView(dev, rt12, &sv, h); }
        t_srv = now_ms() - t; t = now_ms();
        for (UINT i = 0; i < n; i++)
        {
            h.ptr = c0.ptr + (i & 1023) * inc;
            cbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buf) + (i & 4095) * 256; cbv.SizeInBytes = 256;
            ID3D12Device_CreateConstantBufferView(dev, &cbv, h);
        }
        t_cbv = now_ms() - t; t = now_ms();
        for (UINT i = 0; i < n; i++)
        {
            h.ptr = c0.ptr + (i & 1023) * inc; d.ptr = g0.ptr + ((i * 7) & 1023) * inc;
            ID3D12Device_CopyDescriptorsSimple(dev, 1, d, h, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        t_copy = now_ms() - t;
    }
    LOG("BENCH d3d12-desc n=%u srv_ns=%.1f cbv_ns=%.1f copy_ns=%.1f check=OK\n", ndesc,
        t_srv * 1e6 / ndesc, t_cbv * 1e6 / ndesc, t_copy * 1e6 / ndesc);
    ID3D12Resource_Release(buf); ID3D12DescriptorHeap_Release(cpu_heap); ID3D12DescriptorHeap_Release(gpu_heap);
    return 0;
}

static int bench_d3d12_pso(void)
{
    ID3DBlob **ps = calloc(npsos, sizeof(*ps));
    ID3D12PipelineState **pso = calloc(npsos, sizeof(*pso));
    char src[1024];
    double t0, t_create, t_draw;
    int bad = 0;
    for (UINT k = 0; k < npsos; k++) { ps_variant(src, sizeof(src), k + 1000); if (!(ps[k] = build(src, "ps_5_0"))) return 1; }
    t0 = now_ms();
    for (UINT k = 0; k < npsos; k++) if (!(pso[k] = make_pso12(ps[k]))) { LOG("FAIL d3d12-pso: PSO %u\n", k); return 1; }
    t_create = now_ms() - t0;
    if (settle) Sleep(settle);
    t0 = now_ms();
    for (UINT k = 0; k < npsos; k++)
    {
        float c[4] = {0};
        begin_pass12(pso[k]);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, c, 0);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        submit12();
        if (k == npsos - 1 || k == 0) { char who[32]; snprintf(who, sizeof(who), "d3d12-pso #%u", k); bad |= readback12(who, variant_rgba(k + 1000)); }
    }
    t_draw = now_ms() - t0;
    LOG("BENCH d3d12-pso psos=%u create_ms=%.2f ms_per_pso=%.3f first_draws_ms=%.2f check=%s\n",
        npsos, t_create, t_create / npsos, t_draw, bad ? "WRONG" : "OK");
    for (UINT k = 0; k < npsos; k++) { ID3D12PipelineState_Release(pso[k]); ID3D10Blob_Release(ps[k]); }
    free(ps); free(pso);
    return bad;
}

/* 8 threads create PSOS/8 pipelines each at the same time (loading screens do this), wall and CPU time */
static ID3DBlob **mt_ps;
static volatile LONG mt_next, mt_fail;
static DWORD WINAPI mt_worker(void *arg)
{
    LONG k;
    while ((k = InterlockedIncrement(&mt_next) - 1) < (LONG)npsos)
    {
        ID3D12PipelineState *pso = make_pso12(mt_ps[k]);
        if (!pso) InterlockedIncrement(&mt_fail); else ID3D12PipelineState_Release(pso);
    }
    return 0;
}
static int bench_d3d12_pso_mt(void)
{
    enum { NT = 8 };
    HANDLE th[NT];
    char src[1024];
    double t0, c0, wall, cpu;
    mt_ps = calloc(npsos, sizeof(*mt_ps));
    for (UINT k = 0; k < npsos; k++) { ps_variant(src, sizeof(src), k + 3000); if (!(mt_ps[k] = build(src, "ps_5_0"))) return 1; }
    mt_next = 0; mt_fail = 0;
    t0 = now_ms(); c0 = cpu_ms();
    for (int i = 0; i < NT; i++) th[i] = CreateThread(NULL, 0, mt_worker, NULL, 0, NULL);
    WaitForMultipleObjects(NT, th, TRUE, INFINITE);
    wall = now_ms() - t0; cpu = cpu_ms() - c0;
    for (int i = 0; i < NT; i++) CloseHandle(th[i]);
    for (UINT k = 0; k < npsos; k++) ID3D10Blob_Release(mt_ps[k]);
    free(mt_ps);
    LOG("BENCH d3d12-pso-mt threads=%d psos=%u wall_ms=%.2f cpu_ms=%.1f check=%s\n", NT, npsos, wall, cpu, mt_fail ? "WRONG" : "OK");
    return mt_fail ? 1 : 0;
}

/* ---------------------------------------------------------------- D3D11 */

static ID3D11Device *d11;
static ID3D11DeviceContext *ctx;
static ID3D11Texture2D *rt11, *st11;
static ID3D11RenderTargetView *rtv11;
static ID3D11VertexShader *vs11;
static ID3D11Buffer *cb11;
static ID3D11Query *q11;

static void wait11(void)
{
    BOOL done = FALSE;
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)q11);
    ID3D11DeviceContext_Flush(ctx);
    while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)q11, &done, sizeof(done), 0) != S_OK || !done) SwitchToThread();
}

static void begin_pass11(ID3D11PixelShader *ps)
{
    D3D11_VIEWPORT vp = {0, 0, RT, RT, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv11, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs11, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
}

static int readback11(const char *who, UINT32 want)
{
    D3D11_MAPPED_SUBRESOURCE m;
    int r;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st11, (ID3D11Resource *)rt11);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st11, 0, D3D11_MAP_READ, 0, &m))) { LOG("%s: Map staging failed\n", who); return 1; }
    r = check(who, m.pData, m.RowPitch, want);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st11, 0);
    return r;
}

static int init11(void)
{
    D3D_FEATURE_LEVEL fl;
    D3D11_TEXTURE2D_DESC td = {RT, RT, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET};
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT};
    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d11, &fl, &ctx));
    CK(ID3D11Device_CreateTexture2D(d11, &td, NULL, &rt11));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CK(ID3D11Device_CreateTexture2D(d11, &td, NULL, &st11));
    CK(ID3D11Device_CreateRenderTargetView(d11, (ID3D11Resource *)rt11, NULL, &rtv11));
    CK(ID3D11Device_CreateVertexShader(d11, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs11));
    CK(ID3D11Device_CreateBuffer(d11, &bd, NULL, &cb11));
    CK(ID3D11Device_CreateQuery(d11, &qd, &q11));
    return 0;
}

static int bench_d3d11_draw(void)
{
    ID3D11PixelShader *ps;
    double *tot = malloc(frames * sizeof(double)), *sub = malloc(frames * sizeof(double)), t0, t1;
    float c[4];
    CK(ID3D11Device_CreatePixelShader(d11, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps));
    for (UINT f = 0; f < warm + frames; f++)
    {
        t0 = now_ms();
        begin_pass11(ps);
        for (UINT i = 0; i < draws; i++)
        {
            D3D11_MAPPED_SUBRESOURCE m;
            frame_color(f + i, c);
            ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb11, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            memcpy(m.pData, c, sizeof(c));
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb11, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb11);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        t1 = now_ms();
        wait11();
        if (f >= warm) { sub[f - warm] = t1 - t0; tot[f - warm] = now_ms() - t0; }
    }
    frame_color(warm + frames - 1 + draws - 1, c);
    int bad = readback11("d3d11-draw", rgba(c));
    double ms = median(sub, frames), mt = median(tot, frames);
    LOG("BENCH d3d11-draw frames=%u draws=%u submit_ms=%.3f frame_ms=%.3f ns_per_draw=%.1f check=%s\n",
        frames, draws, ms, mt, ms * 1e6 / draws, bad ? "WRONG" : "OK");
    ID3D11PixelShader_Release(ps); free(tot); free(sub);
    return bad;
}

static int bench_d3d11_shader(void)
{
    ID3DBlob **blob = calloc(npsos, sizeof(*blob));
    ID3D11PixelShader **ps = calloc(npsos, sizeof(*ps));
    char src[1024];
    double t0, t_create, t_draw;
    int bad = 0;
    float c[4] = {0};
    D3D11_MAPPED_SUBRESOURCE m;
    for (UINT k = 0; k < npsos; k++) { ps_variant(src, sizeof(src), k + 2000); if (!(blob[k] = build(src, "ps_5_0"))) return 1; }
    ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb11, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, c, sizeof(c));
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb11, 0);
    t0 = now_ms();
    for (UINT k = 0; k < npsos; k++)
        CK(ID3D11Device_CreatePixelShader(d11, ID3D10Blob_GetBufferPointer(blob[k]), ID3D10Blob_GetBufferSize(blob[k]), NULL, &ps[k]));
    t_create = now_ms() - t0;
    if (settle) Sleep(settle);
    t0 = now_ms();
    for (UINT k = 0; k < npsos; k++)
    {
        begin_pass11(ps[k]);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb11);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        wait11();
        if (k == npsos - 1 || k == 0) { char who[32]; snprintf(who, sizeof(who), "d3d11-shader #%u", k); bad |= readback11(who, variant_rgba(k + 2000)); }
    }
    t_draw = now_ms() - t0;
    LOG("BENCH d3d11-shader shaders=%u create_ms=%.2f ms_per_shader=%.3f first_draws_ms=%.2f check=%s\n",
        npsos, t_create, t_create / npsos, t_draw, bad ? "WRONG" : "OK");
    for (UINT k = 0; k < npsos; k++) { ID3D11PixelShader_Release(ps[k]); ID3D10Blob_Release(blob[k]); }
    free(blob); free(ps);
    return bad;
}

/* ---------------------------------------------------------------- main */

static int run_test(int (*fn)(void), const char *name)
{
    double c0 = cpu_ms(), t0 = now_ms();
    int r = fn();
    LOG("BENCH %s-total wall_ms=%.1f cpu_ms=%.1f\n", name, now_ms() - t0, cpu_ms() - c0);
    return r;
}
#define RUN(fn, name) run_test(fn, name)

static int want_test(int argc, char **argv, const char *name)
{
    int any = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strchr(argv[i], '=')) continue;
        any = 1;
        if (!strcmp(argv[i], name) || !strcmp(argv[i], "all")) return 1;
        if (!strcmp(argv[i], "d3d12") && !strncmp(name, "d3d12", 5)) return 1;
        if (!strcmp(argv[i], "d3d11") && !strncmp(name, "d3d11", 5)) return 1;
    }
    return !any;
}

int main(int argc, char **argv)
{
    HMODULE dc;
    double t0;
    int fail = 0;
    LARGE_INTEGER seed;

    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&seed);
    salt = (unsigned)(seed.QuadPart ^ GetCurrentProcessId() * 2654435761u) & 0xffffff;
    for (int i = 1; i < argc; i++)
    {
        unsigned v;
        if (sscanf(argv[i], "frames=%u", &v) == 1) frames = v;
        else if (sscanf(argv[i], "draws=%u", &v) == 1) draws = v;
        else if (sscanf(argv[i], "desc=%u", &v) == 1) ndesc = v;
        else if (sscanf(argv[i], "psos=%u", &v) == 1) npsos = v;
        else if (sscanf(argv[i], "settle=%u", &v) == 1) settle = v;
    }
    if (!(dc = LoadLibraryA("d3dcompiler_47.dll")) || !(compile = (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile")))
    { LOG("FAIL no d3dcompiler_47\n"); return 1; }
    if (!(vsb = build(vs_src, "vs_5_0")) || !(psb = build(ps_cb_src, "ps_5_0"))) return 1;
    LOG("d3dbench frames=%u (+%u warm-up) draws=%u desc=%u psos=%u salt=%u\n", frames, warm, draws, ndesc, npsos, salt);

    if (want_test(argc, argv, "d3d12-draw") || want_test(argc, argv, "d3d12-scene") || want_test(argc, argv, "d3d12-desc") || want_test(argc, argv, "d3d12-pso")
        || want_test(argc, argv, "d3d12-pso-mt"))
    {
        t0 = now_ms();
        if (init12()) return 1;
        LOG("BENCH d3d12-init ms=%.1f\n", now_ms() - t0);
        if (want_test(argc, argv, "d3d12-draw")) fail |= RUN(bench_d3d12_draw, "d3d12-draw");
        if (want_test(argc, argv, "d3d12-scene")) fail |= RUN(bench_d3d12_scene, "d3d12-scene");
        if (want_test(argc, argv, "d3d12-desc")) fail |= RUN(bench_d3d12_desc, "d3d12-desc");
        if (want_test(argc, argv, "d3d12-pso")) fail |= RUN(bench_d3d12_pso, "d3d12-pso");
        if (want_test(argc, argv, "d3d12-pso-mt")) fail |= RUN(bench_d3d12_pso_mt, "d3d12-pso-mt");
    }
    if (want_test(argc, argv, "d3d11-draw") || want_test(argc, argv, "d3d11-shader"))
    {
        t0 = now_ms();
        if (init11()) return 1;
        LOG("BENCH d3d11-init ms=%.1f\n", now_ms() - t0);
        if (want_test(argc, argv, "d3d11-draw")) fail |= RUN(bench_d3d11_draw, "d3d11-draw");
        if (want_test(argc, argv, "d3d11-shader")) fail |= RUN(bench_d3d11_shader, "d3d11-shader");
    }
    LOG("RESULT %s\n", fail ? "FAIL" : "PASS");
    return fail;
}
