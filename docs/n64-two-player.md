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
- main.cpp: `paused` (pausing pauses both), `shake`, `gameSeconds`,
  `gradientTable`, `gradientEnabled`: swap; `savefile`, `frameCounter`,
  `canDraw`, `multiplayer`, `playingBotGame`, `nextSeed`: shared.
- The scene: `GameScene` keeps per-game state in members (`counted`, timers...).
  Each player gets their own scene object, and the global `scene` is swapped.
- Port (`source/n64/liba_n64.cpp`): `currentKeys`, `previousKeys`: per player,
  read from controller 1 or 2.
- Shared and constant: sprites.cpp tables, tetromino.cpp grade tables,
  BlockEngine fumen settings, bot tuning, audio.

## Next phases

2. A player context holding the swapped variables, a second world and scene,
   controller 2, and both worlds rendered side by side. Two independent games
   first, to flush out any state the audit missed (it shows as one player's
   effect appearing on the other's board).
3. Versus: garbage exchange through the existing battle code, win and loss,
   a "2P Battle" entry in the Play menu, results.
4. Layout and speed: each half shows hold, board and next queue (about 160 px
   wide); score, attack and timer above and below the boards. Rendering two
   worlds roughly doubles the frame work (about 10-12 ms), so input to screen in
   two-player will be about 12-14 ms; single player is unaffected.
