/* mfgrab.c — grabs decoded video frames through Media Foundation and writes them as PPM, so the picture itself can be
 * compared with a reference (mfplay only counts non-black pixels). Each mode stops one step earlier on the way from
 * the decoder to MSFS's D3D12 texture, to find the step that breaks the picture:
 *
 *   mfgrab.exe <file> <mode> [frame,frame,...]
 *
 *   shared   IMFMediaEngine (frame-server mode, D3D11 DXGI device manager, output B8G8R8A8) TransferVideoFrame into a
 *            shared B8G8R8A8 texture (bind SRV|RTV, misc SHARED: what MSFS creates), D3D12 opens it and copies it out.
 *   local    same engine, TransferVideoFrame into a plain (not shared) B8G8R8A8 texture, read back with a D3D11 staging copy.
 *   nodev    same engine without a DXGI device manager; TransferVideoFrame into a WIC bitmap (CPU only, no D3D).
 *   reader   IMFSourceReader with MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, output RGB32 (CPU only).
 *   nv12     IMFSourceReader, output NV12 straight from the decoder; written as Y (PGM) and the full frame converted
 *            here with BT.709 limited range in plain C (reference for the decoder alone).
 *
 * Frames (1-based count of frames handed out by MF) default to 30,90. Output: grab-<mode>-<n>.ppm and mfgrab-out.txt.
 * Build: build-mfgrab.sh.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfmediaengine.h>
#include <wincodec.h>

static FILE *out;
static DWORD t0;
#define LOG(...) do { char b_[512]; snprintf(b_, sizeof(b_), __VA_ARGS__); printf("%6.2f %s", (GetTickCount() - t0) / 1000.0, b_); \
    fflush(stdout); if (out) { fprintf(out, "%6.2f %s", (GetTickCount() - t0) / 1000.0, b_); fflush(out); } } while (0)
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { LOG("FAIL %s = %#lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); exit(1); } } while (0)

static const char *mode;
static int want[16], nwant, done_want;

static int wanted(int n)
{
    for (int i = 0; i < nwant; i++) if (want[i] == n) return 1;
    return 0;
}

/* Writes a BGRA (or BGRX) image as PPM; also logs a few numbers so runs can be compared without opening files. */
static void write_bgra(const char *tag, int n, const BYTE *p, size_t pitch, UINT w, UINT h)
{
    char name[128]; FILE *f; BYTE *row = malloc(w * 3); unsigned long long sum[3] = {0};
    snprintf(name, sizeof(name), "grab-%s-%d.ppm", tag, n);
    f = fopen(name, "wb");
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (UINT y = 0; y < h; y++)
    {
        const BYTE *s = p + (size_t)y * pitch;
        for (UINT x = 0; x < w; x++)
        {
            row[x * 3] = s[x * 4 + 2]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4];
            sum[0] += s[x * 4 + 2]; sum[1] += s[x * 4 + 1]; sum[2] += s[x * 4];
        }
        fwrite(row, 1, w * 3, f);
    }
    fclose(f); free(row);
    LOG("wrote %s (%ux%u, pitch %zu), mean R %.2f G %.2f B %.2f\n", name, w, h, pitch,
        sum[0] / (double)w / h, sum[1] / (double)w / h, sum[2] / (double)w / h);
}

/* ---------- media engine modes ---------- */
static HANDLE ev_canplay, ev_error, ev_ended;
static HRESULT STDMETHODCALLTYPE n_qi(IMFMediaEngineNotify *This, REFIID riid, void **obj)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFMediaEngineNotify)) { *obj = This; return S_OK; }
    *obj = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE n_addref(IMFMediaEngineNotify *This) { return 2; }
static ULONG STDMETHODCALLTYPE n_release(IMFMediaEngineNotify *This) { return 1; }
static HRESULT STDMETHODCALLTYPE n_event(IMFMediaEngineNotify *This, DWORD event, DWORD_PTR p1, DWORD p2)
{
    if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY) SetEvent(ev_canplay);
    if (event == MF_MEDIA_ENGINE_EVENT_ERROR) { LOG("EventNotify ERROR (%#llx, %#lx)\n", (unsigned long long)p1, p2); SetEvent(ev_error); }
    if (event == MF_MEDIA_ENGINE_EVENT_ENDED) SetEvent(ev_ended);
    return S_OK;
}
static IMFMediaEngineNotifyVtbl n_vtbl = {n_qi, n_addref, n_release, n_event};
static IMFMediaEngineNotify notify = {&n_vtbl};

/* D3D12 readback of the shared texture */
static ID3D12Device *dev12; static ID3D12Resource *tex12, *rb; static ID3D12CommandQueue *q;
static ID3D12CommandAllocator *ca; static ID3D12GraphicsCommandList *cl; static ID3D12Fence *fence; static UINT64 fval;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; static HANDLE fev;

static void d3d12_open(HANDLE h)
{
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_HEAP_PROPERTIES rbk = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {0}, td; UINT64 total;
    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    CK(ID3D12Device_OpenSharedHandle(dev12, h, &IID_ID3D12Resource, (void **)&tex12));
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
}

static void d3d12_dump(int n, UINT w, UINT h)
{
    D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, src = {tex12, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    BYTE *map;
    dst.PlacedFootprint = fp; src.SubresourceIndex = 0;
    ID3D12CommandAllocator_Reset(ca);
    ID3D12GraphicsCommandList_Reset(cl, ca, NULL);
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    ID3D12CommandQueue_Signal(q, fence, ++fval);
    ID3D12Fence_SetEventOnCompletion(fence, fval, fev);
    WaitForSingleObject(fev, 5000);
    CK(ID3D12Resource_Map(rb, 0, NULL, (void **)&map));
    write_bgra(mode, n, map + fp.Offset, fp.Footprint.RowPitch, w, h);
    ID3D12Resource_Unmap(rb, 0, NULL);
}

static void d3d11_dump(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, int n)
{
    D3D11_TEXTURE2D_DESC d; ID3D11Texture2D *st; D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D_GetDesc(tex, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    CK(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st));
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)tex);
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m));
    write_bgra(mode, n, m.pData, m.RowPitch, d.Width, d.Height);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
    ID3D11Texture2D_Release(st);
}

static int run_engine(const char *file)
{
    ID3D11Device *dev11 = NULL; ID3D11DeviceContext *ctx = NULL; ID3D10Multithread *mt; IMFDXGIDeviceManager *mgr = NULL; UINT token;
    IMFMediaEngineClassFactory *fac; IMFAttributes *attr; IMFMediaEngine *engine; BSTR url; WCHAR wpath[MAX_PATH];
    D3D11_TEXTURE2D_DESC desc = {0, 0, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *tex = NULL; IWICImagingFactory *wic = NULL; IWICBitmap *bmp = NULL;
    D3D_FEATURE_LEVEL fl; DWORD w = 0, hgt = 0, waited, play_start; int transfers = 0; RECT rect; HRESULT hr;
    BOOL shared = !strcmp(mode, "shared"), nodev = !strcmp(mode, "nodev");

    ev_canplay = CreateEventA(NULL, TRUE, FALSE, NULL); ev_error = CreateEventA(NULL, TRUE, FALSE, NULL); ev_ended = CreateEventA(NULL, TRUE, FALSE, NULL);
    CK(CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IMFMediaEngineClassFactory, (void **)&fac));
    CK(MFCreateAttributes(&attr, 3));
    CK(IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_CALLBACK, (IUnknown *)&notify));
    if (!nodev)
    {
        CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                             NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
        CK(ID3D11Device_QueryInterface(dev11, &IID_ID3D10Multithread, (void **)&mt));
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        CK(MFCreateDXGIDeviceManager(&token, &mgr));
        CK(IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev11, token));
        CK(IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_DXGI_MANAGER, (IUnknown *)mgr));
    }
    CK(IMFAttributes_SetUINT32(attr, &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM));
    CK(IMFMediaEngineClassFactory_CreateInstance(fac, 0, attr, &engine));
    IMFMediaEngine_SetMuted(engine, TRUE);
    MultiByteToWideChar(CP_ACP, 0, file, -1, wpath, MAX_PATH);
    url = SysAllocString(wpath);
    CK(IMFMediaEngine_SetSource(engine, url));
    waited = WaitForMultipleObjects(2, (HANDLE[]){ev_canplay, ev_error}, FALSE, 30000);
    LOG("SetSource -> %s\n", waited == 0 ? "CANPLAY" : waited == 1 ? "ERROR" : "timeout");
    if (waited != 0) return 2;
    CK(IMFMediaEngine_GetNativeVideoSize(engine, &w, &hgt));
    LOG("native video size %lux%lu, mode %s\n", w, hgt, mode);

    if (nodev)
    {
        CK(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic));
        CK(IWICImagingFactory_CreateBitmap(wic, w, hgt, &GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnLoad, &bmp));
    }
    else
    {
        HANDLE h; IDXGIResource *dr;
        desc.Width = w; desc.Height = hgt;
        if (shared) desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        CK(ID3D11Device_CreateTexture2D(dev11, &desc, NULL, &tex));
        if (shared)
        {
            CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&dr));
            CK(IDXGIResource_GetSharedHandle(dr, &h));
            d3d12_open(h);
        }
    }

    CK(IMFMediaEngine_Play(engine));
    SetRect(&rect, 0, 0, w, hgt);
    play_start = GetTickCount();
    while (done_want < nwant && GetTickCount() - play_start < 60000 && WaitForSingleObject(ev_ended, 0))
    {
        LONGLONG pts;
        if (IMFMediaEngine_OnVideoStreamTick(engine, &pts) == S_OK)
        {
            if (nodev) hr = IMFMediaEngine_TransferVideoFrame(engine, (IUnknown *)bmp, NULL, &rect, NULL);
            else hr = IMFMediaEngine_TransferVideoFrame(engine, (IUnknown *)tex, NULL, &rect, NULL);
            if (FAILED(hr)) { if (transfers < 3) LOG("TransferVideoFrame -> %#lx\n", (unsigned long)hr); }
            else if (wanted(++transfers))
            {
                LOG("frame %d pts %lld\n", transfers, (long long)pts);
                if (shared) { ID3D11DeviceContext_Flush(ctx); d3d12_dump(transfers, w, hgt); }
                else if (nodev)
                {
                    IWICBitmapLock *lk; WICRect wr = {0, 0, w, hgt}; UINT stride, size; BYTE *p;
                    CK(IWICBitmap_Lock(bmp, &wr, WICBitmapLockRead, &lk));
                    IWICBitmapLock_GetStride(lk, &stride); IWICBitmapLock_GetDataPointer(lk, &size, &p);
                    write_bgra(mode, transfers, p, stride, w, hgt);
                    IWICBitmapLock_Release(lk);
                }
                else d3d11_dump(dev11, ctx, tex, transfers);
                done_want++;
            }
            if (ctx) ID3D11DeviceContext_Flush(ctx);
        }
        Sleep(2);
    }
    LOG("transfers %d\n", transfers);
    IMFMediaEngine_Shutdown(engine);
    return done_want == nwant ? 0 : 4;
}

/* ---------- source reader modes ---------- */
static int run_reader(const char *file, BOOL nv12)
{
    IMFAttributes *attr; IMFSourceReader *rd; IMFMediaType *mt, *cur; WCHAR wpath[MAX_PATH]; int n = 0;
    UINT32 w = 0, h = 0; UINT64 fs; INT32 stride = 0;

    CK(MFCreateAttributes(&attr, 2));
    if (!nv12) CK(IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
    MultiByteToWideChar(CP_ACP, 0, file, -1, wpath, MAX_PATH);
    CK(MFCreateSourceReaderFromURL(wpath, attr, &rd));
    IMFSourceReader_SetStreamSelection(rd, MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    CK(MFCreateMediaType(&mt));
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, nv12 ? &MFVideoFormat_NV12 : &MFVideoFormat_RGB32);
    CK(IMFSourceReader_SetCurrentMediaType(rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt));
    CK(IMFSourceReader_GetCurrentMediaType(rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur));
    IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &fs); w = fs >> 32; h = (UINT32)fs;
    if (FAILED(IMFMediaType_GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, (UINT32 *)&stride))) stride = 0;
    LOG("reader output %s %ux%u, default stride %d\n", nv12 ? "NV12" : "RGB32", w, h, stride);

    while (done_want < nwant)
    {
        DWORD idx, flags; LONGLONG ts; IMFSample *s = NULL; IMFMediaBuffer *b; BYTE *p; DWORD len;
        CK(IMFSourceReader_ReadSample(rd, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s));
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { LOG("end of stream after %d frames\n", n); break; }
        if (!s) continue;
        if (wanted(++n))
        {
            CK(IMFSample_ConvertToContiguousBuffer(s, &b));
            CK(IMFMediaBuffer_Lock(b, &p, NULL, &len));
            LOG("frame %d ts %lld, buffer %lu bytes\n", n, (long long)ts, len);
            if (!nv12)
            {
                /* Wine reports MF_MT_DEFAULT_STRIDE in pixels here; use the real row size from the buffer length. */
                size_t pitch = len / h;
                if (stride < 0) write_bgra(mode, n, p + (size_t)(h - 1) * pitch, (size_t)0 - pitch, w, h);
                else write_bgra(mode, n, p, pitch, w, h);
            }
            else
            {
                /* NV12 with pitch = len / (h * 3 / 2) rounded; convert with BT.709 limited range, plain C. */
                size_t pitch = len / (h + h / 2); BYTE *bgra = malloc((size_t)w * h * 4);
                char name[128]; FILE *f;
                snprintf(name, sizeof(name), "grab-nv12y-%d.pgm", n); f = fopen(name, "wb");
                fprintf(f, "P5\n%u %u\n255\n", w, h);
                for (UINT32 y = 0; y < h; y++) fwrite(p + y * pitch, 1, w, f);
                fclose(f);
                for (UINT32 y = 0; y < h; y++)
                    for (UINT32 x = 0; x < w; x++)
                    {
                        double Y = p[y * pitch + x], U = p[h * pitch + (y / 2) * pitch + (x & ~1u)], V = p[h * pitch + (y / 2) * pitch + (x & ~1u) + 1];
                        double yy = 1.164383 * (Y - 16), u = U - 128, v = V - 128;
                        double r = yy + 1.792741 * v, g = yy - 0.213249 * u - 0.532909 * v, bb = yy + 2.112402 * u;
                        BYTE *o = bgra + ((size_t)y * w + x) * 4;
                        o[0] = bb < 0 ? 0 : bb > 255 ? 255 : (BYTE)(bb + 0.5); o[1] = g < 0 ? 0 : g > 255 ? 255 : (BYTE)(g + 0.5);
                        o[2] = r < 0 ? 0 : r > 255 ? 255 : (BYTE)(r + 0.5); o[3] = 255;
                    }
                LOG("NV12 pitch %zu\n", pitch);
                write_bgra(mode, n, bgra, w * 4, w, h);
                free(bgra);
            }
            IMFMediaBuffer_Unlock(b); IMFMediaBuffer_Release(b);
            done_want++;
        }
        IMFSample_Release(s);
    }
    return done_want == nwant ? 0 : 4;
}

int main(int argc, char **argv)
{
    char *list, *tok; int rc;
    t0 = GetTickCount();
    if (argc < 3) { printf("usage: mfgrab <file> shared|local|nodev|reader|nv12 [frames]\n"); return 1; }
    mode = argv[2];
    list = strdup(argc > 3 ? argv[3] : "30,90");
    for (tok = strtok(list, ","); tok && nwant < 16; tok = strtok(NULL, ",")) want[nwant++] = atoi(tok);
    out = fopen("mfgrab-out.txt", "w");
    LOG("mfgrab %s %s %s\n", argv[1], mode, argc > 3 ? argv[3] : "30,90");
    CK(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    CK(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    if (!strcmp(mode, "reader")) rc = run_reader(argv[1], FALSE);
    else if (!strcmp(mode, "nv12")) rc = run_reader(argv[1], TRUE);
    else rc = run_engine(argv[1]);
    LOG("RESULT %s (%d of %d frames written)\n", rc ? "INCOMPLETE" : "OK", done_want, nwant);
    return rc;
}
