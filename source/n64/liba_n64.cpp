// N64 platform backend (libdragon): video output, input, saves, stubs.
#include "platform.hpp"

#include "def.h"
#include "multiplayerClasses.h"
#include "matchmakingQueue.hpp"
#include "scene.hpp"

#include "stb_image.h"
#include "stb_image_write.h"

#include <libdragon.h>

#include <algorithm>
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

#ifdef APOTRIS_N64_PPU_COMPARE
// Test build: draws every frame on the CPU as well, and logs any pixel where
// the RDP picture differs. Build with APOTRIS_FLAGS=-DAPOTRIS_N64_PPU_COMPARE.
alignas(16) u16 cpuReference[SCREEN_WIDTH * SCREEN_HEIGHT];

void compareWithCpu(surface_t* framebuffer) {
    static uint32_t frames = 0, badFrames = 0, worst = 0;
    static int worstX = 0, worstY = 0;
    static u16 worstRdp = 0, worstCpu = 0;

    ppuRenderFrame(cpuReference, SCREEN_WIDTH * 2);

    uint32_t mismatches = 0;
    int firstX = 0, firstY = 0;
    u16 firstRdp = 0, firstCpu = 0;
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        const u16* rdp = (const u16*)((u8*)framebuffer->buffer +
                                      (ORIGIN_Y + y) * framebuffer->stride) +
                         ORIGIN_X;
        const u16* cpu = &cpuReference[y * SCREEN_WIDTH];
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            // the alpha bit does not reach the screen
            if ((rdp[x] ^ cpu[x]) & ~1) {
                if (mismatches++ == 0) {
                    firstX = x, firstY = y;
                    firstRdp = rdp[x], firstCpu = cpu[x];
                }
            }
        }
    }

    // once, dump both pictures as hex so they can be turned into images
    static bool dumped = false;
    if (!dumped && mismatches > 20) {
        dumped = true;
        for (int which = 0; which < 2; which++) {
            for (int y = 0; y < SCREEN_HEIGHT; y++) {
                char line[SCREEN_WIDTH * 4 + 16];
                int n = sprintf(line, "DUMP %d %d ", which, y);
                for (int x = 0; x < SCREEN_WIDTH; x++) {
                    u16 v = which ? cpuReference[y * SCREEN_WIDTH + x]
                                  : ((const u16*)((u8*)framebuffer->buffer +
                                                  (ORIGIN_Y + y) *
                                                      framebuffer->stride))[ORIGIN_X + x];
                    n += sprintf(line + n, "%04x", v);
                }
                debugf("%s\n", line);
            }
        }
    }

    frames++;
    if (mismatches && (frames % 60) == 1) {
        // what covers the first wrong pixel
        for (int i = 0; i < 128; i++) {
            const OBJ_ATTR* s = &obj_buffer[i];
            const int rw = s->doubleSize ? s->sx * 2 : s->sx;
            const int rh = s->doubleSize ? s->sy * 2 : s->sy;
            if (s->enabled && firstX >= s->x && firstX < s->x + rw &&
                firstY >= s->y && firstY < s->y + rh)
                debugf("  sprite %d at %d,%d %dx%d affine %d double %d prio %d "
                       "mat %.2f %.2f %.2f %.2f\n",
                       i, s->x, s->y, s->sx, s->sy, s->affine, s->doubleSize,
                       (s->priority >> 4) & 3, s->affine_mat[0],
                       s->affine_mat[1], s->affine_mat[2], s->affine_mat[3]);
        }
    }
    if (mismatches) {
        badFrames++;
        if (mismatches > worst) {
            worst = mismatches;
            worstX = firstX, worstY = firstY;
            worstRdp = firstRdp, worstCpu = firstCpu;
        }
    }
    if (frames == 60) {
        debugf("compare: %lu/60 frames differ, worst %lu px (first at %d,%d: "
               "rdp %04x cpu %04x)\n",
               (unsigned long)badFrames, (unsigned long)worst, worstX, worstY,
               worstRdp, worstCpu);
        frames = badFrames = worst = 0;
    }
}
#endif

#ifdef APOTRIS_N64_CPU_PPU
// The CPU renderer draws into cached RAM, then the RDP copies the picture to
// the uncached framebuffer. Two buffers, so the CPU never draws into the one
// the RDP is still copying.
alignas(64) u16 ppuBuffers[2][SCREEN_WIDTH * SCREEN_HEIGHT];
int ppuBufferIndex = 0;
#endif

// --- low latency input --------------------------------------------------
//
// A frame is shown at the vblank after it is drawn, so input read just after
// a vblank is a whole frame old by the time it is on screen. Instead, the
// controller is read as late as the frame's work allows: the busiest recent
// frame (game logic + drawing + the read itself) plus a safety margin is
// left before the next vblank, and the input is read right before that.
// If a frame still misses its vblank, reading goes back to straight after
// the vblank and creeps later again over the next seconds.

// Kept free before the vblank on top of the busiest recent frame
constexpr int INPUT_MARGIN_US = 2000;

uint32_t frameTicks;          // one video frame
uint32_t busyEstimate;        // busiest recent frame, slowly forgotten
uint32_t inputReadAt;         // when the input being drawn was read
uint32_t inputReadTicks;      // how long that read took
uint32_t missedVblanks;       // frames that were not ready in time, for the log

joypad_inputs_t directInput;
bool directInputValid = false;

// Reads controller 1 right now (libdragon only reads the controllers at
// vblank). Returns false when there is no controller, so the caller can
// fall back to libdragon's copy.
bool readControllerNow(joypad_inputs_t* in) {
    uint8_t input[JOYBUS_BLOCK_SIZE] = {};
    uint8_t output[JOYBUS_BLOCK_SIZE];
    input[0] = 1;       // bytes sent
    input[1] = 4;       // bytes received
    input[2] = 0x01;    // N64 controller read
    memset(&input[3], 0xff, 4);
    input[7] = 0xfe;    // end of commands
    input[JOYBUS_BLOCK_SIZE - 1] = 0x01;
    joybus_exec(input, output);

    // the receive length comes back with error flags when nothing answers
    if (output[1] & 0xc0)
        return false;

    const uint8_t* r = &output[3];
    *in = {};
    in->btn.a = !!(r[0] & 0x80);
    in->btn.b = !!(r[0] & 0x40);
    in->btn.z = !!(r[0] & 0x20);
    in->btn.start = !!(r[0] & 0x10);
    in->btn.d_up = !!(r[0] & 0x08);
    in->btn.d_down = !!(r[0] & 0x04);
    in->btn.d_left = !!(r[0] & 0x02);
    in->btn.d_right = !!(r[0] & 0x01);
    in->btn.l = !!(r[1] & 0x20);
    in->btn.r = !!(r[1] & 0x10);
    in->btn.c_up = !!(r[1] & 0x08);
    in->btn.c_down = !!(r[1] & 0x04);
    in->btn.c_left = !!(r[1] & 0x02);
    in->btn.c_right = !!(r[1] & 0x01);
    in->stick_x = (int8_t)r[2];
    in->stick_y = (int8_t)r[3];
    return true;
}

// Called right after the vblank that starts showing the latest frame
void readInputLate(uint32_t vblank, uint32_t busyTicks) {
    static uint32_t lastVblank = 0;

    if (frameTicks == 0) {
        frameTicks = TICKS_PER_SECOND / display_get_refresh_rate();
        busyEstimate = frameTicks;
    }

    busyEstimate = std::max(busyTicks, busyEstimate - busyEstimate / 128);
    // a frame missed its vblank: play safe until the estimate settles again
    if (lastVblank != 0 && vblank - lastVblank > frameTicks * 3 / 2) {
        busyEstimate = frameTicks;
        missedVblanks++;
    }
    lastVblank = vblank;

    const int32_t delay = (int32_t)frameTicks - (int32_t)busyEstimate -
                          (int32_t)TICKS_FROM_US(INPUT_MARGIN_US);
    if (delay > 0)
        wait_ticks(delay);

    inputReadAt = get_ticks();
    directInputValid = readControllerNow(&directInput);
    inputReadTicks = get_ticks() - inputReadAt;
}

u32 currentKeys = 0;
u32 previousKeys = 0;

u32 readPad() {
    joypad_inputs_t in =
        directInputValid ? directInput : joypad_get_inputs(JOYPAD_PORT_1);
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

    // 240p, as sharp as possible: no VI filtering, so every pixel lands on
    // the CRT as drawn. Two framebuffers, as a third would only queue frames
    // and add a frame of input lag.
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE,
                 FILTERS_DISABLED);
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
// by emulators and USB debug tools). Full speed is 60 frames per second, and
// game logic plus drawing has to fit in a 16.7 ms frame.
static void logFrameStats(uint32_t logicTicks, uint32_t drawTicks,
                          uint32_t rdpWaitTicks, uint32_t latencyTicks) {
    static uint32_t secondStart = 0;
    static uint32_t frames = 0, logicTotal = 0, drawTotal = 0, rdpTotal = 0,
                    busyMax = 0, latencyTotal = 0;

    uint32_t now = get_ticks();
    if (secondStart == 0)
        secondStart = now;

    frames++;
    logicTotal += logicTicks;
    drawTotal += drawTicks;
    rdpTotal += rdpWaitTicks;
    latencyTotal += latencyTicks;
    if (logicTicks + drawTicks > busyMax)
        busyMax = logicTicks + drawTicks;

    if (now - secondStart >= TICKS_PER_SECOND) {
        debugf("fps %lu | logic %lu us | draw %lu us (waiting for RDP %lu us) "
               "| busiest frame %lu us | input to screen %lu us | missed "
               "vblanks %lu\n",
               (unsigned long)frames,
               (unsigned long)TICKS_TO_US(logicTotal / frames),
               (unsigned long)TICKS_TO_US(drawTotal / frames),
               (unsigned long)TICKS_TO_US(rdpTotal / frames),
               (unsigned long)TICKS_TO_US(busyMax),
               (unsigned long)TICKS_TO_US(latencyTotal / frames),
               (unsigned long)missedVblanks);
        secondStart = now;
        frames = logicTotal = drawTotal = rdpTotal = busyMax = latencyTotal = 0;
    }
}

void vsync() {
    // the framebuffer for this frame, taken at the end of the previous vsync()
    static surface_t* framebuffer = nullptr;
    static uint32_t logicStart = 0;

    uint32_t drawStart = get_ticks();
    uint32_t logicTicks = logicStart ? drawStart - logicStart : 0;

    onVBlank();

    if (framebuffer == nullptr)
        framebuffer = display_get();

#ifdef APOTRIS_N64_CPU_PPU
    u16* pixels = ppuBuffers[ppuBufferIndex];
    ppuBufferIndex ^= 1;

    ppuRenderFrame(pixels, SCREEN_WIDTH * 2);
    // the RDP reads RDRAM, not the CPU cache
    data_cache_hit_writeback(pixels, sizeof(ppuBuffers[0]));

    surface_t picture =
        surface_make_linear(pixels, FMT_RGBA16, SCREEN_WIDTH, SCREEN_HEIGHT);

    rdpq_attach_clear(framebuffer, NULL);
    rdpq_set_mode_copy(false);
    rdpq_tex_blit(&picture, ORIGIN_X, ORIGIN_Y, NULL);
    rdpq_detach_show();
#else
    rdpq_attach_clear(framebuffer, NULL);
    ppuRenderFrameRdp(ORIGIN_X, ORIGIN_Y);
    rdpq_detach_show();
#endif
    // the RDP reads the game's tile data straight from RAM, so the game
    // must not change it before the picture is finished
    uint32_t rdpWaitStart = get_ticks();
    rspq_wait();
    uint32_t rdpWaitTicks = get_ticks() - rdpWaitStart;

#ifdef APOTRIS_N64_PPU_COMPARE
    compareWithCpu(framebuffer);
#endif

    uint32_t drawTicks = get_ticks() - drawStart;

    // Wait for the next framebuffer here rather than at the start of the
    // next vsync(): it frees up at the vblank that shows this frame, which
    // is also the moment that tells how long input took to reach the screen.
    framebuffer = display_get();
    const uint32_t vblank = get_ticks();

    logFrameStats(logicTicks, drawTicks, rdpWaitTicks,
                  inputReadAt ? vblank - inputReadAt : 0);
    readInputLate(vblank, logicTicks + drawTicks + inputReadTicks);

    logicStart = get_ticks();
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
