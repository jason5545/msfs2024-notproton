/* ecmain.c — x86-64 program (runs in FEX) that calls into ecdll.dll (ARM64EC): exceptions in EC code from the main
 * thread and from 8 threads at once, destructor count, and an x64 callback called from EC code. */
#include <windows.h>
#include <stdio.h>
typedef int (*throw_fn)(int); typedef long (*dtor_fn)(void); typedef int (*call_fn)(int (*)(int), int);
static throw_fn ec_throw;
static int cb(int i) { return i & 1; }
static DWORD WINAPI worker(void *p) { *(int *)p = ec_throw(2000); return 0; }
int main(void) {
  HMODULE m = LoadLibraryA("ecdll.dll");
  if (!m) { printf("FAIL LoadLibrary %lu\n", GetLastError()); return 1; }
  ec_throw = (throw_fn)(void *)GetProcAddress(m, "ec_throw");
  dtor_fn ec_dtors = (dtor_fn)(void *)GetProcAddress(m, "ec_dtors");
  call_fn ec_call = (call_fn)(void *)GetProcAddress(m, "ec_call");
  int fail = 0, r = ec_throw(10000);
  printf("main thread: caught %d / 10000, dtors %ld (want %d)\n", r, ec_dtors(), 10000 * 6); fail |= r != 10000 || ec_dtors() != 60000;
  HANDLE th[8]; int res[8];
  for (int i = 0; i < 8; i++) th[i] = CreateThread(NULL, 0, worker, &res[i], 0, NULL);
  WaitForMultipleObjects(8, th, TRUE, INFINITE);
  for (int i = 0; i < 8; i++) { printf("thread %d: caught %d / 2000\n", i, res[i]); fail |= res[i] != 2000; }
  printf("dtors %ld (want %d)\n", ec_dtors(), 60000 + 8 * 2000 * 6); fail |= ec_dtors() != 60000 + 8 * 2000 * 6;
  r = ec_call(cb, 1000); printf("x64 callback from EC: %d (want 500)\n", r); fail |= r != 500;
  printf("RESULT %s\n", fail ? "FAIL" : "PASS");
  return fail;
}
