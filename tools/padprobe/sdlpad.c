/* sdlpad: what SDL itself reports for the pads on this Mac, with the same SDL the Hadron runtime
 * carries (sdl2-compat + SDL3 from the fork's scratch/deps-arm64). Prints every change of the game
 * controller state. SDL's convention: stick Y negative when pushed up, triggers 0 to 32767.
 * Together with fakepad-test.py (SDL -> winebus -> XInput) it covers the whole path of a real pad.
 *
 *   build: clang -o sdlpad sdlpad.c -I$D/include/SDL2 -D_THREAD_SAFE -L$D/lib -lSDL2-2.0.0
 *          -Wl,-headerpad_max_install_names, then install_name_tool -change @loader_path/libSDL2-2.0.0.dylib to the
 *          runner's lib/libSDL2-2.0.0.dylib (the game's own SDL) and codesign -s -
 *   run:   ./sdlpad [seconds]   (default 30) */
#include <SDL.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    SDL_GameController *c[8] = {0};
    Sint16 prev[8][SDL_CONTROLLER_AXIS_MAX + SDL_CONTROLLER_BUTTON_MAX];
    memset(prev, 0x7f, sizeof(prev));
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) { printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_version v; SDL_GetVersion(&v); printf("SDL %d.%d.%d\n", v.major, v.minor, v.patch);
    Uint32 end = SDL_GetTicks() + secs * 1000;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERDEVICEADDED && e.cdevice.which < 8 && !c[e.cdevice.which]) {
                int i = e.cdevice.which;
                c[i] = SDL_GameControllerOpen(i);
                SDL_Joystick *j = SDL_GameControllerGetJoystick(c[i]);
                printf("pad %d: %s vid %04x pid %04x\nmapping %s\n", i, SDL_GameControllerName(c[i]),
                       SDL_JoystickGetVendor(j), SDL_JoystickGetProduct(j), SDL_GameControllerMapping(c[i]));
            }
        }
        for (int i = 0; i < 8; i++) {
            if (!c[i]) continue;
            Sint16 cur[SDL_CONTROLLER_AXIS_MAX + SDL_CONTROLLER_BUTTON_MAX];
            for (int a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++) cur[a] = SDL_GameControllerGetAxis(c[i], a);
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
                cur[SDL_CONTROLLER_AXIS_MAX + b] = SDL_GameControllerGetButton(c[i], b);
            if (memcmp(cur, prev[i], sizeof(cur))) {
                printf("%8u pad %d lx %6d ly %6d rx %6d ry %6d lt %5d rt %5d buttons", SDL_GetTicks(), i,
                       cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
                for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++)
                    if (cur[SDL_CONTROLLER_AXIS_MAX + b]) printf(" %s", SDL_GameControllerGetStringForButton(b));
                printf("\n");
                fflush(stdout);
                memcpy(prev[i], cur, sizeof(cur));
            }
        }
        SDL_Delay(10);
    }
    return 0;
}
