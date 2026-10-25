// N64 platform backend (libdragon): video output, input, saves, stubs.
#include "platform.hpp"

#include "audio_files.h"
#include "def.h"
#include "n64/twoplayer.hpp"
#include "multiplayerClasses.h"
#include "matchmakingQueue.hpp"
#include "scene.hpp"

#include "stb_image.h"
#include "stb_image_write.h"

#include <libdragon.h>

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstring>
#include <string>

// The picture is drawn by the RDP (ppuRenderFrameRdp). Build with
// -DAPOTRIS_N64_CPU_PPU to draw it on the CPU instead, to compare the two.
extern void ppuRenderFrame(void* dst, int strideBytes);
extern void ppuRenderFrameRdp(int originX, int originY);

void audioInit();
void audioUpdate();
void saveUpdate();

// Longest a 1 KB piece of a save takes to write to SRAM, with margin
constexpr int SAVE_CHUNK_US = 1500;
// Longest a mixer_try_play() call takes (about 2 ms in ares), with margin
constexpr int MIXER_CALL_US = 2500;

LinkUniversal* linkUniversal = nullptr;
MultiplayerLink* multiplayerLink = nullptr;
std::string roomId;
std::string exceptionReason;

Songs songs;

int spriteVOffset = 0;
int blendInfo = 0;

namespace {

// Two ways to show the 240x160 GBA picture (Settings > Video > Screen):
//
// - Full: a 240x180 framebuffer, the picture with the backdrop carried on
//   for 10 lines above and below. That is 4:3, so the VI stretches it over
//   the whole TV with square pixels, inside a 5% margin for CRT overscan.
//   The stretch is not a whole number of lines, so the VI smooths it.
// - Sharp: a 320x240 framebuffer with the picture in the middle, pixel for
//   pixel and unfiltered, with black around it.
//
// Two-player draws its own 320x240 layout (n64/twoplayer.cpp) the sharp way.
enum class Screen { SHARP, FULL };

constexpr int OUT_W = 320;
constexpr int OUT_H = 240;
constexpr int ORIGIN_X = (OUT_W - SCREEN_WIDTH) / 2;
constexpr int ORIGIN_Y = (OUT_H - SCREEN_HEIGHT) / 2;

constexpr int FULL_W = SCREEN_WIDTH;
constexpr int FULL_H = SCREEN_WIDTH * 3 / 4;
constexpr int FULL_Y = (FULL_H - SCREEN_HEIGHT) / 2;

Screen screen = Screen::SHARP;

void initDisplay(Screen s) {
    // Two framebuffers, as a third would only queue frames and add a frame
    // of input lag
    if (s == Screen::FULL) {
        const resolution_t full = {.width = FULL_W,
                                   .height = FULL_H,
                                   .interlaced = INTERLACE_OFF,
                                   .aspect_ratio = 4.0f / 3.0f,
                                   .overscan_margin = VI_CRT_MARGIN};
        display_init(full, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    } else {
        display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE,
                     FILTERS_DISABLED);
    }
    screen = s;
}

Screen wantedScreen() {
#if defined(APOTRIS_N64_CPU_PPU) || defined(APOTRIS_N64_PPU_COMPARE)
    // the CPU picture is drawn for the sharp layout
    return Screen::SHARP;
#else
    if (n64TwoPlayerActive())
        return Screen::SHARP;
    // settings.fullscreen is otherwise unused on N64: 0 (the default) fills
    // the TV, 1 is the sharp picture
    if (savefile != nullptr && savefile->settings.fullscreen)
        return Screen::SHARP;
    return Screen::FULL;
#endif
}

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

// Controller state of each player (port 1 and 2). In two-player mode
// n64SwitchPlayer() picks which one the game's key functions see.
constexpr int INPUT_PORTS = 2;
joypad_inputs_t directInput[INPUT_PORTS];
bool directInputValid[INPUT_PORTS] = {};
int inputPort = 0;
bool inputMuted = false;

// Reads the first `ports` controllers right now, in one transaction
// (libdragon only reads the controllers at vblank). A port that does not
// answer is marked invalid, so the caller can fall back to libdragon's copy.
void readControllersNow(int ports) {
    uint8_t input[JOYBUS_BLOCK_SIZE] = {};
    uint8_t output[JOYBUS_BLOCK_SIZE];
    for (int p = 0; p < ports; p++) {
        uint8_t* cmd = &input[p * 7];
        cmd[0] = 1;         // bytes sent
        cmd[1] = 4;         // bytes received
        cmd[2] = 0x01;      // N64 controller read
        memset(&cmd[3], 0xff, 4);
    }
    input[ports * 7] = 0xfe;    // end of commands
    input[JOYBUS_BLOCK_SIZE - 1] = 0x01;
    joybus_exec(input, output);

    for (int p = 0; p < ports; p++) {
        const uint8_t* reply = &output[p * 7];
        // the receive length comes back with error flags when nothing answers
        directInputValid[p] = !(reply[1] & 0xc0);
        if (!directInputValid[p])
            continue;

        const uint8_t* r = &reply[3];
        joypad_inputs_t* in = &directInput[p];
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
    }
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
    const uint32_t until = vblank + (delay > 0 ? delay : 0);

    // a pending save goes to SRAM a piece at a time, when there is time left
    if ((int32_t)(until - get_ticks()) > (int32_t)TICKS_FROM_US(SAVE_CHUNK_US))
        saveUpdate();

    // Keep the audio fed while waiting, but not close to the read: a call
    // that has to decode more music takes up to about 2 ms.
    while ((int32_t)(get_ticks() - until) < 0) {
        if ((int32_t)(until - get_ticks()) > (int32_t)TICKS_FROM_US(MIXER_CALL_US))
            mixer_try_play();
    }

    inputReadAt = get_ticks();
    readControllersNow(n64TwoPlayerActive() ? 2 : 1);
    inputReadTicks = get_ticks() - inputReadAt;
}

u32 currentKeys[INPUT_PORTS] = {};
u32 previousKeys[INPUT_PORTS] = {};

u32 readPad() {
    joypad_inputs_t in = directInputValid[inputPort]
                             ? directInput[inputPort]
                             : joypad_get_inputs((joypad_port_t)inputPort);
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

    // 240p, never interlaced, so there is no flicker on a CRT
    initDisplay(wantedScreen());
    joypad_init();
    rdpq_init();

    dfs_init(DFS_DEFAULT_LOCATION);
    // the SD card holds extra songs; fine if there is none
    debug_init_sdfs("sd:/", -1);
    audioInit();

    linkUniversal = new LinkUniversal();
    multiplayerLink = new MultiplayerLink(linkUniversal);
}

void deinitialize() {}

void quit() {
    deinitialize();
    exit(0);
}

bool closed() { return true; }

// Where a frame's time goes, in ticks
struct FrameTimes {
    uint32_t logic;     // game update, between frames
    uint32_t game;      // the game's drawing calls (scene draw)
    uint32_t build;     // the RDP commands for the picture
    uint32_t rdp;       // waiting for the RDP to finish after that
    uint32_t audio;     // the mixer
};

// The last second's figures, shown on screen with Settings > Video > Show
// FPS (two lines of debug text)
char perfLines[2][48];

constexpr uint8_t PERF_FONT = 1;

// Once a second, logs the frame rate and where the time goes (debugf, shown
// by emulators and USB debug tools). Full speed is 60 frames per second, and
// game logic plus drawing has to fit in a 16.7 ms frame.
static void logFrameStats(const FrameTimes& t, uint32_t latencyTicks) {
    static uint32_t secondStart = 0;
    static uint32_t frames = 0, busyMax = 0, latencyTotal = 0;
    static FrameTimes total = {};

    uint32_t now = get_ticks();
    if (secondStart == 0)
        secondStart = now;

    frames++;
    total.logic += t.logic;
    total.game += t.game;
    total.build += t.build;
    total.rdp += t.rdp;
    total.audio += t.audio;
    latencyTotal += latencyTicks;
    const uint32_t busy = t.logic + t.game + t.build + t.rdp + t.audio;
    if (busy > busyMax)
        busyMax = busy;

    if (now - secondStart >= TICKS_PER_SECOND) {
        auto us = [&](uint32_t sum) {
            return (unsigned long)TICKS_TO_US(sum / frames);
        };
        debugf("fps %lu | logic %lu us | game draw %lu us | build %lu us | "
               "waiting for RDP %lu us | audio %lu us | busiest frame %lu us "
               "| input to screen %lu us | missed vblanks %lu\n",
               (unsigned long)frames, us(total.logic), us(total.game),
               us(total.build), us(total.rdp), us(total.audio),
               (unsigned long)TICKS_TO_US(busyMax), us(latencyTotal),
               (unsigned long)missedVblanks);

        // in tenths of a millisecond
        auto ms10 = [&](uint32_t sum) {
            return (int)(TICKS_TO_US(sum / frames) / 100);
        };
        auto ms10max = (int)(TICKS_TO_US(busyMax) / 100);
        snprintf(perfLines[0], sizeof(perfLines[0]), "%2lu FPS MAX %d.%d",
                 (unsigned long)frames, ms10max / 10, ms10max % 10);
        const int game = ms10(total.logic + total.game);
        const int build = ms10(total.build), rdp = ms10(total.rdp),
                  audio = ms10(total.audio);
        snprintf(perfLines[1], sizeof(perfLines[1]),
                 "G%d.%d B%d.%d R%d.%d A%d.%d", game / 10, game % 10,
                 build / 10, build % 10, rdp / 10, rdp % 10, audio / 10,
                 audio % 10);

        secondStart = now;
        frames = busyMax = latencyTotal = 0;
        total = {};
    }
}

// The figures above in the top-left corner of the framebuffer (rdpq
// attached): G game, B building the picture, R waiting for the RDP, A audio,
// all in milliseconds per frame, and MAX the busiest frame
static void drawPerfOverlay(int w, int h, int x, int y) {
    if (savefile == nullptr || !savefile->settings.showFPS ||
        perfLines[0][0] == 0)
        return;

    static bool fontLoaded = false;
    if (!fontLoaded) {
        rdpq_text_register_font(PERF_FONT,
                                rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));
        fontLoaded = true;
    }

    rdpq_set_scissor(0, 0, w, h);
    rdpq_set_mode_fill(RGBA16(0, 0, 0, 1));
    rdpq_fill_rectangle(x, y, x + 8 * 17 + 4, y + 2 * 10 + 2);
    rdpq_set_mode_standard();
    rdpq_text_print(NULL, PERF_FONT, x + 2, y + 9, perfLines[0]);
    rdpq_text_print(NULL, PERF_FONT, x + 2, y + 19, perfLines[1]);
}

void vsync() {
    // the framebuffer for this frame, taken at the end of the previous vsync()
    static surface_t* framebuffer = nullptr;
    static uint32_t logicStart = 0;

    FrameTimes times = {};
    uint32_t drawStart = get_ticks();
    times.logic = logicStart ? drawStart - logicStart : 0;

    onVBlank();
    times.game = get_ticks() - drawStart;

    // The screen changes between single player and two-player, and with the
    // setting: hand back the framebuffer (blank) and start over
    if (wantedScreen() != screen) {
        if (framebuffer != nullptr) {
            rdpq_attach_clear(framebuffer, NULL);
            rdpq_detach_show();
            rspq_wait();
        }
        display_close();
        initDisplay(wantedScreen());
        framebuffer = nullptr;
    }

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
    if (n64TwoPlayerActive()) {
        n64RenderTwoPlayer();
    } else if (screen == Screen::FULL) {
        ppuSnapshotGradient();
        n64SnapshotBoardCaps();
        ppuPrepareWorld();
        ppuRenderBackdrop(0, 0, FULL_W, FULL_H, FULL_Y);
        const PpuRegion all = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0, FULL_Y};
        ppuRenderRegions(&all, 1, false);
        ppuRenderBoardCaps(0, FULL_Y, FULL_W, FULL_H);
    } else {
        ppuSnapshotGradient();
        n64SnapshotBoardCaps();
        ppuRenderFrameRdp(ORIGIN_X, ORIGIN_Y);
        ppuRenderBoardCaps(ORIGIN_X, ORIGIN_Y, OUT_W, OUT_H);
    }
    // top left, or in two-player centred above the timers
    if (screen == Screen::FULL)
        drawPerfOverlay(FULL_W, FULL_H, 4, 4);
    else if (n64TwoPlayerActive())
        drawPerfOverlay(OUT_W, OUT_H, (OUT_W - 140) / 2, 5);
    else
        drawPerfOverlay(OUT_W, OUT_H, 24, 16);
    rdpq_detach_show();
#endif
    // the RDP reads the game's tile data straight from RAM, so the game
    // must not change it before the picture is finished
    uint32_t rdpWaitStart = get_ticks();
    times.build = rdpWaitStart - drawStart - times.game;
    rspq_wait();
    times.rdp = get_ticks() - rdpWaitStart;

#ifndef APOTRIS_N64_CPU_PPU
    // the board caps' colours for the next frame, from this one
    if (n64TwoPlayerActive()) {
        const int current = ppuCurrentWorld();
        for (int w = 0; w < tng::WORLD_COUNT; w++) {
            ppuSelectWorld(w);
            ppuReadBoardCaps(framebuffer);
        }
        ppuSelectWorld(current);
    } else {
        ppuReadBoardCaps(framebuffer);
    }
#endif

    // Audio after the picture: the mixer runs on the RSP, which also passes
    // the drawing commands to the RDP, so mixing during drawing slows it down
    const uint32_t audioStart = get_ticks();
    audioUpdate();
    times.audio = get_ticks() - audioStart;

#ifdef APOTRIS_N64_PPU_COMPARE
    compareWithCpu(framebuffer);
#endif

    uint32_t drawTicks = get_ticks() - drawStart;

    // Wait for the next framebuffer here rather than at the start of the
    // next vsync(): it frees up at the vblank that shows this frame, which
    // is also the moment that tells how long input took to reach the screen.
    framebuffer = display_get();
    const uint32_t vblank = get_ticks();

    logFrameStats(times, inputReadAt ? vblank - inputReadAt : 0);
    readInputLate(vblank, times.logic + drawTicks + inputReadTicks);

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

void n64SetInputPort(int port) { inputPort = port; }

void n64SetInputMuted(bool muted) { inputMuted = muted; }

void key_poll() {
    joypad_poll();
    previousKeys[inputPort] = currentKeys[inputPort];
    currentKeys[inputPort] = inputMuted ? 0 : readPad();
}

uint32_t key_is_down(uint32_t key) { return currentKeys[inputPort] & key; }
uint32_t key_hit(uint32_t key) {
    return (~previousKeys[inputPort] & currentKeys[inputPort]) & key;
}
uint32_t key_released(uint32_t key) {
    return (previousKeys[inputPort] & ~currentKeys[inputPort]) & key;
}
uint32_t keys_raw() { return currentKeys[inputPort]; }
uint32_t key_first() {
    for (u32 bit = 0; bit < 32; bit++) {
        if (currentKeys[inputPort] & (1u << bit))
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

// --- saving ---------------------------------------------------------------
//
// The save lives in the cartridge's 32 KB SRAM (the ROM header asks the
// flashcart or emulator for it, see N64_ROM_SAVETYPE in Makefile.n64), in two
// slots written alternately, after the first 1 KB (see SAVE_BASE). Each slot
// is a header (counter, size, checksum) followed by the Save, and the header
// is written last: if the power goes while saving, that slot fails its
// checksum and the other one, one save older, is loaded instead.
//
// saveSavefile() only takes a copy; the copy goes to SRAM 1 KB per frame in
// the idle time before the controller is read (saveUpdate()), so saving
// never costs a frame, even an autosave in the middle of a game.

namespace {

struct SaveHeader {
    u32 magic;
    u32 counter;
    u32 size;
    u32 crc;
};

constexpr u32 SAVE_MAGIC = 0x41504E36; // "APN6"
// The first 1 KB of SRAM is left unused: libdragon's flashcart probing (for
// the SD card) writes the old EverDrive key to 0x08040020, which lands in
// the first bytes of SRAM wherever SRAM repeats every 32 KB, as in ares.
constexpr int SAVE_BASE = 0x400;
constexpr int SAVE_SLOT_SIZE = 0x3E00;
constexpr int SAVE_CHUNK = 1024;
static_assert(sizeof(SaveHeader) + sizeof(Save) <= SAVE_SLOT_SIZE,
              "the save no longer fits an SRAM slot");

bool sramPresent = false;
u32 saveCounter = 0;          // counter of the newest slot
int saveSlot = 0;             // slot holding the newest save

alignas(16) u8 saveBuffer[(sizeof(SaveHeader) + sizeof(Save) + 15) & ~15];
int saveWritten = -1;         // bytes of saveBuffer written, -1 when idle
int saveTarget = 0;           // slot being written

u32 crc32(const u8* data, size_t len) {
    u32 crc = 0xffffffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

void sramRead(int slot) {
    sram_read(saveBuffer, SAVE_BASE + slot * SAVE_SLOT_SIZE, sizeof(saveBuffer));
}

} // namespace

void loadSavefile() {
    if (savefile == nullptr)
        savefile = new Save();

    // No sram_detect(): it probes by writing a test pattern, which in ares
    // lands inside the save data and corrupts it. The ROM header asks for
    // SRAM; without it the checksums fail and the game starts fresh.
    sram_init();
    sramPresent = true;

    // the newest slot that checks out
    int best = -1;
    for (int slot = 0; slot < 2; slot++) {
        sramRead(slot);
        const SaveHeader* h = (const SaveHeader*)saveBuffer;
        if (h->magic != SAVE_MAGIC || h->size != sizeof(Save) ||
            h->crc != crc32(saveBuffer + sizeof(SaveHeader), sizeof(Save)))
            continue;
        if (best == -1 || h->counter > saveCounter) {
            best = slot;
            saveCounter = h->counter;
        }
    }

    if (best == -1) {
        debugf("save: none found, starting fresh\n");
        return;
    }
    sramRead(best);
    memcpy(savefile, saveBuffer + sizeof(SaveHeader), sizeof(Save));
    saveSlot = best;
    debugf("save: loaded slot %d (save %lu)\n", best, (unsigned long)saveCounter);
}

void saveSavefile() {
    if (!sramPresent || savefile == nullptr)
        return;

    // a save still being written is simply replaced by the newer one, in the
    // same slot, so the other slot always keeps the last complete save
    if (saveWritten == -1) {
        saveTarget = saveSlot ^ 1;
        saveCounter++;
    }

    SaveHeader* h = (SaveHeader*)saveBuffer;
    memcpy(saveBuffer + sizeof(SaveHeader), savefile, sizeof(Save));
    h->magic = SAVE_MAGIC;
    h->counter = saveCounter;
    h->size = sizeof(Save);
    h->crc = crc32(saveBuffer + sizeof(SaveHeader), sizeof(Save));
    saveWritten = 0;
}

// Writes the next piece of a pending save, the header last
void saveUpdate() {
    if (saveWritten == -1)
        return;

    const int base = SAVE_BASE + saveTarget * SAVE_SLOT_SIZE;
    const int total = sizeof(SaveHeader) + sizeof(Save);
    const int dataStart = sizeof(SaveHeader);

    if (saveWritten < dataStart)
        saveWritten = dataStart;

    if (saveWritten < total) {
        const int len = std::min(SAVE_CHUNK, total - saveWritten);
        sram_write(saveBuffer + saveWritten, base + saveWritten, len);
        saveWritten += len;
        return;
    }

    sram_write(saveBuffer, base, sizeof(SaveHeader));
    saveSlot = saveTarget;
    saveWritten = -1;
    debugf("save: written to slot %d (save %lu)\n", saveSlot,
           (unsigned long)saveCounter);
}

// --- audio ----------------------------------------------------------------
//
// Effects and songs are wav64 files in the ROM filesystem (see Makefile.n64),
// mixed by libdragon on the RSP. Extra songs can be added on the SD card in
// sd:/apotris/music/ (MENU_... for menu music, anything else plays in game);
// one with the same name as a built-in song replaces it.

namespace {

constexpr int AUDIO_FREQUENCY = 32000;
constexpr int MUSIC_CHANNEL = 0;        // stereo: uses channels 0 and 1
constexpr int SFX_FIRST_CHANNEL = 2;
constexpr int SFX_VOICES = 14;

wav64_t* sfxWaves[SFX_COUNT];

// Effects up to this size (the frequent ones: moving, rotating, placing,
// holding, clears, attacks, menus; about 360 KB in all) are kept in RAM.
// Streaming costs a cartridge read for every playing voice every frame,
// which adds up when both players are making noise.
constexpr long SFX_PRELOAD_MAX = 32 * 1024;
bool sfxLoaded[SFX_COUNT];
int nextSfxVoice = 0;

struct SongFile {
    std::string name;   // file name without .wav64
    std::string path;
};
std::vector<SongFile> songFiles;

wav64_t songWave;
bool songOpen = false;
int currentSong = -1;
bool songLooping = false;
bool songPaused = false;
double songPausedAt = 0;
float musicVolume = 1.0f;
float musicSpeed = 1.0f;

void addSongs(const char* dir) {
    dir_t entry;
    if (dir_findfirst(dir, &entry) != 0)
        return;
    do {
        std::string name = entry.d_name;
        const std::string ext = ".wav64";
        if (entry.d_type != DT_REG || name.size() <= ext.size() ||
            name.compare(name.size() - ext.size(), ext.size(), ext) != 0)
            continue;
        name.resize(name.size() - ext.size());

        SongFile song = {name, std::string(dir) + entry.d_name};
        auto it = std::find_if(songFiles.begin(), songFiles.end(),
                               [&](const SongFile& s) { return s.name == name; });
        if (it != songFiles.end())
            *it = song;
        else
            songFiles.push_back(song);
    } while (dir_findnext(dir, &entry) == 0);
}

void applyMusicSettings() {
    if (!songOpen || songPaused)
        return;
    mixer_ch_set_vol(MUSIC_CHANNEL, musicVolume, musicVolume);
    mixer_ch_set_freq(MUSIC_CHANNEL, songWave.wave.frequency * musicSpeed);
}

// The effects are mastered right up to full scale, so two at once on top of
// the music would clip. Half volume leaves room, and sits them a little
// above the music (which is levelled to -21 dB RMS at build time).
constexpr float SFX_GAIN = 0.5f;

float sfxVolume() {
    return SFX_GAIN * (savefile ? savefile->settings.sfxVolume / 10.0f : 1.0f);
}

} // namespace

void audioInit() {
    audio_init(AUDIO_FREQUENCY, AUDIO_INIT_LATENCY_MS(160));
    mixer_init(SFX_FIRST_CHANNEL + SFX_VOICES);
    wav64_init_compression(2);

    for (int i = 0; i < SFX_COUNT; i++) {
        // "assets/name.wav" -> "rom:/sfx/name.wav64"
        std::string path = SoundEffectPaths[i];
        path = "rom:/sfx/" + path.substr(path.rfind('/') + 1);
        path.replace(path.size() - 4, 4, ".wav64");
        FILE* f = fopen(path.c_str(), "rb");
        if (f == nullptr)
            continue;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fclose(f);
        wav64_loadparms_t parms = {};
        parms.streaming_mode = size <= SFX_PRELOAD_MAX ? WAV64_STREAMING_NONE
                                                       : WAV64_STREAMING_FULL;
        sfxWaves[i] = wav64_load(path.c_str(), &parms);
        sfxLoaded[i] = sfxWaves[i] != nullptr;
    }

    addSongs("rom:/music/");
    addSongs("sd:/apotris/music/");
    std::sort(songFiles.begin(), songFiles.end(),
              [](const SongFile& a, const SongFile& b) { return a.name < b.name; });

    for (int i = 0; i < (int)songFiles.size(); i++) {
        if (songFiles[i].name.rfind("MENU_", 0) == 0)
            songs.menu.push_back(i);
        else
            songs.game.push_back(i);
    }
    debugf("audio: %d songs (%d menu, %d game)\n", (int)songFiles.size(),
           (int)songs.menu.size(), (int)songs.game.size());
    heap_stats_t heap;
    sys_get_heap_stats(&heap);
    debugf("memory: %d KB of %d KB free\n", heap.free / 1024,
           heap.total / 1024);
}

// Every frame: keep the audio buffers filled, and move on to the next song
// when one that does not loop has finished.
void audioUpdate() {
    mixer_try_play();

    if (songOpen && !songLooping && !songPaused && currentSong != -1 &&
        musicVolume > 0 && !mixer_ch_playing(MUSIC_CHANNEL) && savefile) {
        if (savefile->settings.cycleSongs == 1)
            playNextSong();
        else if (savefile->settings.cycleSongs == 2)
            playSongRandom(currentMenu);
    }
}

void sfxRate(int n, float rate) {
    if (n < 0 || n >= SFX_COUNT || !sfxLoaded[n])
        return;
    const int ch = SFX_FIRST_CHANNEL + nextSfxVoice;
    nextSfxVoice = (nextSfxVoice + 1) % SFX_VOICES;
    wav64_play(sfxWaves[n], ch);
    mixer_ch_set_vol(ch, sfxVolume(), sfxVolume());
    if (rate != 1.0f)
        mixer_ch_set_freq(ch, sfxWaves[n]->wave.frequency * rate);
}

void sfx(int n) { sfxRate(n, 1.0f); }

void startSong(int song, bool loop) {
    if (song < 0 || song >= (int)songFiles.size())
        return;
    if (song == currentSong && songOpen && !songPaused &&
        mixer_ch_playing(MUSIC_CHANNEL))
        return;

    stopSong();
    wav64_open(&songWave, songFiles[song].path.c_str());
    songOpen = true;
    wav64_set_loop(&songWave, loop);
    wav64_play(&songWave, MUSIC_CHANNEL);

    currentSong = song;
    songLooping = loop;
    songPaused = false;
    applyMusicSettings();
}

void stopSong() {
    if (!songOpen)
        return;
    mixer_ch_stop(MUSIC_CHANNEL);
    wav64_close(&songWave);
    songOpen = false;
    songPaused = false;
    currentSong = -1;
}

void pauseSong() {
    if (!songOpen || songPaused)
        return;
    songPausedAt = mixer_ch_get_pos(MUSIC_CHANNEL);
    mixer_ch_stop(MUSIC_CHANNEL);
    songPaused = true;
}

void resumeSong() {
    if (!songOpen || !songPaused)
        return;
    songPaused = false;
    wav64_play(&songWave, MUSIC_CHANNEL);
    mixer_ch_set_pos(MUSIC_CHANNEL, songPausedAt);
    applyMusicSettings();
}

// 512 is full volume here, as in the other ports' callers
void setMusicVolume(int volume) {
    musicVolume = std::clamp(volume / 512.0f, 0.0f, 1.0f);
    applyMusicSettings();
}

// 1024 is normal speed; like the PC port, slower also means lower
void setMusicTempo(int tempo) {
    musicSpeed = tempo / 1024.0f;
    applyMusicSettings();
}

std::string getSongName(int song) {
    if (song < 0 || song >= (int)songFiles.size())
        return "Unknown";
    return songFiles[song].name;
}

// --- image files (no filesystem yet) ------------------------------------

unsigned char* stbi_load(char const*, int*, int*, int*, int) { return nullptr; }
void stbi_image_free(void*) {}
int stbi_write_png(char const*, int, int, int, const void*, int) { return 0; }
