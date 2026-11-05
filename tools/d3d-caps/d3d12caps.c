/* d3d12caps.exe: what the D3D12 stack (vkd3d-proton + KosmicKrisp under Squark) reports, for the
 * medium-vs-ultra question (records 25.53):
 *   - feature level, shader model, raytracing tier, mesh shaders, sampler feedback, VRS, tiled resources
 *     (does the game have what it needs for raytraced shadows, displacement mapping?)
 *   - GPU timestamps: GetTimestampFrequency, a timestamp pair around a known GPU load, copy-queue
 *     timestamps, GetClockCalibration (can the game's / our own GPU frame timing work?)
 *   - DXGI QueryVideoMemoryInfo before and after a 512 MiB allocation (is GPU memory use visible?)
 * No window. Prints plain text. Exit code 0 even when a query fails; each line says what failed. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <d3d12.h>
#include <dxgi1_4.h>

static void opt(ID3D12Device *d, D3D12_FEATURE f, void *p, UINT n, const char *name, int *ok)
{
    HRESULT hr = ID3D12Device_CheckFeatureSupport(d, f, p, n);
    *ok = SUCCEEDED(hr);
    if (!*ok) printf("%-40s CheckFeatureSupport failed hr=0x%08lx\n", name, (unsigned long)hr);
}

static void memline(IDXGIAdapter3 *a3, const char *tag)
{
    DXGI_QUERY_VIDEO_MEMORY_INFO l = {0}, nl = {0};
    HRESULT h1 = IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l);
    HRESULT h2 = IDXGIAdapter3_QueryVideoMemoryInfo(a3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl);
    printf("mem %-8s local hr=0x%08lx budget=%llu MiB usage=%llu MiB | nonlocal hr=0x%08lx budget=%llu MiB usage=%llu MiB\n",
           tag, (unsigned long)h1, l.Budget >> 20, l.CurrentUsage >> 20, (unsigned long)h2, nl.Budget >> 20,
           nl.CurrentUsage >> 20);
}

int main(void)
{
    IDXGIFactory4 *fac = NULL; IDXGIAdapter1 *ad = NULL; IDXGIAdapter3 *a3 = NULL; ID3D12Device *dev = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&fac);
    if (FAILED(hr)) { printf("CreateDXGIFactory1 failed 0x%08lx\n", (unsigned long)hr); return 0; }
    if (FAILED(IDXGIFactory4_EnumAdapters1(fac, 0, &ad))) { printf("no adapter\n"); return 0; }
    DXGI_ADAPTER_DESC1 ds; IDXGIAdapter1_GetDesc1(ad, &ds);
    printf("adapter %ls vendor=0x%04x device=0x%04x dedicated=%llu MiB shared=%llu MiB\n", ds.Description,
           ds.VendorId, ds.DeviceId, (unsigned long long)ds.DedicatedVideoMemory >> 20,
           (unsigned long long)ds.SharedSystemMemory >> 20);
    hr = D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
    if (FAILED(hr)) { printf("D3D12CreateDevice failed 0x%08lx\n", (unsigned long)hr); return 0; }
    int ok;

    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                               D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 5, lv, 0 };
    opt(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl), "FEATURE_LEVELS", &ok);
    if (ok) printf("MaxSupportedFeatureLevel 0x%x\n", fl.MaxSupportedFeatureLevel);
    D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_7 };
    opt(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm), "SHADER_MODEL", &ok);
    if (ok) printf("HighestShaderModel 0x%x\n", sm.HighestShaderModel);

    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {0};
    opt(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o), "OPTIONS", &ok);
    if (ok) printf("OPTIONS: ROVs=%d TiledResourcesTier=%d ResourceBindingTier=%d ConservativeRasterTier=%d "
                   "ResourceHeapTier=%d DoublePrecision=%d\n", o.ROVsSupported, o.TiledResourcesTier,
                   o.ResourceBindingTier, o.ConservativeRasterizationTier, o.ResourceHeapTier,
                   o.DoublePrecisionFloatShaderOps);
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {0};
    opt(dev, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1), "OPTIONS1", &ok);
    if (ok) printf("OPTIONS1: WaveOps=%d WaveLaneCountMin=%u Max=%u Int64=%d\n", o1.WaveOps,
                   o1.WaveLaneCountMin, o1.WaveLaneCountMax, o1.Int64ShaderOps);
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {0};
    opt(dev, D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3), "OPTIONS3", &ok);
    if (ok) printf("OPTIONS3: CopyQueueTimestampQueries=%d CastingFullyTypedFormat=%d ViewInstancingTier=%d "
                   "Barycentrics=%d\n", o3.CopyQueueTimestampQueriesSupported,
                   o3.CastingFullyTypedFormatSupported, o3.ViewInstancingTier, o3.BarycentricsSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {0};
    opt(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5), "OPTIONS5", &ok);
    if (ok) printf("OPTIONS5: RaytracingTier=%d (0=not supported, 10=1.0, 11=1.1) RenderPassesTier=%d SRVOnlyTiledTier3=%d\n",
                   o5.RaytracingTier, o5.RenderPassesTier, o5.SRVOnlyTiledResourceTier3);
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {0};
    opt(dev, D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6), "OPTIONS6", &ok);
    if (ok) printf("OPTIONS6: VariableShadingRateTier=%d\n", o6.VariableShadingRateTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {0};
    opt(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7), "OPTIONS7", &ok);
    if (ok) printf("OPTIONS7: MeshShaderTier=%d SamplerFeedbackTier=%d\n", o7.MeshShaderTier, o7.SamplerFeedbackTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {0};
    opt(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12), "OPTIONS12", &ok);
    if (ok) printf("OPTIONS12: EnhancedBarriers=%d\n", o12.EnhancedBarriersSupported);

    /* Pipeline statistics query heap. */
    D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 1, 0 };
    ID3D12QueryHeap *qps = NULL;
    hr = ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&qps);
    printf("pipeline statistics query heap hr=0x%08lx\n", (unsigned long)hr);
    if (qps) ID3D12QueryHeap_Release(qps);

    /* Timestamps on the direct queue around N clears of a 4096^2 RGBA16F target, then the same on a copy queue. */
    for (int qt = 0; qt < 2; qt++) {
        D3D12_COMMAND_LIST_TYPE t = qt ? D3D12_COMMAND_LIST_TYPE_COPY : D3D12_COMMAND_LIST_TYPE_DIRECT;
        const char *tn = qt ? "copy" : "direct";
        D3D12_COMMAND_QUEUE_DESC cq = { t, 0, 0, 0 };
        ID3D12CommandQueue *q = NULL; ID3D12CommandAllocator *ca = NULL; ID3D12GraphicsCommandList *cl = NULL;
        ID3D12QueryHeap *qh = NULL; ID3D12Resource *rb = NULL, *rt = NULL, *src = NULL, *dst = NULL;
        ID3D12DescriptorHeap *dh = NULL; ID3D12Fence *fe = NULL;
        if (FAILED(ID3D12Device_CreateCommandQueue(dev, &cq, &IID_ID3D12CommandQueue, (void **)&q))) {
            printf("ts %s: CreateCommandQueue failed\n", tn); continue; }
        UINT64 freq = 0;
        hr = ID3D12CommandQueue_GetTimestampFrequency(q, &freq);
        printf("ts %s: GetTimestampFrequency hr=0x%08lx freq=%llu Hz\n", tn, (unsigned long)hr, (unsigned long long)freq);
        UINT64 gts = 0, cts = 0;
        hr = ID3D12CommandQueue_GetClockCalibration(q, &gts, &cts);
        printf("ts %s: GetClockCalibration hr=0x%08lx gpu=%llu cpu=%llu\n", tn, (unsigned long)hr,
               (unsigned long long)gts, (unsigned long long)cts);
        ID3D12Device_CreateCommandAllocator(dev, t, &IID_ID3D12CommandAllocator, (void **)&ca);
        ID3D12Device_CreateCommandList(dev, 0, t, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl);
        D3D12_QUERY_HEAP_DESC td = { qt ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
        hr = ID3D12Device_CreateQueryHeap(dev, &td, &IID_ID3D12QueryHeap, (void **)&qh);
        if (FAILED(hr)) { printf("ts %s: CreateQueryHeap failed hr=0x%08lx\n", tn, (unsigned long)hr); continue; }
        D3D12_HEAP_PROPERTIES hrb = { D3D12_HEAP_TYPE_READBACK }, hdef = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
        ID3D12Device_CreateCommittedResource(dev, &hrb, 0, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                             &IID_ID3D12Resource, (void **)&rb);
        int n = 0;
        ID3D12GraphicsCommandList_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        if (!qt) {
            D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4096, 4096, 1, 1,
                                       DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
            ID3D12Device_CreateCommittedResource(dev, &hdef, 0, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                                 &IID_ID3D12Resource, (void **)&rt);
            D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0, 0 };
            ID3D12Device_CreateDescriptorHeap(dev, &dhd, &IID_ID3D12DescriptorHeap, (void **)&dh);
            D3D12_CPU_DESCRIPTOR_HANDLE h;
            dh->lpVtbl->GetCPUDescriptorHandleForHeapStart(dh, &h); /* aggregate return: vtbl form */
            ID3D12Device_CreateRenderTargetView(dev, rt, NULL, h);
            for (n = 0; n < 64; n++) {
                float c[4] = { n / 64.0f, 0.5f, 0.25f, 1.0f };
                ID3D12GraphicsCommandList_ClearRenderTargetView(cl, h, c, 0, NULL);
            }
        } else {
            bd.Width = 256 << 20;
            ID3D12Device_CreateCommittedResource(dev, &hdef, 0, &bd, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                 &IID_ID3D12Resource, (void **)&src);
            ID3D12Device_CreateCommittedResource(dev, &hdef, 0, &bd, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                 &IID_ID3D12Resource, (void **)&dst);
            for (n = 0; n < 8; n++) ID3D12GraphicsCommandList_CopyResource(cl, dst, src);
        }
        ID3D12GraphicsCommandList_EndQuery(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        ID3D12GraphicsCommandList_ResolveQueryData(cl, qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, rb, 0);
        ID3D12GraphicsCommandList_Close(cl);
        ID3D12Device_CreateFence(dev, 0, 0, &IID_ID3D12Fence, (void **)&fe);
        LARGE_INTEGER f0, c0, c1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&c0);
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
        ID3D12CommandQueue_Signal(q, fe, 1);
        HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        ID3D12Fence_SetEventOnCompletion(fe, 1, ev);
        WaitForSingleObject(ev, 10000);
        QueryPerformanceCounter(&c1);
        UINT64 *v = NULL; D3D12_RANGE r = { 0, 16 };
        ID3D12Resource_Map(rb, 0, &r, (void **)&v);
        double gpu_ms = freq ? (double)(v[1] - v[0]) * 1000.0 / freq : -1;
        printf("ts %s: %d %s, t0=%llu t1=%llu delta=%lld ticks = %.3f ms GPU; CPU submit-to-fence %.3f ms\n", tn, n,
               qt ? "copies of 256 MiB" : "clears of 4096x4096 RGBA16F", (unsigned long long)v[0],
               (unsigned long long)v[1], (long long)(v[1] - v[0]), gpu_ms,
               (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / f0.QuadPart);
        ID3D12Resource_Unmap(rb, 0, NULL);
    }

    /* Memory budget visibility. */
    if (SUCCEEDED(IDXGIAdapter1_QueryInterface(ad, &IID_IDXGIAdapter3, (void **)&a3))) {
        memline(a3, "before");
        D3D12_HEAP_PROPERTIES hdef = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 512ull << 20, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
                                   {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
        ID3D12Resource *big = NULL;
        hr = ID3D12Device_CreateCommittedResource(dev, &hdef, 0, &bd, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                  &IID_ID3D12Resource, (void **)&big);
        printf("alloc 512 MiB default heap hr=0x%08lx\n", (unsigned long)hr);
        memline(a3, "after");
    } else printf("IDXGIAdapter3 not available\n");
    printf("done\n");
    return 0;
}
