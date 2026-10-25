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
#include "text.h"
#include "audio_files.h"

#include "n64/renderer.hpp"
#include "n64/twoplayer.hpp"

#include <libdragon.h>

#include <iterator>

extern void ppuCopyWorld(int from, int to);

bool n64TwoPlayerRequested = false;

namespace {

bool active = false;
int livePlayer = 0;

// The two-player screen is put together from pieces of each player's GBA
// screen, all inside the area a CRT shows (overscan crops the outer edges).
// Each half has the board in the middle of the screen and a column on the
// outside with the hold box above the next queue; the timer sits above the
// board and the attack counter below. Player 2's half mirrors player 1's.
struct Piece {
    int x0, y0, x1, y1;     // on the player's GBA screen
    int x, y;               // top-left on the 320x240 screen, player 1
};

constexpr Piece PIECES[] = {
    {72, 0, 169, 160, 59, 40},      // board, its frame and the garbage meter
    // (from x 72: column 71 is the timer box's last)
    {38, 69, 72, 103, 19, 32},      // hold box
    {171, 8, 207, 152, 18, 70},     // next queue
    {0, 8, 72, 16, 84, 28},         // timer
    {0, 136, 72, 152, 84, 204},     // attack counter
};

constexpr int SCREEN_W = 320;

// Each half is backed by its player's backdrop gradient in one piece, as in
// single player, rather than a strip behind each piece: from the outermost
// piece to the middle of the screen, the gradient lined up with the board
// (PIECES[0], which shows the whole height of the GBA screen).
struct Backdrop {
    int x0, y0, y1;
};

constexpr Backdrop backdropArea() {
    Backdrop b = {SCREEN_W / 2, 240, 0};
    for (const Piece& piece : PIECES) {
        b.x0 = piece.x < b.x0 ? piece.x : b.x0;
        b.y0 = piece.y < b.y0 ? piece.y : b.y0;
        const int bottom = piece.y + piece.y1 - piece.y0;
        b.y1 = bottom > b.y1 ? bottom : b.y1;
    }
    return b;
}

constexpr Backdrop BACKDROP = backdropArea();
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

// Wins so far in this session, shown on the result screen
int wins[2] = {};

// Set by a player's game screen when its game has ended
bool gameOverReported = false;

bool countdownDone = false;
bool restartRequested = false;

class SplitScene : public Scene {
public:
    Scene* players[2] = {};
    BlockEngine::Game* games[2] = {};

    bool over = false;

    void init() override {
        active = true;
        livePlayer = 0;
        gameOverReported = false;
        countdownDone = false;
        restartRequested = false;
        // so a retry from the result screen opens the split screen again
        n64TwoPlayerRequested = true;

        // player 1 plays the game startGame() just created
        games[0] = game;
        players[0] = new GameScene();
        players[0]->init();

        // player 2's screen starts from player 1's graphics (font, palettes)
        // and gets a game of their own: same options and the same pieces
        const int seed = game->initSeed;
        ppuCopyWorld(0, 1);
        asPlayer(1, [&] {
            const BlockEngine::Options& options = *previousGameOptions;
            game = new BlockEngine::Game(options.mode, seed, options.bigMode);
            game->setOptions(options);
            games[1] = game;

            players[1] = new GameScene();
            players[1]->init();
        });
    }

    void update() override {
        if (over) {
            resultInput();
            return;
        }

        forBothPlayers([&](int p) {
            players[p]->update();
            // a restart from the pause menu replaces the player's game
            games[p] = game;
        });

        exchangeGarbage();

        if (restartRequested)
            restart();
        else if (gameOverReported)
            showResult();
    }

    // A new game for both players (the result screen's retry, or restart in
    // the pause menu); whatever comes next carries on with player 1's state
    void restart() {
        n64SwitchPlayer(0);
        startGame();
        gameLoop();
    }

    // Lines a player clears become garbage on the other player's board, and
    // a player who tops out loses (as in CPU Battle, without the bot)
    void exchangeGarbage() {
        for (int p = 0; p < 2; p++) {
            BlockEngine::Game* from = games[p];
            BlockEngine::Game* to = games[p ^ 1];
            if (!from->attackQueue.empty()) {
                BlockEngine::Garbage attack = from->attackQueue.front();
                to->addToGarbageQueue(attack.id, attack.amount);
                from->clearAttack(attack.id);
            }
        }
        if (games[0]->lost)
            games[1]->won = true;
        if (games[1]->lost)
            games[0]->won = true;
    }

    // Both games stop; each board says who won, and either player picks
    // what next
    void showResult() {
        over = true;

        int winner = -1;
        if (games[0]->lost != games[1]->lost)
            winner = games[0]->lost ? 1 : 0;
        if (winner != -1)
            wins[winner]++;

        const Position pos = gameLayout[(int)GameElements::FIELD];
        const int x = pos.x / 8 + 1, y = pos.y / 8;

        forBothPlayers([&](int p) {
            clearText();
            aprint(winner == -1 ? "  DRAW  " : (winner == p ? "  WIN!  " : "  LOSE  "),
                   x, y + 6);
            aprint("WINS " + std::to_string(wins[p]), x + 1, y + 9);
            aprint("A RETRY", x + 1, y + 13);
            aprint("B QUIT", x + 1, y + 15);
        });

        sfx(winner == -1 ? SFX_END : SFX_YOUWIN);
    }

    void resultInput() {
        bool retry = false, quit = false;
        forBothPlayers([&](int p) {
            key_poll();
            MenuKeys k = savefile->settings.menuKeys;
            if (key_hit(k.confirm) || key_hit(k.pause))
                retry = true;
            else if (key_hit(k.cancel))
                quit = true;
        });

        if (!retry && !quit)
            return;

        if (retry) {
            sfx(SFX_MENUCONFIRM);
            restart();
        } else {
            sfx(SFX_MENUCANCEL);
            n64SwitchPlayer(0);
            wins[0] = wins[1] = 0;
            changeScene([]() { return new MainMenuScene(); }, Transitions::FADE);
        }
    }

    void draw() override {
        // once the game is over the boards stay as they ended
        if (over)
            return;

        // Inside one player's own loop (their pause menu, the countdown) only
        // that player is drawn: the other board stays frozen, as their game
        // does, instead of taking input
        if (depth > 0) {
            players[livePlayer]->draw();
            ppuSnapshotGradient();
            return;
        }

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

void n64AsOtherPlayer(const std::function<void()>& fn) {
    asPlayer(livePlayer ^ 1, fn);
}

void n64TwoPlayerGameOver() { gameOverReported = true; }

bool n64TwoPlayerCountdownDone() { return countdownDone; }

void n64TwoPlayerCountdownFinished() { countdownDone = true; }

void n64TwoPlayerRestart() { restartRequested = true; }

// Rendering needs only each player's world (which holds a snapshot of their
// gradient), so it switches worlds without swapping the rest of the state
void n64RenderTwoPlayer() {
    for (int p = 0; p < 2; p++) {
        ppuSelectWorld(p);
        ppuPrepareWorld();
        PpuRegion regions[std::size(PIECES)];
        int n = 0;
        for (const Piece& piece : PIECES) {
            const int w = piece.x1 - piece.x0;
            const int x = p == 0 ? piece.x : SCREEN_W - piece.x - w;
            regions[n++] = {piece.x0, piece.y0, piece.x1, piece.y1, x, piece.y};
        }
        const int x0 = p == 0 ? BACKDROP.x0 : SCREEN_W / 2;
        const int x1 = p == 0 ? SCREEN_W / 2 : SCREEN_W - BACKDROP.x0;
        ppuRenderBackdrop(x0, BACKDROP.y0, x1, BACKDROP.y1, PIECES[0].y);
        // a player in their pause menu shows just the board: the menu's
        // other lines (song name, hints) are placed for the full GBA screen
        // and would land in the hold, queue and timer boxes
        const bool pausing = depth > 0 && paused && p == livePlayer;
        ppuRenderRegions(regions, pausing ? 1 : n, false);
    }
    ppuSelectWorld(livePlayer);
}
