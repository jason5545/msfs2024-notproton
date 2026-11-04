# Squark: MSFS 2024 on Apple Silicon Macs (the Hadron route)

[正體中文](README.md) | **English**

Fixes and tools for running Microsoft Flight Simulator 2024 (Steam version) on Apple Silicon Macs. The base is our fork of [Hadron](https://github.com/nheisterkamp/Hadron), a fork of [NotProton](https://github.com/NotProtonNot/NotProton) that adds Steam Play to the native macOS Steam client and builds its own arm64 Wine runtime from source. Its arm64 engine is replaced with the one [Highball](https://github.com/gauthierpiarrette/highball-engine) tuned for MSFS, and our own D3D12 graphics layer is added on top (vkd3d-proton and DXVK built for ARM64EC, running on KosmicKrisp).

Everything is built from open source, and the game files are never touched.

The name is squawk (a transponder code) plus quark (what hadrons are made of); a squark is also the quark's partner particle in supersymmetry. This repo was renamed from `msfs2024-notproton` on Nov 3. Until midday Nov 3 the work went through NotProton (CrossOver 27 Preview's Wine); the patches and tools from that period stay here as a record.

## Status (2026-11-04)

- NotProton route (until midday Nov 3): the game ran from start through automatic Xbox sign-in and the world map; after pressing "Fly" it reached the airport with a correctly rendered cockpit, sitting on the runway at about 16 FPS. No actual flight yet.
- After the switch to Hadron: the runtime is built and passes the full regression set (Hadron's own `check-runtime.sh`, 8 checks; Highball's x64-fidelity tests with the numbers its README lists; our debug-register, exception, SMC, TSO, credential and DPAPI tests; D3D12 and D3D11↔D3D12 shared textures on KosmicKrisp, pixel-identical), and Steam is hooked up.
- First launch on Hadron, the afternoon of Nov 3: automatic Xbox sign-in, no stripes on the loading screen, about 9 minutes from launch to the title screen. Free Flight with the A330-200 at Taoyuan RCTP 05R: "Start flight" reached the cockpit in under two minutes, rendered correctly with the instruments running, at about 30 FPS on the runway (about 60 FPS in the main menu). Quit to Desktop from the main menu ended it cleanly. The whole run was in background mode, see "Background mode" below.
- Nov 4, the first add-on bought from a developer's own store: the TFDi MD-11 (its Install Manager is a WPF program). In Wine it froze as soon as a simulator was ticked: winemac's main thread queued an OpenGL update and then waited for itself. Upstream Wine still has this, so we fixed it with Wine patch 0060. After that the installer ran to the end on Wine's own Wine Mono, no .NET Framework needed. The MD-11F loads into the cockpit at Taipei Songshan RCSS with the PFD, ND, EICAS and MCDU all working, at about 15 FPS on the runway (the A330 does about 30 FPS on the same Mac).

![TFDi MD-11F cockpit on the runway at RCSS, with the PFD, ND, EICAS and MCDU rendering](docs/img/md11-rcss-cockpit.jpg)

## Environment

| Item | Version |
|---|---|
| Mac | M5 Max (40-core GPU), 128 GB, macOS 27.2 |
| Hadron | fork [`jason5545/Hadron`](https://github.com/jason5545/Hadron), branch `steamac` (upstream `87c4265` plus our changes), Hadron 1.0.3, Sparkle auto-update off |
| Wine runtime | `wine-11.18-fex`: WineHQ 11.18 with Hadron's patches 0001–0003 and 0005, Highball's `patches-arm64` 0018–0052 (highball-engine `2eedc50`), and our 0004 and 0053–0056 |
| FEX | FEX-2610 with Highball's `fex/patches` 0001–0033; the unix side is Highball's `fexunixlib_darwin.cpp` (hardware TSO on every thread) |
| Graphics | KosmicKrisp (Mesa `ce576c29ed` plus fxgl/steamac v1.9.7's 0001–0077, plus [`patches/kk/`](patches/kk/)); vkd3d-proton `21a49c97` (v3.0b) and DXVK `3cb664e1` (v2.6.2-23) plus [`patches/vkd3d-proton/`](patches/vkd3d-proton/) and [`patches/dxvk/`](patches/dxvk/), built for ARM64EC |
| Other | GStreamer 1.28.7 (the official framework, trimmed with Highball's scripts; without it the start screen stays black) |
| MSFS 2024 | Steam AppID 2537590, 1.9.21.0 |

The Wine runtime patches, the build and Hadron's own changes live in the fork (`runtime/patches/wine-11.18-fex/`, `runtime/fex/highball/`, `runtime/gstreamer/`, `runtime/build-wine.sh`, `dylib/feats/compat_run.sh`). Highball's files are copied over unchanged; fixes go into new numbered patches. In this repo, `tools/hadron/build-runtime.sh` calls the fork's `build-wine.sh` and brings KosmicKrisp and the ARM64EC vkd3d-proton and DXVK into the runtime.

Wine's loader is the runtime's `Wine.app`. It needs the Cross-architecture Compatibility Framework entitlement to run x86 code on arm64: create an App ID with that capability on the Apple Developer site, and `xcodebuild -allowProvisioningUpdates` then generates the provisioning profile (Xcode does not register an App ID for this capability by itself).

## Our Wine patches (the fork's `runtime/patches/wine-11.18-fex/`)

| patch | What it does |
|---|---|
| 0004 | Link options of the aarch64 loader (PAGEZERO, image base, x86_64 layout) and finding `ntdll.so` from `Wine.app`. Highball covers the rest of Hadron's original 0004, more completely |
| 0053 | A cast in Highball 0030's `dlopen_host_lib`, so `wine/unixlib.h` also compiles as C++ (lsteamclient's unix side) |
| 0054 | `HADRON_WINE_USER` names the Windows user. Wine's DPAPI mixes the user name into its keys, so data protected under CrossOver as `crossover` (the Xbox sign-in) only opens under the same name; `compat_run.sh` sets it for a prefix NotProton left behind |
| 0055 | `HADRON_VK_HOST_SHARING=1` hides `VK_KHR_external_memory_win32`. Highball 0036 provides it through Metal objects, and with it our DXVK and vkd3d-proton skip the host-memory sharing they were validated with, so D3D11↔D3D12 shared textures fail; set automatically for our graphics layer |
| 0056 | `HADRON_MAC_BACKGROUND=1`, a background mode for automated tests: windows sit under the desktop picture (invisible, yet they keep drawing and can be captured), the program never takes the foreground, leaves the host cursor and display modes alone and skips the macOS input method; input comes only from inside Wine |

## Graphics and launch options

In the game's Hadron panel, "Graphics" set to "vkd3d-proton + KosmicKrisp" (`HADRON_GRAPHICS=vkd3d`) puts the overlays `lib/graphics/vkd3d` and `lib/graphics/dxvk` ahead of Wine's own DLLs, points Vulkan at KosmicKrisp, and sets `VKD3D_FEATURE_LEVEL=12_1`, `VKD3D_SHADER_MODEL=6_7` and `DXVK_CONFIG`. Only the arm64 (FEX) runtime offers it. MSFS's Steam launch options are still written the NotProton way, and `compat_run.sh` maps them to Hadron's names:

```
CX_GRAPHICS_BACKEND=vkd3d NOTPROTON_ENV_JSC_useJIT=false %command%
```

`CX_GRAPHICS_BACKEND` maps to `HADRON_GRAPHICS` and `NOTPROTON_ENV_<name>` to `HADRON_ENV_<name>` (a Hadron name already set wins). On the arm64 runtime MSFS (2537590) automatically gets Highball's MSFS SMC settings (`HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 HB_FEX_VM_FASTPATH=1 HB_FEX_SMC_DEFER_CALLRET=1`); launch options win.

DXVK and vkd3d-proton run in the game's process. Built for ARM64EC, the game still loads them as x64 DLLs, but their code runs as native ARM64, so translating D3D to Vulkan no longer has to be emulated in FEX along with the game. Offline tests (`tools/d3d-bench/`): recording a D3D12 draw takes about 7–16% less time, and first-time PSO creation is much faster (96 PSOs from 8 threads: 0.6–1 s for x64, 0.03 s for ARM64EC). The cost is one x64/ARM64EC boundary crossing per call, so small D3D11 calls get slower (about 50 → 190 ns). On Nov 3 the game ran on Hadron with this ARM64EC build, and the main menu and cockpit render correctly; in-game FPS has not been compared with the x64 build yet.

## Background mode (for automated tests)

While the game is being tested, it must not show up on the user's screen or take the keyboard focus, because the user is working on the same Mac at the time. Three layers:

- **Wine (0056)**: with `HADRON_MAC_BACKGROUND=1`, every window is put on the lowest window level (under the desktop picture) as soon as it is created. It is still on screen, so it keeps drawing and `screencapture -l` can capture it. The program never activates or unhides itself, never moves or confines the host cursor, never changes display modes, and key presses skip the macOS input method.
- **Hadron's `compat_run.sh`**: the launch option `HADRON_MAC_BACKGROUND=1`, or a one-shot request file `~/Library/Application Support/hadron/background/<AppID>` (valid for 10 minutes). It is set before any Wine process runs (so a prefix update's wineboot stays hidden too); the game is started with `open -g` and Hadron's launch window stays shut. Before the game, Steam runs an install script (`iscriptevaluator.exe`, verb `run`); that run goes in the background as well, but leaves the request for the game's own run (`waitforexitandrun`). On the first try on Nov 3 the install script used up the request and the game came up in the normal mode, on screen (fixed in fork `56dcd8b`).
- **Input**: hardware input sent from inside Wine (a small Windows program joins the game's wineserver and sends it through win32u's `SendHardwareInput`, the path winemac uses for the Mac's own input), without touching the Mac's cursor or foreground. MSFS buttons need a hover first: two moves and then a click work, and typing into search fields works. A watcher checks the game's and Steam's windows every 30 ms, hides any that gets above the desktop and gives a stolen foreground back. These tools are kept locally, not in the repo.

Over a full run on Nov 3 (launch, sign-in, main menu, Free Flight, cockpit, quit) the game window appeared on screen 0 times and took the foreground 0 times.

## Problems fixed

Items 1–5 were found and fixed on the NotProton route. How each is handled on Hadron is noted at the end of each item. On the Nov 3 launch on Hadron, 1, 2, 3 and 5 did not come back (automatic Xbox sign-in, no stripes, past 8/22, "Start flight" reaches the cockpit).

### 1. Xbox sign-in required on every launch

- XAL reads its stored credentials, including the DeviceKey, only through `CredEnumerateW(filter*)`.
- Wine's `CredEnumerateW` merges the credentials in the registry with those mountmgr finds in the host keychain. When nothing matches, the macOS `search_credentials` returns `STATUS_NOT_FOUND`, which advapi32 sees as `ERROR_NOT_FOUND` (1168); advapi32 only treats `ERROR_NOT_SUPPORTED` as "the host has none", so the whole enumeration fails. Linux returns `NOT_SUPPORTED` and is fine.
- So XAL thought there was no device key every time, generated a new one, and the sign-in had to start over.
- Fix: treat `ERROR_NOT_FOUND` as "the host has no credentials" too (`patches/wine/advapi32-steamac.patch`).
- Verified (NotProton): the DeviceKey hash stays the same across launches, tokens refresh silently, and the sign-in screen no longer appears. `tools/cred-test/` tests CredWrite/Read/Enumerate, the no-match case, and BCrypt ECDSA.
- **On Hadron**: Highball's Wine 0034 is the same fix. Wine's DPAPI also mixes the user name into its keys: `crossover` under CrossOver, `steamuser` on Hadron. Changing the name leaves the old sign-in data unreadable (tested with `tools/cred-test/dpapitest.c`), hence 0054, which keeps `crossover` for a prefix from NotProton.

### 2. Vertical stripes and a yellow-blue cast on the loading screen background

- Wine's `msvproc.dll` bundles FFmpeg, whose ARM64EC `yuv2rgb_neon.S` has a register clash in NV12→BGRA conversion: in every 16 pixels, the red of the second 8 overwrites the blue of the first 8.
- CrossOver 27's original DLL and one built from unmodified upstream Wine 11.17 produce identical output, so the bug is in upstream source.
- After the fix, offline comparison shows the blue error dropping from 67.75 to 0.58; in the game the stripes are gone, and the stripe strength over the same patch of sky drops from 37.48 to 0.27. `mfgrab` in `tools/share-test/` reproduces it offline.
- **On Hadron**: Highball's Wine 0037 fixes the same file and the same bug.

### 3. Stuck at "Authentication in progress (8/22)" during sign-in

- The game sets execute breakpoints with x86 hardware debug registers every frame, which FEX ARM64EC did not support.
- With that added, another problem showed up: after the exception handler (VEH) changed RIP, FEX did not clear the RF flag, so the next hit on the same breakpoints was swallowed and an access violation followed.
- The fixes are 0001 (execute breakpoints) and 0005 (RF) in `patches/fex/`. `tools/dr-test/` and `tools/eh-test/` are the matching tests.
- **On Hadron**: Highball's FEX 0017 and Wine 0029. They follow real Windows: the context a handler gets has no RF, so a handler that continues without changing it hits the same breakpoint again until it disables it (Highball checked this on Windows Server). Our `dr-test` and `eh-test` expect Wine-on-Linux behavior, so their breakpoint parts time out on this FEX; the expectations are not updated yet.

### 4. The "four-slot breakpoint" stretch of loading is slow

- During loading the game hits four breakpoints about 700 times in a row, and every round involves self-modifying code (SMC). ARM64EC FEX takes a page fault for every SMC, so each round took about 0.8 s and the whole stretch about 9 minutes.
- After porting the "hot pages" approach from the Linux FEX (a page that faults too often switches to per-instruction checks), the 698 rounds finished in 85.8 s (0006 in `patches/fex/`). `tools/smc-test/` measures the cost of a single SMC.
- On NotProton the same stretch also lacked hardware TSO (0007 in `patches/fex/`); `tools/tso-bench/` shows the difference.
- **On Hadron**: Highball's FEX has its own SMC handling (0020, 0023 and others), and MSFS automatically gets its MSFS settings: 2–5 times faster than with everything off, all correct (`tools/smc-msfs/`). Hardware TSO is on for every thread. When data and code share a page that keeps being written, it is still slower than our old hot pages (about 20,000 ns per round against 17 ns). Highball's `HB_FEX_SMC_INLINE=1` is far faster but hangs the cross-thread step-by-step code-change test, so it stays off.

### 5. Freeze after pressing "Fly"

- MSFS uses D3D12 ROVs (rasterizer ordered views). The game requires Feature Level 12_1, so `VKD3D_FEATURE_LEVEL=12_1` makes vkd3d-proton report support and emit fragment shader interlock SPIR-V anyway. KosmicKrisp did not recognize `begin/end_invocation_interlock`, printed it as `Unknown intrinsic`, and Metal failed to compile the shader; `vkEndCommandBuffer` returned `VK_ERROR_INVALID_SHADER_NV`, vkd3d-proton submitted anyway, and `vkQueueSubmit2` crashed. The main menu already has 270 such shaders; they are only drawn after pressing Fly. Upstream Mesa's KosmicKrisp does not handle this either.
- Metal's equivalent is raster order groups, but testing (M5 Max, N overlapping fragments each doing read, add one, write back) showed that ordering is only guaranteed for accesses derived from parameters carrying the `raster_order_group` attribute; texture handles that KosmicKrisp casts from plain addresses do not count.
- Fix (0001 in `patches/kk/`): fragment shaders that use interlock get extra attributed parameters, all writable texture and device memory accesses derive their addresses from them, and `VK_EXT_fragment_shader_interlock` is advertised. 0002 makes a pipeline that fails to compile drop only that draw instead of failing the whole command buffer, 0003 saves the MSL that Metal rejects, 0004 removes the `PrimitiveShadingRateKHR` output that KosmicKrisp has no equivalent for (12 more vertex shaders on the main menu), and 0005 turns debug output off by default.
- Verification: in a native Vulkan test with N=1024, every pixel is exactly 1024 with interlock, while the control without it loses counts. In the game, `Unknown intrinsic` went from 270 to 0, and pressing Fly reaches the airport with a correctly rendered cockpit.
- **On Hadron**: the same KosmicKrisp build goes into the runtime (`lib/libvulkan_kosmickrisp.dylib`).

## The NotProton route (until midday Nov 3, retired)

These were used only on the NotProton route and stay as a record:

- [`patches/notproton/`](patches/notproton/), `tools/np-build/`: built the components into our own NotProton (fork `jason5545/NotProton-steamac`, branch `steamac`), which wrote them into CrossOver's FEX runner.
- [`patches/fex/`](patches/fex/): seven FEX patches (execute breakpoints, exception logging, RF, SMC hot pages, hardware TSO), applied in order to upstream main `02e24ca5c`, equal to the fork branch `steamac-darwin`. 0001 detours the ARM64EC `NtSetContextThread`/`NtGetContextThread` of CrossOver's closed-source ntdll at run time; it predates the "don't touch closed-source binaries" rule, and that technique is not used again. The patches were written with AI help, and FEX's `CONTRIBUTING.md` states that AI-generated code is not accepted, so they stay here and are not sent upstream.
- [`patches/wine/`](patches/wine/), `tools/wine-advapi32/`, `tools/wine-msvproc/`: built advapi32 and msvproc from upstream Wine 11.17 to replace CrossOver's.
- `tools/fex-unixlib/`: the macOS unixlib (hardware TSO) for NotProton's FEX.

## Tools

| Path | Purpose |
|---|---|
| `tools/hadron/` | `build-runtime.sh` builds the runtime with the fork; `run.sh` runs one Windows test program on a runtime in a throwaway prefix; `suite.sh` runs the full regression set (our tests plus Highball's x64-fidelity tests) |
| `tools/kk-build/` | Builds KosmicKrisp the way fxgl/steamac v1.9.7 does |
| `tools/d3d-build/` | Builds DXVK/vkd3d-proton (x64 or ARM64EC) |
| `tools/d3d-bench/` | CPU cost of translating D3D11/D3D12 to Vulkan (x64 vs ARM64EC); `ectest/` tests C++ exceptions and callbacks in ARM64EC DLLs |
| `tools/cred-test/` | Credential Manager, BCrypt and DPAPI (`dpapitest.c`) tests |
| `tools/dr-test/` | `SetThreadContext` debug register tests |
| `tools/eh-test/` | ARM64EC exception handling and breakpoint resume tests |
| `tools/smc-test/`, `tools/smc-msfs/` | Cost and correctness of self-modifying code; `smc-msfs` mimics the patterns MSFS uses |
| `tools/tso-bench/` | Correctness and cost of x86 memory ordering (software vs hardware TSO) |
| `tools/share-test/` | D3D11↔D3D12 shared textures, Media Foundation video decoding and `msvproc` output |
| `tools/np-gfx/` | Basic Vulkan and D3D12 tests |

Wine DLLs and the test programs are built with llvm-mingw (with arm64ec).

## Principles

- Only the environment is fixed. Game files are never modified, patched or disassembled.
- Closed-source parts (D3DMetal, the Metal driver, Rosetta, Steam, the game, and CrossOver Preview from the NotProton period) are only examined through their public interfaces and behavior, never disassembled. To learn how a component works, read the matching open-source code.

## License

The patches under `patches/` keep their upstream licenses: Wine is LGPL 2.1 or later, DXVK is zlib, vkd3d-proton is LGPL 2.1, Mesa (KosmicKrisp) is MIT, FEX is MIT.
