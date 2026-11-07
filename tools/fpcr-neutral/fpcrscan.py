#!/usr/bin/env python3
"""fpcrscan.py: which native functions of our own ARM64EC builds may run with FEX's JIT FPCR.

FEX's ExitFunctionEC (FEX patch 0043) leaves the JIT's FPCR (AH, NEP, FIZ and the trap enables armed from MXCSR) in
place for listed targets instead of writing FPCR on the way out and again on the way back (about 8 ns each on Apple
cores). A function qualifies when nothing it can execute, itself or through direct calls and tail calls inside the
same image, is affected by those bits:

- no floating-point data processing: no instruction whose mnemonic starts with 'f' other than fmov, no scvtf/ucvtf,
  no bf* (AH and NEP change floating-point results and what scalar FP instructions leave in the upper lanes; the trap
  enables can make them trap). Integer SIMD, loads and stores, moves and fmov are not affected by FPCR;
- no FPCR write (msr fpcr); reads are fine (RtlCaptureContext stores it in a CONTEXT);
- no indirect branch or call (blr, br, braa...): their targets are not known here, and they include imports, the
  ARM64EC call checker and Wine's syscall and unix call dispatchers. Exception: Wine's syscall stubs (pattern below),
  allowed with --syscalls because Wine patch 0070 clears the JIT bits before the unix side runs;
- no svc/hvc/udf, no brk other than #0x1 (__builtin_trap) and #0xf003 (__fastfail), which end the process.

Only for images built from source here (Wine, vkd3d-proton): this reads their code, never anything closed.

usage: fpcrscan.py <dll> [--syscalls] [--exports | --rvas rva,...] [--why] [--json out]
"""
import json, re, struct, subprocess, sys, os

OBJDUMP = os.environ.get('LLVM_OBJDUMP', os.path.expanduser(
    '~/GitHub/squark-app/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal/bin/llvm-objdump'))


class PE:
    def __init__(self, path):
        self.path = path
        d = self.d = open(path, 'rb').read()
        o = struct.unpack_from('<I', d, 0x3c)[0]
        opt = o + 24
        self.base = struct.unpack_from('<Q', d, opt + 24)[0]
        nsec = struct.unpack_from('<H', d, o + 6)[0]
        soh = struct.unpack_from('<H', d, o + 20)[0]
        self.secs = []
        for i in range(nsec):
            name, vs, va, rs, ra = struct.unpack_from('<8sIIII', d, opt + soh + i * 40)
            ch = struct.unpack_from('<I', d, opt + soh + i * 40 + 36)[0]
            self.secs.append((name.rstrip(b'\0').decode(), va, vs, ra, rs, ch))
        self.dirs = [struct.unpack_from('<II', d, opt + 112 + 8 * i) for i in range(16)]

    def off(self, rva):
        for n, va, vs, ra, rs, ch in self.secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + ra
        return None

    def u32(self, rva):
        return struct.unpack_from('<I', self.d, self.off(rva))[0]

    def text(self):
        return [(va, vs) for n, va, vs, ra, rs, ch in self.secs if ch & 0x20000000]  # IMAGE_SCN_MEM_EXECUTE

    def functions(self):
        """(begin, end) RVAs from .pdata (ARM64 RUNTIME_FUNCTION)."""
        rva, size = self.dirs[3]
        out = []
        for i in range(size // 8):
            begin, unwind = struct.unpack_from('<II', self.d, self.off(rva) + 8 * i)
            if unwind & 3:
                length = ((unwind >> 2) & 0x7ff) * 4
            else:
                length = (self.u32(unwind) & 0x3ffff) * 4
            out.append((begin, begin + length))
        return sorted(out)

    def redirections(self):
        """ARM64X/ARM64EC export redirection: x64 entry RVA -> native RVA."""
        rva, size = self.dirs[10]
        if not rva:
            return {}
        lc = self.off(rva)
        lsize = struct.unpack_from('<I', self.d, lc)[0]
        if lsize < 0xd0:
            return {}
        chpe = struct.unpack_from('<Q', self.d, lc + 0xc8)[0]
        if not chpe:
            return {}
        c = self.off(chpe - self.base)
        # IMAGE_ARM64EC_METADATA: Version, CodeMap, CodeMapCount, CodeRangesToEntryPoints, RedirectionMetadata,
        # 7 dispatch/aux fields, CodeRangesToEntryPointsCount, RedirectionMetadataCount
        f = struct.unpack_from('<14I', self.d, c)
        rm, rmc = f[4], f[13]
        return dict(struct.unpack_from('<II', self.d, self.off(rm) + 8 * i) for i in range(rmc))

    def imports(self):
        """IAT slot RVA -> (dll name in lower case, function name); ordinal imports are left out."""
        rva, size = self.dirs[1]
        out = {}
        if not rva:
            return out
        i = 0
        while True:
            oft, ts, fc, name, ft = struct.unpack_from('<IIIII', self.d, self.off(rva) + 20 * i)
            if not name and not ft:
                break
            no = self.off(name)
            dll = self.d[no:self.d.index(b'\0', no)].decode().lower()
            j = 0
            while True:
                ent = struct.unpack_from('<Q', self.d, self.off(oft or ft) + 8 * j)[0]
                if not ent:
                    break
                if not ent >> 63:
                    ho = self.off(ent & 0x7fffffff) + 2
                    out[ft + 8 * j] = (dll, self.d[ho:self.d.index(b'\0', ho)].decode())
                j += 1
            i += 1
        return out

    def exports(self):
        """name -> native RVA (forwarders skipped)."""
        rva, size = self.dirs[0]
        if not rva:
            return {}
        e = self.off(rva)
        nf, nn, af, an, ao = struct.unpack_from('<IIIII', self.d, e + 20)
        red = self.redirections()
        out = {}
        for i in range(nn):
            nr = self.u32(an + 4 * i)
            od = struct.unpack_from('<H', self.d, self.off(ao) + 2 * i)[0]
            f = self.u32(af + 4 * od)
            no = self.off(nr)
            name = self.d[no:self.d.index(b'\0', no)].decode()
            if rva <= f < rva + size:  # a forwarder: 'NTDLL.RtlFreeHeap'
                fo = self.off(f)
                out[name] = self.d[fo:self.d.index(b'\0', fo)].decode()
                continue
            out[name] = red.get(f, f)
        return out

    def aux_iat(self):
        """(aux IAT RVA, IAT RVA): ARM64EC native code calls imports through the auxiliary IAT, which parallels the IAT."""
        rva, size = self.dirs[10]
        if not rva:
            return None
        lc = self.off(rva)
        if struct.unpack_from('<I', self.d, lc)[0] < 0xd0:
            return None
        chpe = struct.unpack_from('<Q', self.d, lc + 0xc8)[0]
        if not chpe:
            return None
        f = struct.unpack_from('<14I', self.d, self.off(chpe - self.base))
        return f[11], self.dirs[12][0]


INS = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{8}\s+)?(\S+)\s*(.*)$')
TARGET = re.compile(r'0x([0-9a-f]+)')


def disassemble(pe):
    code = {}
    for va, vs in pe.text():
        a, b = pe.base + va, pe.base + va + vs
        out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', '--start-address=%#x' % a, '--stop-address=%#x' % b,
                              pe.path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            m = INS.match(line)
            if m:
                ops = re.sub(r'<[^>]*>', '', m.group(3).split('//')[0]).strip()  # drop the <symbol+0x..> annotation
                code[int(m.group(1), 16) - pe.base] = (m.group(2), ops)
    return code


FP_OK = ('fmov',)


READS_ONLY = ('cbz', 'cbnz', 'tbz', 'tbnz', 'cmp', 'cmn', 'tst', 'ccmp', 'ccmn', 'str', 'strb', 'strh', 'stur',
              'sturb', 'sturh', 'stp', 'stlr', 'stlrb', 'stlrh', 'prfm', 'blr', 'br')


def writes(mn, op, reg):
    """Does this instruction write reg (its first operand)?"""
    return mn not in READS_ONLY and op.split(',')[0].strip() == reg


def fp_affected(mn):
    if mn.startswith('f') and mn not in FP_OK:
        return True
    return mn.startswith(('scvtf', 'ucvtf', 'bfcvt', 'bfm', 'bfdot'))


def dispatcher_vars(pe):
    """RVAs of ntdll's __wine_syscall_dispatcher, __wine_unix_call_dispatcher(_arm64ec) pointer variables."""
    out = subprocess.run([OBJDUMP, '-t', pe.path], capture_output=True, text=True).stdout
    found = []  # an ARM64X ntdll has a copy of each for either half
    for line in out.splitlines():
        m = re.search(r'\(sec\s+(\d+)\).*0x([0-9a-f]+) (__wine_syscall_dispatcher|__wine_unix_call_dispatcher|__wine_unix_call_dispatcher_arm64ec)$', line)
        if m:
            found.append(pe.secs[int(m.group(1)) - 1][1] + int(m.group(2), 16))
    return found


def symbols(pe):
    """function symbol name -> RVA (COFF symbol table of our own builds)."""
    out = subprocess.run([OBJDUMP, '-t', pe.path], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        m = re.search(r'\(sec\s+(\d+)\).*\(ty\s+20\).*0x([0-9a-f]+) (\S+)$', line)
        if m and int(m.group(1)) <= len(pe.secs):
            syms.setdefault(m.group(3), pe.secs[int(m.group(1)) - 1][1] + int(m.group(2), 16))
    return syms


# Raising an exception from a listed function (only error paths do: a unix call failing while a debug message is
# written, heap corruption, a critical section wait timing out) runs Wine's dispatch_exception and the handlers with
# the JIT's FPCR. That is already the case for every exception raised in JIT code (self-modifying-code write faults,
# guest access violations): the unix side hands KiUserExceptionDispatcher the faulting context's FPCR. Nothing new.
FENCED = ('pNotifyMapViewOfSection', 'pNotifyMemoryAlloc', 'pNotifyMemoryFree', 'pNotifyMemoryProtect',
          'pNotifyUnmapViewOfSection', 'pBTCpu64FlushInstructionCache', 'pBTCpu64NotifyMemoryDirty',
          'pBTCpu64NotifyReadFile', 'pBTCpu64IsProcessorFeaturePresent')

SINKS = ('RtlRaiseException', 'RtlRaiseStatus', '#RtlRaiseException', '#RtlRaiseStatus')


REGISTRY = {}  # dll name (lower case) -> Scan, for following imports between our own images
SEARCH = []    # folders holding them


def image(dll, syscalls):
    if dll not in REGISTRY:
        REGISTRY[dll] = None
        for d in SEARCH:
            p = os.path.join(d, dll)
            if os.path.exists(p):
                REGISTRY[dll] = Scan(PE(p), syscalls)
                break
    return REGISTRY[dll]


class Scan:
    def __init__(self, pe, syscalls):
        self.pe = pe
        self.imps = pe.imports()
        self.syms = None
        aux = pe.aux_iat()
        if aux and aux[0]:
            for slot, imp in list(self.imps.items()):
                self.imps[slot - aux[1] + aux[0]] = imp
        self.exps = None
        syms = symbols(pe)
        self.sinks = {syms[n] for n in SINKS if n in syms}
        self.dispatchers = set(dispatcher_vars(pe)) if syscalls else set()
        out = subprocess.run([OBJDUMP, '-t', pe.path], capture_output=True, text=True).stdout
        # __os_arm64x_dispatch_call / _icall: Wine's arm64x_check_call (signal_arm64ec.c, integer only) answers in x11
        # what to branch to. pNotify* / pBTCpu64*: ntdll's pointers to FEX's notification exports, which FEX patch
        # 0043 fences (they clear the JIT's FPCR bits on entry and put them back on return).
        self.checkers, self.icheckers, self.fenced = set(), set(), set()
        for line in out.splitlines():
            m = re.search(r'\(sec\s+(\d+)\).*0x([0-9a-f]+) (\S+)$', line)
            if not m or int(m.group(1)) > len(pe.secs):
                continue
            at = pe.secs[int(m.group(1)) - 1][1] + int(m.group(2), 16)
            name = m.group(3)
            if name == '__os_arm64x_dispatch_call':
                self.checkers.add(at)
            elif name in ('__os_arm64x_dispatch_icall', '__os_arm64x_dispatch_icall_cfg'):
                self.icheckers.add(at)
            elif syscalls and name in FENCED:
                self.fenced.add(at)
        self.code = disassemble(pe)
        self.funcs = pe.functions()
        self.begins = [b for b, e in self.funcs]
        self.syscalls = syscalls
        self.memo = {}

    def extent(self, rva):
        import bisect
        i = bisect.bisect_right(self.begins, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            return self.funcs[i]
        # no .pdata (a leaf without a frame): up to the next function start or ret
        j = bisect.bisect_right(self.begins, rva)
        end = self.begins[j] if j < len(self.begins) else rva + 0x4000
        return (rva, end)

    def is_syscall_stub(self, rva):
        # Wine's ARM64EC syscall thunk: mov x8, #id ; mov x9, x30 ; ldr x16, [..]/adrp..; blr x16 ; ret
        seq = [self.code.get(rva + 4 * i, ('?', ''))[0] for i in range(8)]
        return seq[0] == 'mov' and self.code[rva][1].startswith('x8,') and 'blr' in seq[:6] and 'ret' in seq[:8]

    def check_import_unused(self, dll, name, depth):
        img = image(dll, self.syscalls)
        if img is None:
            return 'import %s!%s: not one of our images' % (dll, name)
        if img.exps is None:
            img.exps = img.pe.exports()
        if name not in img.exps:
            return 'import %s!%s: not exported' % (dll, name)
        target = img.exps[name]
        if isinstance(target, str):  # forwarder
            fdll, fname = target.split('.', 1)
            if depth > 20:
                return 'forwarder loop'
            return self.check_import(fdll.lower() + '.dll', fname, depth + 1)
        return img.check(target, depth + 1)

    def ec_entry(self, name, rva):
        \"\"\"Native callers in an ARM64EC process call the EC copy ('#name') of an ARM64X export.\"\"\"
        if self.syms is None:
            self.syms = symbols(self.pe)
        for n in ('#' + name + '$hp_target', '#' + name):
            if n in self.syms:
                return self.syms[n]
        return rva

    def _rest_ok(self, a, e):
        return True

    def is_jump_table(self, at, ops):
        """br xN whose xN was computed from a table entry: add xN, xM, wK, sxtw|sxtb|sxth|uxtw|lsl ... (not loaded
        as a pointer, which would be a tail call to somewhere unknown)."""
        reg = ops.split(',')[0].strip()
        for k in range(1, 5):
            ins = self.code.get(at - 4 * k)
            if not ins:
                return False
            mn, op = ins
            if op.split(',')[0].strip() != reg:
                continue
            return mn == 'add' and re.search(r'\b[wx]\d+, (sxt[bhw]|uxt[bhw]|lsl)', op) is not None
        return False

    def load_source(self, at, reg):
        """The variable reg was loaded from: ldr reg, [xM, #imm] after adrp xM, page (or None)."""
        for k in range(1, 16):
            ins = self.code.get(at - 4 * k)
            if not ins:
                return None
            mn, op = ins
            if not writes(mn, op, reg):
                continue
            parts = [x.strip() for x in op.replace('[', ',').replace(']', ',').split(',') if x.strip()]
            if mn != 'ldr' or len(parts) < 3:
                return None
            base, imm = parts[1], parts[2].lstrip('#')
            for k2 in range(k + 1, k + 12):
                i2 = self.code.get(at - 4 * k2)
                if i2 and i2[0] == 'adrp' and i2[1].split(',')[0].strip() == base:
                    return int(TARGET.findall(i2[1])[-1], 16) - self.pe.base + int(imm, 0)
            return None
        return None

    def reg_const(self, at, reg):
        """The address put in reg by adrp reg, page ; add reg, reg, #off before at (or None)."""
        off = None
        for k in range(1, 12):
            ins = self.code.get(at - 4 * k)
            if not ins:
                return None
            mn, op = ins
            parts = [x.strip() for x in op.split(',')]
            if not writes(mn, op, reg):
                continue
            if mn == 'add' and off is None and len(parts) == 3 and parts[1] == reg:
                off = int(parts[2].lstrip('#'), 0)
                continue
            if mn == 'adrp':
                return int(TARGET.findall(op)[-1], 16) - self.pe.base + (off or 0)
            return None
        return None

    def dispatch_target(self, at, ops):
        """The address a blr loads its target from: ldr xN, [xM, #imm] after adrp xM, or ldr xN, [xN] after ldr xN, =lit."""
        reg = ops.split(',')[0].strip()
        for k in range(1, 9):
            ins = self.code.get(at - 4 * k)
            if not ins:
                return None
            mn, op = ins
            parts = [x.strip() for x in op.replace('[', ',').replace(']', ',').split(',') if x.strip()]
            if not writes(mn, op, reg):
                continue
            if mn == 'ldr' and len(parts) >= 2 and parts[1] == reg and len(parts) == 2:
                # ldr xN, [xN] : xN came from a literal holding the variable's address
                for k2 in range(k + 1, k + 4):
                    i2 = self.code.get(at - 4 * k2)
                    if i2 and i2[0] == 'ldr' and i2[1].startswith(reg + ',') and '[' not in i2[1]:
                        lit = int(TARGET.findall(i2[1])[-1], 16) - self.pe.base
                        va = struct.unpack_from('<Q', self.pe.d, self.pe.off(lit))[0]
                        return va - self.pe.base
                return None
            if mn == 'ldr' and len(parts) >= 3:
                base, imm = parts[1], parts[2].lstrip('#')
                for k2 in range(k + 1, k + 9):
                    i2 = self.code.get(at - 4 * k2)
                    if i2 and i2[0] == 'adrp' and i2[1].split(',')[0].strip() == base:
                        page = int(TARGET.findall(i2[1])[-1], 16) - self.pe.base
                        return page + int(imm, 0)
                return None
            return None
        return None

    def check(self, rva, depth=0):
        """None if rva and everything it can reach are neutral, else the reason (first bad path found)."""
        return check_node(self, rva)

    def _local(self, rva):
        """Walk the code reachable from rva: both ways of conditional branches, the target of b (a jump inside the
        function or a tail call), past bl (whose callee becomes an edge); ret and the end of the process stop a path.
        Returns (the first bad instruction or None, the edges [(scan, rva, label)])."""
        edges = []
        if rva in self.sinks:
            return None, edges
        work, seen = [rva], set()
        checked_x11 = False
        while work:
            a = work.pop()
            while a not in seen:
                seen.add(a)
                if a in self.sinks:
                    break
                ins = self.code.get(a)
                if ins is None:
                    return 'no code at %#x' % a, edges
                mn, ops = ins
                if fp_affected(mn):
                    return '%#x %s %s' % (a, mn, ops), edges
                if mn == 'msr' and 'fpcr' in ops.lower():  # reading it (RtlCaptureContext) changes nothing
                    return '%#x %s %s' % (a, mn, ops), edges
                if mn == 'brk' and ops in ('#0x1', '#0xf003'):
                    break  # __builtin_trap, __fastfail: the process ends there
                if mn in ('svc', 'hvc', 'brk', 'udf', 'smc', 'eret'):
                    return '%#x %s %s' % (a, mn, ops), edges
                if mn in ('ret', 'retaa', 'retab'):
                    break
                if mn in ('blr', 'br', 'braa', 'brab', 'blraa', 'blrab', 'braaz', 'brabz', 'blraaz', 'blrabz'):
                    slot = self.dispatch_target(a, ops) if mn in ('blr', 'br') else None
                    if slot in self.dispatchers:
                        # ntdll's syscall and unix call stubs (the ARM64EC unix call tail-calls through
                        # __wine_unix_call_dispatcher_arm64ec, which the unix loader points at __wine_unix_call_dispatcher):
                        # Wine patch 0070 clears the JIT's FPCR bits on the way to the unix side, and the dispatchers
                        # put the caller's FPCR back on return
                        if mn == 'br':
                            break
                        a += 4
                        continue
                    if mn == 'blr' and slot in self.icheckers:
                        src = self.load_source(a, 'x11')
                        if src not in self.fenced:
                            return '%#x checked indirect call through %s' % (a, hex(src) if src else '?'), edges
                        checked_x11 = True
                        a += 4
                        continue
                    if mn == 'blr' and slot in self.checkers:
                        # a call to a hybrid_patchable function: x9 holds its native code, the checker returns in x11
                        # what to branch to (that code, or an exit thunk to x64 code that hooked it, which the JIT
                        # runs with its own FPCR)
                        tgt = self.reg_const(a, 'x9')
                        if tgt is None:
                            return '%#x checked call with an unknown target' % a, edges
                        edges.append((self, tgt, '%#x' % tgt))
                        checked_x11 = True
                        a += 4
                        continue
                    if ops.split(',')[0].strip() == 'x11' and checked_x11:
                        checked_x11 = False
                        if mn == 'br':
                            break
                        a += 4
                        continue
                    if slot in self.imps:
                        dll, name = self.imps[slot]
                        node = resolve_import(self, dll, name)
                        if isinstance(node, str):
                            return node, edges
                        edges.append(node + ('%s!%s' % (dll, name),))
                        if mn == 'br':
                            break
                        a += 4
                        continue
                    if mn == 'br' and self.is_jump_table(a, ops):
                        fb, fe = self.extent(a)
                        if fb == a or fe - fb > 0x10000:
                            return '%#x jump table outside a known function' % a, edges
                        work.extend(range(fb, fe, 4))  # every instruction of the function, conservatively
                        break
                    return '%#x %s %s' % (a, mn, ops), edges
                t = TARGET.findall(ops) if (mn in ('bl', 'b') or mn.startswith('b.') or
                                             mn in ('cbz', 'cbnz', 'tbz', 'tbnz')) else None
                if t:
                    tr = int(t[-1], 16) - self.pe.base
                    if mn == 'bl':
                        edges.append((self, tr, '%#x' % tr))
                    elif mn == 'b':
                        a = tr
                        continue
                    else:
                        work.append(tr)
                a += 4
        return None, edges


def resolve_import(scan, dll, name, hops=0):
    """(scan, rva) of an import, following forwarders and api sets, or the reason it can't be followed."""
    if dll.startswith('api-ms-win-crt-'):
        dll = 'ucrtbase.dll'
    elif dll.startswith(('api-ms-win-core-', 'api-ms-win-eventing-', 'api-ms-win-security-')):
        dll = 'kernelbase.dll'
    img = image(dll, scan.syscalls)
    if img is None:
        return 'import %s!%s: not one of our images' % (dll, name)
    if img.exps is None:
        img.exps = img.pe.exports()
    target = img.exps.get(name)
    if target is None and dll == 'kernelbase.dll':
        return resolve_import(scan, 'kernel32.dll', name, hops + 1) if hops < 4 else 'import loop'
    if target is None:
        return 'import %s!%s: not exported' % (dll, name)
    if isinstance(target, str):  # forwarder
        if hops > 8:
            return 'forwarder loop'
        fdll, fname = target.split('.', 1)
        return resolve_import(scan, fdll.lower() + '.dll', fname, hops + 1)
    ec = img.ec_entry(name, target)
    return (img, ec)


RESULT = {}  # (image name, rva) -> None or reason


def check_node(scan, rva):
    """Build the graph reachable from (scan, rva), then mark bad every node that has a bad instruction or reaches one."""
    key = (scan.pe.path, rva)
    if key in RESULT:
        return RESULT[key]
    nodes, order, stack = {}, [], [(scan, rva)]
    while stack:
        sc, r = stack.pop()
        k = (sc.pe.path, r)
        if k in nodes or k in RESULT:
            continue
        reason, edges = sc._local(r)
        nodes[k] = (reason, [(e[0].pe.path, e[1], e[2]) for e in edges])
        order.append(k)
        for e in edges:
            stack.append((e[0], e[1]))
    bad = {k: v[0] for k, v in nodes.items() if v[0]}
    changed = True
    while changed:
        changed = False
        for k, (reason, edges) in nodes.items():
            if k in bad:
                continue
            for ek in edges:
                eki = (ek[0], ek[1])
                why = bad.get(eki) or RESULT.get(eki)
                if why:
                    bad[k] = 'via %s: %s' % (ek[2], why)
                    changed = True
                    break
    for k in nodes:
        RESULT[k] = bad.get(k)
    return RESULT[key]


def main():
    args = sys.argv[1:]
    path = args[0]
    SEARCH.append(os.path.dirname(os.path.abspath(path)))
    for extra in (args[args.index('--search') + 1].split(':') if '--search' in args else []):
        SEARCH.append(extra)
    s = image(os.path.basename(path).lower(), '--syscalls' in args)
    pe = s.pe
    if '--rvas' in args:
        targets = {('%#x' % int(x, 16)): int(x, 16) for x in args[args.index('--rvas') + 1].split(',')}
    else:
        targets = {n: r for n, r in pe.exports().items() if not isinstance(r, str)}
    res = {}
    for name, rva in sorted(targets.items()):
        why = s.check(rva)
        res[name] = (rva, why)
        if '--why' in args or '--rvas' in args:
            print('%-40s %#8x %s' % (name, rva, 'NEUTRAL' if why is None else why))
    ok = sorted(n for n, (r, w) in res.items() if w is None)
    print('# %s: %d of %d neutral' % (os.path.basename(path), len(ok), len(res)), file=sys.stderr)
    if '--json' in args:
        json.dump({n: res[n][0] for n in ok}, open(args[args.index('--json') + 1], 'w'), indent=0)


if __name__ == '__main__':
    main()
