/*
 * d3d12test: the D3D12 path MSFS 2024 needs, in one small Windows x64 program.
 *
 *  1. DXGI factory and adapter; D3D12CreateDevice; max feature level, highest shader model (asks for 6.7),
 *     binding/mesh/raytracing tiers. Says whether d3d12.dll / dxgi.dll are Wine builtins or native (vkd3d-proton, DXVK).
 *  2. A 640x360 window with a FLIP_DISCARD swapchain, 120 presented frames (about 2 s).
 *  3. Frame 30: a geometry shader expands one point into a full-screen green quad. Frame 60: hull and domain shaders
 *     tessellate one full-screen patch, blue. Each is read back from the back buffer before presenting, so a wrong
 *     colour means the stage did not run (or ran wrong), not just that the pipeline was accepted.
 *     Shaders are compiled at run time with d3dcompiler_47.dll (SM 5.0); a stage the compiler refuses is SKIPped.
 *
 * Build: build.sh (llvm-mingw, x86_64-w64-mingw32).
 */
#define COBJMACROS
#define INITGUID
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#define W 640
#define H 360

static const char *hlsl =
  "struct VSOut { float4 pos : SV_Position; float4 col : COLOR; };\n"
  "VSOut vs_point(uint id : SV_VertexID) { VSOut o; o.pos = float4(0, 0, 0, 1); o.col = float4(0, 1, 0, 1); return o; }\n"
  "[maxvertexcount(4)]\n"
  "void gs_quad(point VSOut i[1], inout TriangleStream<VSOut> s) {\n"
  "  VSOut o; o.col = i[0].col;\n"
  "  o.pos = float4(-1, -1, 0, 1); s.Append(o);\n"
  "  o.pos = float4(-1,  1, 0, 1); s.Append(o);\n"
  "  o.pos = float4( 1, -1, 0, 1); s.Append(o);\n"
  "  o.pos = float4( 1,  1, 0, 1); s.Append(o);\n"
  "}\n"
  "float4 ps_main(VSOut i) : SV_Target { return i.col; }\n"
  "struct CP { float4 pos : POSITION; };\n"
  "CP vs_cp(uint id : SV_VertexID) {\n"
  "  CP o; float2 p = id == 0 ? float2(-1, -1) : (id == 1 ? float2(-1, 3) : float2(3, -1));\n"
  "  o.pos = float4(p, 0, 1); return o; }\n"
  "struct PC { float e[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };\n"
  "PC hs_const(InputPatch<CP, 3> ip) { PC o; o.e[0] = 4; o.e[1] = 4; o.e[2] = 4; o.inside = 4; return o; }\n"
  "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)]\n"
  "[patchconstantfunc(\"hs_const\")]\n"
  "CP hs_main(InputPatch<CP, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n"
  "[domain(\"tri\")]\n"
  "VSOut ds_main(PC pc, float3 b : SV_DomainLocation, const OutputPatch<CP, 3> p) {\n"
  "  VSOut o; o.pos = p[0].pos * b.x + p[1].pos * b.y + p[2].pos * b.z; o.col = float4(0, 0, 1, 1); return o; }\n";

typedef HRESULT(WINAPI *compile_fn)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *,
                                     const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
static compile_fn compile;

static ID3DBlob *cs(const char *entry, const char *target) {
  ID3DBlob *code = NULL, *err = NULL;
  HRESULT hr = compile(hlsl, strlen(hlsl), "t.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
  if (FAILED(hr)) {
    printf("  D3DCompile(%s, %s) = 0x%08lx %s\n", entry, target, hr,
           err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
    return NULL;
  }
  return code;
}

static const char *kind(const char *dll) {
  HMODULE m = GetModuleHandleA(dll);
  if (!m) return "not loaded";
  char path[MAX_PATH];
  GetModuleFileNameA(m, path, sizeof(path));
  static char out[4][MAX_PATH + 32];
  static int slot;
  char *o = out[slot++ & 3];
  int builtin = !memcmp((char *)m + 0x40, "Wine builtin DLL", 16);
  snprintf(o, MAX_PATH + 32, "%s (%s)", builtin ? "Wine builtin" : "native", path);
  return o;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;

static void wait_gpu(void) {
  fence_value++;
  ID3D12CommandQueue_Signal(queue, fence, fence_value);
  if (ID3D12Fence_GetCompletedValue(fence) < fence_value) {
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event);
    WaitForSingleObject(fence_event, 5000);
  }
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b = {0};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static ID3D12PipelineState *pso(ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *gs, ID3DBlob *hs, ID3DBlob *ds,
                                ID3DBlob *ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo, const char *name) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {0};
  d.pRootSignature = rs;
#define BC(b) ((D3D12_SHADER_BYTECODE){(b) ? ID3D10Blob_GetBufferPointer(b) : NULL, (b) ? ID3D10Blob_GetBufferSize(b) : 0})
  d.VS = BC(vs); d.GS = BC(gs); d.HS = BC(hs); d.DS = BC(ds); d.PS = BC(ps);
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = 0xffffffff;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = topo;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  ID3D12PipelineState *p = NULL;
  HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&p);
  printf("  CreateGraphicsPipelineState(%s) = 0x%08lx\n", name, hr);
  return SUCCEEDED(hr) ? p : NULL;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int fail = 0;
  HRESULT hr;

  IDXGIFactory4 *factory;
  hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory);
  printf("CreateDXGIFactory2 = 0x%08lx\n", hr);
  if (FAILED(hr)) return 1;
  IDXGIAdapter1 *adapter;
  hr = IDXGIFactory4_EnumAdapters1(factory, 0, &adapter);
  if (FAILED(hr)) { printf("EnumAdapters1 = 0x%08lx\n", hr); return 1; }
  DXGI_ADAPTER_DESC1 ad;
  IDXGIAdapter1_GetDesc1(adapter, &ad);
  printf("adapter: %ls vendor 0x%x device 0x%x dedicated %llu MiB shared %llu MiB\n", ad.Description, ad.VendorId,
         ad.DeviceId, (unsigned long long)ad.DedicatedVideoMemory >> 20, (unsigned long long)ad.SharedSystemMemory >> 20);

  hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev);
  printf("D3D12CreateDevice = 0x%08lx\n", hr);
  printf("d3d12.dll: %s\nd3d12core.dll: %s\ndxgi.dll: %s\n", kind("d3d12.dll"), kind("d3d12core.dll"), kind("dxgi.dll"));
  if (FAILED(hr)) return 1;

  D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
  D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {5, lv, 0};
  hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
  printf("max feature level 0x%x (hr 0x%08lx)\n", fl.MaxSupportedFeatureLevel, hr);
  D3D12_FEATURE_DATA_SHADER_MODEL sm = {(D3D_SHADER_MODEL)0x67};
  hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
  printf("highest shader model 0x%x (hr 0x%08lx)\n", sm.HighestShaderModel, hr);
  D3D12_FEATURE_DATA_D3D12_OPTIONS o = {0};
  ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
  printf("binding tier %d tiled tier %d conservative raster tier %d ROVs %d\n", o.ResourceBindingTier,
         o.TiledResourcesTier, o.ConservativeRasterizationTier, o.ROVsSupported);
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {0};
  ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {0};
  ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7));
  printf("raytracing tier %d mesh shader tier %d\n", o5.RaytracingTier, o7.MeshShaderTier);

  D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
  ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue);
  ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
  ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList,
                                 (void **)&list);
  ID3D12GraphicsCommandList_Close(list);
  ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
  fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

  WNDCLASSA wc = {0, wndproc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "d3d12test"};
  RegisterClassA(&wc);
  RECT rc = {0, 0, W, H};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowA("d3d12test", "d3d12test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, rc.right - rc.left,
                            rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);

  DXGI_SWAP_CHAIN_DESC1 sd = {0};
  sd.Width = W; sd.Height = H; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
  sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1 *sc1;
  hr = IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1);
  printf("CreateSwapChainForHwnd = 0x%08lx\n", hr);
  if (FAILED(hr)) return 1;
  IDXGISwapChain3 *sc;
  IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc);

  D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
  ID3D12DescriptorHeap *rtv_heap;
  ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
  UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv0;
  rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
  ID3D12Resource *bb[2];
  for (int i = 0; i < 2; i++) {
    IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bb[i]);
    D3D12_CPU_DESCRIPTOR_HANDLE h = {rtv0.ptr + i * inc};
    ID3D12Device_CreateRenderTargetView(dev, bb[i], NULL, h);
  }

  /* readback buffer for one back buffer */
  D3D12_RESOURCE_DESC bd;
  bd = ID3D12Resource_GetDesc(bb[0]);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
  UINT64 total;
  ID3D12Device_GetCopyableFootprints(dev, &bd, 0, 1, 0, &fp, NULL, NULL, &total);
  D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                            D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0};
  ID3D12Resource *rb;
  hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                            &IID_ID3D12Resource, (void **)&rb);
  printf("readback buffer = 0x%08lx (%llu bytes)\n", hr, (unsigned long long)total);

  /* shaders */
  HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
  compile = dc ? (compile_fn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
  printf("d3dcompiler_47.dll: %s\n", kind("d3dcompiler_47.dll"));
  ID3D12PipelineState *pso_gs = NULL, *pso_ts = NULL;
  ID3D12RootSignature *rs = NULL;
  if (compile) {
    D3D12_ROOT_SIGNATURE_DESC rsd = {0};
    ID3DBlob *rsb, *rse;
    D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse);
    ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb),
                                     &IID_ID3D12RootSignature, (void **)&rs);
    ID3DBlob *ps = cs("ps_main", "ps_5_0");
    ID3DBlob *vsp = cs("vs_point", "vs_5_0"), *gs = cs("gs_quad", "gs_5_0");
    if (ps && vsp && gs) pso_gs = pso(rs, vsp, gs, NULL, NULL, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, "VS+GS+PS");
    ID3DBlob *vsc = cs("vs_cp", "vs_5_0"), *hs = cs("hs_main", "hs_5_0"), *ds = cs("ds_main", "ds_5_0");
    if (ps && vsc && hs && ds) pso_ts = pso(rs, vsc, NULL, hs, ds, ps, D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH, "VS+HS+DS+PS");
  }

  int presents_ok = 0, gs_result = -1, ts_result = -1;
  DWORD t0 = GetTickCount();
  for (int frame = 0; frame < 120; frame++) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    barrier(bb[idx], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE h = {rtv0.ptr + idx * inc};
    float clear[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    if (frame != 30 && frame != 60) { clear[0] = (frame % 60) / 60.0f; clear[1] = 0.2f; clear[2] = 0.4f; }
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, clear, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &h, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, W, H, 0, 1};
    D3D12_RECT sr = {0, 0, W, H};
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sr);
    int check = 0;
    if (frame == 30 && pso_gs) {
      ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
      ID3D12GraphicsCommandList_SetPipelineState(list, pso_gs);
      ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
      ID3D12GraphicsCommandList_DrawInstanced(list, 1, 1, 0, 0);
      check = 1;
    } else if (frame == 60 && pso_ts) {
      ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
      ID3D12GraphicsCommandList_SetPipelineState(list, pso_ts);
      ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
      ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
      check = 2;
    }
    if (check) {
      barrier(bb[idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint = fp;
      D3D12_TEXTURE_COPY_LOCATION src = {bb[idx], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      src.SubresourceIndex = 0;
      ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
      barrier(bb[idx], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else {
      barrier(bb[idx], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)list};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_gpu();
    if (check) {
      unsigned char *p;
      D3D12_RANGE r = {0, (SIZE_T)total};
      ID3D12Resource_Map(rb, 0, &r, (void **)&p);
      unsigned char *c = p + fp.Offset + (UINT64)(H / 2) * fp.Footprint.RowPitch + (W / 2) * 4;
      unsigned char *k = p + fp.Offset + 4 * fp.Footprint.RowPitch + 4 * 4;
      printf("  frame %d center rgba %u %u %u %u, corner %u %u %u %u\n", frame, c[0], c[1], c[2], c[3], k[0], k[1],
             k[2], k[3]);
      int ok = check == 1 ? (c[0] < 8 && c[1] > 247 && c[2] < 8 && k[1] > 247)
                          : (c[0] < 8 && c[1] < 8 && c[2] > 247 && k[2] > 247);
      if (check == 1) gs_result = ok; else ts_result = ok;
      D3D12_RANGE none = {0, 0};
      ID3D12Resource_Unmap(rb, 0, &none);
    }
    hr = IDXGISwapChain3_Present(sc, 1, 0);
    if (SUCCEEDED(hr)) presents_ok++;
    else if (presents_ok == frame) printf("  Present(frame %d) = 0x%08lx\n", frame, hr);
  }
  DWORD ms = GetTickCount() - t0;
  printf("presents ok %d/120 in %lu ms\n", presents_ok, ms);
  printf("GS test: %s\n", gs_result < 0 ? "SKIP" : gs_result ? "PASS" : "FAIL");
  printf("tessellation test: %s\n", ts_result < 0 ? "SKIP" : ts_result ? "PASS" : "FAIL");
  if (presents_ok != 120 || gs_result == 0 || ts_result == 0) fail = 1;
  HRESULT removed = ID3D12Device_GetDeviceRemovedReason(dev);
  printf("device removed reason 0x%08lx\n", removed);
  if (removed) fail = 1;
  printf("RESULT %s\n", fail ? "FAIL" : "PASS");
  DestroyWindow(hwnd);
  return fail;
}
