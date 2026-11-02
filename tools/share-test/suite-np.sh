#!/bin/bash
# suite-np.sh <d3d-dir> <outdir> [VAR=value...]: sharetest 0/1/2 + d3d12test with one DLL set through run-np.sh
# (e.g. STEAMAC_NO_HOST_SHARING=1 to turn the host-memory path off). Prints one line per test.
R=$HOME/GitHub/msfs2024-steamac
D3D=$1; O=$2; shift 2
for m in 0 1 2; do env "$@" D3D=$D3D TMO=${TMO:-60} nice -n 10 "$R/tools/share-test/run-np.sh" "$R/tools/share-test/build/sharetest.exe" "$O/share-m$m" $m; done
env "$@" D3D=$D3D TMO=90 nice -n 10 "$R/tools/share-test/run-np.sh" "$R/tools/np-gfx/build/d3d12test.exe" "$O/d3d12"
for d in share-m0 share-m1 share-m2 d3d12; do printf '%-9s %s | %s\n' $d "$(grep exit= $O/$d/run.txt)" "$(grep -E 'RESULT|SHARED|FAIL' $O/$d/stdout.txt | tr '\n' ' ')"; done
