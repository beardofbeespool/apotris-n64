# N64 local two-player: plan and state audit

Goal: local versus on one N64, split screen, each player with a full-size board
and their own controller, at 60 fps.

## Approach: one "world" per player

The shared game code draws through free functions (`showPawn()`, `showHold()`,
`showQueue()`, ...) that use a global `game` pointer, the GBA-style graphics
state and many file-scope variables. Instead of rewriting that code, each player
gets a copy of everything it touches, and the port switches between the copies:

1. Make player 1's world current, read controller 1, run the frame's update and
   draw as in single player.
2. Make player 2's world current, read controller 2, do the same.
3. Render both worlds into their halves of the 320x240 screen.

The battle rules already exist: CPU Battle runs two `Game` objects (`game` and
`botGame`) and exchanges garbage between them. In two-player, controller 2 drives
the second game instead of the bot.

## Phase 1 (done): switchable graphics state

All graphics state the game draws into is now in `tng::World`
(`include/n64/renderer.hpp`): VRAM (tiles and tilemaps), palettes, layer, blend,
mosaic and window settings, and the sprite table. `tng::world` points at the
current one, `ppuSelectWorld()` switches, and on N64 `obj_buffer` is a pointer to
the current world's sprite table (an array on the other platforms). Switching is a
pointer change, not a 100 KB copy. With one world in use, single player renders
pixel for pixel as before (checked with the compare build).

## State audit: what has to be per player

Found from the symbol tables of the compiled objects, which also lists statics
inside functions. "Swap" means the variable must be saved and restored with the
player's world in phase 2.

### game.cpp (game screen): swap

`game`, `current`, `previous`, `edges`, `glow`, `latestDrop`, `clearTimer`,
`eventPauseTimer`, `flashTimer`, `wiggleTimer`, `fullMeterTimer`,
`attackFlashTimer`, `gridUpdateTimer`, `restartTimer`, `goalLineHeight`,
`previousHeight`, `previousPalette`, `push`, `peek`, `peekShift`, `peekValue`,
`damp`, `dampTimer`, `shakeBuff`, `shakeVelocity`, `rainbow`, `rainbowTimer`,
`rainbowIncreasing`, `refresh`, `refreshSkin`, `holdingSave`, `onStates`,
`effectList`, `floatingList`, `placeEffectList`, `clearTypeText`,
the static locals of `showAttackBar()` (`cachedAmounts`, `cachedIds`,
`lastLayer`, `lastWasEmpty`), and the credit roll (`creditCurrentHeight`,
`creditLastRead`, `creditRefresh`).

Sprite pointers into the sprite table: `pawnSprite`, `pawnShadow`, `holdSprite`,
`holdFrameSprite`, `moveSprites`, `queueSprites`, `queueFrameSprites`. They point
into the world that was current when they were set, so they stay right as long as
they are swapped together with their world.

### game.cpp: shared

`gameLayout`, `altLayout`, `queueBGLengths`, `units`, `maxClearTimer`,
`dampTimerMax`, `gridUpdateTimerMax` (settings and constants), `framesSinceLastSave`,
`exitGame`, `handlingTest`, `enableBot`, and replays (`replaying`,
`replayIndex`, `replayBytePos`, `replayFrameTimer`; no replays in two-player).

### multiplayer.cpp

`botGame` becomes "the other player's game" (each player's world sees the other
as its opponent). The mini enemy board (`enemyBoard`, `enemyBoardSprite`,
`enemyBoardUpdates`, `currentScanHeight`) is not needed with two full boards.
Swap: `attackAnimationTimers`, `botClearTimer`, `botIncomingHeight`. Shared:
`multiplayerStart`, `ownPower`, `rank`.

### Other files

- text.cpp: `allocatedTiles`, `textArea` (text tiles are allocated in the
  world's VRAM): swap.
- main.cpp: `shake`, `gameSeconds`, `gradientTable`, `gradientEnabled`,
  `paused`: swap; `savefile`, `frameCounter`,
  `canDraw`, `multiplayer`, `playingBotGame`, `nextSeed`: shared.
- The scene: `GameScene` keeps per-game state in members (`counted`, timers...).
  Each player gets their own scene object, and the global `scene` is swapped.
- Port (`source/n64/liba_n64.cpp`): `currentKeys`, `previousKeys`: per player,
  read from controller 1 or 2.
- Shared and constant: sprites.cpp tables, tetromino.cpp grade tables,
  BlockEngine fumen settings, bot tuning, audio.

## Phase 2 (done): two games side by side

- Each file's per-player globals are listed with `N64_PLAYER_STATE()`
  (`include/n64/player_state.hpp`): game.cpp, multiplayer.cpp, text.cpp,
  main.cpp. The live set and a stash are swapped when the player changes;
  plain arrays are swapped with memcpy. `paused` is swapped too (phase 3): the
  pause menu runs inside the pausing player's turn, which blocks both games.
- `source/n64/twoplayer.cpp`: `n64SwitchPlayer()` swaps that state, selects the
  player's world and controller. `SplitScene` runs a `GameScene` per player.
  Player 2's world starts as a copy of player 1's (font, palettes) and gets its
  own `Game` with the same options.
- Switching costs about 0.3 ms (mostly the board arrays), so each pass over
  the players starts with the one already live: two switches per frame.
  Inside the game's own blocking loops (pause, end screen) the live player is
  restored after each call.
- Rendering only needs each player's world, which keeps a snapshot of the
  player's backdrop gradient, so it switches worlds without swapping state.
- At first each half showed GBA columns 42-202 of that player's screen, which
  cut the hold box and queue at the TV's edges; phase 3 replaced this with the
  region layout. All 5 queue pieces fit.
- Controllers 1 and 2 are read in one Joybus transaction. Player 2 uses player
  1's button mapping for now.
- "2P Battle" in the Play menu (N64 only) starts it. For now both players play
  their own Marathon game; "Multi Battle" (link cable and online) is hidden on
  N64.

Checked in ares: separate pieces, holds, queues and scores, each driven only by
its own controller; pausing, resuming and quitting to the menu; single player
afterwards (pixel for pixel as before, per the compare build).

Speed in ares during a two-player game: about 13 ms of work per frame, 59-60
fps with an occasional missed frame, input on screen after about 17 ms.
Rendering is about 4.5 ms per player, of which sprites are 2.9 ms. Single
player is unchanged.


## Phase 3 (done): battle, layout, speed

- Both games start with the same seed, so they get the same pieces. After each
  frame, `SplitScene` moves each game's outgoing attack into the other's
  garbage queue (`exchangeGarbage()`), and when one game is lost the other is
  won.
- One countdown: player 1's countdown runs and prints READY?/GO on both boards;
  player 2's game waits for it and starts on GO, without restarting the music.
- End of a round: instead of the single-player end screen (and its leaderboard
  and name entry), each board shows WIN, LOSE or DRAW and the session's win
  tally. A or Start plays again, B quits to the menu. Battle keeps no scores.
- Pause: the pause menu opens on the board of the player who pressed Start,
  driven by their controller; only that player is drawn while it runs.
  Restart restarts both games (`n64TwoPlayerRestart()`).
- Layout: the screen is built from pieces of each player's GBA screen
  (`PpuRegion`): board, hold box, queue, timer and attack bar. They are placed
  inside the CRT safe area, player 2's mirrored, so nothing is lost at the
  edges of the TV. Pieces must not overlap on the GBA screen: the board
  starts at x 72 because column 71 is the timer box's last, which showed as a
  thin bar beside the board.
- Backdrop: each half is filled with its player's gradient in one piece
  (`ppuRenderBackdrop()`), lined up with the board and carried on above and
  below it, and the pieces are drawn on top without a backdrop of their own.
  A player in their pause menu shows only the board, since the menu's other
  lines are placed for the whole GBA screen.
- Each world has its own palette (TLUT) and gradient buffers: the RDP reads
  one world's while the CPU prepares the other's, and shared buffers made a
  board flicker in the other player's colours (seen when one player paused).
- Speed: `ppuRenderRegions()` draws all of a world's regions in one pass, layer
  by layer, so the palette (TLUT) is loaded only when it changes; rotated and
  scaled sprites are cached between frames, keyed by their tiles' version
  counters. Steady 60 fps in ares: about 12 ms of work per frame, input on
  screen after about 17 ms.

Checked in ares: same pieces for both, garbage both ways, WIN/LOSE and the
tally, Retry and Quit, either player pausing, and single player pixel for pixel
as before (compare build).

## Phase 4 (done): full screen, CPU Battle, fixes

- Single player can fill the TV (Settings > Video > Screen, see the README).
  Two-player keeps its 320x240 layout, unfiltered, with the backdrop now
  covering the whole screen.
- Board caps: the frame of each board is closed above and below
  (`ppuRenderBoardCaps()`). The game notes where (`n64SnapshotBoardCaps()`,
  next to `ppuSnapshotGradient()`); the colours are read back from the
  finished framebuffer after each frame, as the frame is background tiles or
  sprites depending on mode and skin.
- CPU Battle runs in `SplitScene` with `n64CpuOpponent` set: player 2's game
  is set up as `startBotGame()` sets up the CPU's game, `testBot` plays it
  (from the end of the countdown, between line clears), and controller 2 is
  muted (`n64SetInputMuted()`).
- Player 1's colours were dimmer than player 2's: the scene fades ran on the
  current world, and drawing a frame leaves the other player's world current
  (one state swap per frame instead of two), so the steps of a fade
  alternated between the worlds and player 1's never got the last one. The
  fades now go over every world, each from its own saved palette.
- The countdown is a loop of its own with `paused` set, so the rule that
  shows only a paused player's board now keys off the pause menu itself
  (`n64PauseMenuOpen`). Player 2's backdrop is set up when the countdown
  starts, instead of staying black until GO.

## Next

- Player 2's own button mapping.
