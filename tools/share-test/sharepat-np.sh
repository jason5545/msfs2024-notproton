#!/bin/bash
# sharepat-np.sh <d3d-dir> <outdir>: sharepat.exe pattern matrix (4 modes x 5 sizes/formats) + negative cases
# with one DLL set through run-np.sh. Prints one line per test.
R=$HOME/GitHub/msfs2024-steamac
D3D=$1; O=$2
EXE=$R/tools/share-test/build/sharepat.exe
for sz in "600 600 87" "1920 1080 10" "3840 2160 87" "333 77 61" "1280 720 24"; do
  set -- $sz
  for m in 0 1 2 3; do
    n=pat-m$m-$1x$2-f$3
    D3D=$D3D TMO=60 nice -n 10 "$R/tools/share-test/run-np.sh" "$EXE" "$O/$n" $m $1 $2 $3
    printf '%-26s %s | %s\n' $n "$(grep exit= $O/$n/run.txt)" "$(grep -E 'matching|RESULT|FAIL|Sync|-> 0x8' $O/$n/stdout.txt | tr '\n' ' ')"
  done
done
D3D=$D3D TMO=60 nice -n 10 "$R/tools/share-test/run-np.sh" "$EXE" "$O/neg" neg
printf '%-26s %s\n' neg "$(grep exit= $O/neg/run.txt)"; cat "$O/neg/stdout.txt"
