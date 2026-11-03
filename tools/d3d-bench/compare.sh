#!/bin/bash
# compare.sh <outdir> <rounds> <name=d3d-dir>... [-- d3dbench args]
# Runs d3dbench.exe through tools/share-test/run-np.sh once per DLL set per round, alternating the sets each round
# (so drift and thermal effects hit every set alike), then prints the median of every metric per set.
# Caches are off (VKD3D_SHADER_CACHE_PATH=0, DXVK_STATE_CACHE=0) and the benchmark salts its shaders per run.
#   KK_ICD, TMO pass through to run-np.sh.
set -u
R=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; ROUNDS=$2; shift 2
SETS=(); while [ $# -gt 0 ] && [ "$1" != "--" ]; do SETS+=("$1"); shift; done
[ "${1:-}" = "--" ] && shift
export VKD3D_SHADER_CACHE_PATH=0 DXVK_STATE_CACHE=0 VKD3D_DEBUG=${VKD3D_DEBUG:-err} DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-warn}
mkdir -p "$OUT"
for r in $(seq 1 "$ROUNDS"); do
  n=${#SETS[@]}
  for i in $(seq 0 $((n - 1))); do
    s=${SETS[$(( (i + r) % n ))]}; name=${s%%=*}; dir=${s#*=}
    D3D=$dir TMO=${TMO:-600} nice -n 0 "$R/tools/share-test/run-np.sh" "$R/tools/d3d-bench/build/d3dbench.exe" "$OUT/$name-r$r" "$@"
    echo "round $r $name: $(grep exit= "$OUT/$name-r$r/run.txt") $(grep RESULT "$OUT/$name-r$r/stdout.txt")"
  done
done
python3 - "$OUT" "${SETS[@]}" <<'EOF'
import sys, glob, re, statistics
out, sets = sys.argv[1], [s.split('=')[0] for s in sys.argv[2:]]
data = {}  # (test, metric) -> set -> [values]
checks = {}
for s in sets:
    for f in sorted(glob.glob(f"{out}/{s}-r*/stdout.txt")):
        for line in open(f):
            if not line.startswith("BENCH "): continue
            parts = line.split(); test = parts[1]
            for kv in parts[2:]:
                k, v = kv.split("=", 1)
                if k == "check": checks.setdefault(s, set()).add(f"{test}:{v}"); continue
                if k in ("frames", "draws", "n", "psos", "shaders"): continue
                data.setdefault((test, k), {}).setdefault(s, []).append(float(v))
print(f"{'test':14} {'metric':15} " + " ".join(f"{s:>14}" for s in sets) + ("   ratio " + sets[1] + "/" + sets[0] if len(sets) > 1 else ""))
for (test, k), per in data.items():
    med = [statistics.median(per[s]) if s in per else float('nan') for s in sets]
    ratio = f"   {med[1] / med[0]:.2f}x" if len(sets) > 1 and med[0] else ""
    print(f"{test:14} {k:15} " + " ".join(f"{m:14.3f}" for m in med) + ratio + f"   (n={min(len(per.get(s, [])) for s in sets)})")
for s in sets:
    bad = sorted(c for c in checks.get(s, ()) if not c.endswith(":OK"))
    print(f"checks {s}: {'all OK' if not bad else ' '.join(bad)}")
EOF
