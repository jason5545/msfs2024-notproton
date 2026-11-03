#!/bin/bash
# Regression suite on a Hadron runtime, each test in the same throwaway prefix, raw output in <out>/<test>/.
#
#   tools/hadron/suite.sh <runtime dir> <prefix dir> <out dir> [test...]
#
# Ours come from tools/*/build; Highball's x64-fidelity programs from $FID (default $MSFS_SRC/hb/fid, built out
# of tree from highball-engine tests/x64-fidelity, not copied into this repo). The environment passes through
# run.sh. One summary line per test goes to <out>/summary.txt.
set -u
MSFS_SRC=${MSFS_SRC:-$HOME/GitHub/msfs-src}
FID=${FID:-$MSFS_SRC/hb/fid}
RT=$1; PFX=$2; OUT=$3; shift 3
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
T=$(cd "$(dirname "$0")/.." && pwd)
RUN="$T/hadron/run.sh $RT $PFX"
OURS="smctest drtest eh eh-dr eh-skip tsobench smcmsfs credtest vktest"
HB="ordertest smcdata smcretire memtest cowtest regiontest imagequery imagemap ctxtest fibertest hb-drtest drdbg x87test cpuidtest kattest tsctest syscalltest globalroot topdown"
TESTS=${*:-$OURS $HB}
for t in $TESTS; do
  o=$OUT/$t
  case $t in
    smctest)   $RUN "$o" "$T/smc-test/build/smctest.exe" ;;
    drtest)    $RUN "$o" "$T/dr-test/build/drtest.exe" ;;
    eh)        $RUN "$o" "$T/eh-test/build/ehtest.exe" ;;
    eh-dr)     $RUN "$o" "$T/eh-test/build/ehtest.exe" dr ;;
    eh-skip)   $RUN "$o" "$T/eh-test/build/ehtest.exe" x dr onlyskip ;;
    tsobench)  $RUN "$o" "$T/tso-bench/build/tsobench.exe" ;;
    smcmsfs)   $RUN "$o" "$T/smc-msfs/build/smcmsfs.exe" ${SMCMSFS_ARGS:-} ;;
    credtest)  $RUN "$o" "$T/cred-test/build/credtest.exe" ;;
    vktest)    $RUN "$o" "$T/np-gfx/build/vktest.exe" ;;
    d3d12test) $RUN "$o" "$T/np-gfx/build/d3d12test.exe" ;;
    share-m0|share-m1|share-m2) $RUN "$o" "$T/share-test/build/sharetest.exe" "${t#share-m}" ;;
    hb-drtest) $RUN "$o" "$FID/drtest.exe" ;;
    *)         $RUN "$o" "$FID/$t.exe" ;;
  esac
  res=$( (cat "$o"/*-out.txt "$o/stdout.txt" 2>/dev/null) | grep -E '^(SUMMARY|RESULT|PHASE|T[0-9]+|MP |FINAL|FAIL |PASS$|PASS:|SHARED|total)' | awk '!s[$0]++' | tr '\n' ';' | cut -c1-400)
  printf '%-12s %s %s\n' "$t" "$(grep -o 'exit=[0-9]* wall=[0-9.]*' "$o/run.txt")" "$res"
done | tee -a "$OUT/summary.txt"
