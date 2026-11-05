/* d3d12gpu.exe: GPU time of small D3D12 workloads through vkd3d-proton + KosmicKrisp, offscreen (no window),
 * measured with D3D12 timestamp queries (records 25.56). Shaders are HLSL compiled at run time with
 * d3dcompiler_47 (Wine's builtin, DXBC shader model 5).
 *
 *   d3d12gpu cb [iters]
 *      Constant buffer reads: a full-screen pixel shader at W x H loops <iters> times (default 64) over a
 *      4 KiB cbuffer bound through a descriptor table, half the reads at a uniform index, half at a
 *      per-pixel index. Then two checks of out-of-range behaviour: a CBV of 256 bytes whose shader reads
 *      elements 16..19 (past the view, inside the buffer), and a null CBV.
 *   d3d12gpu cbfix [iters]
 *      Same, but the reads are at fixed offsets (an unrolled loop over <iters> rows), like most shader
 *      constants; the values are exact so every build gives the same checksum.
 *   d3d12gpu rt <format> [splits] [content]
 *      Render target bandwidth: <splits> (default 8) times, a G-buffer pass that draws one full-screen
 *      triangle into 4 render targets of <format> plus a D32 depth buffer, then a pass into a fifth target
 *      that samples the 4 (like deferred lighting), so the targets are stored, read as textures and loaded
 *      again. <format>: rgba8 (R8G8B8A8_UNORM), rgba8srgb, rgba8tl (R8G8B8A8_TYPELESS viewed as UNORM),
 *      rgb10a2, rg16 (R16G16_UNORM), rgba16f (R16G16B16A16_FLOAT). <content>: smooth (default) or noise.
 *   d3d12gpu rtcast
 *      Creates an R8G8B8A8_UNORM render target and SRVs of it as UNORM_SRGB and as UINT (a cast D3D12
 *      allows), for checking what a driver says about the UINT view (records 25.56).
 *
 * Each workload is recorded once and executed REPS (30) times after 3 warm-ups; prints the median and min
 * GPU ms of one execution, then a checksum of the output target so runs can be compared for correctness. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#define W 2056
#define H 1329
#define REPS 30

typedef HRESULT(WINAPI *compile_fn)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *,
                                     const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
static compile_fn compile;
static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;
static UINT64 ts_freq;

static void die(const char *what, HRESULT hr)
{
    printf("FAIL %s hr=0x%08lx\n", what, (unsigned long)hr);
    fflush(stdout);
    exit(1);
}

static ID3DBlob *shader(const char *src, const char *entry, const char *target, const D3D_SHADER_MACRO *defs)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = compile(src, strlen(src), "g.hlsl", defs, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("D3DCompile(%s) failed 0x%08lx: %s\n", entry, (unsigned long)hr,
               err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
        exit(1);
    }
    return code;
}

static void wait_idle(void)
{
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event);
    WaitForSingleObject(fence_event, 30000);
}

static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, 0, &rd, state, NULL, &IID_ID3D12Resource,
                                                      (void **)&r);
    if (FAILED(hr)) die("CreateCommittedResource(buffer)", hr);
    return r;
}

static ID3D12Resource *texture(DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                               const D3D12_CLEAR_VALUE *clear)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, fmt, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, 0, &rd, state, clear, &IID_ID3D12Resource,
                                                      (void **)&r);
    if (FAILED(hr)) die("CreateCommittedResource(texture)", hr);
    return r;
}

static ID3D12DescriptorHeap *heap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, BOOL shader_visible)
{
    D3D12_DESCRIPTOR_HEAP_DESC d = { type, n, shader_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : 0, 0 };
    ID3D12DescriptorHeap *h = NULL;
    HRESULT hr = ID3D12Device_CreateDescriptorHeap(dev, &d, &IID_ID3D12DescriptorHeap, (void **)&h);
    if (FAILED(hr)) die("CreateDescriptorHeap", hr);
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap *h, UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    h->lpVtbl->GetCPUDescriptorHandleForHeapStart(h, &c);
    D3D12_DESCRIPTOR_HEAP_DESC d;
    h->lpVtbl->GetDesc(h, &d);
    c.ptr += (SIZE_T)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, d.Type);
    return c;
}

static D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap *h, UINT i)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    h->lpVtbl->GetGPUDescriptorHandleForHeapStart(h, &g);
    D3D12_DESCRIPTOR_HEAP_DESC d;
    h->lpVtbl->GetDesc(h, &d);
    g.ptr += (UINT64)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, d.Type);
    return g;
}

static void transition(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                       D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER rb = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, 0 };
    rb.Transition.pResource = r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = a;
    rb.Transition.StateAfter = b;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &rb);
}

static ID3D12RootSignature *root_signature(const D3D12_ROOT_SIGNATURE_DESC *d)
{
    ID3DBlob *b = NULL, *e = NULL;
    HRESULT hr = D3D12SerializeRootSignature(d, D3D_ROOT_SIGNATURE_VERSION_1, &b, &e);
    if (FAILED(hr)) {
        printf("D3D12SerializeRootSignature: %s\n", e ? (const char *)ID3D10Blob_GetBufferPointer(e) : "");
        die("D3D12SerializeRootSignature", hr);
    }
    ID3D12RootSignature *rs = NULL;
    hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b),
                                          &IID_ID3D12RootSignature, (void **)&rs);
    if (FAILED(hr)) die("CreateRootSignature", hr);
    return rs;
}

static ID3D12PipelineState *pso(ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *ps, UINT nrt,
                                const DXGI_FORMAT *rtf, DXGI_FORMAT dsf)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    memset(&d, 0, sizeof(d));
    d.pRootSignature = rs;
    d.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
    d.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
    d.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
    d.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
    for (UINT i = 0; i < 8; i++)
        d.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = ~0u;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    if (dsf != DXGI_FORMAT_UNKNOWN) {
        d.DepthStencilState.DepthEnable = TRUE;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = nrt;
    for (UINT i = 0; i < nrt; i++)
        d.RTVFormats[i] = rtf[i];
    d.DSVFormat = dsf;
    d.SampleDesc.Count = 1;
    ID3D12PipelineState *p = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&p);
    if (FAILED(hr)) die("CreateGraphicsPipelineState", hr);
    return p;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Executes <cl> (recorded with timestamps 0 and 1 around the work, resolved into <rb>) 3 + REPS times. */
static void time_list(ID3D12GraphicsCommandList *cl, ID3D12Resource *rb, const char *label)
{
    double ms[REPS];
    for (int i = -3; i < REPS; i++) {
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
        wait_idle();
        UINT64 *v = NULL;
        D3D12_RANGE r = { 0, 16 };
        ID3D12Resource_Map(rb, 0, &r, (void **)&v);
        if (i >= 0)
            ms[i] = (double)(v[1] - v[0]) * 1000.0 / (double)ts_freq;
        ID3D12Resource_Unmap(rb, 0, NULL);
    }
    qsort(ms, REPS, sizeof(double), cmp_double);
    printf("%s  median %.3f ms  min %.3f ms\n", label, ms[REPS / 2], ms[0]);
}

/* FNV-1a over a texture read back through a buffer. */
static unsigned long long checksum(ID3D12GraphicsCommandList *cl, ID3D12CommandAllocator *ca,
                                   ID3D12Resource *tex, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC td;
    tex->lpVtbl->GetDesc(tex, &td);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0, row_bytes = 0;
    ID3D12Device_GetCopyableFootprints(dev, &td, 0, 1, 0, &fp, NULL, &row_bytes, &total);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList_Reset(cl, ca, NULL);
    transition(cl, tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    transition(cl, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
    wait_idle();
    unsigned char *p = NULL;
    D3D12_RANGE r = { 0, total };
    ID3D12Resource_Map(rb, 0, &r, (void **)&p);
    unsigned long long h = 1469598103934665603ull;
    for (UINT y = 0; y < fp.Footprint.Height; y++) {
        /* Only the texels, not the row padding. */
        const unsigned char *row = p + fp.Offset + (UINT64)y * fp.Footprint.RowPitch;
        for (UINT64 x = 0; x < row_bytes; x++)
            h = (h ^ row[x]) * 1099511628211ull;
    }
    ID3D12Resource_Unmap(rb, 0, NULL);
    ID3D12Resource_Release(rb);
    return h;
}

static const char vs_src[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "V vs_main(uint id : SV_VertexID) {\n"
    "  V o; float2 p = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0); o.uv = p; return o; }\n";

static const char cb_src[] =
    "cbuffer C : register(b0) { float4 arr[256]; };\n"
    "float4 ps_cb(float4 pos : SV_Position) : SV_Target {\n"
    "  uint px = (uint)pos.x;\n"
    "  float4 acc = 0;\n"
    "  [loop] for (uint i = 0; i < ITERS; i++)\n"
    "    acc += arr[i & 255] * arr[(i + px) & 255];\n"
    "  return acc; }\n"
    /* Fixed offsets, like most shader constants (matrices, material parameters): an unrolled loop reads
     * ITERS rows at literal indices and does a little math with each. */
    "float4 ps_cbfix(float4 pos : SV_Position) : SV_Target {\n"
    /* Multiples of 1/16 times multiples of 1/16, summed: exact in float, so any evaluation order gives the
     * same bits and the checksum compares builds. */
    "  float4 p = float4(((uint)pos.x & 15) / 16.0, ((uint)pos.y & 15) / 16.0, 0.5, 1.0);\n"
    "  float4 acc = 0;\n"
    "  [unroll] for (uint i = 0; i < ITERS; i++)\n"
    "    acc += float4(dot(p, arr[i]), dot(p.yzwx, arr[i]), dot(p.zwxy, arr[i]), 1.0) * arr[(i * 7) & 255];\n"
    "  return acc; }\n"
    "cbuffer S : register(b1) { float4 small[16]; };\n"
    "float4 ps_oob(float4 pos : SV_Position) : SV_Target {\n"
    "  uint k = (uint)pos.x & 3;\n"
    "  return small[16 + k]; }\n"
    "float4 ps_null(float4 pos : SV_Position) : SV_Target {\n"
    "  uint k = (uint)pos.x & 15;\n"
    "  return small[k]; }\n";

static int run_cb(int iters, int fixed)
{
    char iters_s[16];
    snprintf(iters_s, sizeof(iters_s), "%d", iters);
    D3D_SHADER_MACRO defs[] = { { "ITERS", iters_s }, { NULL, NULL } };
    ID3DBlob *vs = shader(vs_src, "vs_main", "vs_5_0", NULL);
    ID3DBlob *ps = shader(cb_src, fixed ? "ps_cbfix" : "ps_cb", "ps_5_0", defs);
    ID3DBlob *ps_oob = shader(cb_src, "ps_oob", "ps_5_0", defs);
    ID3DBlob *ps_null = shader(cb_src, "ps_null", "ps_5_0", defs);

    /* Descriptor table: b0 (4 KiB view), b1 (256-byte view, or null). */
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 2, 0, 0, 0 };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, NULL, 0 };
    ID3D12RootSignature *rs = root_signature(&rsd);
    DXGI_FORMAT f = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ID3D12PipelineState *p_cb = pso(rs, vs, ps, 1, &f, DXGI_FORMAT_UNKNOWN);
    ID3D12PipelineState *p_oob = pso(rs, vs, ps_oob, 1, &f, DXGI_FORMAT_UNKNOWN);
    ID3D12PipelineState *p_null = pso(rs, vs, ps_null, 1, &f, DXGI_FORMAT_UNKNOWN);

    /* 8 KiB upload buffer: arr at 0 (4 KiB), small at 4096 (256 bytes viewed, 4 KiB written). Values are
     * small exact binary fractions so the sums are exact and order independent. */
    ID3D12Resource *cb = buffer(D3D12_HEAP_TYPE_UPLOAD, 8192, D3D12_RESOURCE_STATE_GENERIC_READ);
    float *m = NULL;
    D3D12_RANGE none = { 0, 0 };
    ID3D12Resource_Map(cb, 0, &none, (void **)&m);
    for (int i = 0; i < 1024; i++)
        m[i] = (float)((i * 7) % 16) / 16.0f;
    for (int i = 0; i < 1024; i++)
        m[1024 + i] = 100.0f + i; /* small[k] = 100 + 4k.., small[16..] = 164.. (past the 256-byte view) */
    ID3D12Resource_Unmap(cb, 0, NULL);

    ID3D12DescriptorHeap *cbv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, TRUE);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv = { ID3D12Resource_GetGPUVirtualAddress(cb), 4096 };
    ID3D12Device_CreateConstantBufferView(dev, &cv, cpu_handle(cbv_heap, 0));
    D3D12_CONSTANT_BUFFER_VIEW_DESC cv2 = { ID3D12Resource_GetGPUVirtualAddress(cb) + 4096, 256 };
    ID3D12Device_CreateConstantBufferView(dev, &cv2, cpu_handle(cbv_heap, 1));
    /* Table 2 at index 2: b0 again, b1 null. */
    ID3D12Device_CreateConstantBufferView(dev, &cv, cpu_handle(cbv_heap, 2));
    ID3D12Device_CreateConstantBufferView(dev, NULL, cpu_handle(cbv_heap, 3));

    ID3D12Resource *rt = texture(f, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                 NULL);
    ID3D12DescriptorHeap *rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, FALSE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_handle(rtv_heap, 0);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
    ID3D12QueryHeap *qh = NULL;
    HRESULT hr = ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&qh);
    if (FAILED(hr)) die("CreateQueryHeap", hr);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);

    ID3D12CommandAllocator *ca = NULL;
    ID3D12GraphicsCommandList *cl = NULL;
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                        (void **)&ca);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL,
                                   &IID_ID3D12GraphicsCommandList, (void **)&cl);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };

    /* Timed: 8 full-screen draws. */
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &cbv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, gpu_handle(cbv_heap, 0));
    ID3D12GraphicsCommandList_SetPipelineState(cl, p_cb);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    for (int i = 0; i < 8; i++)
        ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb, 0);
    ID3D12GraphicsCommandList_Close(cl);
    char label[64];
    snprintf(label, sizeof(label), "%s iters=%d 8 draws %dx%d", fixed ? "cbfix" : "cb", iters, W, H);
    time_list(cl, rb, label);
    printf("cb checksum %016llx\n", checksum(cl, ca, rt, D3D12_RESOURCE_STATE_RENDER_TARGET));

    /* Out-of-range and null CBV reads: draw once, read back pixels 0..3 of row 0. */
    for (int t = 0; t < 2; t++) {
        ID3D12CommandAllocator_Reset(ca);
        ID3D12GraphicsCommandList_Reset(cl, ca, t ? p_null : p_oob);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &cbv_heap);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 0, gpu_handle(cbv_heap, t ? 2 : 0));
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        transition(cl, rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RESOURCE_DESC td;
        rt->lpVtbl->GetDesc(rt, &td);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
        UINT64 total = 0;
        ID3D12Device_GetCopyableFootprints(dev, &td, 0, 1, 0, &fp, NULL, NULL, &total);
        ID3D12Resource *px = buffer(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = { px, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_BOX box = { 0, 0, 0, 4, 1, 1 };
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, &box);
        transition(cl, rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_Close(cl);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
        wait_idle();
        float *v = NULL;
        D3D12_RANGE r = { 0, 64 };
        ID3D12Resource_Map(px, 0, &r, (void **)&v);
        printf("%s:", t ? "null CBV small[0..3].x" : "CBV of 256 B, small[16..19].x");
        for (int i = 0; i < 4; i++)
            printf(" %g", v[i * 4]);
        printf("%s\n", t ? "   (0 = reads as zero)"
                         : "   (0 = bounds-checked; 164 168 172 176 = the bytes past the view)");
        ID3D12Resource_Unmap(px, 0, NULL);
        ID3D12Resource_Release(px);
    }
    return 0;
}

static const char rt_src[] =
    "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "struct G { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3;\n"
    "           float z : SV_Depth; };\n"
    "cbuffer K : register(b0) { float pass_id; };\n"
    "uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }\n"
    "G ps_smooth(V i) {\n"
    "  float2 u = i.uv; G g;\n"
    "  g.a = float4(u.x, u.y, 0.5 + 0.01 * pass_id, 1.0);\n"
    "  g.b = float4(normalize(float3(u - 0.5, 1.0)) * 0.5 + 0.5, 0.0);\n"
    "  g.c = float4(0.2, 0.4 + 0.2 * u.x, 0.6, 1.0);\n"
    "  g.d = float4(u.y * 0.5, 0.25, u.x, 0.0);\n"
    "  g.z = 0.25 + 0.5 * u.y; return g; }\n"
    "G ps_noise(V i) {\n"
    "  uint2 p = (uint2)i.pos.xy; uint h = hash(p.x * 7919u + p.y * 104729u + (uint)pass_id);\n"
    "  float4 r = float4(hash(h) & 0xffff, hash(h + 1u) & 0xffff, hash(h + 2u) & 0xffff, hash(h + 3u) & 0xffff)\n"
    "             / 65535.0;\n"
    "  G g; g.a = r; g.b = r.yzwx; g.c = r.zwxy; g.d = r.wxyz; g.z = r.x; return g; }\n"
    "Texture2D<float4> ta : register(t0); Texture2D<float4> tb : register(t1);\n"
    "Texture2D<float4> tc : register(t2); Texture2D<float4> td : register(t3);\n"
    "Texture2D<float> tz : register(t4);\n"
    "float4 ps_resolve(V i) : SV_Target {\n"
    "  int3 p = int3((int2)i.pos.xy, 0);\n"
    "  return ta.Load(p) + tb.Load(p) * 0.5 + tc.Load(p) * 0.25 + td.Load(p) * 0.125 + tz.Load(p); }\n";

static int run_rt(const char *fmt_s, int splits, const char *content)
{
    DXGI_FORMAT res_fmt, view_fmt;
    if (!strcmp(fmt_s, "rgba8"))
        res_fmt = view_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (!strcmp(fmt_s, "rgba8srgb"))
        res_fmt = view_fmt = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    else if (!strcmp(fmt_s, "rgba8tl"))
        res_fmt = DXGI_FORMAT_R8G8B8A8_TYPELESS, view_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (!strcmp(fmt_s, "rgb10a2"))
        res_fmt = view_fmt = DXGI_FORMAT_R10G10B10A2_UNORM;
    else if (!strcmp(fmt_s, "rg16"))
        res_fmt = view_fmt = DXGI_FORMAT_R16G16_UNORM;
    else if (!strcmp(fmt_s, "rgba16f"))
        res_fmt = view_fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    else {
        printf("unknown format %s\n", fmt_s);
        return 2;
    }
    ID3DBlob *vs = shader(vs_src, "vs_main", "vs_5_0", NULL);
    ID3DBlob *ps = shader(rt_src, !strcmp(content, "noise") ? "ps_noise" : "ps_smooth", "ps_5_0", NULL);
    ID3DBlob *ps_res = shader(rt_src, "ps_resolve", "ps_5_0", NULL);

    D3D12_ROOT_PARAMETER params[2];
    memset(params, 0, sizeof(params));
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 1;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, params, 0, NULL, 0 };
    ID3D12RootSignature *rs = root_signature(&rsd);
    const DXGI_FORMAT f4[4] = { view_fmt, view_fmt, view_fmt, view_fmt };
    ID3D12PipelineState *p_g = pso(rs, vs, ps, 4, f4, DXGI_FORMAT_D32_FLOAT);
    ID3D12PipelineState *p_r = pso(rs, vs, ps_res, 1, f4, DXGI_FORMAT_UNKNOWN);

    ID3D12Resource *rt[5], *depth;
    for (int i = 0; i < 5; i++)
        rt[i] = texture(res_fmt, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
    D3D12_CLEAR_VALUE dcv = { DXGI_FORMAT_D32_FLOAT };
    dcv.DepthStencil.Depth = 1.0f;
    depth = texture(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                    D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv);
    ID3D12DescriptorHeap *rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 5, FALSE);
    ID3D12DescriptorHeap *dsv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, FALSE);
    ID3D12DescriptorHeap *srv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5, TRUE);
    D3D12_RENDER_TARGET_VIEW_DESC rvd = { view_fmt, D3D12_RTV_DIMENSION_TEXTURE2D };
    D3D12_SHADER_RESOURCE_VIEW_DESC svd = { view_fmt, D3D12_SRV_DIMENSION_TEXTURE2D,
                                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
    svd.Texture2D.MipLevels = 1;
    for (int i = 0; i < 5; i++)
        ID3D12Device_CreateRenderTargetView(dev, rt[i], &rvd, cpu_handle(rtv_heap, i));
    for (int i = 0; i < 4; i++)
        ID3D12Device_CreateShaderResourceView(dev, rt[i], &svd, cpu_handle(srv_heap, i));
    D3D12_SHADER_RESOURCE_VIEW_DESC zvd = { DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_TEXTURE2D,
                                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
    zvd.Texture2D.MipLevels = 1;
    /* An R32_FLOAT SRV needs an R32_TYPELESS resource, and the depth buffer is D32_FLOAT: t4 is a null SRV
     * (reads 0). The depth buffer is still loaded and stored by every G-buffer pass. */
    ID3D12Device_CreateShaderResourceView(dev, NULL, &zvd, cpu_handle(srv_heap, 4));
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4], dsv = cpu_handle(dsv_heap, 0), out_rtv = cpu_handle(rtv_heap, 4);
    for (int i = 0; i < 4; i++)
        rtvs[i] = cpu_handle(rtv_heap, i);
    ID3D12Device_CreateDepthStencilView(dev, depth, NULL, dsv);

    D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
    ID3D12QueryHeap *qh = NULL;
    HRESULT hr = ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&qh);
    if (FAILED(hr)) die("CreateQueryHeap", hr);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12GraphicsCommandList *cl = NULL;
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                        (void **)&ca);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL,
                                   &IID_ID3D12GraphicsCommandList, (void **)&cl);

    /* Fill the targets once so the first timed pass loads defined data. */
    float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
    for (int i = 0; i < 5; i++)
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, cpu_handle(rtv_heap, i), clear, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(cl, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
    wait_idle();

    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList_Reset(cl, ca, p_g);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &srv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    /* Each split: a G-buffer pass into the 4 targets and depth, then a pass into the fifth target that
     * samples the 4 (like deferred lighting), so the G-buffer is stored, read as textures and loaded
     * again by the next split. */
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, gpu_handle(srv_heap, 0));
    for (int s = 0; s < splits; s++) {
        float id = (float)s;
        ID3D12GraphicsCommandList_SetPipelineState(cl, p_g);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 1, &id, 0);
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 4, rtvs, FALSE, &dsv);
        ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        for (int i = 0; i < 4; i++)
            transition(cl, rt[i], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12GraphicsCommandList_SetPipelineState(cl, p_r);
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &out_rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        for (int i = 0; i < 4; i++)
            transition(cl, rt[i], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    ID3D12GraphicsCommandList_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb, 0);
    ID3D12GraphicsCommandList_Close(cl);
    char label[96];
    snprintf(label, sizeof(label), "rt format=%-9s content=%-6s splits=%d", fmt_s, content, splits);
    time_list(cl, rb, label);
    printf("rt checksum out %016llx gbuf0 %016llx\n",
           checksum(cl, ca, rt[4], D3D12_RESOURCE_STATE_RENDER_TARGET),
           checksum(cl, ca, rt[0], D3D12_RESOURCE_STATE_RENDER_TARGET));
    return 0;
}

/* A cast view on a render target: an R8G8B8A8_UNORM target (no UAV) viewed as R8G8B8A8_UINT, which D3D12
 * allows with CastingFullyTypedFormatSupported. With VKD3D_SQUARK_RT_NOCAST=1 the target's format list no
 * longer has UINT and KosmicKrisp is expected to log "kk: view as ...". */
static int run_rtcast(void)
{
    ID3D12Resource *rt = texture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
    ID3D12DescriptorHeap *srv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, TRUE);
    D3D12_SHADER_RESOURCE_VIEW_DESC svd = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D12_SRV_DIMENSION_TEXTURE2D,
                                            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
    svd.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, rt, &svd, cpu_handle(srv_heap, 0));
    printf("rtcast: SRV as R8G8B8A8_UNORM_SRGB created (in the format list either way)\n");
    svd.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    ID3D12Device_CreateShaderResourceView(dev, rt, &svd, cpu_handle(srv_heap, 1));
    printf("rtcast: SRV as R8G8B8A8_UINT created\n");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: d3d12gpu cb [iters] | rt <format> [splits] [smooth|noise]\n");
        return 2;
    }
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    compile = dc ? (compile_fn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!compile) {
        printf("FAIL no D3DCompile\n");
        return 1;
    }
    IDXGIFactory4 *fac = NULL;
    IDXGIAdapter1 *ad = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&fac);
    if (FAILED(hr)) die("CreateDXGIFactory1", hr);
    if (FAILED(IDXGIFactory4_EnumAdapters1(fac, 0, &ad))) die("EnumAdapters1", E_FAIL);
    hr = D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr)) die("D3D12CreateDevice", hr);
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0 };
    hr = ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
    if (FAILED(hr)) die("CreateCommandQueue", hr);
    ID3D12CommandQueue_GetTimestampFrequency(queue, &ts_freq);
    ID3D12Device_CreateFence(dev, 0, 0, &IID_ID3D12Fence, (void **)&fence);
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    const char *hb = getenv("HB_KK_NO_UBO_ROBUST");
    printf("HB_KK_NO_UBO_ROBUST=%s VKD3D_SQUARK_RT_NOCAST=%s\n", hb ? hb : "(unset)",
           getenv("VKD3D_SQUARK_RT_NOCAST") ? getenv("VKD3D_SQUARK_RT_NOCAST") : "(unset)");
    if (!strcmp(argv[1], "cb") || !strcmp(argv[1], "cbfix"))
        return run_cb(argc > 2 ? atoi(argv[2]) : 64, !strcmp(argv[1], "cbfix"));
    if (!strcmp(argv[1], "rtcast"))
        return run_rtcast();
    if (!strcmp(argv[1], "rt") && argc > 2)
        return run_rt(argv[2], argc > 3 ? atoi(argv[3]) : 8, argc > 4 ? argv[4] : "smooth");
    printf("usage: d3d12gpu cb [iters] | rt <format> [splits] [smooth|noise]\n");
    return 2;
}
