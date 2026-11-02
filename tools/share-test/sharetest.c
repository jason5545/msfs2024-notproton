/* sharetest.c — D3D11 <-> D3D12 shared texture round trip, the way MSFS 2024 shows its Media Foundation
 * video frames (D3D11 texture, shared handle, ID3D12Device::OpenSharedHandle).
 *
 *   sharetest.exe 0   D3D11 creates (MISC_SHARED | MISC_SHARED_NTHANDLE), D3D12 opens the NT handle
 *   sharetest.exe 1   D3D11 creates (MISC_SHARED, legacy KMT handle), D3D12 opens the KMT handle
 *   sharetest.exe 2   D3D12 creates (HEAP_FLAG_SHARED), D3D11 opens via ID3D11Device1::OpenSharedResource1
 *
 * The producer clears the texture to (0.25, 0.5, 0.75, 1.0); the consumer copies it to a CPU-readable
 * resource and counts matching pixels. Output goes to stdout and sharetest-out.txt.
 * Build: x86_64-w64-mingw32-gcc -O1 -o sharetest.exe sharetest.c -ld3d11 -ld3d12 -ldxgi -ldxguid
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#define W 256
#define H 256
static FILE *out;
#define LOG(...) do { printf(__VA_ARGS__); fflush(stdout); if (out) { fprintf(out, __VA_ARGS__); fflush(out); } } while (0)
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { LOG("FAIL %s = %#lx (line %d)\n", #x, (unsigned long)hr_, __LINE__); return 1; } } while (0)

static const float clear_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
/* B8G8R8A8_UNORM bytes in memory: B, G, R, A */
static const unsigned char want[4] = {191, 128, 64, 255};

static int count_pixels(const unsigned char *base, UINT row_pitch, const char *who)
{
    int ok = 0, x, y;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
        {
            const unsigned char *p = base + y * row_pitch + x * 4;
            if (abs(p[0] - want[0]) <= 1 && abs(p[1] - want[1]) <= 1 && abs(p[2] - want[2]) <= 1 && p[3] == want[3]) ok++;
        }
    LOG("%s: px(0,0) = %u %u %u %u, px(255,255) = %u %u %u %u; matching %d / %d -> %s\n", who,
        base[0], base[1], base[2], base[3],
        base[(H - 1) * row_pitch + (W - 1) * 4], base[(H - 1) * row_pitch + (W - 1) * 4 + 1],
        base[(H - 1) * row_pitch + (W - 1) * 4 + 2], base[(H - 1) * row_pitch + (W - 1) * 4 + 3],
        ok, W * H, ok == W * H ? "SHARED OK" : "NOT SHARED");
    return ok == W * H ? 0 : 2;
}

static int d3d12_read_back(ID3D12Device *dev12, ID3D12Resource *tex)
{
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {0}, td;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    D3D12_TEXTURE_COPY_LOCATION dst, src;
    ID3D12CommandQueue *q; ID3D12CommandAllocator *ca; ID3D12GraphicsCommandList *cl;
    ID3D12Resource *rb; ID3D12Fence *fence; HANDLE ev; UINT64 total; void *map;

    CK(ID3D12Device_CreateCommandQueue(dev12, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CK(ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca));
    CK(ID3D12Device_CreateCommandList(dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    tex->lpVtbl->GetDesc(tex, &td); /* aggregate return: hidden pointer argument in the C vtable */
    LOG("D3D12 resource: %llux%u format %d layout %d flags %#x\n", (unsigned long long)td.Width, td.Height, td.Format, td.Layout, td.Flags);
    ID3D12Device_GetCopyableFootprints(dev12, &td, 0, 1, 0, &fp, NULL, NULL, &total);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(ID3D12Device_CreateCommittedResource(dev12, &hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    CK(ID3D12Device_CreateFence(dev12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CK(ID3D12CommandQueue_Signal(q, fence, 1));
    CK(ID3D12Fence_SetEventOnCompletion(fence, 1, ev));
    WaitForSingleObject(ev, 10000);
    CK(ID3D12Resource_Map(rb, 0, NULL, &map));
    return count_pixels((unsigned char *)map + fp.Offset, fp.Footprint.RowPitch, "D3D12 reads D3D11 texture");
}

static int d3d11_to_d3d12(int kmt)
{
    D3D11_TEXTURE2D_DESC desc = {W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Device *dev11; ID3D11DeviceContext *ctx; ID3D11Texture2D *tex; ID3D11RenderTargetView *rtv;
    ID3D12Device *dev12; ID3D12Resource *res12 = NULL; HANDLE h = NULL; D3D_FEATURE_LEVEL fl; HRESULT hr;

    desc.MiscFlags = kmt ? D3D11_RESOURCE_MISC_SHARED : (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
    CK(ID3D11Device_CreateTexture2D(dev11, &desc, NULL, &tex));
    CK(ID3D11Device_CreateRenderTargetView(dev11, (ID3D11Resource *)tex, NULL, &rtv));
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear_color);
    ID3D11DeviceContext_Flush(ctx);
    if (kmt)
    {
        IDXGIResource *dr;
        CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&dr));
        CK(IDXGIResource_GetSharedHandle(dr, &h));
    }
    else
    {
        IDXGIResource1 *dr1;
        CK(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&dr1));
        CK(IDXGIResource1_CreateSharedHandle(dr1, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &h));
    }
    LOG("D3D11 texture created (%s), shared handle %p\n", kmt ? "MISC_SHARED, KMT" : "MISC_SHARED_NTHANDLE", h);
    Sleep(200);
    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    hr = ID3D12Device_OpenSharedHandle(dev12, h, &IID_ID3D12Resource, (void **)&res12);
    LOG("ID3D12Device::OpenSharedHandle -> %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) { LOG("RESULT NOT SHARED (open failed)\n"); return 3; }
    return d3d12_read_back(dev12, res12);
}

static int d3d12_to_d3d11(void)
{
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS};
    D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_B8G8R8A8_UNORM, {0.25f, 0.5f, 0.75f, 1.0f}};
    D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12Device *dev12; ID3D12Resource *tex; ID3D12DescriptorHeap *heap; ID3D12CommandQueue *q;
    ID3D12CommandAllocator *ca; ID3D12GraphicsCommandList *cl; ID3D12Fence *fence;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv; HANDLE h = NULL, ev; HRESULT hr;
    ID3D11Device *dev11; ID3D11Device1 *dev11_1; ID3D11DeviceContext *ctx; ID3D11Texture2D *t11 = NULL, *staging;
    D3D11_TEXTURE2D_DESC sd; D3D11_MAPPED_SUBRESOURCE m; D3D_FEATURE_LEVEL fl;

    CK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    CK(ID3D12Device_CreateCommittedResource(dev12, &hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void **)&tex));
    CK(ID3D12Device_CreateDescriptorHeap(dev12, &dh, &IID_ID3D12DescriptorHeap, (void **)&heap));
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &rtv);
    ID3D12Device_CreateRenderTargetView(dev12, tex, NULL, rtv);
    CK(ID3D12Device_CreateCommandQueue(dev12, &qd, &IID_ID3D12CommandQueue, (void **)&q));
    CK(ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&ca));
    CK(ID3D12Device_CreateCommandList(dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear_color, 0, NULL);
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
    CK(ID3D12Device_CreateFence(dev12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CK(ID3D12CommandQueue_Signal(q, fence, 1));
    CK(ID3D12Fence_SetEventOnCompletion(fence, 1, ev));
    WaitForSingleObject(ev, 10000);
    hr = ID3D12Device_CreateSharedHandle(dev12, (ID3D12DeviceChild *)tex, NULL, GENERIC_ALL, NULL, &h);
    LOG("ID3D12Device::CreateSharedHandle -> %#lx, handle %p\n", (unsigned long)hr, h);
    if (FAILED(hr)) { LOG("RESULT NOT SHARED (create handle failed)\n"); return 3; }
    CK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &dev11, &fl, &ctx));
    CK(ID3D11Device_QueryInterface(dev11, &IID_ID3D11Device1, (void **)&dev11_1));
    hr = ID3D11Device1_OpenSharedResource1(dev11_1, h, &IID_ID3D11Texture2D, (void **)&t11);
    LOG("ID3D11Device1::OpenSharedResource1 -> %#lx\n", (unsigned long)hr);
    if (FAILED(hr)) { LOG("RESULT NOT SHARED (open failed)\n"); return 3; }
    ID3D11Texture2D_GetDesc(t11, &sd);
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    CK(ID3D11Device_CreateTexture2D(dev11, &sd, NULL, &staging));
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)t11);
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
    return count_pixels(m.pData, m.RowPitch, "D3D11 reads D3D12 texture");
}

int main(int argc, char **argv)
{
    int mode = argc > 1 ? atoi(argv[1]) : 0, r;
    char name[64];
    snprintf(name, sizeof(name), "sharetest-out-%d.txt", mode);
    out = fopen(name, "w");
    LOG("sharetest mode %d (%s)\n", mode, mode == 2 ? "D3D12 -> D3D11" : mode == 1 ? "D3D11 KMT -> D3D12" : "D3D11 NT -> D3D12");
    r = mode == 2 ? d3d12_to_d3d11() : d3d11_to_d3d12(mode == 1);
    LOG("RESULT %s (code %d)\n", r == 0 ? "PASS" : "FAIL", r);
    if (out) fclose(out);
    return r;
}
