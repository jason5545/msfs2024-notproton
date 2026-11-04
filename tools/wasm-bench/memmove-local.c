// Replacement memmove linked into the x64 exe (build.sh's -Dmemmove=bench_memmove variants), so a
// wasm memory.copy stays in x64 code instead of calling into ucrtbase (ARM64EC under Hadron).
//   MM_REP: rep movsb (what MSVC's vcruntime memmove uses on CPUs with ERMS/FSRM)
//   MM_SSE: 16-byte unaligned SSE loads and stores
// bench_memset (-Dmemset=bench_memset) is the same for memory.fill.
#include <stddef.h>
#include <stdint.h>
#include <emmintrin.h>

void *bench_memmove(void *d, const void *s, size_t n) {
  unsigned char *dp = d;
  const unsigned char *sp = s;
  if (dp == sp || !n) return d;
  if (dp < sp || dp >= sp + n) {
#if defined(MM_REP)
    __asm__ volatile("rep movsb" : "+D"(dp), "+S"(sp), "+c"(n) : : "memory");
#else
    while (n >= 16) {
      _mm_storeu_si128((__m128i *)dp, _mm_loadu_si128((const __m128i *)sp));
      dp += 16; sp += 16; n -= 16;
    }
    while (n--) *dp++ = *sp++;
#endif
    return d;
  }
  dp += n; sp += n;
  while (n--) *--dp = *--sp;
  return d;
}

void *bench_memset(void *d, int c, size_t n) {
  unsigned char *dp = d;
  __m128i v = _mm_set1_epi8((char)c);
  while (n >= 16) { _mm_storeu_si128((__m128i *)dp, v); dp += 16; n -= 16; }
  while (n--) *dp++ = (unsigned char)c;
  return d;
}
