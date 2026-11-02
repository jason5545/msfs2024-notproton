# MSFS 2024 on Apple Silicon Macs: the NotProton route

[正體中文](README.md) | **English**

Fixes and tools for running Microsoft Flight Simulator 2024 (Steam version) on Apple Silicon Macs. It runs through [NotProton](https://github.com/NotProtonNot/NotProton), which adds Steam Play to the native macOS Steam client on top of CrossOver 27 Preview's arm64 Wine and FEX ARM64EC.

The approach: build a few open-source components from source and layer them onto a copy of the runner through NotProton's runner variants. CrossOver's own files are left as they are, and the game files are never touched.

## Status (2026-11-03)

- The game runs from start to finish: intro videos, loading, Xbox sign-in that happens automatically after pressing Start (no more signing in on every launch), the world map (free flight menu), and a clean exit.
- No actual flight yet.

## Environment

| Item | Version |
|---|---|
| Mac | M5 Max (40-core GPU), 128 GB, macOS 27.2 |
| NotProton | NotProton.app 1.2.1, runner `crossover-27.0.0.41073-fex` (CrossOver Preview 20261015, Wine `wine-11.17-8910-g4683e69e153`, FEX `FEX-2604-1037-gf0b5147`) |
| MSFS 2024 | Steam AppID 2537590, 1.9.21.0 |

## Components layered onto the runner

Configured in [`config/notproton-variants-steamac.conf`](config/notproton-variants-steamac.conf). Steam launch options:

```
NOTPROTON_STEAMAC=1 NOTPROTON_RUNNER_VARIANT=steamacfex+steamackk+steamacadv+steamacvproc NOTPROTON_ENV_JSC_useJIT=false %command%
```

| Variant | Replaces | Source |
|---|---|---|
| `steamacfex` | `xtajit64.dll` (FEX ARM64EC) | Upstream FEX `02e24ca5c` plus the six patches in `patches/fex/` (x86 execute breakpoints, exception logging, RF fix, SMC hot pages) |
| `steamackk` | Vulkan loader 1.4.363, wired to KosmicKrisp | Mesa `ce576c29` plus the fxgl/steamac v1.9.7 KosmicKrisp patches, `tools/kk-build/` |
| `steamacadv` | `advapi32.dll` | Upstream Wine 11.17 plus `patches/wine/advapi32-steamac.patch`, `tools/wine-advapi32/` |
| `steamacvproc` | `msvproc.dll` | Upstream Wine 11.17 plus `patches/wine/msvproc-arm64ec-yuv2rgb.patch`, `tools/wine-msvproc/` |

The D3D layer comes from a NotProton plugin (`NOTPROTON_STEAMAC=1`, configured in [`config/notproton-plugin-steamac.conf`](config/notproton-plugin-steamac.conf)). On every launch it copies the DXVK and vkd3d-proton DLLs into the prefix's `system32`, points Vulkan at KosmicKrisp, and sets `VKD3D_FEATURE_LEVEL=12_1` and `VKD3D_SHADER_MODEL=6_7`. The current builds are DXVK `3cb664e1` (v2.6.2-23) and vkd3d-proton `21a49c97` (v3.0b), each with one patch that lets D3D11 and D3D12 share textures through host memory (`patches/dxvk/`, `patches/vkd3d-proton/`). `tools/d3d-build/` builds and swaps them.

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
| `tools/wine-msvproc/` | Builds ARM64X `msvproc.dll`; `variant.sh` checks the sha256 and installs it into the NotProton variant |
| `tools/kk-build/` | Builds KosmicKrisp for NotProton the same way fxgl/steamac v1.9.7 does |
| `tools/d3d-build/` | Builds DXVK and vkd3d-proton; `swap-np-d3d.sh` backs up, swaps, and restores the DLLs |
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
