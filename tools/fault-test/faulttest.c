// faulttest: faults inside translated code must report the exact guest instruction. FEX finds the block a host PC lies
// in to rebuild RIP (squark FEX 0039: the JIT no longer stores the current block at every entry point, the fault path
// looks it up), so this checks ExceptionAddress, Context.Rip and the registers at access violations placed in the
// entry block of a function, right after a call returns, three calls deep, inside a hot loop, and in code compiled
// after thousands of other blocks; then misaligned LOCK operations across a 64-byte line (FEX's unaligned-access
// handler patches the block it finds the same way). Prints one line per check and "RESULT PASS" or "RESULT FAIL".
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

// Each fault site is a "mov eax, [rbx]" (8B 03, 2 bytes) at a global label; the handler checks RIP, skips the
// instruction and returns 0x5A5A in eax.
__asm__(".intel_syntax noprefix\n"
        ".text\n"
        // fault in the entry block: no call before the site
        ".globl f_entry\nf_entry:\n"
        "  push rbx\n  mov rbx, rcx\n  add rcx, 8\n"
        ".globl s_entry\ns_entry:\n  mov eax, dword ptr [rbx]\n"
        "  pop rbx\n  ret\n"
        // fault in the block a call returns to
        ".globl f_after_call\nf_after_call:\n"
        "  push rbx\n  mov rbx, rcx\n  call f_leaf\n  add eax, 1\n"
        ".globl s_after_call\ns_after_call:\n  mov eax, dword ptr [rbx]\n"
        "  pop rbx\n  ret\n"
        "f_leaf:\n  mov eax, 7\n  ret\n"
        // fault three calls deep
        ".globl f_deep\nf_deep:\n  push rbx\n  mov rbx, rcx\n  call f_deep2\n  pop rbx\n  ret\n"
        "f_deep2:\n  sub rsp, 8\n  call f_deep3\n  add rsp, 8\n  ret\n"
        "f_deep3:\n  call f_leaf\n  xor ecx, ecx\n"
        ".globl s_deep\ns_deep:\n  mov eax, dword ptr [rbx]\n  ret\n"
        // fault in a loop on its 1000th iteration (rcx = bad pointer, rdx = good pointer)
        ".globl f_loop\nf_loop:\n  push rbx\n  xor r8d, r8d\n"
        "1:\n  mov rbx, rdx\n  cmp r8d, 999\n  cmove rbx, rcx\n"
        ".globl s_loop\ns_loop:\n  mov eax, dword ptr [rbx]\n"
        "  inc r8d\n  cmp r8d, 1000\n  jb 1b\n  pop rbx\n  ret\n"
        // misaligned LOCK XADD / LOCK CMPXCHG on rcx
        ".globl f_xadd\nf_xadd:\n  mov eax, edx\n  lock xadd dword ptr [rcx], eax\n  ret\n"
        ".globl f_cmpxchg\nf_cmpxchg:\n  mov eax, edx\n  lock cmpxchg qword ptr [rcx], r8\n  ret\n"
        ".att_syntax\n");

extern int f_entry(void* p), f_after_call(void* p), f_deep(void* p), f_loop(void* bad, void* good);
extern unsigned f_xadd(void* p, unsigned add);
extern uint64_t f_cmpxchg(void* p, uint64_t expected, uint64_t desired);
extern char s_entry[], s_after_call[], s_deep[], s_loop[];

static volatile uint64_t g_addr, g_rip, g_rbx, g_count;
static LONG CALLBACK handler(EXCEPTION_POINTERS* ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  g_addr = (uint64_t)ep->ExceptionRecord->ExceptionAddress;
  g_rip = ep->ContextRecord->Rip;
  g_rbx = ep->ContextRecord->Rbx;
  g_count++;
  ep->ContextRecord->Rip += 2;
  ep->ContextRecord->Rax = 0x5A5A;
  return EXCEPTION_CONTINUE_EXECUTION;
}

static int fails;
static void check(const char* name, int ret, void* site, void* bad) {
  int ok = ret == 0x5A5A && g_addr == (uint64_t)site && g_rip == (uint64_t)site && g_rbx == (uint64_t)bad && g_count == 1;
  printf("%s %s: ret=%#x addr=%#llx rip=%#llx site=%#llx rbx=%#llx faults=%llu\n", ok ? "PASS" : "FAIL", name, ret,
         (unsigned long long)g_addr, (unsigned long long)g_rip, (unsigned long long)site, (unsigned long long)g_rbx,
         (unsigned long long)g_count);
  fails += !ok;
  g_addr = g_rip = g_rbx = g_count = 0;
}

// Many distinct small functions, so the code buffer holds thousands of blocks before the late fault.
#define F(n) static __attribute__((noinline)) int fill##n(int x) { return x * (n + 3) + (x >> (n % 7)); }
#define F10(n) F(n##0) F(n##1) F(n##2) F(n##3) F(n##4) F(n##5) F(n##6) F(n##7) F(n##8) F(n##9)
#define F100(n) F10(n##0) F10(n##1) F10(n##2) F10(n##3) F10(n##4) F10(n##5) F10(n##6) F10(n##7) F10(n##8) F10(n##9)
F100(1) F100(2) F100(3)
#define C(n) s += fill##n(i);
#define C10(n) C(n##0) C(n##1) C(n##2) C(n##3) C(n##4) C(n##5) C(n##6) C(n##7) C(n##8) C(n##9)
#define C100(n) C10(n##0) C10(n##1) C10(n##2) C10(n##3) C10(n##4) C10(n##5) C10(n##6) C10(n##7) C10(n##8) C10(n##9)

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  AddVectoredExceptionHandler(1, handler);
  void* bad = (void*)(uintptr_t)0x10;
  static int good = 1;

  for (int round = 0; round < 3; round++) {
    // round 0 compiles the sites cold, later rounds run them from the code cache
    check("entry", f_entry(bad), s_entry, bad);
    check("after_call", f_after_call(bad), s_after_call, bad);
    check("deep", f_deep(bad), s_deep, bad);
    check("loop", f_loop(bad, &good), s_loop, bad);
  }

  volatile int s = 0;
  for (int i = 0; i < 3; i++) {
    C100(1) C100(2) C100(3)
  }
  check("entry_late", f_entry(bad), s_entry, bad);
  check("loop_late", f_loop(bad, &good), s_loop, bad);

  // misaligned atomics across a 64-byte line
  static __attribute__((aligned(64))) unsigned char buf[192];
  unsigned* p32 = (unsigned*)(buf + 62);
  uint64_t* p64 = (uint64_t*)(buf + 124);
  *p32 = 1000;
  *p64 = 5;
  int bad_atomic = 0;
  for (int i = 0; i < 1000; i++) {
    unsigned old = f_xadd(p32, 3);
    if (old != 1000u + 3u * i) bad_atomic++;
    uint64_t prev = f_cmpxchg(p64, 5 + (uint64_t)i, 6 + (uint64_t)i);
    if (prev != 5 + (uint64_t)i) bad_atomic++;
  }
  int ok = !bad_atomic && *p32 == 1000u + 3000u && *p64 == 1005;
  printf("%s misaligned_lock: xadd=%u cmpxchg=%llu wrong=%d\n", ok ? "PASS" : "FAIL", *p32, (unsigned long long)*p64, bad_atomic);
  fails += !ok;

  printf("RESULT %s fails=%d sum=%d\n", fails ? "FAIL" : "PASS", fails, s);
  return fails ? 1 : 0;
}
