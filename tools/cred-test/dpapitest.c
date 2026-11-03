/*
 * dpapitest: does data CryptProtectData protected open again? Wine's DPAPI mixes the user name
 * (GetUserNameA) into its key, so a prefix that ran as "crossover" under CrossOver only opens its
 * protected data (Xbox sign-in tokens among them) under that name.
 *
 *   dpapitest protect <file>     protect a fixed text, write the blob to <file>
 *   dpapitest unprotect <file>   read the blob, unprotect it, compare with the text
 *
 * Prints the user name and RESULT PASS / FAIL. Build: x86_64-w64-mingw32-gcc -O1 -o dpapitest.exe dpapitest.c -lcrypt32
 */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

static const char text[] = "steamac dpapi probe";

int main(int argc, char **argv)
{
    char user[256];
    DWORD len = sizeof(user);
    DATA_BLOB in, out;
    FILE *f;

    if (argc != 3) { printf("usage: dpapitest protect|unprotect <file>\n"); return 2; }
    if (!GetUserNameA(user, &len)) strcpy(user, "?");
    printf("user %s\n", user);
    if (!strcmp(argv[1], "protect"))
    {
        in.pbData = (BYTE *)text; in.cbData = sizeof(text);
        if (!CryptProtectData(&in, L"probe", NULL, NULL, NULL, 0, &out))
        { printf("CryptProtectData failed %lu\nRESULT FAIL\n", GetLastError()); return 1; }
        if (!(f = fopen(argv[2], "wb")) || fwrite(out.pbData, 1, out.cbData, f) != out.cbData)
        { printf("cannot write %s\nRESULT FAIL\n", argv[2]); return 1; }
        fclose(f);
        printf("protected %lu bytes\nRESULT PASS\n", out.cbData);
        return 0;
    }
    else
    {
        static BYTE buf[65536];
        size_t n;
        if (!(f = fopen(argv[2], "rb"))) { printf("cannot read %s\nRESULT FAIL\n", argv[2]); return 1; }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        in.pbData = buf; in.cbData = (DWORD)n;
        if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out))
        { printf("CryptUnprotectData failed %lu\nRESULT FAIL\n", GetLastError()); return 1; }
        if (out.cbData != sizeof(text) || memcmp(out.pbData, text, sizeof(text)))
        { printf("unprotected data differs\nRESULT FAIL\n"); return 1; }
        printf("unprotected the same %lu bytes\nRESULT PASS\n", out.cbData);
        return 0;
    }
}
