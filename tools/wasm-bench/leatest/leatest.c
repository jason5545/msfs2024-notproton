// x86-64 LEA forms with a 32-bit (and 16-, 64-bit) destination, each run on chosen register values and checked
// against the result computed in C. Prints one line per case and "LEATEST pass N fail M". Built for x64 Windows,
// run under FEX to check its LEA translation (squark FEX patch 0036).
#include <stdint.h>
#include <stdio.h>

static int pass, fail;
static void check(const char *name, uint64_t got, uint64_t want) {
  if (got == want) pass++; else fail++;
  printf("%-34s got=%016llx want=%016llx %s\n", name, (unsigned long long)got, (unsigned long long)want, got == want ? "ok" : "FAIL");
}
static volatile uint64_t vals[] = {0, 1, 0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, 0x100000000ULL,
                                   0xfffffffffffffff8ULL, 0xffffffffffffffffULL, 0x123456789abcdef0ULL, 0x8000000000000000ULL};
static const uint64_t GARBAGE = 0xdeadbeefcafef00dULL;

int main(void) {
  char name[64];
  for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
    for (unsigned j = 0; j < sizeof vals / sizeof vals[0]; j++) {
      uint64_t a = vals[i], c = vals[j], d;
      d = GARBAGE; __asm__ volatile("leal 0x1(%1), %k0" : "+r"(d) : "r"(a));
      snprintf(name, sizeof name, "lea32 1(a) a=%u", i); if (j == 0) check(name, d, (uint32_t)(a + 1));
      d = GARBAGE; __asm__ volatile("leal -8(%1,%2,4), %k0" : "+r"(d) : "r"(a), "r"(c));
      snprintf(name, sizeof name, "lea32 -8(a,c,4) %u,%u", i, j); check(name, d, (uint32_t)(a + c * 4 - 8));
      d = GARBAGE; __asm__ volatile("leal (%1,%2), %k0" : "+r"(d) : "r"(a), "r"(c));
      snprintf(name, sizeof name, "lea32 (a,c) %u,%u", i, j); check(name, d, (uint32_t)(a + c));
      d = GARBAGE; __asm__ volatile("leal 0x7fffffff(%1,%2,8), %k0" : "+r"(d) : "r"(a), "r"(c));
      snprintf(name, sizeof name, "lea32 0x7fffffff(a,c,8) %u,%u", i, j); check(name, d, (uint32_t)(a + c * 8 + 0x7fffffff));
      d = GARBAGE; __asm__ volatile("leal -0x80000000(,%1,2), %k0" : "+r"(d) : "r"(c));
      snprintf(name, sizeof name, "lea32 -0x80000000(,c,2) %u", j); if (i == 0) check(name, d, (uint32_t)(c * 2 - 0x80000000ULL));
      d = GARBAGE; __asm__ volatile("addr32 leal 0x10(%1,%2), %k0" : "+r"(d) : "r"(a), "r"(c));
      snprintf(name, sizeof name, "addr32 lea32 0x10(a,c) %u,%u", i, j); check(name, d, (uint32_t)((uint32_t)a + (uint32_t)c + 0x10));
      d = GARBAGE; __asm__ volatile("leaw 0x3(%1,%2), %w0" : "+r"(d) : "r"(a), "r"(c));
      snprintf(name, sizeof name, "lea16 3(a,c) %u,%u", i, j); check(name, d, (GARBAGE & ~0xffffULL) | (uint16_t)(a + c + 3));
      d = GARBAGE; __asm__ volatile("leaq -0x20(%1,%2,2), %0" : "+r"(d) : "r"(a), "r"(c));
      snprintf(name, sizeof name, "lea64 -0x20(a,c,2) %u,%u", i, j); check(name, d, a + c * 2 - 0x20);
      d = GARBAGE; __asm__ volatile("leal (%1), %k0" : "+r"(d) : "r"(a));
      snprintf(name, sizeof name, "lea32 (a) %u", i); if (j == 0) check(name, d, (uint32_t)a);
    }
  }
  uint64_t d = GARBAGE, want;
  __asm__ volatile("leal vals(%%rip), %k0" : "+r"(d));
  want = (uint32_t)(uintptr_t)vals; check("lea32 vals(%rip)", d, want);
  d = GARBAGE; __asm__ volatile("leal vals+0x40(%%rip), %k0" : "+r"(d));
  want = (uint32_t)((uintptr_t)vals + 0x40); check("lea32 vals+0x40(%rip)", d, want);
  printf("LEATEST pass %d fail %d\n", pass, fail);
  return fail != 0;
}
