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
 *   d3d12gpu stagger [frames] [lists] [iters] [two|one] [reuse|fresh]
 *      A DIRECT and a COMPUTE queue submitting every frame (vkd3d-proton staggered submissions), see run_stagger.
 *   d3d12gpu gaps <nobar|uav|gfx|ts|tsgfx|tsdraw|lists|tslists|exec|tsresolve|tsrescs> [n] [frames] [iters] [groups]
 *      GPU idle between many tiny dispatches in one list or in many (records 25.59), see run_gaps.
 *   d3d12gpu tscheck [frames] [iters]
 *      Timestamp values through write, resolve, reset and write again in one list (records 25.59).
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

/* stagger: a DIRECT and a COMPUTE queue that both submit every frame, the pattern that turns on
 * vkd3d-proton's staggered submissions on a device with one queue family (records 25.58). Each frame the
 * compute queue runs <lists> command lists (one dispatch each) and signals a fence; the direct queue waits
 * for it, runs its own <lists> and signals; the CPU keeps at most 2 frames in flight. Each list writes
 * timestamps around its dispatch. Prints the frame interval (CPU, between frame submissions), the GPU time
 * per frame (sum of the dispatches) and the GPU span of a frame, then a checksum of both buffers.
 * <mode> one: all 2 x <lists> lists go to the direct queue and the compute queue stays idle (control).
 * <rec> fresh: reset and re-record every list each frame, as games do; default (reuse) records them once and
 * submits the same lists again, which KosmicKrisp re-records internally (Metal command buffers are one-time). */
static const char stagger_src[] =
    "RWStructuredBuffer<uint> B : register(u0);\n"
    "cbuffer K : register(b0) { uint salt; };\n"
    "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  uint a = B[id.x];\n"
    "  [loop] for (uint i = 0; i < ITERS; i++) { a = a * 1664525u + 1013904223u + salt; a ^= a >> 13; }\n"
    "  B[id.x] = a; }\n";

#define SG_GROUPS 256
#define SG_MAXL 128

/* One stagger list: timestamp, dispatch on <b>, UAV barrier, timestamp, resolve both into <rb> at <qi>. */
static void sg_record(ID3D12GraphicsCommandList *c, ID3D12RootSignature *rs, ID3D12Resource *b,
                      ID3D12QueryHeap *qh, ID3D12Resource *rb, UINT qi, UINT salt)
{
    ID3D12GraphicsCommandList_SetComputeRootSignature(c, rs);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(c, 0, ID3D12Resource_GetGPUVirtualAddress(b));
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(c, 1, salt, 0);
    ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, qi);
    ID3D12GraphicsCommandList_Dispatch(c, SG_GROUPS, 1, 1);
    D3D12_RESOURCE_BARRIER ub = { D3D12_RESOURCE_BARRIER_TYPE_UAV, 0 };
    ub.UAV.pResource = b;
    ID3D12GraphicsCommandList_ResourceBarrier(c, 1, &ub);
    ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, qi + 1);
    ID3D12GraphicsCommandList_ResolveQueryData(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, qi, 2, rb, (UINT64)qi * 8);
    HRESULT hr = ID3D12GraphicsCommandList_Close(c);
    if (FAILED(hr)) die("Close", hr);
}

static int run_stagger(int frames, int lists, int iters, int one, int fresh)
{
    if (lists < 1 || lists > SG_MAXL || frames < 10) {
        printf("stagger: 10 <= frames, 1 <= lists <= %d\n", SG_MAXL);
        return 2;
    }
    char its[16];
    snprintf(its, sizeof(its), "%d", iters);
    D3D_SHADER_MACRO defs[] = { { "ITERS", its }, { NULL, NULL } };
    ID3DBlob *cs = shader(stagger_src, "cs", "cs_5_0", defs);

    D3D12_ROOT_PARAMETER rp[2];
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 1;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 0, NULL, 0 };
    ID3D12RootSignature *rs = root_signature(&rsd);
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd;
    memset(&cpd, 0, sizeof(cpd));
    cpd.pRootSignature = rs;
    cpd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    cpd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    ID3D12PipelineState *ps = NULL;
    HRESULT hr = ID3D12Device_CreateComputePipelineState(dev, &cpd, &IID_ID3D12PipelineState, (void **)&ps);
    if (FAILED(hr)) die("CreateComputePipelineState", hr);

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0 };
    ID3D12CommandQueue *cq = NULL;
    hr = ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&cq);
    if (FAILED(hr)) die("CreateCommandQueue(compute)", hr);
    UINT64 cfreq = 0;
    ID3D12CommandQueue_GetTimestampFrequency(cq, &cfreq);

    const UINT64 bsize = SG_GROUPS * 64 * 4;
    /* buf[0]: work of the compute queue (or the first half in mode one), buf[1]: the direct queue's. */
    ID3D12Resource *buf[2];
    for (int q = 0; q < 2; q++) {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, bsize, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                   {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
        hr = ID3D12Device_CreateCommittedResource(dev, &hp, 0, &rd, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                  &IID_ID3D12Resource, (void **)&buf[q]);
        if (FAILED(hr)) die("CreateCommittedResource(uav)", hr);
    }

    /* Per frame slot (2) and per queue (2): <lists> lists, 2 timestamps each. */
    const UINT nq = 2 * 2 * lists * 2;
    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, nq, 0 };
    ID3D12QueryHeap *qh = NULL;
    hr = ID3D12Device_CreateQueryHeap(dev, &qhd, &IID_ID3D12QueryHeap, (void **)&qh);
    if (FAILED(hr)) die("CreateQueryHeap", hr);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)nq * 8, D3D12_RESOURCE_STATE_COPY_DEST);

    static ID3D12GraphicsCommandList *cl[2][2][SG_MAXL];
    static ID3D12CommandAllocator *cal[2][2][SG_MAXL];
    for (int s = 0; s < 2; s++)
        for (int q = 0; q < 2; q++)
            for (int l = 0; l < lists; l++) {
                /* q 0 = compute queue, q 1 = direct queue; in mode one both are direct lists. */
                D3D12_COMMAND_LIST_TYPE t = (q == 0 && !one) ? D3D12_COMMAND_LIST_TYPE_COMPUTE
                                                             : D3D12_COMMAND_LIST_TYPE_DIRECT;
                ID3D12CommandAllocator *ca = NULL;
                hr = ID3D12Device_CreateCommandAllocator(dev, t, &IID_ID3D12CommandAllocator, (void **)&ca);
                if (FAILED(hr)) die("CreateCommandAllocator", hr);
                ID3D12GraphicsCommandList *c = NULL;
                hr = ID3D12Device_CreateCommandList(dev, 0, t, ca, ps, &IID_ID3D12GraphicsCommandList,
                                                    (void **)&c);
                if (FAILED(hr)) die("CreateCommandList", hr);
                cal[s][q][l] = ca;
                cl[s][q][l] = c;
                sg_record(c, rs, buf[q], qh, rb, ((s * 2 + q) * lists + l) * 2, (UINT)(q * 1000 + l));
            }

    ID3D12Fence *fc = NULL, *fd = NULL;
    ID3D12Device_CreateFence(dev, 0, 0, &IID_ID3D12Fence, (void **)&fc);
    ID3D12Device_CreateFence(dev, 0, 0, &IID_ID3D12Fence, (void **)&fd);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    LARGE_INTEGER qpf, t0, t1;
    QueryPerformanceFrequency(&qpf);
    const int warm = 20;
    double *iv = calloc(frames, sizeof(double)), *gw = calloc(frames, sizeof(double)),
           *gs = calloc(frames, sizeof(double));
    int ngpu = 0;
    LARGE_INTEGER prev = { 0 }, mstart = { 0 };
    for (int f = 1; f <= frames + warm; f++) {
        int s = f & 1;
        if (f > 2) {
            /* Frame f - 2 used this slot: wait for it, then read its timestamps. */
            if (ID3D12Fence_GetCompletedValue(fd) < (UINT64)(f - 2)) {
                ID3D12Fence_SetEventOnCompletion(fd, f - 2, ev);
                WaitForSingleObject(ev, 30000);
            }
            if (f - 2 > warm) {
                UINT64 *v = NULL;
                D3D12_RANGE r = { 0, (SIZE_T)nq * 8 };
                ID3D12Resource_Map(rb, 0, &r, (void **)&v);
                UINT64 lo = ~0ull, hi = 0;
                double sum = 0;
                for (int q = 0; q < 2; q++)
                    for (int l = 0; l < lists; l++) {
                        UINT qi = ((s * 2 + q) * lists + l) * 2;
                        sum += (double)(v[qi + 1] - v[qi]);
                        if (v[qi] < lo) lo = v[qi];
                        if (v[qi + 1] > hi) hi = v[qi + 1];
                    }
                ID3D12Resource_Unmap(rb, 0, NULL);
                gw[ngpu] = sum * 1000.0 / (double)ts_freq;
                gs[ngpu] = (double)(hi - lo) * 1000.0 / (double)ts_freq;
                ngpu++;
            }
            if (fresh)
                for (int q = 0; q < 2; q++)
                    for (int l = 0; l < lists; l++) {
                        ID3D12CommandAllocator_Reset(cal[s][q][l]);
                        ID3D12GraphicsCommandList_Reset(cl[s][q][l], cal[s][q][l], ps);
                        sg_record(cl[s][q][l], rs, buf[q], qh, rb, ((s * 2 + q) * lists + l) * 2,
                                  (UINT)(q * 1000 + l));
                    }
        }
        QueryPerformanceCounter(&t0);
        if (f == warm + 1)
            mstart = t0;
        if (f > warm + 1)
            iv[f - warm - 2] = (double)(t0.QuadPart - prev.QuadPart) * 1000.0 / (double)qpf.QuadPart;
        prev = t0;
        if (one) {
            for (int q = 0; q < 2; q++)
                for (int l = 0; l < lists; l++)
                    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl[s][q][l]);
            ID3D12CommandQueue_Signal(queue, fd, f);
        } else {
            for (int l = 0; l < lists; l++)
                ID3D12CommandQueue_ExecuteCommandLists(cq, 1, (ID3D12CommandList **)&cl[s][0][l]);
            ID3D12CommandQueue_Signal(cq, fc, f);
            ID3D12CommandQueue_Wait(queue, fc, f);
            for (int l = 0; l < lists; l++)
                ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl[s][1][l]);
            ID3D12CommandQueue_Signal(queue, fd, f);
        }
    }
    ID3D12Fence_SetEventOnCompletion(fd, frames + warm, ev);
    WaitForSingleObject(ev, 30000);
    QueryPerformanceCounter(&t1);
    double total = (double)(t1.QuadPart - mstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
    int niv = frames - 1;
    qsort(iv, niv, sizeof(double), cmp_double);
    qsort(gw, ngpu, sizeof(double), cmp_double);
    qsort(gs, ngpu, sizeof(double), cmp_double);
    printf("stagger mode=%s rec=%s frames=%d lists=%d per queue iters=%d groups=%d VKD3D_CONFIG=%s\n",
           one ? "one" : "two", fresh ? "fresh" : "reuse", frames, lists, iters, SG_GROUPS,
           getenv("VKD3D_CONFIG") ? getenv("VKD3D_CONFIG") : "(unset)");
    printf("frame ms (total/frames) %.3f   interval median %.3f  p90 %.3f  max %.3f\n", total / frames,
           iv[niv / 2], iv[niv * 9 / 10], iv[niv - 1]);
    printf("GPU ms per frame: work median %.3f  span median %.3f   busy (work / frame) %.1f%%\n", gw[ngpu / 2],
           gs[ngpu / 2], 100.0 * gw[ngpu / 2] / (total / frames));
    printf("timestamp freq direct %llu compute %llu\n", (unsigned long long)ts_freq, (unsigned long long)cfreq);

    /* Checksum of both buffers (FNV-1a), through a readback copy on the direct queue. */
    ID3D12Resource *crb = buffer(D3D12_HEAP_TYPE_READBACK, bsize * 2, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                        (void **)&ca);
    ID3D12GraphicsCommandList *c = NULL;
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL,
                                   &IID_ID3D12GraphicsCommandList, (void **)&c);
    for (int q = 0; q < 2; q++)
        ID3D12GraphicsCommandList_CopyBufferRegion(c, crb, bsize * q, buf[q], 0, bsize);
    ID3D12GraphicsCommandList_Close(c);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&c);
    wait_idle();
    unsigned char *p = NULL;
    D3D12_RANGE r = { 0, (SIZE_T)(bsize * 2) };
    ID3D12Resource_Map(crb, 0, &r, (void **)&p);
    unsigned long long h = 1469598103934665603ull;
    for (UINT64 i = 0; i < bsize * 2; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    ID3D12Resource_Unmap(crb, 0, NULL);
    printf("checksum %016llx\n", h);
    return 0;
}

/* gaps: GPU idle between many tiny dispatches (records 25.59). In flight (records 25.57, 25.58) the GPU sat idle
 * 40-160 us about 76 times a frame, between encoders of 2-3 us. Each frame records <n> dispatches of <groups>
 * thread groups (each on a region of its own, so every variant computes the same buffer) and submits them; the
 * CPU keeps at most 2 frames in flight so the GPU is never waiting on the CPU. <variant>:
 *   nobar  one list, the dispatches back to back with no barrier
 *   uav    one list, a UAV barrier after each dispatch
 *   gfx    one list, each dispatch followed by a draw of one triangle into a 64 x 64 target (compute and render
 *          encoders alternate)
 *   ts     one list, a timestamp query after each dispatch
 *   tsgfx  gfx with a timestamp query between each dispatch and its draw
 *   tsdraw gfx with a timestamp query after each draw (inside the render pass)
 *   lists  <n> lists of one dispatch each, in one ExecuteCommandLists call
 *   exec   <n> lists of one dispatch each, one ExecuteCommandLists call per list
 *   tslists  lists with a timestamp query after each dispatch
 *   tsresolve  <n> lists in one ExecuteCommandLists, each shaped like a profiled pass of a game: a timestamp, a
 *          dispatch, a draw, a timestamp inside the render pass, ResolveQueryData of the two at the end
 *   tsrescs  tsresolve without the draw (compute only)
 * Timestamps open the first list and close the last one: the GPU span of a frame divided by <n> is the cost of
 * one dispatch plus whatever comes between two. Lists are reset and re-recorded every frame (KosmicKrisp
 * re-records a list submitted twice, records 25.58). */
static const char gaps_src[] =
    "RWStructuredBuffer<uint> B : register(u0);\n"
    "cbuffer K : register(b0) { uint salt; uint base; };\n"
    "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  uint a = B[base + id.x];\n"
    "  [loop] for (uint i = 0; i < ITERS; i++) { a = a * 1664525u + 1013904223u + salt; a ^= a >> 13; }\n"
    "  B[base + id.x] = a; }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target { return float4((salt & 255) / 255.0, 0.25, 0.5, 1.0); }\n";

#define GP_MAXN 512

static int run_gaps(const char *variant, int n, int frames, int iters, int groups)
{
    enum { NOBAR, UAV, GFX, TS, LISTS, EXEC, TSGFX, TSLISTS, TSDRAW, TSRES, TSRESCS } v;
    if (!strcmp(variant, "nobar")) v = NOBAR;
    else if (!strcmp(variant, "uav")) v = UAV;
    else if (!strcmp(variant, "gfx")) v = GFX;
    else if (!strcmp(variant, "ts")) v = TS;
    else if (!strcmp(variant, "lists")) v = LISTS;
    else if (!strcmp(variant, "exec")) v = EXEC;
    else if (!strcmp(variant, "tsgfx")) v = TSGFX;
    else if (!strcmp(variant, "tslists")) v = TSLISTS;
    else if (!strcmp(variant, "tsdraw")) v = TSDRAW;
    else if (!strcmp(variant, "tsresolve")) v = TSRES;
    else if (!strcmp(variant, "tsrescs")) v = TSRESCS;
    else {
        printf("gaps: variant nobar|uav|gfx|ts|tsgfx|tsdraw|lists|tslists|exec|tsresolve|tsrescs\n");
        return 2;
    }
    if (n < 1 || n > GP_MAXN || frames < 10 || groups < 1) {
        printf("gaps: 1 <= n <= %d, frames >= 10, groups >= 1\n", GP_MAXN);
        return 2;
    }
    const int nl = (v == LISTS || v == EXEC || v == TSLISTS || v == TSRES || v == TSRESCS) ? n : 1;
    const int gfx = v == GFX || v == TSGFX || v == TSDRAW || v == TSRES;
    const int ts = v == TS || v == TSGFX || v == TSLISTS || v == TSDRAW, res = v == TSRES || v == TSRESCS;
    char its[16];
    snprintf(its, sizeof(its), "%d", iters);
    D3D_SHADER_MACRO defs[] = { { "ITERS", its }, { NULL, NULL } };
    ID3DBlob *cs = shader(gaps_src, "cs", "cs_5_0", defs);

    D3D12_ROOT_PARAMETER rp[2];
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 2;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 0, NULL, 0 };
    ID3D12RootSignature *rs = root_signature(&rsd);
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd;
    memset(&cpd, 0, sizeof(cpd));
    cpd.pRootSignature = rs;
    cpd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    cpd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    ID3D12PipelineState *cps = NULL;
    HRESULT hr = ID3D12Device_CreateComputePipelineState(dev, &cpd, &IID_ID3D12PipelineState, (void **)&cps);
    if (FAILED(hr)) die("CreateComputePipelineState", hr);

    /* The draw of gfx: one triangle covering a 64 x 64 target. */
    ID3D12PipelineState *gps = NULL;
    ID3D12Resource *rt = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = { 0 };
    const DXGI_FORMAT rtf = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (gfx) {
        ID3DBlob *vs = shader(vs_src, "vs_main", "vs_5_0", NULL);
        ID3DBlob *ps = shader(gaps_src, "ps", "ps_5_0", defs);
        gps = pso(rs, vs, ps, 1, &rtf, DXGI_FORMAT_UNKNOWN);
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, rtf, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
        hr = ID3D12Device_CreateCommittedResource(dev, &hp, 0, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                                  &IID_ID3D12Resource, (void **)&rt);
        if (FAILED(hr)) die("CreateCommittedResource(rt)", hr);
        ID3D12DescriptorHeap *rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, FALSE);
        rtv = cpu_handle(rtv_heap, 0);
        ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    }

    const UINT region = (UINT)groups * 64;
    const UINT64 bsize = (UINT64)n * region * 4;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, bsize, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                               D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    ID3D12Resource *b = NULL;
    hr = ID3D12Device_CreateCommittedResource(dev, &hp, 0, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                              &IID_ID3D12Resource, (void **)&b);
    if (FAILED(hr)) die("CreateCommittedResource(uav)", hr);

    /* Per frame slot: 2 timestamps around the frame, and n more for variant ts. */
    const UINT qper = 2 + (ts ? n : 0) + (res ? 2 * n : 0), nq = 2 * qper;
    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, nq, 0 };
    ID3D12QueryHeap *qh = NULL;
    hr = ID3D12Device_CreateQueryHeap(dev, &qhd, &IID_ID3D12QueryHeap, (void **)&qh);
    if (FAILED(hr)) die("CreateQueryHeap", hr);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)nq * 8, D3D12_RESOURCE_STATE_COPY_DEST);

    static ID3D12GraphicsCommandList *cl[2][GP_MAXN];
    ID3D12CommandAllocator *ca[2];
    for (int s = 0; s < 2; s++) {
        hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                                 (void **)&ca[s]);
        if (FAILED(hr)) die("CreateCommandAllocator", hr);
        for (int l = 0; l < nl; l++) {
            hr = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca[s], cps,
                                                &IID_ID3D12GraphicsCommandList, (void **)&cl[s][l]);
            if (FAILED(hr)) die("CreateCommandList", hr);
            ID3D12GraphicsCommandList_Close(cl[s][l]);
        }
    }
    D3D12_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT sc = { 0, 0, 64, 64 };

    ID3D12Fence *fd = NULL;
    ID3D12Device_CreateFence(dev, 0, 0, &IID_ID3D12Fence, (void **)&fd);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    LARGE_INTEGER qpf, t0, t1, mstart = { 0 };
    QueryPerformanceFrequency(&qpf);
    const int warm = 20;
    double *span = calloc(frames, sizeof(double));
    int nspan = 0;
    for (int f = 1; f <= frames + warm; f++) {
        const int s = f & 1;
        if (f > 2) {
            /* Frame f - 2 used this slot: wait for it, then read its timestamps. */
            if (ID3D12Fence_GetCompletedValue(fd) < (UINT64)(f - 2)) {
                ID3D12Fence_SetEventOnCompletion(fd, f - 2, ev);
                WaitForSingleObject(ev, 30000);
            }
            if (f - 2 > warm) {
                UINT64 *t = NULL;
                D3D12_RANGE r = { 0, (SIZE_T)nq * 8 };
                ID3D12Resource_Map(rb, 0, &r, (void **)&t);
                span[nspan++] = (double)(t[s * qper + 1] - t[s * qper]) * 1000.0 / (double)ts_freq;
                ID3D12Resource_Unmap(rb, 0, NULL);
            }
        }
        if (f == warm + 1) {
            QueryPerformanceCounter(&t0);
            mstart = t0;
        }
        ID3D12CommandAllocator_Reset(ca[s]);
        for (int l = 0; l < nl; l++) {
            ID3D12GraphicsCommandList *c = cl[s][l];
            ID3D12GraphicsCommandList_Reset(c, ca[s], cps);
            ID3D12GraphicsCommandList_SetComputeRootSignature(c, rs);
            ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(c, 0, ID3D12Resource_GetGPUVirtualAddress(b));
            if (gfx) {
                ID3D12GraphicsCommandList_SetGraphicsRootSignature(c, rs);
                ID3D12GraphicsCommandList_OMSetRenderTargets(c, 1, &rtv, FALSE, NULL);
                ID3D12GraphicsCommandList_RSSetViewports(c, 1, &vp);
                ID3D12GraphicsCommandList_RSSetScissorRects(c, 1, &sc);
                ID3D12GraphicsCommandList_IASetPrimitiveTopology(c, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
            if (l == 0)
                ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, s * qper);
            const int d0 = nl == 1 ? 0 : l, d1 = nl == 1 ? n : l + 1;
            for (int d = d0; d < d1; d++) {
                UINT k[2] = { (UINT)d, (UINT)d * region };
                const UINT qr = s * qper + 2 + 2 * d; /* tsresolve: this list's two timestamps */
                if (res)
                    ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, qr);
                if (gfx)
                    ID3D12GraphicsCommandList_SetPipelineState(c, cps);
                ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(c, 1, 2, k, 0);
                ID3D12GraphicsCommandList_Dispatch(c, groups, 1, 1);
                if (ts && v != TSDRAW)
                    ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, s * qper + 2 + d);
                if (v == UAV) {
                    D3D12_RESOURCE_BARRIER ub = { D3D12_RESOURCE_BARRIER_TYPE_UAV, 0 };
                    ub.UAV.pResource = b;
                    ID3D12GraphicsCommandList_ResourceBarrier(c, 1, &ub);
                } else if (gfx) {
                    ID3D12GraphicsCommandList_SetPipelineState(c, gps);
                    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(c, 1, 2, k, 0);
                    ID3D12GraphicsCommandList_DrawInstanced(c, 3, 1, 0, 0);
                    if (v == TSDRAW)
                        ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, s * qper + 2 + d);
                }
                if (res) {
                    ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, qr + 1);
                    ID3D12GraphicsCommandList_ResolveQueryData(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, qr, 2, rb,
                                                               (UINT64)qr * 8);
                }
            }
            if (l == nl - 1) {
                ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, s * qper + 1);
                ID3D12GraphicsCommandList_ResolveQueryData(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, s * qper, qper, rb,
                                                           (UINT64)s * qper * 8);
            }
            hr = ID3D12GraphicsCommandList_Close(c);
            if (FAILED(hr)) die("Close", hr);
        }
        if (v == EXEC)
            for (int l = 0; l < nl; l++)
                ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl[s][l]);
        else
            ID3D12CommandQueue_ExecuteCommandLists(queue, nl, (ID3D12CommandList **)cl[s]);
        ID3D12CommandQueue_Signal(queue, fd, f);
    }
    ID3D12Fence_SetEventOnCompletion(fd, frames + warm, ev);
    WaitForSingleObject(ev, 30000);
    QueryPerformanceCounter(&t1);
    const double total = (double)(t1.QuadPart - mstart.QuadPart) * 1000.0 / (double)qpf.QuadPart;
    qsort(span, nspan, sizeof(double), cmp_double);
    printf("gaps variant=%s n=%d frames=%d iters=%d groups=%d\n", variant, n, frames, iters, groups);
    printf("frame ms (total/frames) %.4f   GPU span per frame: median %.4f  min %.4f  p90 %.4f ms\n",
           total / frames, span[nspan / 2], span[0], span[nspan * 9 / 10]);
    printf("per dispatch: frame %.2f us  GPU span median %.2f us  min %.2f us\n", 1000.0 * total / frames / n,
           1000.0 * span[nspan / 2] / n, 1000.0 * span[0] / n);

    /* FNV-1a of the buffer through a readback copy. */
    ID3D12Resource *crb = buffer(D3D12_HEAP_TYPE_READBACK, bsize, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList *c = cl[0][0];
    ID3D12CommandAllocator_Reset(ca[0]);
    ID3D12GraphicsCommandList_Reset(c, ca[0], NULL);
    transition(c, b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(c, crb, 0, b, 0, bsize);
    ID3D12GraphicsCommandList_Close(c);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&c);
    wait_idle();
    unsigned char *p = NULL;
    D3D12_RANGE r = { 0, (SIZE_T)bsize };
    ID3D12Resource_Map(crb, 0, &r, (void **)&p);
    unsigned long long h = 1469598103934665603ull;
    for (UINT64 i = 0; i < bsize; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    ID3D12Resource_Unmap(crb, 0, NULL);
    printf("checksum %016llx\n", h);
    return 0;
}

/* tscheck: timestamp values through write, resolve (copy), reset and write again in one list (records 25.59).
 * Each frame, one list: timestamps q0, q1 around dispatch A (<iters> loop iterations, 64 groups), ResolveQueryData
 * into slots 0-1; q0, q1 written again around dispatch B (twice the iterations; vkd3d-proton resets a query
 * written twice in a list inside the list), resolved into slots 2-3; then q2 before and q3 after a draw, q3 inside
 * the render pass, resolved into slots 4-5. Checks every frame: all values nonzero, t1 > t0, t2 >= t1, t3 > t2,
 * t5 > t4, and t0 later than the previous frame's t5 (fresh values, not left over); over all frames, the median
 * B / A between 1.5 and 2.5 (per frame the GPU's timing jitters past those bounds now and then, with or without
 * KK 0014, so the ratio is a check of the medians). Prints the medians. */
static int run_tscheck(int frames, int iters)
{
    char its[16];
    snprintf(its, sizeof(its), "%d", iters);
    D3D_SHADER_MACRO defs[] = { { "ITERS", its }, { NULL, NULL } };
    ID3DBlob *cs = shader(gaps_src, "cs", "cs_5_0", defs);
    char its2[16];
    snprintf(its2, sizeof(its2), "%d", 2 * iters);
    D3D_SHADER_MACRO defs2[] = { { "ITERS", its2 }, { NULL, NULL } };
    ID3DBlob *cs2 = shader(gaps_src, "cs", "cs_5_0", defs2);
    D3D12_ROOT_PARAMETER rp[2];
    memset(rp, 0, sizeof(rp));
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[1].Constants.Num32BitValues = 2;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 0, NULL, 0 };
    ID3D12RootSignature *rs = root_signature(&rsd);
    ID3D12PipelineState *ca_ps = NULL, *cb_ps = NULL;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cpd;
    memset(&cpd, 0, sizeof(cpd));
    cpd.pRootSignature = rs;
    cpd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs);
    cpd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs);
    HRESULT hr = ID3D12Device_CreateComputePipelineState(dev, &cpd, &IID_ID3D12PipelineState, (void **)&ca_ps);
    if (FAILED(hr)) die("CreateComputePipelineState", hr);
    cpd.CS.pShaderBytecode = ID3D10Blob_GetBufferPointer(cs2);
    cpd.CS.BytecodeLength = ID3D10Blob_GetBufferSize(cs2);
    hr = ID3D12Device_CreateComputePipelineState(dev, &cpd, &IID_ID3D12PipelineState, (void **)&cb_ps);
    if (FAILED(hr)) die("CreateComputePipelineState", hr);
    ID3DBlob *vs = shader(vs_src, "vs_main", "vs_5_0", NULL);
    ID3DBlob *ps = shader(gaps_src, "ps", "ps_5_0", defs);
    const DXGI_FORMAT rtf = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12PipelineState *gps = pso(rs, vs, ps, 1, &rtf, DXGI_FORMAT_UNKNOWN);
    ID3D12Resource *rt = texture(rtf, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
    ID3D12DescriptorHeap *rtv_heap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, FALSE);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_handle(rtv_heap, 0);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    const UINT groups = 64;
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, groups * 64 * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                               {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    ID3D12Resource *b = NULL;
    hr = ID3D12Device_CreateCommittedResource(dev, &hp, 0, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                              &IID_ID3D12Resource, (void **)&b);
    if (FAILED(hr)) die("CreateCommittedResource(uav)", hr);
    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0 };
    ID3D12QueryHeap *qh = NULL;
    hr = ID3D12Device_CreateQueryHeap(dev, &qhd, &IID_ID3D12QueryHeap, (void **)&qh);
    if (FAILED(hr)) die("CreateQueryHeap", hr);
    ID3D12Resource *rb = buffer(D3D12_HEAP_TYPE_READBACK, 6 * 8, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator *ca = NULL;
    ID3D12GraphicsCommandList *c = NULL;
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, ca_ps, &IID_ID3D12GraphicsCommandList,
                                   (void **)&c);
    ID3D12GraphicsCommandList_Close(c);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    double *da = calloc(frames, sizeof(double)), *db = calloc(frames, sizeof(double)),
           *dg = calloc(frames, sizeof(double)), *dd = calloc(frames, sizeof(double));
    int bad = 0;
    UINT64 prev_end = 0;
    for (int f = 0; f < frames; f++) {
        ID3D12CommandAllocator_Reset(ca);
        ID3D12GraphicsCommandList_Reset(c, ca, ca_ps);
        ID3D12GraphicsCommandList_SetComputeRootSignature(c, rs);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(c, 0, ID3D12Resource_GetGPUVirtualAddress(b));
        UINT k[2] = { (UINT)f, 0 };
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(c, 1, 2, k, 0);
        ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        ID3D12GraphicsCommandList_Dispatch(c, groups, 1, 1);
        ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        ID3D12GraphicsCommandList_ResolveQueryData(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb, 0);
        ID3D12GraphicsCommandList_SetPipelineState(c, cb_ps);
        ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        ID3D12GraphicsCommandList_Dispatch(c, groups, 1, 1);
        ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        ID3D12GraphicsCommandList_ResolveQueryData(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb, 16);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(c, rs);
        ID3D12GraphicsCommandList_SetPipelineState(c, gps);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(c, 1, 2, k, 0);
        ID3D12GraphicsCommandList_OMSetRenderTargets(c, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(c, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(c, 1, &sc);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(c, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 2);
        ID3D12GraphicsCommandList_DrawInstanced(c, 3, 1, 0, 0);
        ID3D12GraphicsCommandList_EndQuery(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 3);
        ID3D12GraphicsCommandList_ResolveQueryData(c, qh, D3D12_QUERY_TYPE_TIMESTAMP, 2, 2, rb, 32);
        hr = ID3D12GraphicsCommandList_Close(c);
        if (FAILED(hr)) die("Close", hr);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&c);
        wait_idle();
        UINT64 *t = NULL;
        D3D12_RANGE r = { 0, 48 };
        ID3D12Resource_Map(rb, 0, &r, (void **)&t);
        const int ok = t[0] && t[1] && t[2] && t[3] && t[4] && t[5] && t[1] > t[0] && t[2] >= t[1] && t[3] > t[2] &&
                       t[5] > t[4] && t[0] > prev_end;
        prev_end = t[5];
        if (!ok && bad++ < 5)
            printf("frame %d: %llu %llu %llu %llu %llu %llu\n", f, (unsigned long long)t[0], (unsigned long long)t[1],
                   (unsigned long long)t[2], (unsigned long long)t[3], (unsigned long long)t[4], (unsigned long long)t[5]);
        da[f] = (double)(t[1] - t[0]) * 1e6 / (double)ts_freq;
        db[f] = (double)(t[3] - t[2]) * 1e6 / (double)ts_freq;
        dg[f] = ((double)t[2] - (double)t[1]) * 1e6 / (double)ts_freq;
        dd[f] = ((double)t[5] - (double)t[4]) * 1e6 / (double)ts_freq;
        ID3D12Resource_Unmap(rb, 0, NULL);
    }
    qsort(da, frames, sizeof(double), cmp_double);
    qsort(db, frames, sizeof(double), cmp_double);
    qsort(dg, frames, sizeof(double), cmp_double);
    qsort(dd, frames, sizeof(double), cmp_double);
    printf("tscheck frames=%d iters=%d: dispatch A median %.1f us, B median %.1f us (B/A %.2f), A end to B start "
           "median %.1f us, draw median %.1f us\n", frames, iters, da[frames / 2], db[frames / 2],
           db[frames / 2] / da[frames / 2], dg[frames / 2], dd[frames / 2]);
    const double ratio = db[frames / 2] / da[frames / 2];
    const int ratio_ok = ratio >= 1.5 && ratio <= 2.5;
    printf("tscheck %s (%d of %d frames with bad values; median B/A %.2f %s)\n", bad || !ratio_ok ? "FAIL" : "PASS",
           bad, frames, ratio, ratio_ok ? "in 1.5-2.5" : "OUT of 1.5-2.5");
    return bad || !ratio_ok ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        printf("usage: d3d12gpu cb [iters] | rt <format> [splits] [smooth|noise] | stagger [frames] [lists] [iters] [two|one] [reuse|fresh] | gaps <nobar|uav|gfx|ts|tsgfx|tsdraw|lists|tslists|exec|tsresolve|tsrescs> [n] [frames] [iters] [groups] | tscheck [frames] [iters]\n");
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
    if (!strcmp(argv[1], "stagger"))
        return run_stagger(argc > 2 ? atoi(argv[2]) : 300, argc > 3 ? atoi(argv[3]) : 32,
                           argc > 4 ? atoi(argv[4]) : 2000, argc > 5 && !strcmp(argv[5], "one"),
                           argc > 6 && !strcmp(argv[6], "fresh"));
    if (!strcmp(argv[1], "tscheck"))
        return run_tscheck(argc > 2 ? atoi(argv[2]) : 50, argc > 3 ? atoi(argv[3]) : 20000);
    if (!strcmp(argv[1], "gaps") && argc > 2)
        return run_gaps(argv[2], argc > 3 ? atoi(argv[3]) : 64, argc > 4 ? atoi(argv[4]) : 500,
                        argc > 5 ? atoi(argv[5]) : 16, argc > 6 ? atoi(argv[6]) : 4);
    printf("usage: d3d12gpu cb [iters] | rt <format> [splits] [smooth|noise] | stagger [frames] [lists] [iters] [two|one] [reuse|fresh] | gaps <nobar|uav|gfx|ts|tsgfx|tsdraw|lists|tslists|exec|tsresolve|tsrescs> [n] [frames] [iters] [groups] | tscheck [frames] [iters]\n");
    return 2;
}
