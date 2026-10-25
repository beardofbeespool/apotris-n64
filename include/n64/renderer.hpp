#pragma once

#include <array>
#include <cstddef>

#include <cstdint>
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

class SpriteAttributes;

namespace tng {

constexpr size_t SPRITE_TILE_COUNT = 1024;
constexpr size_t SPRITE_OBJ_COUNT = 128;

// 4bpp 8x8 tile
struct ImageTile {
    u32 rows[8];
};

// GBA has 64 KB of VRAM for this?
constexpr size_t BG_TILEMAP_STORAGE_SIZE = 64 * 1024;

constexpr size_t BG_SCREENBLOCK_TILEMAP_TILE_COUNT = 1024;
constexpr size_t BG_SCREENBLOCK_TILEMAP_COUNT =
    BG_TILEMAP_STORAGE_SIZE / 2 / BG_SCREENBLOCK_TILEMAP_TILE_COUNT;

constexpr size_t BG_CHARBLOCK_TILESET_COUNT = 4;
constexpr size_t BG_CHARBLOCK_TILESET_TILE_COUNT =
    BG_TILEMAP_STORAGE_SIZE / BG_CHARBLOCK_TILESET_COUNT / sizeof(ImageTile);

using BgScreenblockTilemap = u16[BG_SCREENBLOCK_TILEMAP_TILE_COUNT];
using BgScreenblockAll = BgScreenblockTilemap[BG_SCREENBLOCK_TILEMAP_COUNT];

using BgCharblockTileset = ImageTile[BG_CHARBLOCK_TILESET_TILE_COUNT];
using BgCharblockAll = BgCharblockTileset[BG_CHARBLOCK_TILESET_COUNT];

constexpr size_t BG_LAYERS_COUNT = 4;

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

enum BlendMode {
    BLEND_OFF = 0,
    BLEND_STD = 1,
    BLEND_WHITE = 2,
    BLEND_BLACK = 3,
};

enum PaletteType {
    PALETTE_BG = 0u,
    PALETTE_SPRITE = 1u,
};

// Clip windows used by the renderer (screen coordinates, GBA 240x160 space)
struct ClipWindow {
    int left = 0, top = 0, right = 0, bottom = 0;
    bool enabled = false;
    bool invert = false;
};

using Palettes = std::array<std::array<u16, 16>, 16>;

// Everything the game draws into: the GBA-style VRAM, palettes, layer and
// window settings, and the sprite table (OAM). Two-player split-screen keeps
// one World per player and switches between them (ppuSelectWorld()), so the
// shared game code draws each player's screen exactly as in single player.
struct World {
    alignas(16) u8 bg_storage[BG_TILEMAP_STORAGE_SIZE];
    alignas(16) ImageTile sprite_tile_images[SPRITE_TILE_COUNT];

    // bumped on every write to a sprite tile, so converted copies of
    // sprites can tell when they are out of date
    u32 sprite_tile_version[SPRITE_TILE_COUNT];
    bool sprite_obj_dirty_flag[SPRITE_OBJ_COUNT];
    bool bg_screenblock_fill_screen[BG_SCREENBLOCK_TILEMAP_COUNT];

    // RGBA5551, background and sprite palettes (PALETTE_BG, PALETTE_SPRITE)
    std::array<Palettes, 2> palettes;

    LayerInfo layers[BG_LAYERS_COUNT];

    std::array<bool, BG_LAYERS_COUNT> blend_top_bg;
    bool blend_top_sprites;
    bool blend_top_backdrop;
    std::array<bool, BG_LAYERS_COUNT> blend_bottom_bg;
    bool blend_bottom_sprites;
    bool blend_bottom_backdrop;
    BlendMode blend_mode;

    // Mosaic size - 1
    u8 bg_mosaic_x;
    u8 bg_mosaic_y;

    ClipWindow bg_clip_windows[BG_LAYERS_COUNT];
    ClipWindow sprites_clip_window;

    // the sprite table; obj_buffer points at the current world's
    SpriteAttributes* oam;

    // The backdrop gradient (GBA colours, one per line) as it was when the
    // world was last drawn, see ppuSnapshotGradient()
    u16 gradient[160];
    bool gradientEnabled;

    // Caps that close the board's frame where the board runs off the top
    // and bottom of the GBA screen, drawn where the N64 shows more than it
    // (see ppuRenderBoardCaps()). The game says where they go; their colours
    // are read back from the frame as it was last drawn.
    struct BoardCaps {
        bool top = false, bottom = false;
        int x0 = 0, x1 = 0;     // the frame's outer edges, x1 exclusive
        bool fromRight = false; // read the colours from the right side
        int sampleY = 0;        // and on this line (GBA screen)

        // from the outermost line of the frame in (RGBA5551)
        u16 colors[4] = {};
        bool colorsRead = false;
        // where the GBA screen was last drawn on the framebuffer
        int originX = 0, originY = 0;
    } caps;
};

constexpr int WORLD_COUNT = 2;

// The world being drawn into and rendered
extern World* world;

inline BgScreenblockAll& screenblocks() {
    return *(BgScreenblockAll*)world->bg_storage;
}

inline BgCharblockAll& charblocks() {
    return *(BgCharblockAll*)world->bg_storage;
}

} /* namespace tng */

// Make a world current: the game's drawing and the renderer both use it
void ppuSelectWorld(int index);
int ppuCurrentWorld();

// Copies the game's backdrop gradient into the current world, so the world
// can be rendered later without the rest of its player's state
void ppuSnapshotGradient();

// Notes in the current world where the board's caps go (game.cpp)
void n64SnapshotBoardCaps();

// A rectangle of a world's GBA screen and where it goes on the framebuffer
struct PpuRegion {
    int x0, y0, x1, y1;     // on the GBA screen
    int dx, dy;             // top-left on the framebuffer
};

// Per world, once per frame, before rendering any of it
void ppuPrepareWorld();

// Renders regions of the current world in one pass (rdpq attached). Without
// backdrop, only the layers and sprites (see ppuRenderBackdrop())
void ppuRenderRegions(const PpuRegion* regions, int count, bool backdrop = true);

// Fills a framebuffer rectangle with the current world's backdrop gradient,
// its first line at framebuffer line lineY, carried on above and below
void ppuRenderBackdrop(int x0, int y0, int x1, int y1, int lineY);

// Draws the current world's board caps, with the GBA screen's top-left at
// (originX, originY) on a framebuffer of w x h
void ppuRenderBoardCaps(int originX, int originY, int w, int h);

// Reads the current world's frame colours for its caps from the finished
// framebuffer (the RDP done with it), for the next frame
struct surface_s;
void ppuReadBoardCaps(const struct surface_s* framebuffer);
