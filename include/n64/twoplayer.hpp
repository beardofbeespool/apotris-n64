#pragma once

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

Scene* n64CreateSplitScene();

// Renders both players' halves of the screen (rdpq attached to it)
void n64RenderTwoPlayer();

// Which controller the game's key functions read (liba_n64.cpp)
void n64SetInputPort(int port);

// The per-player globals of each source file (see n64/player_state.hpp)
void n64SwapGameScreenState();
void n64SwapBattleState();
void n64SwapTextState();
void n64SwapMainState();
