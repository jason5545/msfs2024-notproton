# lldb script: dump every block of the perf map to <out>/code.bin with an index (host start, size, offset).
import lldb, os
def dump(debugger, command, result, internal_dict):
    pmap, out = command.split()
    proc = debugger.GetSelectedTarget().GetProcess()
    idx = open(out + '/code.idx', 'w')
    binf = open(out + '/code.bin', 'wb')
    off = 0; n = 0; bad = 0
    for line in open(pmap, errors='replace'):
        p = line.split(' ', 2)
        if len(p) < 3: continue
        h, sz = int(p[0], 16), int(p[1], 16)
        err = lldb.SBError()
        data = proc.ReadMemory(h, sz, err)
        if not err.Success():
            bad += 1; continue
        binf.write(data)
        idx.write('%x %x %x %s\n' % (h, sz, off, p[2].strip()))
        off += sz; n += 1
    idx.close(); binf.close()
    print('dumped %d blocks, %d bytes, %d failed' % (n, off, bad))
def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f lldb_dump.dump jitdump')
