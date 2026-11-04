import lldb, collections
def x9(debugger, command, result, internal_dict):
    addr, n = command.split(); addr = int(addr, 16); n = int(n)
    target = debugger.GetSelectedTarget(); proc = target.GetProcess()
    debugger.SetAsync(False)
    bp = target.BreakpointCreateByAddress(addr)
    seen = collections.Counter(); lr = collections.Counter()
    for i in range(n):
        proc.Continue()
        th = proc.GetSelectedThread()
        if th.GetStopReason() != lldb.eStopReasonBreakpoint:
            for t in proc:
                if t.GetStopReason() == lldb.eStopReasonBreakpoint: th = t; break
        fr = th.GetFrameAtIndex(0)
        seen[fr.FindRegister('x9').GetValueAsUnsigned()] += 1
    target.BreakpointDelete(bp.GetID())
    for a, c in seen.most_common(): print('x9=%x %d' % (a, c))
def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('command script add -f lldb_x9.x9 x9hits')
