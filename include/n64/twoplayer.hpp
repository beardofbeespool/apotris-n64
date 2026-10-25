#pragma once

#include <functional>

// N64 local two-player split-screen (see docs/n64-two-player.md)

class Scene;

// A two-player game is running
bool n64TwoPlayerActive();

// Player whose state is live (0 or 1)
int n64CurrentPlayer();

// Make a player's state live: their graphics world, the game's per-player
// globals and their controller
void n64SwitchPlayer(int player);

// Set by the 2P options screen just before the game starts, so gameLoop()
// opens the split screen instead of a normal game
extern bool n64TwoPlayerRequested;

// Player 2 is the CPU (CPU Battle): the bot plays their game and controller 2
// is ignored. Set together with n64TwoPlayerRequested.
extern bool n64CpuOpponent;

Scene* n64CreateSplitScene();

// Renders both players' halves of the screen (rdpq attached to it)
void n64RenderTwoPlayer();

// Runs fn with the other player's state live (e.g. to print on their board)
void n64AsOtherPlayer(const std::function<void()>& fn);

// Called by the live player's game screen when its game has ended
void n64TwoPlayerGameOver();

// The shared countdown (player 1's) has finished
bool n64TwoPlayerCountdownDone();
void n64TwoPlayerCountdownFinished();

// The live player's pause menu is open (menus.cpp)
extern bool n64PauseMenuOpen;

// Restart both games at the end of the current frame (pause menu)
void n64TwoPlayerRestart();

// Which controller the game's key functions read (liba_n64.cpp)
void n64SetInputPort(int port);

// While muted, the game's key functions see no buttons (the CPU's turn)
void n64SetInputMuted(bool muted);

// The per-player globals of each source file (see n64/player_state.hpp)
void n64SwapGameScreenState();
void n64SwapBattleState();
void n64SwapTextState();
void n64SwapMainState();
