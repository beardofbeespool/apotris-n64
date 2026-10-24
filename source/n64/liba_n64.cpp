// N64 platform backend (libdragon): video output, input, saves, stubs.
#include "platform.hpp"

#include "def.h"
#include "multiplayerClasses.h"
#include "matchmakingQueue.hpp"
#include "scene.hpp"

#include "stb_image.h"
#include "stb_image_write.h"

#include <libdragon.h>

#include <cmath>
#include <cstring>
#include <string>

// The picture is drawn by the RDP (ppuRenderFrameRdp). Build with
// -DAPOTRIS_N64_CPU_PPU to draw it on the CPU instead, to compare the two.
extern void ppuRenderFrame(void* dst, int strideBytes);
extern void ppuRenderFrameRdp(int originX, int originY);

LinkUniversal* linkUniversal = nullptr;
MultiplayerLink* multiplayerLink = nullptr;
std::string roomId;
std::string exceptionReason;

Songs songs;

int spriteVOffset = 0;
int blendInfo = 0;

namespace {

// The GBA picture is 240x160; centre it on the 320x240 output for now.
constexpr int OUT_W = 320;
constexpr int OUT_H = 240;
constexpr int ORIGIN_X = (OUT_W - SCREEN_WIDTH) / 2;
constexpr int ORIGIN_Y = (OUT_H - SCREEN_HEIGHT) / 2;

#ifdef APOTRIS_N64_CPU_PPU
// The CPU renderer draws into cached RAM, then the RDP copies the picture to
// the uncached framebuffer. Two buffers, so the CPU never draws into the one
// the RDP is still copying.
alignas(64) u16 ppuBuffers[2][SCREEN_WIDTH * SCREEN_HEIGHT];
int ppuBufferIndex = 0;
#endif

u32 currentKeys = 0;
u32 previousKeys = 0;

u32 readPad() {
    joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
    u32 k = 0;

    if (in.btn.a)
        k |= KEY_A;
    if (in.btn.b)
        k |= KEY_B;
    if (in.btn.z)
        k |= KEY_SELECT;
    if (in.btn.start)
        k |= KEY_START;
    if (in.btn.d_up)
        k |= KEY_DUP;
    if (in.btn.d_down)
        k |= KEY_DDOWN;
    if (in.btn.d_left)
        k |= KEY_DLEFT;
    if (in.btn.d_right)
        k |= KEY_DRIGHT;
    if (in.btn.l)
        k |= KEY_L;
    if (in.btn.r)
        k |= KEY_R;
    if (in.btn.c_up)
        k |= KEY_CSTICK_UP;
    if (in.btn.c_down)
        k |= KEY_CSTICK_DOWN;
    if (in.btn.c_left)
        k |= KEY_CSTICK_LEFT;
    if (in.btn.c_right)
        k |= KEY_CSTICK_RIGHT;

    constexpr int deadzone = 40;
    if (in.stick_x > deadzone)
        k |= KEY_CPAD_RIGHT;
    if (in.stick_x < -deadzone)
        k |= KEY_CPAD_LEFT;
    if (in.stick_y > deadzone)
        k |= KEY_CPAD_UP;
    if (in.stick_y < -deadzone)
        k |= KEY_CPAD_DOWN;

    return k & VALID_KEY_MASK;
}

} // namespace

void platformInit() {
    debug_init_isviewer();
    debug_init_usblog();

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE,
                 FILTERS_RESAMPLE);
    joypad_init();
    rdpq_init();

    linkUniversal = new LinkUniversal();
    multiplayerLink = new MultiplayerLink(linkUniversal);
}

void deinitialize() {}

void quit() {
    deinitialize();
    exit(0);
}

bool closed() { return true; }

// Once a second, logs the frame rate and where the time goes (debugf, shown
// by emulators and USB debug tools). Full speed is 60 frames per second.
static void logFrameStats(uint32_t frameStart, uint32_t drawTicks) {
    static uint32_t secondStart = 0;
    static uint32_t frames = 0, drawTotal = 0, frameMax = 0;

    uint32_t now = get_ticks();
    if (secondStart == 0)
        secondStart = now;

    frames++;
    drawTotal += drawTicks;
    if (now - frameStart > frameMax)
        frameMax = now - frameStart;

    if (now - secondStart >= TICKS_PER_SECOND) {
        debugf("fps %lu | draw avg %lu us | slowest frame %lu us\n",
               (unsigned long)frames,
               (unsigned long)TICKS_TO_US(drawTotal / frames),
               (unsigned long)TICKS_TO_US(frameMax));
        secondStart = now;
        frames = drawTotal = frameMax = 0;
    }
}

void vsync() {
    static uint32_t frameStart = 0;

    onVBlank();

#ifdef APOTRIS_N64_CPU_PPU
    u16* pixels = ppuBuffers[ppuBufferIndex];
    ppuBufferIndex ^= 1;

    uint32_t drawStart = get_ticks();
    ppuRenderFrame(pixels, SCREEN_WIDTH * 2);
    uint32_t drawTicks = get_ticks() - drawStart;
    // the RDP reads RDRAM, not the CPU cache
    data_cache_hit_writeback(pixels, sizeof(ppuBuffers[0]));

    surface_t picture =
        surface_make_linear(pixels, FMT_RGBA16, SCREEN_WIDTH, SCREEN_HEIGHT);

    surface_t* d = display_get();
    rdpq_attach_clear(d, NULL);
    rdpq_set_mode_copy(false);
    rdpq_tex_blit(&picture, ORIGIN_X, ORIGIN_Y, NULL);
    rdpq_detach_show();
#else
    surface_t* d = display_get();

    uint32_t drawStart = get_ticks();
    rdpq_attach_clear(d, NULL);
    ppuRenderFrameRdp(ORIGIN_X, ORIGIN_Y);
    rdpq_detach_show();
    // the RDP reads the game's tile data straight from RAM, so the game
    // must not change it before the picture is finished
    rspq_wait();
    uint32_t drawTicks = get_ticks() - drawStart;
#endif

    if (frameStart != 0)
        logFrameStats(frameStart, drawTicks);
    frameStart = get_ticks();
}

void onVBlank() {
    if (canDraw) {
        canDraw = 0;
        scene->draw();
        MatchmakingQueue::drawOverlay();
    }

    frameCounter++;
}

void stopDMA() {}
void toggleHBlank(bool state) {
    if (state)
        gradientEnabled = true;
}
void toggleRendering(bool) {}
void refreshWindowSize() {}
void initRumble() {}
void rumbleStop() {}

s16 sinLut(int angle) {
    float r = (float)angle * ((3.1459f) / 256);
    int adj = (int)((float)(1 << 12) * sinf(r));
    return (adj & 0xffff);
}

// --- input ---------------------------------------------------------------

void key_poll() {
    joypad_poll();
    previousKeys = currentKeys;
    currentKeys = readPad();
}

uint32_t key_is_down(uint32_t key) { return currentKeys & key; }
uint32_t key_hit(uint32_t key) { return (~previousKeys & currentKeys) & key; }
uint32_t key_released(uint32_t key) {
    return (previousKeys & ~currentKeys) & key;
}
uint32_t keys_raw() { return currentKeys; }
uint32_t key_first() {
    for (u32 bit = 0; bit < 32; bit++) {
        if (currentKeys & (1u << bit))
            return 1u << bit;
    }
    return KEY_FULL - 1;
}

std::map<int, std::string> keyToString = {
    {KEY_A, "A"},           {KEY_B, "B"},
    {KEY_SELECT, "Z"},      {KEY_START, "Start"},
    {KEY_DRIGHT, "d-R"},    {KEY_DLEFT, "d-L"},
    {KEY_DUP, "d-U"},       {KEY_DDOWN, "d-D"},
    {KEY_R, "R"},           {KEY_L, "L"},
    {KEY_CSTICK_RIGHT, "C-R"}, {KEY_CSTICK_LEFT, "C-L"},
    {KEY_CSTICK_UP, "C-U"}, {KEY_CSTICK_DOWN, "C-D"},
    {KEY_CPAD_RIGHT, "a-R"}, {KEY_CPAD_LEFT, "a-L"},
    {KEY_CPAD_UP, "a-U"},   {KEY_CPAD_DOWN, "a-D"},
};

void setUpPresentException(std::string error) {
    printf("%s\n", error.c_str());
}

// --- saving (not persistent yet) ----------------------------------------

void loadSavefile() {
    if (savefile == nullptr)
        savefile = new Save();
}

void saveSavefile() {}

// --- audio (silent for now) ---------------------------------------------

void sfx(int) {}
void sfxRate(int, float) {}
void startSong(int, bool) {}
void stopSong() {}
void pauseSong() {}
void resumeSong() {}
void setMusicVolume(int) {}
void setMusicTempo(int) {}
std::string getSongName(int) { return ""; }

// --- image files (no filesystem yet) ------------------------------------

unsigned char* stbi_load(char const*, int*, int*, int*, int) { return nullptr; }
void stbi_image_free(void*) {}
int stbi_write_png(char const*, int, int, int, const void*, int) { return 0; }
