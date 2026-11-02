/* credtest: black-box test of Wine's CredWriteW/CredReadW/CredEnumerateW and BCrypt ECDSA P-256
 * export/import/sign, the way XAL appears to use them (Generic creds, Persist=LOCAL_MACHINE,
 * DeviceKey = ECCPRIVATE_BLOB).  x86-64 PE, no CRT.  Uses throwaway keys only; never enumerates
 * with a NULL filter (that would walk the macOS login keychain).
 * usage: credtest.exe            -> all tests except the no-star filter
 *        credtest.exe nostar     -> also CredEnumerateW with a filter that has no '*'  */
typedef unsigned int DWORD; typedef unsigned char BYTE; typedef int BOOL; typedef long NTSTATUS;
typedef unsigned long ULONG; typedef void *HANDLE; typedef unsigned long long SIZE_T; typedef unsigned short wchar_t;
typedef struct { DWORD lo, hi; } FILETIME;
typedef struct {
    DWORD Flags; DWORD Type; wchar_t *TargetName; wchar_t *Comment; FILETIME LastWritten;
    DWORD CredentialBlobSize; BYTE *CredentialBlob; DWORD Persist; DWORD AttributeCount;
    void *Attributes; wchar_t *TargetAlias; wchar_t *UserName;
} CREDENTIALW;
#define WINAPI __stdcall
__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void WINAPI ExitProcess(DWORD);
__declspec(dllimport) DWORD WINAPI GetLastError(void);
__declspec(dllimport) void WINAPI SetLastError(DWORD);
__declspec(dllimport) char *WINAPI GetCommandLineA(void);
__declspec(dllimport) BOOL WINAPI CredWriteW(CREDENTIALW *, DWORD);
__declspec(dllimport) BOOL WINAPI CredReadW(const wchar_t *, DWORD, DWORD, CREDENTIALW **);
__declspec(dllimport) BOOL WINAPI CredEnumerateW(const wchar_t *, DWORD, DWORD *, CREDENTIALW ***);
__declspec(dllimport) BOOL WINAPI CredDeleteW(const wchar_t *, DWORD, DWORD);
__declspec(dllimport) void WINAPI CredFree(void *);
__declspec(dllimport) BOOL WINAPI GetUserNameW(wchar_t *, DWORD *);
__declspec(dllimport) NTSTATUS WINAPI BCryptOpenAlgorithmProvider(void **, const wchar_t *, const wchar_t *, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptSetProperty(void *, const wchar_t *, BYTE *, ULONG, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptGenerateKeyPair(void *, void **, ULONG, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptFinalizeKeyPair(void *, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptExportKey(void *, void *, const wchar_t *, BYTE *, ULONG, ULONG *, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptImportKeyPair(void *, void *, const wchar_t *, void **, BYTE *, ULONG, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptSignHash(void *, void *, BYTE *, ULONG, BYTE *, ULONG, ULONG *, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptVerifySignature(void *, void *, BYTE *, ULONG, BYTE *, ULONG, ULONG);
__declspec(dllimport) NTSTATUS WINAPI BCryptDestroyKey(void *);

static HANDLE out;
static void puts_(const char *s) { DWORD n = 0, w; while (s[n]) n++; WriteFile(out, s, n, &w, 0); }
static void hex_(unsigned long long v) { char b[19]; int i; b[0]='0'; b[1]='x'; for (i = 0; i < 16; i++) b[2+i] = "0123456789abcdef"[(v >> (60 - 4*i)) & 15]; b[18]=0;
    { char *p = b + 2; while (*p == '0' && p[1]) p++; puts_("0x"); puts_(p); } }
static void dec_(unsigned long v) { char b[24]; int i = 23; b[i] = 0; do { b[--i] = '0' + v % 10; v /= 10; } while (v); puts_(b + i); }
static int fails;
static void result(const char *name, int ok, unsigned long long code) {
    puts_(ok ? "PASS " : "FAIL "); puts_(name); puts_(" code="); hex_(code); puts_("\n"); if (!ok) fails++; }
static int memeq(const BYTE *a, const BYTE *b, DWORD n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static void memset_(BYTE *p, int c, SIZE_T n) { while (n--) *p++ = (BYTE)c; }
static int wprefix(const wchar_t *s, const wchar_t *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static BOOL write_cred(const wchar_t *name, BYTE *blob, DWORD size) {
    CREDENTIALW c; memset_((BYTE *)&c, 0, sizeof(c));
    c.Type = 1 /* CRED_TYPE_GENERIC */; c.TargetName = (wchar_t *)name; c.Comment = L"steamac cred test";
    c.CredentialBlobSize = size; c.CredentialBlob = blob; c.Persist = 2 /* LOCAL_MACHINE */;
    return CredWriteW(&c, 0);
}
static int read_cmp(const wchar_t *name, BYTE *blob, DWORD size) {
    CREDENTIALW *c = 0; int ok;
    if (!CredReadW(name, 1, 0, &c)) return -(int)GetLastError();
    ok = c->CredentialBlobSize == size && memeq(c->CredentialBlob, blob, size) && c->Persist == 2 && c->Type == 1;
    CredFree(c); return ok;
}
static void enum_test(const char *label, const wchar_t *filter, int expect) {
    DWORD count = 0, i, mine = 0; CREDENTIALW **list = 0; BOOL ok;
    SetLastError(0);
    ok = CredEnumerateW(filter, 0, &count, &list);
    if (ok) { for (i = 0; i < count; i++) if (list[i]->TargetName && wprefix(list[i]->TargetName, L"SteamacTest|")) mine++; CredFree(list); }
    puts_("     "); puts_(label); puts_(": ret="); dec_(ok); puts_(" lasterror="); dec_(ok ? 0 : GetLastError());
    puts_(" count="); dec_(ok ? count : 0); puts_(" ours="); dec_(mine); puts_(" expected_ours="); dec_(expect); puts_("\n");
    result(label, ok && mine == (DWORD)expect, ok ? 0 : GetLastError());
}

void __cdecl Entry(void) {
    void *alg = 0, *key = 0, *key2 = 0, *pub = 0; NTSTATUS st; ULONG len = 0, siglen = 0;
    BYTE priv[256], pubblob[256], hash[32], sig[64], big[2560]; DWORD i; int r;
    const char *cmd = GetCommandLineA(); int nostar = 0;
    for (; *cmd; cmd++) if (cmd[0]=='n' && cmd[1]=='o' && cmd[2]=='s' && cmd[3]=='t' && cmd[4]=='a' && cmd[5]=='r') nostar = 1;
    out = GetStdHandle((DWORD)-11);
    for (i = 0; i < 32; i++) hash[i] = (BYTE)(i * 7 + 1);
    for (i = 0; i < sizeof(big); i++) big[i] = (BYTE)(i * 31 + 3);
    CredDeleteW(L"SteamacTest|DeviceKey", 1, 0); CredDeleteW(L"SteamacTest|Big", 1, 0); CredDeleteW(L"SteamacTest|Big|1", 1, 0);

    st = BCryptOpenAlgorithmProvider(&alg, L"ECDSA_P256", 0, 0);           result("open ECDSA_P256", st == 0, st);
    st = BCryptGenerateKeyPair(alg, &key, 256, 0);                         result("generate P-256", st == 0, st);
    st = BCryptFinalizeKeyPair(key, 0);                                    result("finalize", st == 0, st);
    st = BCryptExportKey(key, 0, L"ECCPRIVATEBLOB", priv, sizeof(priv), &len, 0);
    result("export ECCPRIVATEBLOB", st == 0 && len == 104 && *(DWORD *)priv == 0x32534345 && ((DWORD *)priv)[1] == 32, st);
    st = BCryptExportKey(key, 0, L"ECCPUBLICBLOB", pubblob, sizeof(pubblob), &len, 0);
    result("export ECCPUBLICBLOB", st == 0 && len == 72, st);

    result("CredWriteW DeviceKey (104 B)", write_cred(L"SteamacTest|DeviceKey", priv, 104), GetLastError());
    r = read_cmp(L"SteamacTest|DeviceKey", priv, 104);                     result("CredReadW DeviceKey same bytes", r == 1, r < 0 ? -r : 0);
    result("CredWriteW 2560 B", write_cred(L"SteamacTest|Big", big, 2560), GetLastError());
    r = read_cmp(L"SteamacTest|Big", big, 2560);                           result("CredReadW 2560 B same bytes", r == 1, r < 0 ? -r : 0);

    {   CREDENTIALW *c = 0; BYTE back[104];
        if (CredReadW(L"SteamacTest|DeviceKey", 1, 0, &c)) { for (i = 0; i < 104; i++) back[i] = c->CredentialBlob[i]; CredFree(c); }
        st = BCryptImportKeyPair(alg, 0, L"ECCPRIVATEBLOB", &key2, back, 104, 0); result("import ECCPRIVATEBLOB read back", st == 0, st);
    }
    st = BCryptSignHash(key2, 0, hash, 32, sig, sizeof(sig), &siglen, 0);   result("sign with imported key", st == 0 && siglen == 64, st);
    st = BCryptVerifySignature(key, 0, hash, 32, sig, siglen, 0);           result("verify with original key", st == 0, st);
    st = BCryptImportKeyPair(alg, 0, L"ECCPUBLICBLOB", &pub, pubblob, 72, 0); result("import ECCPUBLICBLOB", st == 0, st);
    st = BCryptVerifySignature(pub, 0, hash, 32, sig, siglen, 0);           result("verify with imported public", st == 0, st);
    sig[5] ^= 1;
    st = BCryptVerifySignature(pub, 0, hash, 32, sig, siglen, 0);           result("tampered signature rejected", st != 0, st);
    {   void *galg = 0, *gk = 0;
        st = BCryptOpenAlgorithmProvider(&galg, L"ECDSA", 0, 0);          result("open generic ECDSA", st == 0, st);
        if (!st) { st = BCryptSetProperty(galg, L"ECCCurveName", (BYTE *)L"nistP256", 18, 0); result("set curve nistP256", st == 0, st);
                   st = BCryptImportKeyPair(galg, 0, L"ECCPRIVATEBLOB", &gk, priv, 104, 0); result("import ECS2 on generic ECDSA", st == 0, st); }
    }

    enum_test("CredEnumerateW(\"SteamacTest|*\")", L"SteamacTest|*", 2);
    enum_test("CredEnumerateW(\"steamactest|*\") case-insensitive", L"steamactest|*", 2);
    {   DWORD count = 0; CREDENTIALW **list = 0; BOOL ok; DWORD err;
        SetLastError(0); ok = CredEnumerateW(L"SteamacNothing|*", 0, &count, &list); err = ok ? 0 : GetLastError();
        if (ok) CredFree(list);
        puts_("     CredEnumerateW(\"SteamacNothing|*\"): ret="); dec_(ok); puts_(" lasterror="); dec_(err); puts_("\n");
        result("no match still ERROR_NOT_FOUND", !ok && err == 1168, err); }
    {   wchar_t name[64]; DWORD n = 64; BOOL ok = GetUserNameW(name, &n); static const wchar_t cx[] = L"crossover"; int i, same = ok && n == 10;
        for (i = 0; same && i < 10; i++) if (name[i] != cx[i]) same = 0;
        puts_("     GetUserNameW: ret="); dec_(ok); puts_(" len="); dec_(n); puts_(same ? " == crossover\n" : " != crossover\n");
        result("GetUserNameW is crossover (CW Hack 12735)", same, ok ? 0 : GetLastError()); }
    if (nostar) enum_test("CredEnumerateW(\"SteamacTest|DeviceKey\") no star", L"SteamacTest|DeviceKey", 1);

    CredDeleteW(L"SteamacTest|DeviceKey", 1, 0); CredDeleteW(L"SteamacTest|Big", 1, 0); CredDeleteW(L"SteamacTest|Big|1", 1, 0);
    puts_("fails="); dec_(fails); puts_("\n");
    ExitProcess(fails);
}
