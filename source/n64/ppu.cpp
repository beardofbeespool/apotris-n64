// GBA-style PPU emulation for the N64 port.
//
// The shared game code drives a GBA-like tile/sprite API. The state tables
// and API functions below follow the 3DS port's "tng" layer (the data is kept
// in GBA-native formats); the N64 specific part draws them in vsync().
#include "platform.hpp"

#include "def.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <algorithm>
#include <type_traits>

#include <libdragon.h>

// Tiling Next Gen
namespace tng {

class LayerInfo {
public:
    int id = -1;
    bool enabled = true;
    int priority = -1;
    int sbb = 0;
    int cbb = 0;
    int scroll_x = 0;
    int scroll_y = 0;
};

static LayerInfo layerInfo[4];

enum BlendMode {
    BLEND_OFF = 0,
    BLEND_STD = 1,
    BLEND_WHITE = 2,
    BLEND_BLACK = 3,
};

std::array<bool, BG_LAYERS_COUNT> blend_top_bg = {};
bool blend_top_sprites = false;
bool blend_top_backdrop = false;
std::array<bool, BG_LAYERS_COUNT> blend_bottom_bg = {};
bool blend_bottom_sprites = false;
bool blend_bottom_backdrop = false;

BlendMode blend_mode = BLEND_OFF;

// Mosaic size - 1
u8 bg_mosaic_x = 0;
u8 bg_mosaic_y = 0;

enum PaletteType {
    PALETTE_BG = 0u,
    PALETTE_SPRITE = 1u,
};

// The palettes now use RGBA5551 format.
static std::array<std::array<std::array<u16, 16>, 16>, 2> palettes = {};

static std::array<std::array<u16, 16>, 16>& palettes_bg = palettes[PALETTE_BG];
static std::array<std::array<u16, 16>, 16>& palettes_sprite =
    palettes[PALETTE_SPRITE];

constexpr u16 color_rgb5a1_unsafe(u8 c_r, u8 c_g, u8 c_b, u8 c_a = 1) {
    return (c_r << 11) | (c_g << 6) | (c_b << 1) | (c_a);
}

constexpr u16 color_gba_to_tng(const u16 colorgb5) {
    const u8 c_r = colorgb5 & 0b0001'1111;
    const u8 c_g = (colorgb5 >> 5) & 0b0001'1111;
    const u8 c_b = (colorgb5 >> 10) & 0b0001'1111;
    const u8 c_a = 0xff;
    return color_rgb5a1_unsafe(c_r, c_g, c_b, c_a & 0x1);
}

constexpr u16 color_tng_to_gba(const u16 rgb5a1) {
    const u8 c_r = (rgb5a1 >> 11) & 0b0001'1111;
    const u8 c_g = (rgb5a1 >> 6) & 0b0001'1111;
    const u8 c_b = (rgb5a1 >> 1) & 0b0001'1111;
    const u8 c_a = rgb5a1 & 0x1;
    return RGB15(c_r, c_g, c_b);
}

constexpr void palette_gba_to_tng(const u16* src, u16* dst, size_t count) {
    for (; count > 0; count--, src++, dst++) {
        *dst = color_gba_to_tng(*src);
    }
}

constexpr void palette_tng_to_gba(const u16* src, u16* dst, size_t count) {
    for (; count > 0; count--, src++, dst++) {
        *dst = color_tng_to_gba(*src);
    }
}

alignas(16) ImageTile sprite_tile_images[SPRITE_TILE_COUNT];

bool sprite_tile_dirty_flag[SPRITE_TILE_COUNT] = {};

// Using the same global sprite attributes
SpriteAttributes* sprite_obj_attrs = obj_buffer;

bool sprite_obj_dirty_flag[SPRITE_OBJ_COUNT] = {};

alignas(16) static u8 bg_storage[BG_TILEMAP_STORAGE_SIZE] = {};

BgScreenblockAll& bg_screenblocks = *(BgScreenblockAll*)&bg_storage;
BgCharblockAll& bg_charblocks = *(BgCharblockAll*)&bg_storage;
static_assert(sizeof(bg_screenblocks) == sizeof(bg_storage));
static_assert(sizeof(bg_charblocks) == sizeof(bg_storage));

std::array<bool, std::extent_v<BgScreenblockAll>> bg_screenblock_fill_screen{};
bool* const bg_screenblock_fill_screen_p = bg_screenblock_fill_screen.data();

} /* namespace tng */

using tng::LayerInfo;
using tng::layerInfo;


// Clip windows used by the renderer (screen coordinates, GBA 240x160 space)
namespace cr {

struct ClipWindow {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool enabled = false;
    bool invert = false;
};

ClipWindow bg_clip_windows[4];
ClipWindow sprites_clip_window;

struct SceneOffset {
    int left, right;
};
constexpr SceneOffset SCENE_OFFSET_CENTERED = {0, 240};

} // namespace cr

void toggleBG(int layer, bool state) {
    layerInfo[layer].enabled = state;
}

void toggleSprites(bool state) {
    // TODO: stub
}

INLINE u8 buildLayerPrio(int layer, int priority) {
    return ((priority & 0x3) << 4) + (layer & 0x3);
}

void buildBG(int layer, int cbb, int sbb, int size, int prio, int mos) {
    int p = buildLayerPrio(layer, prio);

    layerInfo[layer].priority = p;
    layerInfo[layer].sbb = sbb;
    layerInfo[layer].cbb = cbb;

}

void showSprites(int n) {
}


void loadPalette(int palette, int index, const void* src, int count) {
    tng::palette_gba_to_tng((const u16*)src,
                            &tng::palettes[palette / 16][palette % 16][index],
                            count);

}


void loadTiles(int tileset, int index, const void* src, int count) {
    memcpy32_fast(&tng::bg_charblocks[tileset][index], src, count * 8);

}

void loadTilemap(int tilemap, int index, const void* src, int count) {
    u16* dst = tng::bg_screenblocks[tilemap];
    memcpy16_fast(&dst[index], src, count);

    tng::bg_screenblock_fill_screen[tilemap] = false;

}

void clearTilemap(int tilemap) {
    memset16(tng::bg_screenblocks[tilemap], 0, 32 * 32);

    tng::bg_screenblock_fill_screen[tilemap] = false;

}

void sprite_hide(OBJ_ATTR* sprite) {
    if (sprite->enabled) {
        sprite->enabled = false;
    }
};

void sprite_unhide(OBJ_ATTR* sprite, int mode) { sprite->enabled = true; };

void sprite_set_pos(OBJ_ATTR* sprite, int x, int y) {
    sprite->x = x;
    sprite->y = y + spriteVOffset;

    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
};

void sprite_set_attr(OBJ_ATTR* sprite, int shape, int size, int tile_start,
                     int palette, int priority) {
    sprite->sx = sizeTable[shape][size][0];
    sprite->sy = sizeTable[shape][size][1];

    sprite->tile_start = tile_start;
    sprite->palette = palette;
    sprite->priority = buildLayerPrio(0, priority);
    sprite->affine = false;
    sprite->doubleSize = false;
    sprite->flipx = false;
    sprite->flipy = false;
    sprite->rotate = false;
    sprite->scalex = 1;
    sprite->scaley = 1;
    sprite->blend = false;
    sprite->mosaic = false;

    sprite->affine_mat[0] = 1;
    sprite->affine_mat[1] = 0;
    sprite->affine_mat[2] = 0;
    sprite->affine_mat[3] = 1;

    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
};

void setLayerScroll(int layer, int x, int y) {
    layerInfo[layer].scroll_x = x;
    layerInfo[layer].scroll_y = y;

}

void loadSpriteTiles(int index, const void* src, int lengthX, int lengthY) {
    memcpy32_fast(&tng::sprite_tile_images[index], src, lengthX * lengthY * 8);
    for (int i = 0; i < lengthX + lengthY; i++) {
        tng::sprite_tile_dirty_flag[index + i] = true;
    }

}

void loadSpriteTilesPartial(int index, const void* src, int tx, int ty,
                            int lengthX, int lengthY, int rowLength) {
    memcpy32_fast(&tng::sprite_tile_images[index + ty * rowLength + tx], src,
                  lengthX * lengthY * 8);
    for (int i = 0; i < lengthX + lengthY; i++) {
        tng::sprite_tile_dirty_flag[index + ty * rowLength + tx + i] = true;
    }

}

void clearSpriteTiles(int index, int lengthX, int lengthY) {
    memset32_fast(&tng::sprite_tile_images[index], 0, lengthX * lengthY * 8);
    for (int i = 0; i < lengthX + lengthY; i++) {
        tng::sprite_tile_dirty_flag[index + i] = true;
    }

}

void clearSpriteTile(int index, int tx, int ty, int width) {
    memset32_fast(&tng::sprite_tile_images[index + ty * width + tx], 0, 8);
    tng::sprite_tile_dirty_flag[index + ty * width + tx] = true;

}

void enableBlend(int info) {
    tng::blend_mode = (tng::BlendMode)((info >> 6) & 0x03);
    u32 blend_top = info & 0x3f;
    tng::blend_top_bg[0] = blend_top & 0b1;
    tng::blend_top_bg[1] = blend_top & 0b10;
    tng::blend_top_bg[2] = blend_top & 0b100;
    tng::blend_top_bg[3] = blend_top & 0b1000;
    tng::blend_top_sprites = blend_top & 0b10000;
    tng::blend_top_backdrop = blend_top & 0b100000;
    u32 blend_bottom = (info >> 8) & 0x3f;
    tng::blend_bottom_bg[0] = blend_bottom & 0b1;
    tng::blend_bottom_bg[1] = blend_bottom & 0b10;
    tng::blend_bottom_bg[2] = blend_bottom & 0b100;
    tng::blend_bottom_bg[3] = blend_bottom & 0b1000;
    tng::blend_bottom_sprites = blend_bottom & 0b10000;
    tng::blend_bottom_backdrop = blend_bottom & 0b100000;


    blendInfo = info;
}

void setTiles(int tilemap, int index, int count, u32 value) {
    u16* dst = tng::bg_screenblocks[tilemap];
    memset16(&dst[index], value, count);

    if (count == 32 * 32) {
        tng::bg_screenblock_fill_screen[tilemap] = true;
    } else {
        tng::bg_screenblock_fill_screen[tilemap] = false;
    }

}

void clearSprites(int n) {
    for (int counter = 0; counter < n; counter++) {
        sprite_hide(&obj_buffer[counter]);
    }
}

void setPaletteColor(int palette, int index, u16 color, int count) {
    memset16(&tng::palettes[palette / 16][palette % 16][index],
             tng::color_gba_to_tng(color), count);

}

u16 getPaletteColor(int palette, int index) {
    return tng::color_tng_to_gba(
        tng::palettes[palette / 16][palette % 16][index]);
}

void sprite_enable_mosaic(OBJ_ATTR* sprite) {
    sprite->mosaic = true;
    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
}

void sprite_set_size(OBJ_ATTR* sprite, FIXED size, int aff_id) {
    float n = fx2float(size);
    if (size == 0 || n == 0)
        return;

    float scale = 1 / (n);

    sprite->scalex = scale;
    sprite->scaley = scale;

    sprite->affine_mat[0] = scale;
    sprite->affine_mat[1] = 0;
    sprite->affine_mat[2] = 0;
    sprite->affine_mat[3] = scale;

    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
}

void sprite_rotscale(OBJ_ATTR* sprite, FIXED sizex, FIXED sizey, int angle,
                     int aff_id) {
    // sprite->affine = false;
    sprite->flipx = (sizex < 0);
    // sprite->flipy = (sizey < 0);

    int n = angle / 0x4000;

    if ((abs(n)) % 2 == 1) {
        sprite->rotate = true;
    }

    // if(n > 1){
    //     sprite->flipy = !sprite->flipy;
    // }

    // sprite_set_size(sprite, sizex, aff_id);

    float angle_f = (float)angle * (M_PI * 2.f / 0x10000);
    float fcos = std::cosf(angle_f);
    float fsin = std::sinf(angle_f);
    sprite->affine_mat[0] = fcos;
    sprite->affine_mat[1] = fsin;
    sprite->affine_mat[2] = -fsin;
    sprite->affine_mat[3] = fcos;

    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
};

void sprite_enable_affine(OBJ_ATTR* sprite, int affineId, bool doubleSize) {
    sprite->affine = true;
    sprite->doubleSize = doubleSize;

    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
}

void sprite_enable_flip(OBJ_ATTR* sprite, bool flipX, bool flipY) {
    sprite->flipx = flipX;
    sprite->flipy = flipY;

    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
}

extern "C" {

void posprintf(char* buff, const char* str, ...) {

    va_list args;
    va_start(args, str);
    vsnprintf(buff, 64, str, args);
    va_end(args);
}
}

u32 Sqrt(u32 n) { return std::sqrt(n); }

void color_fade_palette(int palette, int index, const COLOR* src, COLOR color,
                        int count, u32 alpha) {
    u16* dst = &tng::palettes[palette / 16][palette % 16][index];
    tng::palette_tng_to_gba(dst, dst, count);
    color_fade(dst, src, color, count, alpha);
    tng::palette_gba_to_tng(dst, dst, count);

}

void color_fade(COLOR* dst, const COLOR* src, COLOR color, int count,
                u32 alpha) {
    if (!count)
        return;

    u32 clra, parta, part;
    const u32 rbmask = (RED_MASK | BLUE_MASK), gmask = GREEN_MASK;
    const u32 rbhalf = 0x4010, ghalf = 0x0200;

    // precalc color B parts
    u32 partb_rb = color & rbmask, partb_g = color & gmask;

    for (int ii = 0; ii < count; ii++) {
        clra = src[ii];

        // Red and blue
        parta = clra & rbmask;
        part = (partb_rb - parta) * alpha + parta * 32 + rbhalf;
        color = (part / 32) & rbmask;

        // Green
        parta = clra & gmask;
        part = (partb_g - parta) * alpha + parta * 32 + ghalf;
        color |= (part / 32) & gmask;

        dst[ii] = color;
    }
}

void color_blend(COLOR* dst, const COLOR* srca, const COLOR* srcb, int nclrs,
                 u32 alpha) {
    if (!nclrs)
        return;

    int ii;
    u32 clra, clrb, clr;
    u32 parta, partb, part;
    const u32 rbmask = (RED_MASK | BLUE_MASK), gmask = GREEN_MASK;
    const u32 rbhalf = 0x4010, ghalf = 0x0200;

    for (ii = 0; ii < nclrs; ii++) {
        clra = srca[ii];
        clrb = srcb[ii];

        // Red and blue
        parta = clra & rbmask;
        partb = clrb & rbmask;
        part = (partb - parta) * alpha + parta * 32 + rbhalf;
        clr = (part / 32) & rbmask;

        // Green
        parta = clra & gmask;
        partb = clrb & gmask;
        part = (partb - parta) * alpha + parta * 32 + ghalf;
        clr |= (part / 32) & gmask;

        dst[ii] = clr;
    }
}

void color_adj_MEM(COLOR* dst, const COLOR* src, u32 count, u32 bright) {
    u32 ii, clr;
    int rr, gg, bb;

    bright >>= 3;
    for (ii = 0; ii < count; ii++) {
        clr = src[ii];

        rr = ((clr) & 31) + bright;
        gg = ((clr >> 5) & 31) + bright;
        bb = ((clr >> 10) & 31) + bright;

        dst[ii] = RGB15(bf_clamp(rr, 5), bf_clamp(gg, 5), bf_clamp(bb, 5));
    }
}

void color_adj_MEM_tng(COLOR* dst, const COLOR* src, u32 count, u32 bright) {
    u32 ii, clr;
    int rr, gg, bb;

    bright >>= 3;
    for (ii = 0; ii < count; ii++) {
        clr = src[ii];

        rr = ((clr) & 31) + bright;
        gg = ((clr >> 5) & 31) + bright;
        bb = ((clr >> 10) & 31) + bright;

        dst[ii] = tng::color_rgb5a1_unsafe(bf_clamp(rr, 5), bf_clamp(gg, 5),
                                           bf_clamp(bb, 5));
    }
}

void color_adj_brightness(int palette, int index, const COLOR* src, u32 count,
                          FIXED bright) {
    color_adj_MEM_tng(&tng::palettes[palette / 16][palette % 16][index], src,
                      count, bright);

}

void clr_grayscale(COLOR* dst, const COLOR* src, u32 nclrs) {
    u32 ii;
    u32 clr, gray, rr, gg, bb;

    for (ii = 0; ii < nclrs; ii++) {
        clr = *src++;

        // Do RGB conversion in .8 fixed point
        rr = ((clr) & 31) * 0x4C;       // 29.7%
        gg = ((clr >> 5) & 31) * 0x96;  // 58.6%
        bb = ((clr >> 10) & 31) * 0x1E; // 11.7%
        gray = (rr + gg + bb + 0x80) >> 8;

        *dst++ = RGB15(gray, gray, gray);
    }
}

void clr_rgbscale(COLOR* dst, const COLOR* src, uint nclrs, COLOR clr) {
    uint ii;
    u32 rr, gg, bb, scale, gray;

    // Normalize target color
    rr = (clr) & 31;
    gg = (clr >> 5) & 31;
    bb = (clr >> 10) & 31;

    scale = max(max(rr, gg), bb);
    if (scale == 0) // Can't scale to black : use gray instead
    {
        clr_grayscale(dst, src, nclrs);
        return;
    }

    scale = ((1 << 16) / scale) & 0xffff;

    rr *= scale; // rr, gg, bb -> 0.16f
    gg *= scale;
    bb *= scale;

    for (ii = 0; ii < nclrs; ii++) {
        clr = *src++;

        // Grayscaly in 5.8f
        gray = ((clr) & 31) * 0x4C;        // 29.7%
        gray += ((clr >> 5) & 31) * 0x96;  // 58.6%
        gray += ((clr >> 10) & 31) * 0x1E; // 11.7%

        // Match onto color vector
        dst[ii] = RGB15(rr * gray >> 24, gg * gray >> 24, bb * gray >> 24);
    }
}

void clearTiles(int tileset, int index, int count) {
    memset32_fast(&tng::bg_charblocks[tileset][index], 0, 8 * count);

}

void clearTilemapEntries(int tilemap, int index, int count) {
    u16* dst = tng::bg_screenblocks[tilemap];
    memset16(&dst[index], 0, count);

    tng::bg_screenblock_fill_screen[tilemap] = false;

}

void setMosaic(int sx, int sy) {
    tng::bg_mosaic_x = sx;
    tng::bg_mosaic_y = sy;

}

COLOR rgb8to5(u32* c) {
    int b = (*c >> (16 + 3)) & 0x1f;
    int g = (*c >> (8 + 3)) & 0x1f;
    int r = (*c >> (0 + 3)) & 0x1f;

    return r | (g << 5) | (b << 10);
}

void savePalette(COLOR* dst) {
    tng::palette_tng_to_gba((u16*)tng::palettes.data(), dst, 512);

}

void color_fade_fast(int palette, int index, const COLOR* src, COLOR color,
                     int count, u32 alpha) {
    u16* dst = &tng::palettes[palette / 16][palette % 16][index];
    tng::palette_tng_to_gba(dst, dst, count);
    color_fade(dst, src, color, count, alpha);
    tng::palette_gba_to_tng(dst, dst, count);

}

void mirrorPalettes(int index, int count) {
    for (int i = index; i < 8 * 16; i++) {
        tng::palettes_bg[i / 16][i % 16] = tng::palettes_sprite[i / 16][i % 16];
    }

}

void sprite_enable_blend(OBJ_ATTR* sprite) {
    sprite->blend = true;

    int index = sprite - obj_buffer;
    assert(index >= 0 && index < 128);
    tng::sprite_obj_dirty_flag[index] = true;
}

void addColorToPalette(int palette, int index, COLOR color, int count) {
    for (int i = 0; i < count; i++) {
        u16 c = tng::palettes[palette / 16][palette % 16][index + i];
        c = tng::color_tng_to_gba(c);
        c += color;
        c = tng::color_gba_to_tng(c);
        tng::palettes[palette / 16][palette % 16][index + i] = c;
    }

}

void enableLayerWindow(int layer, int x1, int y1, int x2, int y2, bool invert) {
    auto& window = cr::bg_clip_windows[layer];
    window.left = x1;
    window.top = y1;
    window.right = x2;
    window.bottom = y2;
    window.enabled = true;
    window.invert = invert;

}

void disableLayerWindow(int layer) {
    cr::bg_clip_windows[layer].enabled = false;

}

void setSpriteMaskRegion(int shift) {
    auto& window = cr::sprites_clip_window;
    window.left = cr::SCENE_OFFSET_CENTERED.left;
    window.top = shift;
    window.right = cr::SCENE_OFFSET_CENTERED.right;
    window.bottom = shift + 164;
    window.enabled = true;
    window.invert = false;

}

// ---------------------------------------------------------------------------
// Scanline compositor
//
// Draws the GBA-style layers into a 240x160 RGBA5551 region. Layers are
// composited back to front per scanline in GBA priority order: for each
// priority 3..0, BGs (high index first) and then sprites (high OAM index
// first). Palette index 0 is transparent. Blending and mosaic are not
// implemented yet.
// ---------------------------------------------------------------------------

namespace {

constexpr int PPU_W = 240;
constexpr int PPU_H = 160;

struct SpriteDraw {
    const OBJ_ATTR* s;
};

u8 spriteList[4][128];
int spriteCount[4];

inline bool windowAllows(const cr::ClipWindow& w, int x, int y) {
    if (!w.enabled)
        return true;
    const bool inside =
        x >= w.left && x < w.right && y >= w.top && y < w.bottom;
    return w.invert ? !inside : inside;
}

void buildSpriteLists() {
    for (int p = 0; p < 4; p++)
        spriteCount[p] = 0;

    // Higher OAM index first so lower indices are drawn on top.
    for (int i = 127; i >= 0; i--) {
        const OBJ_ATTR* s = &obj_buffer[i];
        if (!s->enabled || s->sx <= 0 || s->sy <= 0)
            continue;
        int p = (s->priority >> 4) & 3;
        spriteList[p][spriteCount[p]++] = i;
    }
}

void drawBgLine(u16* line, int layer, int y) {
    const tng::LayerInfo& li = layerInfo[layer];
    const cr::ClipWindow& win = cr::bg_clip_windows[layer];
    const u16* map = tng::bg_screenblocks[li.sbb];
    const tng::ImageTile* tiles = &tng::bg_charblocks[li.cbb][0];
    const auto& pals = tng::palettes_bg;

    const int sy = (y + li.scroll_y) & 255;
    const int ty = sy >> 3;
    const int py = sy & 7;
    const int sx0 = li.scroll_x & 255;
    const int xo = sx0 & 7;
    const int col0 = sx0 >> 3;

    for (int i = 0; i < 31; i++) {
        const u16 e = map[ty * 32 + ((col0 + i) & 31)];
        const tng::ImageTile& tile = tiles[e & 0x3ff];
        const u32 row = tile.rows[(e & 0x800) ? 7 - py : py];
        if (!row)
            continue;

        const u16* pal = pals[e >> 12].data();
        const bool flipx = e & 0x400;
        const int dx = i * 8 - xo;

        for (int k = 0; k < 8; k++) {
            const int x = dx + k;
            if (x < 0 || x >= PPU_W)
                continue;
            const u32 c = (row >> ((flipx ? 7 - k : k) * 4)) & 0xf;
            if (!c)
                continue;
            if (win.enabled && !windowAllows(win, x, y))
                continue;
            line[x] = pal[c];
        }
    }
}

void drawSpriteLine(u16* line, const OBJ_ATTR* s, int y) {
    const u16* pal = tng::palettes_sprite[s->palette & 15].data();
    const int w = s->sx, h = s->sy;
    const int wt = w >> 3;
    const cr::ClipWindow& win = cr::sprites_clip_window;

    if (!s->affine) {
        const int dy = y - s->y;
        if (dy < 0 || dy >= h)
            return;
        const int ty = s->flipy ? h - 1 - dy : dy;

        for (int dx = 0; dx < w; dx++) {
            const int x = s->x + dx;
            if (x < 0 || x >= PPU_W)
                continue;
            const int tx = s->flipx ? w - 1 - dx : dx;
            const tng::ImageTile& t =
                tng::sprite_tile_images[(s->tile_start + (ty >> 3) * wt +
                                         (tx >> 3)) &
                                        (tng::SPRITE_TILE_COUNT - 1)];
            const u32 c = (t.rows[ty & 7] >> ((tx & 7) * 4)) & 0xf;
            if (!c)
                continue;
            if (win.enabled && !windowAllows(win, x, y))
                continue;
            line[x] = pal[c];
        }
        return;
    }

    // Affine: affine_mat maps sprite space to screen space around the
    // centre of the (optionally double sized) render box.
    const int rw = s->doubleSize ? w * 2 : w;
    const int rh = s->doubleSize ? h * 2 : h;
    const int dy = y - s->y;
    if (dy < 0 || dy >= rh)
        return;

    const float a = s->affine_mat[0], b = s->affine_mat[1];
    const float c = s->affine_mat[2], d = s->affine_mat[3];
    const float det = a * d - b * c;
    if (det == 0.0f)
        return;
    const float ia = d / det, ib = -b / det, ic = -c / det, id = a / det;

    const float cy = rh * 0.5f, cx = rw * 0.5f;
    for (int dx = 0; dx < rw; dx++) {
        const int x = s->x + dx;
        if (x < 0 || x >= PPU_W)
            continue;
        const float px = dx + 0.5f - cx, py = dy + 0.5f - cy;
        int tx = (int)floorf(ia * px + ib * py + w * 0.5f);
        int ty = (int)floorf(ic * px + id * py + h * 0.5f);
        if (tx < 0 || tx >= w || ty < 0 || ty >= h)
            continue;
        if (s->flipx)
            tx = w - 1 - tx;
        if (s->flipy)
            ty = h - 1 - ty;
        const tng::ImageTile& t =
            tng::sprite_tile_images[(s->tile_start + (ty >> 3) * wt +
                                     (tx >> 3)) &
                                    (tng::SPRITE_TILE_COUNT - 1)];
        const u32 col = (t.rows[ty & 7] >> ((tx & 7) * 4)) & 0xf;
        if (!col)
            continue;
        if (win.enabled && !windowAllows(win, x, y))
            continue;
        line[x] = pal[col];
    }
}

} // namespace

// dst points at the top-left pixel of the 240x160 region, stride in bytes.
void ppuRenderFrame(void* dst, int strideBytes) {
    buildSpriteLists();

    for (int y = 0; y < PPU_H; y++) {
        u16* line = (u16*)((u8*)dst + y * strideBytes);

        const u16 backdrop =
            gradientEnabled ? tng::color_gba_to_tng(gradientTable[y])
                            : tng::palettes_bg[0][0];
        for (int x = 0; x < PPU_W; x++)
            line[x] = backdrop;

        for (int p = 3; p >= 0; p--) {
            for (int l = 3; l >= 0; l--) {
                const tng::LayerInfo& li = layerInfo[l];
                if (li.enabled && li.priority >= 0 &&
                    ((li.priority >> 4) & 3) == p)
                    drawBgLine(line, l, y);
            }
            for (int i = 0; i < spriteCount[p]; i++)
                drawSpriteLine(line, &obj_buffer[spriteList[p][i]], y);
        }
    }
}

// ---------------------------------------------------------------------------
// RDP renderer
//
// Draws the same picture as ppuRenderFrame(), in the same back to front
// order, but on the RDP: every 8x8 tile becomes a textured rectangle using
// the tile data straight from RAM (CI4 with the GBA palettes as TLUT), and
// affine sprites become two textured triangles. Palette index 0 has alpha 0
// and is dropped by the alpha compare, as on the GBA.
//
// Call with rdpq attached to the framebuffer. The tile data must not change
// until the RDP is done, so the caller waits for it before running the game.
// ---------------------------------------------------------------------------

namespace {

constexpr rdpq_tile_t TILE_DRAW = TILE0;
constexpr rdpq_tile_t TILE_LOAD = TILE6;

alignas(16) u16 tlutBg[256];
alignas(16) u16 tlutSprite[256];

// Linear copies of affine sprites, one per sprite as the RDP reads them later
constexpr int AFFINE_MAX_BYTES = 64 * 64 / 2;
alignas(16) u8 affineBuffers[tng::SPRITE_OBJ_COUNT][AFFINE_MAX_BYTES];

struct Region {
    int x0, y0, x1, y1;
};

int originX, originY;

// What the RDP state already holds, so commands are only sent on a change.
// Every rdpq command costs the CPU a couple of microseconds, which adds up
// over the hundreds of tiles in a frame.
const u8* imageBase;          // texture image the tiles are loaded from
const void* lastLoaded;       // tile currently in TMEM
int drawPalette;              // palette set on TILE_DRAW, -1 if unknown

void resetRdpState() {
    imageBase = nullptr;
    lastLoaded = nullptr;
    drawPalette = -1;
}

inline bool tileEmpty(const tng::ImageTile& t) {
    return !(t.rows[0] | t.rows[1] | t.rows[2] | t.rows[3] | t.rows[4] |
             t.rows[5] | t.rows[6] | t.rows[7]);
}

// Whether each tile is empty, worked out once per frame: a layer uses the
// same few tiles hundreds of times, and every check of a tile that is not
// in the cache costs a slow trip to RAM. 0 = not checked yet, 1 = empty,
// 2 = has pixels. BG tile numbers can reach past the last charblock.
u8 bgTileState[4 * 512 + 1024];
u8 spriteTileState[tng::SPRITE_TILE_COUNT];

inline bool tileEmptyCached(u8& state, const tng::ImageTile& t) {
    if (state == 0)
        state = tileEmpty(t) ? 1 : 2;
    return state == 1;
}

void buildTlut(u16* dst, const std::array<std::array<u16, 16>, 16>& pals) {
    for (int p = 0; p < 16; p++) {
        for (int c = 0; c < 16; c++)
            dst[p * 16 + c] = pals[p][c];
        // index 0 of every palette is transparent
        dst[p * 16] &= ~1;
    }
}

Region scissor;

void setScissor(Region r) {
    if (r.x0 == scissor.x0 && r.y0 == scissor.y0 && r.x1 == scissor.x1 &&
        r.y1 == scissor.y1)
        return;
    scissor = r;
    rdpq_set_scissor(originX + r.x0, originY + r.y0, originX + r.x1,
                     originY + r.y1);
}

// Runs draw() once per rectangle the window lets through: the window itself,
// or the up to four strips around it when inverted.
template <typename F> void forEachWindowRect(const cr::ClipWindow& w, F draw) {
    const Region screen = {0, 0, PPU_W, PPU_H};

    if (!w.enabled) {
        setScissor(screen);
        draw();
        return;
    }

    const Region inside = {std::max(w.left, 0), std::max(w.top, 0),
                           std::min(w.right, PPU_W), std::min(w.bottom, PPU_H)};

    if (!w.invert) {
        if (inside.x0 < inside.x1 && inside.y0 < inside.y1) {
            setScissor(inside);
            draw();
        }
        return;
    }

    const Region strips[4] = {
        {0, 0, PPU_W, inside.y0},
        {0, inside.y1, PPU_W, PPU_H},
        {0, inside.y0, inside.x0, inside.y1},
        {inside.x1, inside.y0, PPU_W, inside.y1},
    };
    for (const Region& r : strips) {
        if (r.x0 < r.x1 && r.y0 < r.y1) {
            setScissor(r);
            draw();
        }
    }
}

// A block of w x h copies of one 8x8 CI4 tile, top left at (x, y) in GBA
// screen space. The tile descriptor wraps every 8 texels, so a single
// rectangle repeats the tile across the whole block.
void drawTileBlock(const tng::ImageTile* tile, int palette, bool flipx,
                   bool flipy, int x, int y, int w, int h) {
    if (tile != lastLoaded) {
        // Tiles are loaded as an image 4 bytes (8 texels) wide with one tile
        // per 8 rows, so one image covers about 128 tiles (load coordinates
        // stay below 1024) and a load only needs the row. TMEM lines are 8
        // bytes, which leaves room for the padding.
        const u8* p = (const u8*)tile;
        if (imageBase == nullptr || p < imageBase || p + 32 > imageBase + 4064) {
            imageBase = (const u8*)((uintptr_t)p & ~(uintptr_t)7);
            rdpq_set_texture_image_raw(0, PhysicalAddr(imageBase), FMT_CI8, 4,
                                       1024);
        }
        const int row = (p - imageBase) / 4;
        rdpq_load_tile(TILE_LOAD, 0, row, 4, row + 8);
        lastLoaded = tile;
    }

    if (palette != drawPalette) {
        rdpq_tileparms_t parms = {};
        parms.palette = (uint8_t)palette;
        parms.s.mask = parms.t.mask = 3;
        rdpq_set_tile(TILE_DRAW, FMT_CI4, 0, 8, &parms);
        drawPalette = palette;
    }

    // Tile rows are u32 values with the left pixel in the low nibble, so in
    // big-endian memory the RDP sees each row right to left. An unflipped
    // tile is therefore read with s counting down, a flipped one counting up;
    // half a texel keeps the point sampling inside the right texel.
    const int pw = w * 8, ph = h * 8;
    const float s0 = flipx ? 0.0f : pw - 0.5f;
    const float t0 = flipy ? ph - 0.5f : 0.0f;
    rdpq_texture_rectangle_scaled(TILE_DRAW, originX + x, originY + y,
                                  originX + x + pw, originY + y + ph, s0, t0,
                                  flipx ? s0 + pw : s0 - pw,
                                  flipy ? t0 - ph : t0 + ph);
}

inline void drawTile(const tng::ImageTile* tile, int palette, bool flipx,
                     bool flipy, int x, int y) {
    drawTileBlock(tile, palette, flipx, flipy, x, y, 1, 1);
}

// Blocks of identical tiles in a layer, packed as sort keys
u32 layerBlocks[31 * 21];

void drawBgLayer(int layer) {
    const tng::LayerInfo& li = layerInfo[layer];
    const u16* map = tng::bg_screenblocks[li.sbb];
    const tng::ImageTile* tiles = &tng::bg_charblocks[li.cbb][0];
    u8* tileState = &bgTileState[li.cbb * 512];

    const int sx = li.scroll_x & 255, sy = li.scroll_y & 255;

    // Runs of the same map entry along each row become one block. The tiles
    // of a layer never overlap, so blocks can be drawn in any order: sorted
    // by tile and palette, each tile is loaded once and the palette is set
    // once per run, and equal runs on consecutive rows end up next to each
    // other, ready to be merged into taller blocks.
    int count = 0;
    for (int row = 0; row < 21; row++) {
        const int ty = ((sy >> 3) + row) & 31;
        const u16* line = &map[ty * 32];
        int col = 0;
        while (col < 31) {
            const u16 e = line[((sx >> 3) + col) & 31];
            int len = 1;
            while (col + len < 31 && line[((sx >> 3) + col + len) & 31] == e)
                len++;
            if (!tileEmptyCached(tileState[e & 0x3ff], tiles[e & 0x3ff])) {
                // map entry (tile, flips, palette) 16 bits | column 5 |
                // length - 1 5 | row 5... packed into 31 bits
                layerBlocks[count++] = ((u32)e << 15) | ((u32)col << 10) |
                                       ((u32)(len - 1) << 5) | (u32)row;
            }
            col += len;
        }
    }
    if (count == 0)
        return;
    std::sort(layerBlocks, layerBlocks + count);

    forEachWindowRect(cr::bg_clip_windows[layer], [&] {
        for (int i = 0; i < count;) {
            const u32 key = layerBlocks[i];
            // the same run on the rows below
            int rows = 1;
            while (i + rows < count && layerBlocks[i + rows] == key + rows)
                rows++;
            i += rows;

            const u16 e = key >> 15;
            const int col = (key >> 10) & 31, len = ((key >> 5) & 31) + 1;
            const int row = key & 31;
            drawTileBlock(&tiles[e & 0x3ff], e >> 12, e & 0x400, e & 0x800,
                          col * 8 - (sx & 7), row * 8 - (sy & 7), len, rows);
        }
    });
}

// A sprite's tiles are consecutive in memory, so they are loaded together,
// up to 32 at a time (64 bytes of TMEM each with the line padding, next to
// the palettes), and each tile is then picked by its row in TMEM: one
// command per tile instead of a load and a draw.
constexpr rdpq_tile_t TILE_SPRITE = TILE1;
constexpr int SPRITE_TILES_PER_LOAD = 32;

void drawSpriteTiles(const OBJ_ATTR* s, int first, int wt, int ht) {
    const int rowsPerLoad = std::max(1, SPRITE_TILES_PER_LOAD / wt);

    for (int srcY0 = 0; srcY0 < ht; srcY0 += rowsPerLoad) {
        const int srcY1 = std::min(ht, srcY0 + rowsPerLoad);

        // skip loads where every tile is empty or off screen
        bool any = false;
        for (int srcY = srcY0; srcY < srcY1 && !any; srcY++) {
            const int cy = s->flipy ? ht - 1 - srcY : srcY;
            const int y = s->y + cy * 8;
            if (y >= PPU_H || y + 8 <= 0)
                continue;
            for (int srcX = 0; srcX < wt; srcX++) {
                const int index = first + srcY * wt + srcX;
                if (!tileEmptyCached(spriteTileState[index],
                                     tng::sprite_tile_images[index])) {
                    any = true;
                    break;
                }
            }
        }
        if (!any)
            continue;

        const int count = (srcY1 - srcY0) * wt;
        const u8* p = (const u8*)&tng::sprite_tile_images[first + srcY0 * wt];
        if (imageBase == nullptr || p < imageBase ||
            p + count * 32 > imageBase + 4064) {
            imageBase = p;
            rdpq_set_texture_image_raw(0, PhysicalAddr(imageBase), FMT_CI8, 4,
                                       1024);
        }
        const int row = (p - imageBase) / 4;
        rdpq_load_tile(TILE_LOAD, 0, row, 4, row + count * 8);
        lastLoaded = nullptr;

        rdpq_tileparms_t parms = {};
        parms.palette = (uint8_t)(s->palette & 15);
        rdpq_set_tile(TILE_SPRITE, FMT_CI4, 0, 8, &parms);
        rdpq_set_tile_size(TILE_SPRITE, 0, 0, 8, count * 8);

        for (int srcY = srcY0; srcY < srcY1; srcY++) {
            const int cy = s->flipy ? ht - 1 - srcY : srcY;
            const int y = s->y + cy * 8;
            if (y >= PPU_H || y + 8 <= 0)
                continue;
            for (int srcX = 0; srcX < wt; srcX++) {
                const int index = first + srcY * wt + srcX;
                if (spriteTileState[index] == 1)
                    continue;
                const int cx = s->flipx ? wt - 1 - srcX : srcX;
                const int x = s->x + cx * 8;
                if (x >= PPU_W || x + 8 <= 0)
                    continue;
                if (tileEmptyCached(spriteTileState[index],
                                    tng::sprite_tile_images[index]))
                    continue;

                // rows read backwards unless flipped, see drawTileBlock()
                const float t = ((srcY - srcY0) * wt + srcX) * 8;
                const float s0 = s->flipx ? 0.0f : 7.5f;
                const float t0 = s->flipy ? t + 7.5f : t;
                rdpq_texture_rectangle_scaled(
                    TILE_SPRITE, originX + x, originY + y, originX + x + 8,
                    originY + y + 8, s0, t0, s->flipx ? 8.0f : -0.5f,
                    s->flipy ? t - 0.5f : t + 8.0f);
            }
        }
    }
}

void drawSprite(const OBJ_ATTR* s) {
    const int w = s->sx, h = s->sy;
    const int wt = w >> 3, ht = h >> 3;

    if (!s->affine) {
        if (s->x >= PPU_W || s->x + w <= 0 || s->y >= PPU_H || s->y + h <= 0)
            return;
        const int first = s->tile_start & (tng::SPRITE_TILE_COUNT - 1);
        if (first + wt * ht <= (int)tng::SPRITE_TILE_COUNT) {
            drawSpriteTiles(s, first, wt, ht);
            return;
        }
        // tile numbers wrap past the end of sprite memory: one tile at a time
        for (int cy = 0; cy < ht; cy++) {
            const int srcY = s->flipy ? ht - 1 - cy : cy;
            for (int cx = 0; cx < wt; cx++) {
                const int srcX = s->flipx ? wt - 1 - cx : cx;
                const int index = (s->tile_start + srcY * wt + srcX) &
                                  (tng::SPRITE_TILE_COUNT - 1);
                const tng::ImageTile* tile = &tng::sprite_tile_images[index];
                if (tileEmptyCached(spriteTileState[index], *tile))
                    continue;
                drawTile(tile, s->palette & 15, s->flipx, s->flipy,
                         s->x + cx * 8, s->y + cy * 8);
            }
        }
        return;
    }

    const int rw = s->doubleSize ? w * 2 : w;
    const int rh = s->doubleSize ? h * 2 : h;
    if (s->x >= PPU_W || s->x + rw <= 0 || s->y >= PPU_H || s->y + rh <= 0)
        return;
    if (w > 64 || h > 64)
        return;

    {
        // Not scaled or rotated (at most flipped): the same pixels as a plain
        // sprite centred in the render box, drawn the fast way.
        const float* m = s->affine_mat;
        if (m[1] == 0.0f && m[2] == 0.0f && fabsf(m[0]) == 1.0f &&
            fabsf(m[3]) == 1.0f) {
            OBJ_ATTR plain = *s;
            plain.affine = false;
            plain.x += (rw - w) / 2;
            plain.y += (rh - h) / 2;
            plain.flipx = s->flipx != (m[0] < 0);
            plain.flipy = s->flipy != (m[3] < 0);
            forEachWindowRect(cr::sprites_clip_window, [&] { drawSprite(&plain); });
            return;
        }
    }

    // the GBA only draws inside the render box
    const cr::ClipWindow& win = cr::sprites_clip_window;
    Region box = {std::max(s->x, 0), std::max(s->y, 0), std::min(s->x + rw, PPU_W),
                  std::min(s->y + rh, PPU_H)};
    if (win.enabled && !win.invert) {
        box.x0 = std::max(box.x0, win.left);
        box.y0 = std::max(box.y0, win.top);
        box.x1 = std::min(box.x1, win.right);
        box.y1 = std::min(box.y1, win.bottom);
    }
    if (box.x0 >= box.x1 || box.y0 >= box.y1)
        return;

    // Copy the sprite into a linear CI4 image. TMEM lines are 8 bytes, so
    // narrow sprites are padded.
    const int pitch = std::max(w / 2, 8);
    u8* image = affineBuffers[s - obj_buffer];
    for (int y = 0; y < h; y++) {
        for (int cx = 0; cx < wt; cx++) {
            const tng::ImageTile& t =
                tng::sprite_tile_images[(s->tile_start + (y >> 3) * wt + cx) &
                                        (tng::SPRITE_TILE_COUNT - 1)];
            // GBA rows keep the left pixel in the lowest nibble, CI4 wants it
            // in the highest: reverse the order of the 8 nibbles
            u32 row = t.rows[y & 7];
            row = ((row >> 4) & 0x0f0f0f0f) | ((row & 0x0f0f0f0f) << 4);
            *(u32*)(image + y * pitch + cx * 4) = __builtin_bswap32(row);
        }
    }
    data_cache_hit_writeback(image, pitch * h);

    rdpq_set_texture_image_raw(0, PhysicalAddr(image), FMT_CI8, pitch, h);
    rdpq_set_tile(TILE_LOAD, FMT_CI8, 0, pitch, NULL);
    rdpq_load_tile(TILE_LOAD, 0, 0, pitch, h);
    rdpq_set_tile(TILE_LOAD, FMT_CI8, 0, 8, NULL);
    resetRdpState();

    rdpq_tileparms_t parms = {};
    parms.palette = (uint8_t)(s->palette & 15);
    parms.s.clamp = parms.t.clamp = true;
    rdpq_set_tile(TILE_DRAW, FMT_CI4, 0, pitch, &parms);
    rdpq_set_tile_size(TILE_DRAW, 0, 0, w, h);

    // Same mapping as the CPU renderer: affine_mat takes a texel's offset
    // from the sprite centre to its offset from the render box centre.
    const float a = s->affine_mat[0], b = s->affine_mat[1];
    const float c = s->affine_mat[2], d = s->affine_mat[3];
    const float cx = originX + s->x + rw * 0.5f;
    const float cy = originY + s->y + rh * 0.5f;

    setScissor(box);

    if (b == 0.0f && c == 0.0f && a != 0.0f && d != 0.0f) {
        // Scaled but not rotated: a scaled texture rectangle covering exactly
        // the pixels whose centre maps inside the sprite, as on the GBA.
        const float ia = 1.0f / a, id = 1.0f / d;
        const float hw = w * 0.5f * fabsf(a), hh = h * 0.5f * fabsf(d);
        const float centerX = s->x + rw * 0.5f, centerY = s->y + rh * 0.5f;
        const int x0 = (int)ceilf(centerX - hw - 0.5f);
        const int x1 = (int)ceilf(centerX + hw - 0.5f);
        const int y0 = (int)ceilf(centerY - hh - 0.5f);
        const int y1 = (int)ceilf(centerY + hh - 0.5f);
        if (x0 >= x1 || y0 >= y1)
            return;

        // texel at the centre of the first pixel, then one step per pixel
        float s0 = ia * (x0 + 0.5f - centerX) + w * 0.5f;
        float t0 = id * (y0 + 0.5f - centerY) + h * 0.5f;
        float ds = ia, dt = id;
        // flipping maps texel n to w-1-n
        constexpr float EDGE = 1.0f / 256;
        if (s->flipx)
            s0 = w - EDGE - s0, ds = -ds;
        if (s->flipy)
            t0 = h - EDGE - t0, dt = -dt;

        rdpq_texture_rectangle_scaled(TILE_DRAW, originX + x0, originY + y0,
                                      originX + x1, originY + y1, s0, t0,
                                      s0 + ds * (x1 - x0), t0 + dt * (y1 - y0));
        rdpq_set_tile_size(TILE_DRAW, 0, 0, 8, 8);
        return;
    }

    // The RDP samples and covers pixels at their top-left corner, the GBA
    // (and the CPU renderer) at their centre. Moving the quad up and left by
    // half a pixel makes both pick the same pixels and texels.
    const float sx = cx - 0.5f, sy = cy - 0.5f;

    float v[4][5];
    const float corners[4][2] = {{0, 0}, {(float)w, 0}, {(float)w, (float)h}, {0, (float)h}};
    for (int i = 0; i < 4; i++) {
        const float du = corners[i][0] - w * 0.5f;
        const float dv = corners[i][1] - h * 0.5f;
        v[i][0] = sx + a * du + b * dv;
        v[i][1] = sy + c * du + d * dv;
        v[i][2] = s->flipx ? w - corners[i][0] : corners[i][0];
        v[i][3] = s->flipy ? h - corners[i][1] : corners[i][1];
        v[i][4] = 1.0f;
    }

    rdpq_triangle(&TRIFMT_TEX, v[0], v[1], v[2]);
    rdpq_triangle(&TRIFMT_TEX, v[0], v[2], v[3]);

    // back to the 8x8 tiles used everywhere else
    rdpq_set_tile_size(TILE_DRAW, 0, 0, 8, 8);
}

void drawSprites(int priority) {
    for (int i = 0; i < spriteCount[priority]; i++) {
        const OBJ_ATTR* s = &obj_buffer[spriteList[priority][i]];
        if (s->affine) {
            drawSprite(s);
            continue;
        }
        forEachWindowRect(cr::sprites_clip_window, [&] { drawSprite(s); });
    }
}

} // namespace

// The backdrop gradient as a texture: one colour per line, 4 texels wide to
// fill a TMEM line, stretched across the screen by a single rectangle.
alignas(16) u16 gradientTexture[PPU_H][4];

void ppuRenderFrameRdp(int ox, int oy) {
    originX = ox;
    originY = oy;
    resetRdpState();
    scissor = {-1, -1, -1, -1};
    memset(bgTileState, 0, sizeof(bgTileState));
    memset(spriteTileState, 0, sizeof(spriteTileState));

    buildSpriteLists();
    buildTlut(tlutBg, tng::palettes_bg);
    buildTlut(tlutSprite, tng::palettes_sprite);
    if (gradientEnabled) {
        for (int y = 0; y < PPU_H; y++) {
            const u16 c = tng::color_gba_to_tng(gradientTable[y]);
            gradientTexture[y][0] = gradientTexture[y][1] = c;
            gradientTexture[y][2] = gradientTexture[y][3] = c;
        }
    }

    // Everything the RDP reads (tiles, palettes, the gradient) has to be in
    // RAM, not just in the CPU cache. Flushing the whole 8 KB cache is far
    // quicker than walking the 96 KB of tile memory line by line.
    data_cache_writeback_invalidate_all();

    setScissor({0, 0, PPU_W, PPU_H});
    if (gradientEnabled) {
        rdpq_set_mode_standard();
        surface_t gradient =
            surface_make_linear(gradientTexture, FMT_RGBA16, 4, PPU_H);
        rdpq_tex_upload(TILE_DRAW, &gradient, NULL);
        rdpq_texture_rectangle_scaled(TILE_DRAW, ox, oy, ox + PPU_W, oy + PPU_H,
                                      0, 0, 0, PPU_H);
    } else {
        rdpq_set_mode_fill(color_from_packed16(tng::palettes_bg[0][0]));
        rdpq_fill_rectangle(ox, oy, ox + PPU_W, oy + PPU_H);
        rdpq_set_mode_standard();
    }

    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_mode_alphacompare(1);
    rdpq_set_tile(TILE_LOAD, FMT_CI8, 0, 8, NULL);
    rdpq_set_tile_size(TILE_DRAW, 0, 0, 8, 8);

    const u16* tlut = nullptr;
    auto useTlut = [&](const u16* t) {
        if (tlut != t) {
            rdpq_tex_upload_tlut((u16*)t, 0, 256);
            tlut = t;
            // the upload points the texture image at the palette
            imageBase = nullptr;
        }
    };

    for (int p = 3; p >= 0; p--) {
        for (int l = 3; l >= 0; l--) {
            const tng::LayerInfo& li = layerInfo[l];
            if (li.enabled && li.priority >= 0 && ((li.priority >> 4) & 3) == p) {
                useTlut(tlutBg);
                drawBgLayer(l);
            }
        }
        if (spriteCount[p] > 0) {
            useTlut(tlutSprite);
            drawSprites(p);
        }
    }
}
