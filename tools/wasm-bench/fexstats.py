#!/usr/bin/env python3
# Reads FEX's live per-thread stats of a running Wine process (FEX_PROFILESTATS=1): the file
# $TMPDIR/fex-<unix pid>-stats that Highball's fexunixlib_darwin.cpp maps, laid out as FEXCore's
# include/FEXCore/Utils/SHMStats.h (ThreadStatsHeader, then a linked list of ThreadStats).
# The file goes away when the process exits, so read it while it runs.
#
#   fexstats.py <stats file> [--threads]     totals, or one line per thread
#
# Times are CNTVCT ticks (24 MHz on Apple Silicon), printed in ms. "clears" is Highball's count of
# ClearCodeCache (kept in AccumulatedSMCNativeUndec, which ARM64EC's native-store write faults bump too).
import struct, sys

FIELDS = """JITTime SignalTime SIGBUSCount SMCCount FloatFallbackCount CacheMissCount CacheReadLockTime
CacheWriteLockTime JITCount DiskCacheHitCount DiskCacheMissCount DiskCacheLookupTime Padding SMCStoresEmulated
SMCStoresUnchanged SMCStoresOnCode SMCStoresUndecoded SMCRetiredMisses SMCBlocksReinstated JITInstructions
JITWritableCount JITWritableTime JITCompileTime JITLockWaitTime LinkLockWaitTime SMCLockWaitTime SMCLockHoldTime
JITWritableInstructions JITProtectTime TCMWaitTime AVTCMWaitTime VMNotifyCount VMHoldTime VMTrackerTime HbVMHoldBegin
SMCInvBuffersTime SMCInvThreadsTime SMCUntrapTime SMCDeferredResetTime SMCJitSame SMCJitDiff SMCNativeSame
SMCNativeDiff SMCNativeUndec SMCLastNativePc JITHotCount JITHotInstructions""".split()
TICK_MS = 1000 / 24e6
SHOW = [("JITCount", "compiles", 1), ("JITInstructions", "insns", 1), ("JITTime", "jit_ms", TICK_MS),
        ("JITCompileTime", "compile_ms", TICK_MS), ("CacheMissCount", "lookup_miss", 1),
        ("SMCNativeUndec", "clears", 1), ("SMCCount", "smc", 1), ("SignalTime", "signal_ms", TICK_MS)]


def read(path):
    data = open(path, "rb").read()
    ver, app, tsize = struct.unpack_from("<BBH", data, 0)
    fexver = data[4:52].split(b"\0")[0].decode()
    head, size = struct.unpack_from("<II", data, 52)
    threads, off, seen = [], head, set()
    while off and off not in seen and off + tsize <= len(data):
        seen.add(off)
        nxt, tid = struct.unpack_from("<II", data, off)
        n = min(len(FIELDS), (tsize - 8) // 8)
        vals = struct.unpack_from("<%dQ" % n, data, off + 8)
        threads.append((tid, dict(zip(FIELDS, vals))))
        off = nxt
    return ver, app, fexver, threads


def fmt(d):
    return " ".join("%s=%.1f" % (lbl, d.get(k, 0) * m) if m != 1 else "%s=%d" % (lbl, d.get(k, 0)) for k, lbl, m in SHOW)


ver, app, fexver, threads = read(sys.argv[1])
print("stats v%d app=%d fex=%s threads=%d" % (ver, app, fexver, len(threads)))
if "--threads" in sys.argv:
    for tid, d in sorted(threads, key=lambda t: -t[1]["JITTime"]):
        if d["JITCount"] or d["SMCNativeUndec"]:
            print("tid=%-6d %s" % (tid, fmt(d)))
tot = {k: sum(d[k] for _, d in threads) for k in FIELDS}
print("TOTAL " + fmt(tot))
