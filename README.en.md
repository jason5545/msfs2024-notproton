# MSFS 2024 on Apple Silicon Macs: the NotProton route

[正體中文](README.md) | **English**

Fixes and tools for running Microsoft Flight Simulator 2024 (Steam version) on Apple Silicon Macs. It runs through [NotProton](https://github.com/NotProtonNot/NotProton), which adds Steam Play to the native macOS Steam client on top of CrossOver 27 Preview's arm64 Wine and FEX ARM64EC.

The approach: build a few open-source components from source and build them into our own NotProton, which writes them into the runner (keeping CrossOver's originals as backups). The game files are never touched.

## Status (2026-11-03)

- The game runs from start through the menus: intro videos, loading, Xbox sign-in that happens automatically after pressing Start (no more signing in on every launch), and the world map (free flight menu).
- After pressing "Fly", loading finishes and the game reaches the airport: the cockpit renders correctly, the aircraft sits on the runway, about 16 FPS (Nov 3).
- No actual flight yet.

## Environment

| Item | Version |
|---|---|
| Mac | M5 Max (40-core GPU), 128 GB, macOS 27.2 |
| NotProton | our own NotProton 1.2.3 build (upstream `4df944f` plus `patches/notproton/`), runner `crossover-27.0.0.41073-fex` (CrossOver Preview 20261015, Wine `wine-11.17-8910-g4683e69e153`, FEX `FEX-2604-1037-gf0b5147`) |
| MSFS 2024 | Steam AppID 2537590, 1.9.21.0 |

## Components built into the runner

These components are built into our own NotProton (fork `jason5545/NotProton-steamac`, branch `steamac`; the changes are in [`patches/notproton/`](patches/notproton/)), with no variants or plugins. `tools/np-build/build-notproton.sh install` collects each component's build output into NotProton's app payload, builds and signs the app, and replaces `/Applications/NotProton.app`. When you press update in NotProton, it writes the components into the FEX runner and verifies them; replaced files are kept as `.notproton-orig`. They apply to CrossOver Preview 20261015 (runner `27.0.0.41073-fex`) only.

| Written into the runner | Replaces | Source |
|---|---|---|
| `lib/wine/aarch64-windows/xtajit64.dll` | FEX ARM64EC (CrossOver's `libarm64ecfex.so` is removed) | upstream FEX `02e24ca5c` plus the six patches in `patches/fex/` (x86 execute breakpoints, exception logging, RF fix, SMC hot pages) |
| `lib/wine/aarch64-windows/advapi32.dll` | Wine's advapi32 | upstream Wine 11.17 plus `patches/wine/advapi32-steamac.patch`, `tools/wine-advapi32/` |
| `lib/wine/aarch64-windows/msvproc.dll` | Wine's msvproc | upstream Wine 11.17 plus `patches/wine/msvproc-arm64ec-yuv2rgb.patch`, `tools/wine-msvproc/` |
| `lib/aarch64/libMoltenVK.dylib` | the Vulkan loader 1.4.363; CrossOver's MoltenVK moves to `lib/aarch64/moltenvk/` as its ICD | Homebrew `vulkan-loader` |
| `lib/steamac/kk/` | KosmicKrisp | Mesa `ce576c29` plus fxgl/steamac v1.9.7's KosmicKrisp patches, plus `patches/kk/` (ROV and more, see problem 5), `tools/kk-build/` |
| `lib/steamac/d3d/` | DXVK, vkd3d-proton (ARM64EC) | DXVK `3cb664e1` (v2.6.2-23) and vkd3d-proton `21a49c97` (v3.0b) plus `patches/dxvk/` and `patches/vkd3d-proton/` (D3D11↔D3D12 texture sharing through host memory), `tools/d3d-build/` (`ARCH=arm64ec`) |

The first four are used by every FEX game; a game that does not pick the D3D layer keeps Vulkan on MoltenVK (the same device features as before). The D3D layer is picked per game: the Compatibility page's Graphics menu has a new "vkd3d-proton + KosmicKrisp" entry. With it, every launch copies the five DLLs into the prefix's `system32`, adds the DLL overrides, points Vulkan at KosmicKrisp, and sets `VKD3D_FEATURE_LEVEL=12_1` and `VKD3D_SHADER_MODEL=6_7`. MSFS's Steam launch options:

```
CX_GRAPHICS_BACKEND=vkd3d NOTPROTON_ENV_JSC_useJIT=false %command%
```

DXVK and vkd3d-proton live in the game's process. Built as ARM64EC, the game still loads them as x64 DLLs, but their code runs as native ARM64, so the D3D-to-Vulkan translation is no longer emulated in FEX along with the game. Offline tests (`tools/d3d-bench/`) pass all correctness checks; recording a D3D12 draw takes about 7–16% less time, and first-time PSO creation is much faster (96 PSOs on 8 threads at once: 0.6–1 s for x64, 0.03 s for ARM64EC). The cost is that every call from the game crosses the x64/ARM64EC boundary, so D3D11 calls that do very little each get slower (about 50 → 190 ns). **The ARM64EC set is not yet tested in the game.**

## Problems fixed

### 1. Xbox sign-in required on every launch

- XAL reads stored credentials, including the DeviceKey, only through `CredEnumerateW(filter*)`.
- Wine's `CredEnumerateW` merges credentials from the registry with those mountmgr finds in the host keychain. On macOS, `search_credentials` returns `STATUS_NOT_FOUND` when nothing matches, which reaches advapi32 as `ERROR_NOT_FOUND` (1168). advapi32 only treats `ERROR_NOT_SUPPORTED` as "no host credentials", so the whole enumeration failed. Linux returns `NOT_SUPPORTED`, so it never hit this.
- As a result XAL thought there was no device key every time, generated a new one, and sign-in had to start over.
- Fix: treat `ERROR_NOT_FOUND` as "no host credentials" too. The patch also reproduces CrossOver's own behaviour of returning `crossover` from `GetUserNameA/W` (CrossOver Hack 12735; set `CX_REPORT_REAL_USERNAME` to get the real name).
- Verified: the DeviceKey hash stays the same across launches, tokens refresh silently, and the sign-in screen no longer appears. `tools/cred-test/` tests CredWrite/Read/Enumerate, the no-match case, and BCrypt ECDSA inside the runner.

### 2. Vertical stripes and a yellow-blue cast on the loading screen background

- Wine's `msvproc.dll` bundles FFmpeg, whose ARM64EC `yuv2rgb_neon.S` has a register conflict in the NV12→BGRA path: in every 16 pixels, the blue of the first 8 is overwritten by the red of the last 8.
- CrossOver 27's own DLL and one built unmodified from upstream Wine 11.17 produce identical output, so the bug is in the upstream source.
- After the fix, the offline blue error drops from 67.75 to 0.58. In the game the stripes are gone; the stripe strength on the same patch of sky drops from 37.48 to 0.27. `mfgrab` in `tools/share-test/` reproduces it offline.

### 3. Stuck at "Authentication in progress (8/22)" during sign-in

- The game sets execute breakpoints with the x86 hardware debug registers every frame. FEX ARM64EC did not support them, so that had to be added.
- With that in place, one more problem remained: after an exception handler (VEH) changed RIP, FEX did not clear the RF marker, so the next hit on the same breakpoint was swallowed, followed by an access violation.
- The fixes are 0001 (execute breakpoints) and 0005 (RF) in `patches/fex/`. `tools/dr-test/` and `tools/eh-test/` are the matching test programs.

### 4. The "four-slot breakpoint" stretch during loading is slow

- During loading, one stretch hits four breakpoints repeatedly for about 700 rounds, and every round involves self-modifying code (SMC). FEX ARM64EC takes a page fault for every SMC write, so each round took about 0.8 s and the whole stretch about 9 minutes.
- After porting the "hot pages" scheme from the Linux FEX work (a page that faults too often switches to per-instruction checks), the 698 rounds finish in 85.8 s (0006 in `patches/fex/`). `tools/smc-test/` measures the cost of a single SMC.

### 5. The game froze after pressing "Fly"

- MSFS uses D3D12 ROVs (rasterizer ordered views). The game requires Feature Level 12_1, so `VKD3D_FEATURE_LEVEL=12_1` makes vkd3d-proton report support and emit fragment shader interlock SPIR-V anyway. KosmicKrisp did not recognise `begin/end_invocation_interlock`, printed `Unknown intrinsic`, and Metal refused the shader; `vkEndCommandBuffer` returned `VK_ERROR_INVALID_SHADER_NV`, vkd3d-proton submitted the command buffer anyway, and `vkQueueSubmit2` crashed. 270 such shaders already failed in the main menu, but they were only drawn after pressing "Fly". Upstream Mesa's KosmicKrisp does not have this yet.
- Metal's counterpart is raster order groups. Measured on an M5 Max (N overlapping fragments each read, add one, and write back), ordering only holds for accesses derived from a parameter that carries the `raster_order_group` attribute; texture handles that KosmicKrisp casts from a plain address do not count.
- Fix (0001 in `patches/kk/`): fragment shaders that use interlock get extra attributed parameters, and every writable texture and device memory access derives its address from them; the driver now advertises `VK_EXT_fragment_shader_interlock`. 0002 drops only the draw of a pipeline that failed to compile instead of failing the whole command buffer, 0003 saves the MSL that Metal refuses, and 0004 removes the `PrimitiveShadingRateKHR` output KosmicKrisp has no equivalent for (12 more vertex shaders in the main menu).
- Verification: a native Vulkan test with N=1024 gives exactly 1024 on every pixel with interlock, while the control without it loses updates. In game, `Unknown intrinsic` went from 270 to 0, and pressing "Fly" now reaches the airport with the cockpit rendering correctly.

## FEX changes

`patches/fex/` holds six FEX patches. Applied in order to upstream main `02e24ca5c`, they produce the same tree as `c33b6a5e9` on the fork branch `steamac-darwin` (checked with `git apply`).

- 0001: x86 execute breakpoints. CrossOver 27 Preview's ntdll is closed source and drops the debug registers when it converts an x64 context to ARM64, so this patch detours ntdll's ARM64EC `NtSetContextThread`/`NtGetContextThread` at run time and keeps DR0–DR7 in FEX's thread state. This was done before the project's "don't touch closed-source binaries" rule was set; it is kept as is and the approach will not be extended.
- 0002–0004: with `STEAMAC_EXCLOG=1`, log the exceptions handed to the game. Debugging only.
- 0005: clear the RF marker after an exception handler changes RIP.
- 0006: SMC hot pages.

These patches were written with AI assistance. FEX's `CONTRIBUTING.md` says it does not accept AI-generated code, so they live here only and are not submitted upstream. Build instructions are in `patches/README.md` (Chinese).

## Tools

| Path | Purpose |
|---|---|
| `tools/wine-advapi32/` | Builds ARM64X `advapi32.dll` from upstream Wine 11.17 source |
| `tools/wine-msvproc/` | Builds ARM64X `msvproc.dll` |
| `tools/kk-build/` | Builds KosmicKrisp for NotProton the same way fxgl/steamac v1.9.7 does |
| `tools/np-build/` | Collects the components into our NotProton, builds, signs and installs it |
| `tools/d3d-build/` | Builds DXVK and vkd3d-proton (x64 or ARM64EC) |
| `tools/d3d-bench/` | CPU cost of the D3D11/D3D12-to-Vulkan translation (x64 vs. ARM64EC builds); `ectest/` checks C++ exceptions and callbacks in an ARM64EC DLL |
| `tools/cred-test/` | Credential Manager and BCrypt tests |
| `tools/dr-test/` | `SetThreadContext` debug register test |
| `tools/eh-test/` | ARM64EC exception handling and breakpoint resume test |
| `tools/smc-test/` | Cost and correctness of self-modifying code (including cross-thread writes and protection changes) |
| `tools/share-test/` | D3D11↔D3D12 shared textures, Media Foundation video decoding, and `msvproc` output |
| `tools/np-gfx/` | Basic Vulkan and D3D12 tests |

The Wine DLLs are built with llvm-mingw (with arm64ec), Wine is configured with `--enable-archs=arm64ec,aarch64`, and bison needs the newer version from Homebrew.

## Principles

- Only the environment is patched. Game files are never modified, patched, or disassembled.
- Closed-source parts (CrossOver Preview's Wine and FEX, D3DMetal, the Metal driver, Steam, the game) are examined only through public interfaces and behaviour, never disassembled. To learn how a component works, read the corresponding open-source code.

## License

The patches under `patches/` follow their upstream licenses: Wine is LGPL 2.1 or later, DXVK is zlib, and vkd3d-proton is LGPL 2.1.
