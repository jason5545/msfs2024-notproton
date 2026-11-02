/* sharepat.c — stricter checks for D3D11 <-> D3D12 shared textures than sharetest.c (which clears to one color):
 * a per-byte pattern, so a row pitch mismatch between the two sides shows up, other sizes and formats,
 * keyed mutex, and the cases the host-memory path must refuse instead of creating an unshared texture.
 *
 *   sharepat.exe <mode> <width> <height> <dxgi format>   pattern round trip, byte compare
 *       mode 0  D3D11 (MISC_SHARED_NTHANDLE) -> D3D12 OpenSharedHandle
 *       mode 1  D3D11 (MISC_SHARED, KMT handle) -> D3D12
 *       mode 2  D3D12 (HEAP_FLAG_SHARED) -> D3D11 OpenSharedResource1
 *       mode 3  D3D11 (MISC_SHARED_KEYEDMUTEX, KMT) with AcquireSync(0)/ReleaseSync(1) -> D3D12
 *   sharepat.exe neg   unsupported shared textures must fail; the same textures unshared must still work
 *
 * Formats with a known element size: 87 B8G8R8A8_UNORM, 28 R8G8B8A8_UNORM, 24 R10G10B10A2_UNORM,
 * 10 R16G16B16A16_FLOAT, 2 R32G32B32A32_FLOAT, 61 R8_UNORM. Copies are raw, so bytes compare exactly.
 * Output: stdout and sharepat-out.txt. Build: build-sharepat.sh.
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

static FILE *out;
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); if (out) { fprintf(out, __VA_ARGS__); fflush(out); } } while (0)
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { LOG("FAIL %s = %#lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

static UINT W, H, BPP;
static DXGI_FORMAT FMT;

static UINT bpp_of(DXGI_FORMAT f)
{
    switch (f)
    {
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM: return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        case DXGI_FORMAT_R8_UNORM: return 1;
        default: return 0;
    }
}

static unsigned char pat(UINT x, UINT y, UINT i)
{
    return (unsigned char)(x * 7 + y * 13 + i * 29 + (x >> 8) * 3 + (y >> 8) * 5 + 1);
}

static unsigned char *make_pattern(void)
{
    unsigned char *p = malloc((size_t)W * H * BPP);
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++)
            for (UINT i = 0; i < BPP; i++)
                p[((size_t)y * W + x) * BPP + i] = pat(x, y, i);
    /* FLOAT formats: keep the exponent away from NaN so nothing can canonicalize the bits */
    if (FMT == DXGI_FORMAT_R16G16B16A16_FLOAT || FMT == DXGI_FORMAT_R32G32B32A32_FLOAT)
        for (size_t k = 0; k < (size_t)W * H * BPP; k += 2) p[k + 1] &= 0x3f;
    return p;
}

static int compare(const unsigned char *base, UINT row_pitch, const unsigned char *want, const char *who)
{
    size_t bad_px = 0; long first = -1;
    for (UINT y = 0; y < H; y++)
        for (UINT x = 0; x < W; x++)
            if (memcmp(base + (size_t)y * row_pitch + (size_t)x * BPP, want + ((size_t)y * W + x) * BPP, BPP))
            {
                if (first < 0) first = (long)y * W + x;
                bad_px++;
            }
    LOG("%s: %ux%u format %d, read row pitch %u; matching %zu / %u pixels -> %s", who, W, H, FMT, row_pitch,
        (size_t)W * H - bad_px, W * H, bad_px ? "NOT SHARED" : "SHARED OK");
    if (first >= 0) LOG(" (first bad pixel %ld,%ld)", first % W, first / W);
    LOG("\n");
    return bad_px ? 2 : 0;
}

static int wait_queue(ID3D12Device *dev12, ID3D12CommandQueue *q)
{
    ID3D12Fence *fence; HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CK(ID3D12Device_CreateFence(dev12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    CK(ID3D12CommandQueue_Signal(q, fence, 1));
    CK(ID3D12Fence_SetEventOnCompletion(fence, 1, ev));
    WaitForSingleObject(ev, 10000);
    ID3D12Fence_Release(fence);
    CloseHandle(ev);
    return 0;
}

static int d3d12_read(ID3D12Device *dev12, ID3D12Resource *tex, const unsigned char *want)
{
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {0}, td;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    ID3D12CommandQueue *q; ID3D12CommandAllocator *ca; ID3D12GraphicsCommandList *cl; ID3D12Resource *rb;
    UINT64 total; void *map;

    CK(ID3D12Device_CreateCommandQueue(dev12, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CK(ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca));
    CK(ID3D12Device_CreateCommandList(dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    tex->lpVtbl->GetDesc(tex, &td);
    ID3D12Device_GetCopyableFootprints(dev12, &td, 0, 1, 0, &fp, NULL, NULL, &total);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev12, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    if (wait_queue(dev12, q)) return 1;
    CK(ID3D12Resource_Map(rb, 0, NULL, &map));
    return compare((unsigned char *)map + fp.Offset, fp.Footprint.RowPitch, want, "D3D12 reads D3D11 texture");
}

static int d3d11_to_d3d12(int mode)
{
    D3D11_TEXTURE2D_DESC desc = {W, H, 1, 1, FMT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    unsigned char *want = make_pattern();
    ID3D11Device *dev11; ID3D11DeviceContext *ctx; ID3D11Texture2D *tex;
    ID3D12Device *dev12; ID3D12Resource *res12 = NULL; HANDLE h = NULL; D3D_FEATURE_LEVEL fl; HRESULT hr;

    desc.MiscFlags = mode == 0 ? (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
                   : mode == 3 ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
    CK(ID3D11Device_CreateTexture2D(dev11, &desc, NULL, &tex));
    if (mode == 3)
    {
        IDXGIKeyedMutex *km;
        CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIKeyedMutex, (void **)&km));
        hr = IDXGIKeyedMutex_AcquireSync(km, 0, 1000);
        LOG("AcquireSync(0) -> %#lx\n", (unsigned long)hr);
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, want, W * BPP, 0);
        hr = IDXGIKeyedMutex_ReleaseSync(km, 1);
        LOG("ReleaseSync(1) -> %#lx\n", (unsigned long)hr);
    }
    else
    {
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)tex, 0, NULL, want, W * BPP, 0);
        ID3D11DeviceContext_Flush(ctx);
    }
    if (mode == 0)
    {
        IDXGIResource1 *dr1;
        CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&dr1));
        CK(IDXGIResource1_CreateSharedHandle(dr1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h));
    }
    else
    {
        IDXGIResource *dr;
        CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&dr));
        CK(IDXGIResource_GetSharedHandle(dr, &h));
    }
    LOG("D3D11 texture %ux%u format %d misc %#x, shared handle %p\n", W, H, FMT, desc.MiscFlags, h);
    Sleep(200);
    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    hr = ID3D12Device_OpenSharedHandle(dev12, h, &IID_ID3D12Resource, (void **)&res12);
    LOG("ID3D12Device::OpenSharedHandle -> %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 3;
    if (mode == 0) CloseHandle(h); /* the app owns NT handles; the opened resource must not depend on it */
    return d3d12_read(dev12, res12, want);
}

static int d3d12_to_d3d11(void)
{
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT}, up = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, FMT, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS};
    D3D12_RESOURCE_DESC bd = {0};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    unsigned char *want = make_pattern(), *map;
    ID3D12Device *dev12; ID3D12Resource *tex, *ub; ID3D12CommandQueue *q;
    ID3D12CommandAllocator *ca; ID3D12GraphicsCommandList *cl;
    HANDLE h = NULL; HRESULT hr; UINT64 total;
    ID3D11Device *dev11; ID3D11Device1 *dev11_1; ID3D11DeviceContext *ctx; ID3D11Texture2D *t11 = NULL, *staging;
    D3D11_TEXTURE2D_DESC sd; D3D11_MAPPED_SUBRESOURCE m; D3D_FEATURE_LEVEL fl;

    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    CK(ID3D12Device_CreateCommittedResource(dev12, &hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex));
    ID3D12Device_GetCopyableFootprints(dev12, &td, 0, 1, 0, &fp, NULL, NULL, &total);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev12, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&ub));
    CK(ID3D12Resource_Map(ub, 0, NULL, (void **)&map));
    for (UINT y = 0; y < H; y++) memcpy(map + fp.Offset + (size_t)y * fp.Footprint.RowPitch, want + (size_t)y * W * BPP, (size_t)W * BPP);
    CK(ID3D12Device_CreateCommandQueue(dev12, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CK(ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca));
    CK(ID3D12Device_CreateCommandList(dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    src.pResource = ub; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    if (wait_queue(dev12, q)) return 1;
    hr = ID3D12Device_CreateSharedHandle(dev12, (ID3D12DeviceChild *)tex, NULL, GENERIC_ALL, NULL, &h);
    LOG("D3D12 texture %ux%u format %d; CreateSharedHandle -> %#lx, handle %p\n", W, H, FMT, (unsigned long)hr, h);
    if (FAILED(hr)) return 3;
    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
    CK(ID3D11Device_QueryInterface(dev11, &IID_ID3D11Device1, (void **)&dev11_1));
    hr = ID3D11Device1_OpenSharedResource1(dev11_1, h, &IID_ID3D11Texture2D, (void **)&t11);
    LOG("ID3D11Device1::OpenSharedResource1 -> %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 3;
    CloseHandle(h);
    ID3D11Texture2D_GetDesc(t11, &sd);
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    CK(ID3D11Device_CreateTexture2D(dev11, &sd, NULL, &staging));
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)t11);
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
    return compare(m.pData, m.RowPitch, want, "D3D11 reads D3D12 texture");
}

/* unsupported shared textures must fail; unshared ones with the same description must still work */
static int negative(void)
{
    static const struct { const char *name; UINT w, h, mips, layers, samples; DXGI_FORMAT f; UINT bind; } cases[] = {
        {"mips 2",        256, 256, 2, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET},
        {"array 2",       256, 256, 1, 2, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET},
        {"MSAA 4",        256, 256, 1, 1, 4, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET},
        {"NV12",          256, 256, 1, 1, 1, DXGI_FORMAT_NV12,           D3D11_BIND_SHADER_RESOURCE},
        {"D24S8 depth",   256, 256, 1, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL},
    };
    ID3D11Device *dev11; ID3D11DeviceContext *ctx; ID3D12Device *dev12; D3D_FEATURE_LEVEL fl;
    int bad = 0;

    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        D3D11_TEXTURE2D_DESC d = {cases[i].w, cases[i].h, cases[i].mips, cases[i].layers, cases[i].f, {cases[i].samples, 0},
                                  D3D11_USAGE_DEFAULT, cases[i].bind, 0, D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, cases[i].w, cases[i].h, (UINT16)cases[i].layers,
                                  (UINT16)cases[i].mips, cases[i].f, {cases[i].samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
        ID3D11Texture2D *t = NULL; ID3D12Resource *r = NULL; HRESULT s11, u11, s12, u12;
        HANDLE h = NULL;

        if (cases[i].bind & D3D11_BIND_DEPTH_STENCIL) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        else if (cases[i].bind & D3D11_BIND_RENDER_TARGET) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        s11 = ID3D11Device_CreateTexture2D(dev11, &d, NULL, &t);
        if (SUCCEEDED(s11))
        {
            /* if creation succeeded anyway, a handle must not come out of an unshared texture */
            IDXGIResource1 *dr1;
            if (SUCCEEDED(ID3D11Texture2D_QueryInterface(t, &IID_IDXGIResource1, (void **)&dr1)))
                s11 = IDXGIResource1_CreateSharedHandle(dr1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h);
            if (SUCCEEDED(s11)) s11 = S_OK + 1; /* marker: created and shared */
            ID3D11Texture2D_Release(t); t = NULL;
        }
        d.MiscFlags = 0;
        u11 = ID3D11Device_CreateTexture2D(dev11, &d, NULL, &t);
        if (t) ID3D11Texture2D_Release(t);

        s12 = ID3D12Device_CreateCommittedResource(dev12, &hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r);
        if (r) { ID3D12Resource_Release(r); r = NULL; }
        u12 = ID3D12Device_CreateCommittedResource(dev12, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&r);
        if (r) ID3D12Resource_Release(r);

        LOG("neg %-12s D3D11 shared %#010lx unshared %#010lx | D3D12 shared %#010lx unshared %#010lx\n", cases[i].name,
            (unsigned long)s11, (unsigned long)u11, (unsigned long)s12, (unsigned long)u12);
        if (s11 == S_OK + 1 || SUCCEEDED(s12)) { LOG("  -> shared texture was created: should have failed\n"); bad++; }
    }
    LOG("neg: %d case(s) created a shared texture that host memory cannot back\n", bad);
    return bad ? 2 : 0;
}

int main(int argc, char **argv)
{
    int mode, r;
    char name[96];

    if (argc > 1 && !strcmp(argv[1], "neg"))
    {
        out = fopen("sharepat-out-neg.txt", "w");
        r = negative();
        LOG("RESULT %s (code %d)\n", r == 0 ? "PASS" : "FAIL", r);
        return r;
    }
    if (argc < 5) { printf("usage: sharepat <mode 0-3> <width> <height> <dxgi format> | sharepat neg\n"); return 1; }
    mode = atoi(argv[1]); W = atoi(argv[2]); H = atoi(argv[3]); FMT = (DXGI_FORMAT)atoi(argv[4]);
    if (!(BPP = bpp_of(FMT))) { printf("unknown format %d\n", FMT); return 1; }
    snprintf(name, sizeof(name), "sharepat-out-%d-%ux%u-%d.txt", mode, W, H, FMT);
    out = fopen(name, "w");
    LOG("sharepat mode %d (%s), %ux%u format %d\n", mode, mode == 2 ? "D3D12 -> D3D11" : mode == 1 ? "D3D11 KMT -> D3D12"
        : mode == 3 ? "D3D11 keyed mutex KMT -> D3D12" : "D3D11 NT -> D3D12", W, H, FMT);
    r = mode == 2 ? d3d12_to_d3d11() : d3d11_to_d3d12(mode);
    LOG("RESULT %s (code %d)\n", r == 0 ? "PASS" : "FAIL", r);
    return r;
}
