#!/bin/bash
# suite.sh <out dir> [test...]: runs the regression suite under the test runner copy in $HB (default
# $MSFS_SRC/hb: CrossOver/ and pfx/) with whatever FEX DLL swap.sh put there. Raw output goes to <out dir>/<test>/.
# Our programs come from tools/*/build, Highball's x64-fidelity programs from $FID (built out of tree from
# highball-engine tests/x64-fidelity with its build.sh; not copied into this repo). Environment passes through.
set -u
MSFS_SRC=${MSFS_SRC:-$HOME/GitHub/msfs-src}
HB=${HB:-$MSFS_SRC/hb}; FID=${FID:-$HB/fid}
OUT=$1; shift; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
T=$(cd "$(dirname "$0")/.." && pwd)
RUN="$T/hb-fex/run-np.sh $HB/CrossOver $HB/pfx"
ALL="smctest drtest eh eh-dr eh-skip tsobench smcmsfs smcdata smcretire ordertest memtest cowtest regiontest imagequery ctxtest fibertest hb-drtest x87test cpuidtest kattest tsctest syscalltest"
TESTS=${*:-$ALL}
for t in $TESTS; do
  o=$OUT/$t
  case $t in
    smctest)   $RUN $o $T/smc-test/build/smctest.exe ;;
    drtest)    $RUN $o $T/dr-test/build/drtest.exe ;;
    eh)        $RUN $o $T/eh-test/build/ehtest.exe ;;
    eh-dr)     $RUN $o $T/eh-test/build/ehtest.exe dr ;;
    eh-skip)   $RUN $o $T/eh-test/build/ehtest.exe x dr onlyskip ;;
    tsobench)  $RUN $o $T/tso-bench/build/tsobench.exe ;;
    smcmsfs)   $RUN $o $T/smc-msfs/build/smcmsfs.exe ${SMCMSFS_ARGS:-} ;;
    hb-drtest) $RUN $o $FID/drtest.exe ;;
    *)         $RUN $o $FID/$t.exe ;;
  esac
  res=$( (cat $o/*-out.txt $o/stdout.txt 2>/dev/null) | grep -E '^(SUMMARY|RESULT|PHASE|T[0-9]+|MP |FINAL|PASS |FAIL )' | grep -vE '^PASS ' | awk '!s[$0]++' | tr '\n' ';' | cut -c1-400)
  printf '%-12s %s %s\n' "$t" "$(grep -o 'exit=[0-9]* wall=[0-9.]*' $o/run.txt)" "$res"
done | tee -a $OUT/summary.txt
