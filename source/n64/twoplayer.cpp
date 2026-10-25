// N64 local two-player split-screen (see docs/n64-two-player.md).
//
// Each player has a graphics world (n64/renderer.hpp) and a copy of the
// game's per-player globals (n64/player_state.hpp). SplitScene runs a
// GameScene for each player, switching the live state before every call, so
// the shared game code runs unchanged; vsync() then renders the two worlds
// side by side.
#include "platform.hpp"

#include "def.h"
#include "prng.h"
#include "scene.hpp"

#include "n64/renderer.hpp"
#include "n64/twoplayer.hpp"

#include <libdragon.h>

extern void ppuRenderFrameRdp(int originX, int originY, int viewX0, int viewX1);
extern void ppuCopyWorld(int from, int to);

bool n64TwoPlayerRequested = false;

namespace {

bool active = false;
int livePlayer = 0;

// Each player's half of the 320x240 screen shows this slice of their GBA
// screen: the hold box, the board and the next queue. They span about 166
// pixels, so 3 pixels of frame are cut on each side.
constexpr int VIEW_X0 = 42;
constexpr int VIEW_WIDTH = 160;
constexpr int SCREEN_Y = (240 - SCREEN_HEIGHT) / 2;

// Swaps every per-player global with its stash. With two players, one set is
// live and the other stashed, so one swap switches between them.
void swapPlayerState() {
    n64SwapGameScreenState();
    n64SwapBattleState();
    n64SwapTextState();
    n64SwapMainState();
}

// Calls into player code nested deeper than the top-level frame (see below)
int depth = 0;

// Runs fn with a player live, then puts back whichever player was live
// before: the game sometimes draws from inside a loop of its own (pause, end
// screen), so a draw can happen in the middle of a player's update.
template <typename F> void asPlayer(int player, F fn) {
    const int previous = livePlayer;
    n64SwitchPlayer(player);
    depth++;
    fn();
    depth--;
    n64SwitchPlayer(previous);
}

// Runs fn for both players. Each switch swaps a few KB of state, so at the
// top level this starts with the player already live and leaves the other
// one live: one switch per pass instead of two. Nested inside a player's own
// code, the live player is restored as asPlayer() does.
template <typename F> void forBothPlayers(F fn) {
    if (depth > 0) {
        for (int p = 0; p < 2; p++)
            asPlayer(p, [&] { fn(p); });
        return;
    }
    const int first = livePlayer;
    for (int p : {first, first ^ 1}) {
        n64SwitchPlayer(p);
        depth++;
        fn(p);
        depth--;
    }
}

class SplitScene : public Scene {
public:
    Scene* players[2] = {};

    void init() override {
        active = true;
        livePlayer = 0;

        // player 1 plays the game startGame() just created
        players[0] = new GameScene();
        players[0]->init();

        // player 2's screen starts from player 1's graphics (font, palettes)
        // and gets a game of their own with the same options
        ppuCopyWorld(0, 1);
        asPlayer(1, [&] {
            const BlockEngine::Options& options = *previousGameOptions;
            game = new BlockEngine::Game(options.mode, (int)randNext(),
                                         options.bigMode);
            game->setOptions(options);

            players[1] = new GameScene();
            players[1]->init();
        });
    }

    void update() override {
        forBothPlayers([&](int p) { players[p]->update(); });
    }

    void draw() override {
        // each player's draw also reads that player's controller
        forBothPlayers([&](int p) {
            players[p]->draw();
            ppuSnapshotGradient();
        });
    }

    bool control() override { return false; }

    void deinit() override {
        asPlayer(1, [&] {
            delete players[1];
            players[1] = nullptr;
            delete game;
            game = nullptr;
        });
        // single player carries on with player 1's state and world
        n64SwitchPlayer(0);
        delete players[0];
        players[0] = nullptr;

        active = false;
        n64TwoPlayerRequested = false;
    }

    std::function<Scene*()> previousScene() override { return nullptr; }

    ~SplitScene() {
        if (active)
            deinit();
    }
};

} // namespace

bool n64TwoPlayerActive() { return active; }

int n64CurrentPlayer() { return livePlayer; }

void n64SwitchPlayer(int player) {
    if (player == livePlayer)
        return;
    swapPlayerState();
    ppuSelectWorld(player);
    n64SetInputPort(player);
    livePlayer = player;
}

Scene* n64CreateSplitScene() { return new SplitScene(); }

// Rendering needs only each player's world (which holds a snapshot of their
// gradient), so it switches worlds without swapping the rest of the state
void n64RenderTwoPlayer() {
    for (int p = 0; p < 2; p++) {
        ppuSelectWorld(p);
        ppuRenderFrameRdp(p * VIEW_WIDTH - VIEW_X0, SCREEN_Y, VIEW_X0,
                          VIEW_X0 + VIEW_WIDTH);
    }
    ppuSelectWorld(livePlayer);
}
