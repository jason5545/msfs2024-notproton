/* padprobe: does a Windows program in this prefix see a gamepad? XInput slots, raw-input HID game
 * controllers, and whether GameInputCreate works. Prints only device names and states. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef struct { WORD b; BYTE lt, rt; SHORT lx, ly, rx, ry; } PADX;
typedef struct { DWORD pkt; PADX g; } STATEX;
typedef DWORD (WINAPI *XGS)(DWORD, STATEX *);
typedef HRESULT (WINAPI *GIC)(void **);
int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 0;
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    XGS xgs = x ? (XGS)GetProcAddress(x, "XInputGetState") : NULL;
    UINT n = 0;
    GetRawInputDeviceList(NULL, &n, sizeof(RAWINPUTDEVICELIST));
    RAWINPUTDEVICELIST *l = calloc(n ? n : 1, sizeof(*l));
    n = GetRawInputDeviceList(l, &n, sizeof(RAWINPUTDEVICELIST));
    for (UINT i = 0; i < n; i++) {
        RID_DEVICE_INFO info = { sizeof(info) }; UINT sz = sizeof(info);
        if (l[i].dwType != RIM_TYPEHID) continue;
        GetRawInputDeviceInfoA(l[i].hDevice, RIDI_DEVICEINFO, &info, &sz);
        char name[512] = ""; sz = sizeof(name);
        GetRawInputDeviceInfoA(l[i].hDevice, RIDI_DEVICENAME, name, &sz);
        printf("rawinput HID vid %04lx pid %04lx usage %x/%x %s\n", info.hid.dwVendorId, info.hid.dwProductId,
               info.hid.usUsagePage, info.hid.usUsage, name);
    }
    HMODULE g = LoadLibraryA("gameinput.dll");
    GIC gic = g ? (GIC)GetProcAddress(g, "GameInputCreate") : NULL;
    void *gi = NULL;
    printf("GameInputCreate: %s hr=0x%08lx\n", gic ? "found" : "missing", gic ? (unsigned long)gic(&gi) : 0ul);
    STATEX prev[4] = {0}; DWORD prevr[4] = {9, 9, 9, 9};
    DWORD end = GetTickCount() + secs * 1000;
    do {
        for (DWORD s = 0; s < 4 && xgs; s++) {
            STATEX st = {0};
            DWORD r = xgs(s, &st);
            if (r != prevr[s] || memcmp(&st.g, &prev[s].g, sizeof(st.g))) {
                SYSTEMTIME t; GetLocalTime(&t);
                printf("%02d:%02d:%02d.%03d xinput slot %lu: %s buttons=%04x LT=%u RT=%u lx=%d ly=%d rx=%d ry=%d\n",
                    t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, s, r == 0 ? "connected" : "none",
                    st.g.b, st.g.lt, st.g.rt, st.g.lx, st.g.ly, st.g.rx, st.g.ry);
                prev[s] = st; prevr[s] = r;
            }
        }
        fflush(stdout);
        Sleep(10);
    } while (secs && GetTickCount() < end);
    return 0;
}
