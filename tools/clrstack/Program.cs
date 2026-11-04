// clrstack <pid> [suspend] — print the managed stack of every thread of a running .NET Framework process (ClrMD).
// For Wine: run with the same runner and prefix as the target. Output: one block per thread.
using System;
using System.Linq;
using Microsoft.Diagnostics.Runtime;

class P {
    static int Main(string[] a) {
        int pid = int.Parse(a[0]);
        bool suspend = a.Length > 1 && a[1] == "suspend";
        using var dt = DataTarget.AttachToProcess(pid, suspend);
        foreach (var ci in dt.ClrVersions) Console.WriteLine($"clr {ci.Version} {ci.Flavor}");
        using var rt = dt.ClrVersions[0].CreateRuntime();
        foreach (var t in rt.Threads) {
            Console.WriteLine($"--- managed {t.ManagedThreadId} os 0x{t.OSThreadId:x} alive={t.IsAlive} gcmode={t.GCMode}");
            int n = 0;
            foreach (var f in t.EnumerateStackTrace()) {
                Console.WriteLine($"  {f.Kind,-8} {f.FrameName} {f.Method?.Signature}");
                if (++n > 60) break;
            }
        }
        return 0;
    }
}
