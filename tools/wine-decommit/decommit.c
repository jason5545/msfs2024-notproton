/*
 * decommit (squark, 11/5): does MEM_DECOMMIT followed by MEM_COMMIT give zero pages back, for ranges that do not start
 * or end on a 16 KB boundary? Windows says a decommitted page reads zero once committed again; FEX relies on it to
 * clear a thread's call-return stack (Allocator::VirtualDontNeed: decommit, recommit). On Apple Silicon a host page
 * is 16 KB, and FEX's call-return stack starts 4 KB into its reservation (a guard page first), so its first and last
 * 16 KB host pages are shared with a guard page.
 *
 * The layout is FEX's (CallRetStack::InitializeThread): reserve SIZE + 8 KB no-access, commit SIZE read-write at
 * +4 KB. Each 4 KB page of the committed range is filled with 0xAA, the whole range is decommitted and committed
 * again (as VirtualDontNeed does), and every 4 KB page is checked for zero. SIZE is 64 KB (env SIZE_KB to change).
 * Output: the pages that kept their old bytes, then "RESULT decommit kept=<n> pages=<n>" (kept must be 0).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  const char *env = getenv("SIZE_KB");
  const size_t size = (env ? (size_t)atoi(env) : 64) * 1024;
  unsigned char *alloc = VirtualAlloc(NULL, size + 0x2000, MEM_RESERVE, PAGE_NOACCESS);
  unsigned char *base = alloc + 0x1000;
  if (!alloc || !VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE)) {
    printf("alloc failed %lu\n", GetLastError());
    return 2;
  }
  memset(base, 0xaa, size);
  if (!VirtualFree(base, size, MEM_DECOMMIT) || !VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE)) {
    printf("decommit/commit failed %lu\n", GetLastError());
    return 2;
  }
  int kept = 0;
  const int pages = (int)(size / 0x1000);
  for (int p = 0; p < pages; p++) {
    const unsigned char *q = base + p * 0x1000;
    int nonzero = 0;
    for (int i = 0; i < 0x1000; i++) {
      nonzero += q[i] != 0;
    }
    if (nonzero) {
      printf("page %d (base+0x%x, host page offset 0x%x): %d bytes kept\n", p, p * 0x1000,
             (unsigned)(((ULONG_PTR)q) & 0x3fff), nonzero);
      kept++;
    }
  }
  printf("RESULT decommit kept=%d pages=%d (alloc %p, base %p)\n", kept, pages, alloc, base);
  return kept ? 1 : 0;
}
