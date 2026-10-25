# Apotris

<https://apotris.com>

You can find builds/releases at https://apotris.com/downloads

Apotris is a block stacking game for the Gameboy Advance! It features satisfying graphics, responsive controls and a large amount of customization so that you can tailor the game to your preferences!

Join the [discord server](https://discord.com/invite/jQnxmXS7tr) for updates, beta releases, or to give me feedback!

## Audio customization

### Supported music formats

Tracker formats are `.mod`, `.it`, `.xm`, and `.s3m`. Use lowercase extensions.

| Platform | Music formats |
|----------|---------------|
| Windows, Linux, macOS, Android, PortMaster, Switch, Web | Tracker formats, `.mp3`, `.ogg` (Vorbis), `.flac`, `.wav` |
| Nintendo 3DS | Tracker formats, `.ogg` (Vorbis or Opus), `.opus`, `.mp3` |
| GBA | Tracker formats only (maximum 8 channels; 32 MB ROM limit) |

### Customizing audio

Close the game, then edit the audio folder:

- **Windows / Linux / PortMaster / Switch:** `assets/` beside the game. For Linux, use the extracted release rather than the AppImage.
- **macOS:** `Apotris.app/Contents/Resources/assets/` (right-click the app > **Show Package Contents**).
- **3DS:** `/3ds/Apotris/assets/` on the SD card.
- **Android:** launch once to extract files to `Documents/Apotris/assets/`, or the storage folder reported by the game. Grant **All files access** if prompted.
- **GBA:** `audio/` in the extracted release archive.

Add songs directly to that folder, prefixing filenames with `MENU_` for menus or
`GAME_` for gameplay (e.g. `GAME_my_song.mp3`). Remove unwanted music files, or
disable tracks in **Settings > Audio > Track List**. To replace a sound effect,
replace its `.wav` file while keeping the exact filename. Restart to load changes.

**Android:** disable bundled tracks instead of deleting them, since missing files
are restored. Back up replacements: updates/re-extraction may overwrite them.

**GBA:** after editing, run `WINDOWS_build.bat` or `bash LINUX_or_MAC_build.sh`
(requires Python 3 on Linux/macOS) from the release folder to rebuild `Apotris.gba`.
See [the GBA release instructions](tools/README.txt) for hardware-specific patching.

**Source / Web builds:** edit `assets/`, update music entries in
`assets/meson.build`, and rebuild to package the changes.

## Docker build

Want to bypass all the work and use a system shipped by the developers intended to build Apotris? Try the docker image:

First, [install Docker](https://docs.docker.com/get-docker/) for your system. Then get it running and open up a terminal:

```bash
# Download and log in to the Apotris builder docker image
docker run -it docker.io/akouzoukos/apotris bash
# OR Optional: build the image yourself
docker build -t apotris-builder . && docker run -it apotris-builder bash
# You are now in the docker image environment, use Ctrl+D or "exit" command to return to main OS

# Clone the latest sources
git clone --recursive https://gitea.com/akouzoukos/apotris.git
# Enter source dir
cd apotris
# From here you can build one of the available targets

# GBA
meson setup --cross-file=meson/gba.ini build-gba && meson compile -C build-gba
# Windows
meson setup --cross-file=meson/mingw.ini build-windows && meson compile -C build-windows
# Linux
meson setup build-linux && meson compile -C build-linux

# NOTE: If you're trying to produce a build exactly like the released builds, you want to add flags as well like below
# Some emulators and platforms will behave differently without these flags (e.g. a debug build)
meson setup -Db_lto=true -Db_lto_mode=thin --buildtype=release ...
# Also note that some builds (specifically the Windows build) may fail with the LTO-related flags, so remove those if the sources no longer compile

# Built executables are now in the Docker environment's apotris/build-*/ folders
```

OR mount the repo as a volume:

```bash
# Clone the repo locally (add --config core.autocrlf=false if you are on Windows)
git clone --recursive https://gitea.com/akouzoukos/apotris.git 
# Enter source dir
cd apotris 
# Build the image
docker build -t apotris-builder . 
# Run the image with the project directory mounted
docker run -it -v ${PWD}/:/root/apotris -w /root/apotris apotris-builder bash

# GBA
meson setup --cross-file=meson/gba.ini build-gba && meson compile -C build-gba
# Windows
meson setup --cross-file=meson/mingw.ini build-windows && meson compile -C build-windows
# Linux
meson setup build-linux && meson compile -C build-linux
# Built executables are now in the local environment's apotris/build-*/ folders
```

## Meson build

### Overview of requirements for most systems

As all the libraries and tools are compiled from source, the only things you
need to have installed and in your PATH are:

- meson (at least version 1.3.0 or later)
  - If your system has an out of date meson, install `pip` and run `pip install -U meson` to get the latest
- git
- A compiler for your system (`gcc`, `clang`, etc.)
- A compiler for the GBA (`arm-none-eabi-gcc`)
  - On Windows you can download the [.exe installer](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) for `mingw-w64-i686-arm-none-eabi-gcc`
  - On Ubuntu download at least 13.2 of arm-none-eabi-gcc for your system
    [here](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) 
    - Extract the archive into your system path with `tar xf ./arm-gnu-toolchain-*-arm-none-eabi.tar.xz -C /usr/share/`
    - Make sure to symlink everything `sudo ln -sf /usr/share/arm-gnu-toolchain-YOUR_VERSION/bin/* /usr/bin/`

**Note:** On macOS one can `brew install --cask gcc-arm-embedded && brew install meson ninja opus pkg-config cmake` to get all required deps.

### Windows (native) requirements

You will need to install:

- [Python 3](https://www.python.org/downloads/windows/)
  - get the latest stable Windows installer for your platform
  - ensure you "add to PATH" when you run the installer (unless you know what you're doing otherwise)
- Meson
  - Run `pip install meson` after installing Python to get meson
  - An alternative is [the Windows installer](https://github.com/mesonbuild/meson/releases/download/1.4.0/meson-1.4.0-64.msi)
- A Windows-native compiler and build tools
  - [Strawberry Perl](https://strawberryperl.com) works out of the box, get the system installer to make it easiest

#### Windows GBA build

For the GBA target, you will need to update the `meson/gba.ini` file to have the path to your ARM compiler:

```ini
[constants]
path = 'C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.2 Rel1\bin'
# ... rest of original file
```

You can avoid editing a file by setting this with a powershell command per-session

```powershell
$env:PATH="$env:PATH;C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\13.2 Rel1\bin"
```

Ensure the version string and directory paths are correct for your installation. They may change with future updates.

### Ubuntu 22.04+ and Windows (WSL) dependencies

#### For all Linux systems

Install the following packages:

```bash
sudo apt install build-essential git meson cmake xxd
```

also install `mingw-w64` if you plan to cross compile for Windows

#### For Linux native builds

Install the following dependencies of SDL2:

```bash
sudo apt-get install build-essential git make \
  pkg-config cmake ninja-build gnome-desktop-testing libasound2-dev libpulse-dev \
  libaudio-dev libjack-dev libsndio-dev libx11-dev libxext-dev \
  libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev \
  libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev \
  libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev fcitx-libs-dev libglew-dev
```

**Note:** On some Ubuntu installations the SDL2 libraries are not imported correctly by the build. Removing `libsdl2-dev` may resolve these issues. 

## Compiling

Once you have everything, `git clone --recursive https://gitea.com/akouzoukos/apotris.git` or [download](https://gitea.com/akouzoukos/apotris/archive/main.zip) this repository, then
navigate to the directory in a terminal (or "command-line") window, and run some or all of the
following commands depending on your desired target:

```sh
# Native build
meson setup build
meson compile -C build

# GBA Build
meson setup --cross-file=meson/gba.ini build-gba
meson compile -C build-gba

# MingW build (Windows using Linux, WSL, or macOS to build)
# Note: the following command is a really bad hack that probably has no consequences but is required
sudo ln -sf $(which x86_64-w64-mingw32-g++-posix) $(which x86_64-w64-mingw32-g++)
meson setup --cross-file=meson/mingw.ini build-mingw
meson compile -C build-mingw
```

The built Apotris ROMs (.elf, .gba) will be in your `build-gba` directory.

The built Apotris executable and its required `assets/` folder will be in your build directories for other builds.

### PC developer scenarios

PC builds can record and replay complete input paths, including menu navigation.

- Press `F10` to reset to title screen and begin recording. Press `F10` again to
  save next numbered scenario in `tests/`, such as `tests/scenario-1.scn`, plus
  its initial save snapshot, `tests/scenario-1.scn.save`.
- Press `F9` to reload last scenario and restart it from title screen.
- Start a saved scenario directly with:

  ```sh
  ./Apotris --scenario path/to/my-scenario.scn
  ```

Scenario playback suppresses writes to normal `Apotris.sav`. Keep scenario file
and its matching `.save` sidecar together. Input events use frame timing, so
record from title screen and replay using same build/platform.

### Unique GBA builds

GBA has a few unique targets for different flash carts and hardware platforms. Included is a table of all targets and
their equivalent target invocation names:

| Hardware | Meson ninja target |
|----------|----------|
| Analogue Pocket Rumble  | `Apotris-Pocket.gba` |
| EZFlash Air | `Apotris-Air.gba` |

To build one of these locally, in the final `meson compile` step replace the command with
```sh
ninja -C build-gba Apotris-pocket.gba
```
replacing the filename with the desired target.

UPS patch files can be generated for all targets (for inclusion in a release .zip) via the `patches` alias, e.g.:
```sh
ninja -C build-gba patches
```
To generate UPS files, `go` must be present in the system path to build the `ups` patcher tool.

## Less common platforms

### Nintendo Switch

The Switch build requires one have the DevkitARM A64 toolchain installed. Installing this is covered by [this reference](https://devkitpro.org/wiki/Getting_Started). You will want the `switch-dev` and `switch-portlibs` targets installed.

Ensure the compiler is in your PATH and run the cross build:

```bash
meson setup --cross-file=meson/switch.ini build-switch
meson compile -C build-switch
```

The file is output to `build-switch/Apotris.nro`.

### Emscripten

The Emscripten build requires one to have the Emscripten toolchain/SDK installed. See [this reference](https://emscripten.org/docs/getting_started/downloads.html). The latest should work, but at least `3.1.51` is required.

The Emscripten build has a bug. The workaround is relatively simple.

```bash
$ meson setup --cross-file=meson/emscripten.ini build-web

$ meson compile -C build-web # this command will throw an error:

AssertionError: attempt to lock the cache while a parent process is holding the lock (sysroot/lib/wasm32-emscripten/libSDL2.a)
em++: error: subprocess 1/34 failed (returned 1)! (cmdline: /opt/homebrew/Cellar/emscripten/3.1.58/libexec/emcc -c /opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/src/mixer.c -o /opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports-builds/sdl2_mixer/src/mixer.c.o -g -sSTRICT -Werror -O2 -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0 -sUSE_SDL=2 -O2 -DMUSIC_WAV -sUSE_VORBIS -DMUSIC_OGG -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/include -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/src -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/src/codecs)
ninja: build stopped: subcommand failed.
```

Take the command it failed on (in the `cmdline:` section) and run it manually:

```bash
$ /opt/homebrew/Cellar/emscripten/3.1.58/libexec/emcc -c /opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/src/mixer.c -o /opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports-builds/sdl2_mixer/src/mixer.c.o -g -sSTRICT -Werror -O2 -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0 -sUSE_SDL=2 -O2 -DMUSIC_WAV -sUSE_VORBIS -DMUSIC_OGG -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/include -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/src -I/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/ports/sdl2_mixer/SDL_mixer-release-2.8.0/src/codecs

cache:INFO: generating port: sysroot/lib/wasm32-emscripten/libSDL2.a... (this will be cached in "/opt/homebrew/Cellar/emscripten/3.1.58/libexec/cache/sysroot/lib/wasm32-emscripten/libSDL2.a" for subsequent builds)
system_libs:INFO: compiled 117 inputs in 2.99s
cache:INFO:  - ok
```

Then re-run the compile step again:

```bash
$ meson compile -C build-web

ninja: Entering directory `build-web`
[1/1] Linking target Apotris.html # success !
```

### Portmaster

You'll need to load the portmaster development environment, covered [here](https://portmaster.games/docker.html).

Once inside the container, clone the repo and configure as if it were a native build, but with one key config:

```bash
meson setup -Dportmaster=true build-pm
meson compile -C build-pm
```

For some platforms (namely the TrimUI Smart Pro or other devices with specifically a Cortex A-53) you'll need to add the native-file argument to bring in some patches for hardware bugs.

```bash
meson setup --native-file=meson/portmaster.ini -Dportmaster=true build-pm
meson compile -C build-pm
```

**Note:** on typical Intel/AMD machines this can take a *long* time.


### Nintendo 3DS

Compiling the 3DS build requires you to install the devkitARM toolchain from devkitPro. You can refer to the [3DBrew](https://www.3dbrew.org/wiki/Setting_up_Development_Environment) and [devkitPro](https://devkitpro.org/wiki/Getting_Started) guides to set it up. Following the guide, you should install all packages in the package group `3ds-dev` using `pacman` (or `dkp-pacman`).

You will also need to install the libraries `mbedtls`, `libmpg123`, `libvorbisidec` and `libxmp` using `pacman` (or `dkp-pacman`):

```bash
pacman -S 3ds-mbedtls 3ds-mpg123 3ds-libvorbisidec 3ds-libxmp
```

To perform the cross build:

```bash
meson setup --cross-file=meson/n3ds.ini build-n3ds
meson compile -C build-n3ds
```

The build output file can be found as `build-n3ds/Apotris.3dsx`.

To run the build, the audio assets must be placed on the SD card under `/3ds/Apotris/assets/` beforehand.

#### Building a CIA file

Normally, there is no reason to use a CIA file. Especially if you are developing and debugging code, the 3dsx file can be easily loaded using `3dslink` which also allows you to see stdout and stderr output with its server.

To build the CIA, you will need to install [`bannertool`](https://github.com/Epicpkmn11/bannertool) and [`makerom`](https://github.com/3DSGuy/Project_CTR/tree/master/makerom) manually and have them available on the `PATH` environment variable while building.

```bash
meson compile -C build-n3ds cia
```

### Nintendo 64

The N64 port uses [libdragon](https://github.com/DragonMinded/libdragon) (preview branch) and its own Makefile instead of Meson. With libdragon installed and `N64_INST` set:

```bash
make -f Makefile.n64
```

The build also needs `python3` and `openmpt123` (Debian/Ubuntu: `apt install openmpt123`), which renders the music. The build output file is `apotris.z64`. Copy it to a flashcart such as the SummerCart64, or run it in [ares](https://ares-emu.net).

Controls:

| N64 | Apotris |
|---|---|
| D-pad | Move, menu navigation |
| A / B | A / B |
| Start | Start |
| Z | Select |
| L / R | L / R |

The C buttons and the analog stick are passed to the game as extra inputs of their own. By default Hold is on both L and R.

Every action can be remapped in Settings > Controls, to one button or several: select an action, then press each button you want for it (one after another or together). The binding is set half a second after you let go of the last one, and any of those buttons used by another action is taken off it. Z on an action puts its default back.

Saving: settings, records and everything else the game keeps are saved in 32 KB of cartridge SRAM, which the ROM header asks the flashcart (or emulator) for. There are two copies, written alternately with a checksum, so losing power while saving loses at most the latest save. A save is written to SRAM 1 KB per frame in the idle time before the controller is read, so it never costs a frame. The first 1 KB of SRAM is left unused: libdragon's flashcart detection writes to an address that lands there on some setups (ares, for one).

Picture and latency:

- The output is 240p with all VI filtering off, so every pixel reaches a CRT exactly as drawn: no interlace flicker, no blur and no dithering. The 240x160 picture sits in the middle of the 320x240 screen at its original aspect ratio, well clear of the overscan on most TVs.
- Two framebuffers rather than three, as a third only queues up frames and adds a frame of lag.
- Low input latency: a frame appears at the vblank after it is drawn, so the controller is read as late in the frame as the frame's work allows. The busiest recent frame (logic + drawing + the read) plus a 2 ms margin is left before the next vblank, and controller 1 is read directly right before that. In a game this puts input on screen about 8-9 ms after it is read, where reading right after the vblank would take a whole frame. If a frame still misses its vblank (this happens at some screen transitions), reading moves back to the start of the frame and creeps later again over the next seconds.

How the port works:

- The shared game code draws through the same GBA-style tile and sprite API as the other ports. `source/n64/ppu.cpp` keeps that state and draws it on the RDP in the GBA's back to front order, with the tile data read straight from RAM as CI4 textures and the GBA palettes as the TLUT. A whole frame takes the CPU about 5-6 ms during a game.
  - Background layers: runs of the same tile become one repeating rectangle, and blocks are sorted by tile and palette so each tile is loaded into TMEM once per layer.
  - Sprites: all tiles of a sprite are loaded together (up to 32 at a time) and each tile is one rectangle.
  - Scaled sprites are scaled rectangles covering exactly the pixels the GBA would draw. Rotated sprites are two textured triangles.
  - The backdrop gradient is a 1-pixel-wide texture stretched across the screen.
- All of that graphics state (tiles, tilemaps, palettes, layer and window settings, the sprite table) lives in a `tng::World` (`include/n64/renderer.hpp`), and the renderer draws the current one. This is groundwork for local two-player split-screen, where each player gets a world; see `docs/n64-two-player.md` for the plan and the audit of what the game keeps per player.
- The output matches a straightforward CPU renderer pixel for pixel. Build with `APOTRIS_FLAGS=-DAPOTRIS_N64_PPU_COMPARE` to check: every frame is drawn both ways and differences are logged (the first bad frame is dumped as hex). `APOTRIS_FLAGS=-DAPOTRIS_N64_CPU_PPU` uses the CPU renderer for display instead; it is far too slow for 60 fps. Changing `APOTRIS_FLAGS` rebuilds everything.
- The GBA assets in `data/` are little-endian, while the N64 is big-endian. `tools/n64_swap.py` swaps their byte order at build time (tiles as 32-bit rows, palettes and tilemaps as 16-bit values), so the game code reads them the same way as on the GBA.
- Once a second the game logs its frame rate, the time spent on game logic and drawing (and how much of that was waiting for the RDP), the busiest frame, the input-to-screen time and any missed vblanks through `debugf`. ares shows it in its terminal; on a flashcart it goes to the USB debug log.

Sound:

- The sound effects in `assets/` are converted to mono 22 kHz wav64 files and streamed from ROM, at half volume so that several at once on top of the music do not clip.
- libdragon plays no MOD or IT files, so the songs are rendered with `openmpt123` at build time, levelled to the same loudness by `tools/n64_level_wav.py`, and stored as looping, ULC-compressed wav64 (about 40 KB per second of music). Changing the music tempo changes the playback speed, so slower music also plays lower, as in the PC version.
- Extra songs, only in the N64 build, written as modules by `tools/n64_tetris_music.py`: two arrangements of the folk song Korobeiniki (the Tetris "A" theme) made for this port, and Bach's Menuet from the French Suite No. 3, BWV 814 (the Tetris "C" theme). See `assets-n64/CREDITS.md` for the sources and licenses (the Menuet is CC BY-SA 4.0); the game's Credits screen lists them too.
- Your own music: put `.wav64` files in `sd:/apotris/music/` on the flashcart's SD card. As in the PC version, `MENU_...` songs play in menus and anything else in game, and a file with the same name as a built-in song replaces it. To convert music (tracker modules, `.mp3`, `.wav`, `.ogg`, `.flac` and anything else `ffmpeg` reads):

  ```bash
  python3 tools/n64_convert_music.py my-music/ -o sd-music/          # in game
  python3 tools/n64_convert_music.py --menu calm.mp3 -o sd-music/    # menu music
  ```

  It needs `openmpt123`, `ffmpeg` and libdragon's `audioconv64`. Only add music you have the right to use; it stays on your SD card and never goes into the ROM.

Two players: "2P Battle" in the Play menu is a versus game between controller 1 and controller 2 on one console. Each player has a full-size board, hold box and 5-piece queue, and both get the same piece sequence. Line clears send garbage to the other player, and the first to top out loses. One shared READY/GO countdown starts both games. At the end each board shows WIN, LOSE or DRAW and the win tally for the session; A (or Start) plays again and B returns to the menu. Either player can pause, and the pause menu opens on their own board; Restart restarts both games. Battle has no score leaderboard or name entry: the knockout decides the winner. The layout keeps both screens inside the CRT safe area, and it runs at 60 fps. Player 2 uses player 1's button mapping for now. The link cable and online modes ("Multi Battle") need a second console or a network and are hidden on N64. See `docs/n64-two-player.md` for how it works.

### Android
You will need the Android NDK installed and configured.

#### Prerequisites

1.  **Android NDK:** Ensure you have the Android NDK installed.
2.  **Environment Variables:**
    *   Set `ANDROID_NDK_HOME` to your NDK installation directory.
    *   Add the NDK toolchain bin directory to your `PATH`.
        ```bash
        export PATH=$PATH:$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64/bin
        ```

#### Building

    ```bash
    meson setup --cross-file=meson/android.ini build-android 
    meson compile -C build-android
    ```

**Metadata**:

* Unique ID: `0xA9715`
* Title ID: `000400000A971500`
* Product Code: `CTR-H-APTR`
