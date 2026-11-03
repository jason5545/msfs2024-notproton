// ecdll.cpp — ARM64EC DLL built like the DXVK/vkd3d-proton ARM64EC builds (llvm-mingw, libc++, static runtime):
// C++ exceptions thrown and caught inside EC code, through several frames with destructors, as DXVK does on errors.
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>
static volatile long dtors;
struct Guard { ~Guard() { InterlockedIncrement(&dtors); } };
__attribute__((noinline)) static void deep(int d, int n) {
  Guard g; std::vector<int> v(16, d);
  if (d == 0) throw std::runtime_error("ec error " + std::to_string(n));
  deep(d - 1, n);
}
// throws and catches n times; returns the number caught (or -1 on a wrong message)
extern "C" __declspec(dllexport) int ec_throw(int n) {
  int caught = 0;
  for (int i = 0; i < n; i++) {
    try { deep(5, i); }
    catch (const std::exception &e) { if (std::string(e.what()) != "ec error " + std::to_string(i)) return -1; caught++; }
  }
  return caught;
}
extern "C" __declspec(dllexport) long ec_dtors(void) { return dtors; }
// x64 callback called from EC code, the way DXVK/vkd3d call back into the game (e.g. a message callback)
extern "C" __declspec(dllexport) int ec_call(int (*cb)(int), int n) { int s = 0; for (int i = 0; i < n; i++) s += cb(i); return s; }
