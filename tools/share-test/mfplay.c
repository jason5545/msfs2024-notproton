/* mfplay.c — plays a video file through Media Foundation the way MSFS 2024 shows its intro videos, without the
 * game: an IMFMediaEngine in frame-server mode with a D3D11 DXGI device manager (DXVK), OnVideoStreamTick +
 * TransferVideoFrame into a shared B8G8R8A8 texture each frame, and D3D12 (vkd3d-proton) opening that texture
 * through its shared handle. Once a second D3D12 copies it out and counts pixels that are not black.
 *
 *   mfplay.exe <file> [seconds] [lock]
 *
 * "lock" imitates an application lock: the main loop holds a critical section around OnVideoStreamTick and
 * TransferVideoFrame, and EventNotify (called on Media Foundation threads) takes the same lock.
 * A watchdog thread reports STALL and exits with code 9 when the main loop makes no progress for 15 s.
 * Output: stdout and mfplay-out.txt. Build: build-mfplay.sh.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfmediaengine.h>

static FILE *out;
static DWORD t0;
#define LOG(...) do { char b_[512]; snprintf(b_, sizeof(b_), __VA_ARGS__); printf("%6.2f %04lx %s", (GetTickCount() - t0) / 1000.0, GetCurrentThreadId(), b_); \
    fflush(stdout); if (out) { fprintf(out, "%6.2f %04lx %s", (GetTickCount() - t0) / 1000.0, GetCurrentThreadId(), b_); fflush(out); } } while (0)
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { LOG("FAIL %s = %#lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

static CRITICAL_SECTION app_cs;
static BOOL use_lock;
static HANDLE ev_canplay, ev_error, ev_ended;
static volatile LONG progress, phase;

/* IMFMediaEngineNotify */
static HRESULT STDMETHODCALLTYPE n_qi(IMFMediaEngineNotify *This, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFMediaEngineNotify)) { *obj = This; return S_OK; }
    *obj = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE n_addref(IMFMediaEngineNotify *This) { return 2; }
static ULONG STDMETHODCALLTYPE n_release(IMFMediaEngineNotify *This) { return 1; }
static HRESULT STDMETHODCALLTYPE n_event(IMFMediaEngineNotify *This, DWORD event, DWORD_PTR p1, DWORD p2)
{
    if (event != MF_MEDIA_ENGINE_EVENT_TIMEUPDATE && event != MF_MEDIA_ENGINE_EVENT_PROGRESS)
        LOG("EventNotify %lu (%#llx, %#lx)\n", event, (unsigned long long)p1, p2);
    if (use_lock) { EnterCriticalSection(&app_cs); LeaveCriticalSection(&app_cs); }
    if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY) SetEvent(ev_canplay);
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) SetEvent(ev_error);
    if (event == MF_MEDIA_ENGINE_EVENT_ENDED) SetEvent(ev_ended);
    return S_OK;
}
static IMFMediaEngineNotifyVtbl n_vtbl = {n_qi, n_addref, n_release, n_event};
static IMFMediaEngineNotify notify = {&n_vtbl};

static DWORD WINAPI watchdog(void *arg)
{
    LONG last = -1; int still = 0;
    for (;;)
    {
        Sleep(1000);
        if (progress == last) { if (++still == 15) { LOG("STALL: no progress for 15 s in phase %ld\n", phase); ExitProcess(9); } }
        else { last = progress; still = 0; }
    }
}

/* D3D12 side */
static ID3D12Device *dev12; static ID3D12Resource *tex12, *rb; static ID3D12CommandQueue *q;
static ID3D12CommandAllocator *ca; static ID3D12GraphicsCommandList *cl; static ID3D12Fence *fence; static UINT64 fval;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; static HANDLE fev;

static int d3d12_open(HANDLE h)
{
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_HEAP_PROPERTIES rbk = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {0}, td; UINT64 total; HRESULT hr;
    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    hr = ID3D12Device_OpenSharedHandle(dev12, h, &IID_ID3D12Resource, (void **)&tex12);
    LOG("ID3D12Device::OpenSharedHandle -> %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 1;
    tex12->lpVtbl->GetDesc(tex12, &td);
    ID3D12Device_GetCopyableFootprints(dev12, &td, 0, 1, 0, &fp, NULL, NULL, &total);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev12, &rbk, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    CK(ID3D12Device_CreateCommandQueue(dev12, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CK(ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca));
    CK(ID3D12Device_CreateCommandList(dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    CK(ID3D12GraphicsCommandList_Close(cl));
    CK(ID3D12Device_CreateFence(dev12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    fev = CreateEventA(NULL, FALSE, FALSE, NULL);
    return 0;
}

static void d3d12_check(UINT w, UINT h, int transfers)
{
    D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {tex12, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    unsigned char *map; size_t nonblack = 0; UINT32 center;
    dst.PlacedFootprint = fp; src.SubresourceIndex = 0;
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList_Reset(cl, ca, NULL);
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    ID3D12CommandQueue_Signal(q, fence, ++fval);
    ID3D12Fence_SetEventOnCompletion(fence, fval, fev);
    WaitForSingleObject(fev, 5000);
    if (FAILED(ID3D12Resource_Map(rb, 0, NULL, (void **)&map))) return;
    map += fp.Offset;
    for (UINT y = 0; y < h; y += 4)
        for (UINT x = 0; x < w; x += 4)
        {
            UINT32 v; memcpy(&v, map + (size_t)y * fp.Footprint.RowPitch + (size_t)x * 4, 4);
            if (v & 0x00ffffff) nonblack++;
        }
    memcpy(&center, map + (size_t)(h / 2) * fp.Footprint.RowPitch + (size_t)(w / 2) * 4, 4);
    ID3D12Resource_Unmap(rb, 0, NULL);
    LOG("frames transferred %d; D3D12 sees %zu / %u sampled pixels not black, center %08x\n", transfers, nonblack, (w / 4) * (h / 4), center);
}

int main(int argc, char **argv)
{
    ID3D11Device *dev11; ID3D11DeviceContext *ctx; ID3D10Multithread *mt; IMFDXGIDeviceManager *mgr; UINT token;
    IMFMediaEngineClassFactory *fac; IMFAttributes *attr; IMFMediaEngine *engine; BSTR url; WCHAR wpath[MAX_PATH];
    D3D11_TEXTURE2D_DESC desc = {0, 0, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED};
    ID3D11Texture2D *tex; IDXGIResource *dr; HANDLE h; D3D_FEATURE_LEVEL fl; DWORD w = 0, hgt = 0, waited;
    double seconds = argc > 2 ? atof(argv[2]) : 6.0; int transfers = 0, ticks_ok = 0; DWORD next_check, play_start; RECT rect; HRESULT hr;

    t0 = GetTickCount();
    if (argc < 2) { printf("usage: mfplay <file> [seconds] [lock]\n"); return 1; }
    use_lock = argc > 3 && !strcmp(argv[3], "lock");
    out = fopen("mfplay-out.txt", "w");
    InitializeCriticalSection(&app_cs);
    ev_canplay = CreateEventA(NULL, TRUE, FALSE, NULL); ev_error = CreateEventA(NULL, TRUE, FALSE, NULL); ev_ended = CreateEventA(NULL, TRUE, FALSE, NULL);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    LOG("mfplay %s, %.1f s, app lock %s\n", argv[1], seconds, use_lock ? "on" : "off");

    phase = 1;
    CK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    CK(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                         NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
    CK(ID3D11Device_QueryInterface(dev11, &IID_ID3D10Multithread, (void **)&mt));
    ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
    CK(MFCreateDXGIDeviceManager(&token, &mgr));
    CK(IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev11, token));
    CK(CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IMFMediaEngineClassFactory, (void **)&fac));
    CK(MFCreateAttributes(&attr, 3));
    CK(IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_CALLBACK, (IUnknown *)&notify));
    CK(IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_DXGI_MANAGER, (IUnknown *)mgr));
    CK(IMFAttributes_SetUINT32(attr, &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM));
    CK(IMFMediaEngineClassFactory_CreateInstance(fac, 0, attr, &engine));
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wpath, MAX_PATH);
    url = SysAllocString(wpath);
    phase = 2;
    CK(IMFMediaEngine_SetSource(engine, url));
    waited = WaitForMultipleObjects(2, (HANDLE[]){ev_canplay, ev_error}, FALSE, 20000);
    LOG("SetSource -> %s\n", waited == 0 ? "CANPLAY" : waited == 1 ? "ERROR" : "timeout");
    if (waited != 0) { MF_MEDIA_ENGINE_ERR e = 0; IMFMediaError *me; if (SUCCEEDED(IMFMediaEngine_GetError(engine, &me)) && me) e = IMFMediaError_GetErrorCode(me); LOG("error code %d\n", e); return 2; }
    CK(IMFMediaEngine_GetNativeVideoSize(engine, &w, &hgt));
    LOG("native video size %lux%lu\n", w, hgt);

    desc.Width = w; desc.Height = hgt;
    CK(ID3D11Device_CreateTexture2D(dev11, &desc, NULL, &tex));
    CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&dr));
    CK(IDXGIResource_GetSharedHandle(dr, &h));
    if (d3d12_open(h)) return 3;

    phase = 3;
    CK(IMFMediaEngine_Play(engine));
    SetRect(&rect, 0, 0, w, hgt);
    next_check = GetTickCount() + 1000;
    play_start = GetTickCount();
    while ((GetTickCount() - play_start) / 1000.0 < seconds + 2 && WaitForSingleObject(ev_ended, 0))
    {
        LONGLONG pts;
        if (use_lock) EnterCriticalSection(&app_cs);
        hr = IMFMediaEngine_OnVideoStreamTick(engine, &pts);
        if (hr == S_OK)
        {
            ticks_ok++;
            if (SUCCEEDED(hr = IMFMediaEngine_TransferVideoFrame(engine, (IUnknown *)tex, NULL, &rect, NULL))) transfers++;
            else if (transfers == 0 && ticks_ok < 5) LOG("TransferVideoFrame -> %#lx\n", (unsigned long)hr);
            ID3D11DeviceContext_Flush(ctx);
        }
        if (use_lock) LeaveCriticalSection(&app_cs);
        InterlockedIncrement(&progress);
        if (GetTickCount() >= next_check) { d3d12_check(w, hgt, transfers); next_check += 1000; }
        Sleep(5);
    }
    phase = 4;
    d3d12_check(w, hgt, transfers);
    LOG("ticks with a new frame %d, transfers %d, ended %s\n", ticks_ok, transfers, WaitForSingleObject(ev_ended, 0) ? "no" : "yes");
    IMFMediaEngine_Shutdown(engine);
    phase = 5;
    LOG("RESULT %s\n", transfers ? "FRAMES" : "NO FRAMES");
    return transfers ? 0 : 4;
}
