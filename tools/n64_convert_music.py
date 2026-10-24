#!/usr/bin/env python3
"""Convert your own music for the N64 port's SD card folder.

Usage: n64_convert_music.py [--menu] <files or folders...> -o <output folder>

Takes tracker modules (.mod .xm .it .s3m ...), .mp3, .wav, and anything else
ffmpeg can read (.ogg .flac .m4a ...), and writes looping .wav64 files the
game plays, levelled to the same loudness as the built-in songs. Copy the
results to sd:/apotris/music/ on the SummerCart64's SD card.

Names decide where a song plays, as in the PC version: MENU_... in menus,
GAME_... in game. Files without either prefix get GAME_ (or MENU_ with
--menu). A file with the same name as a built-in song replaces it.

Needs openmpt123, ffmpeg and libdragon's audioconv64, all present in the
dev container.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIOCONV = os.path.join(os.environ.get("N64_INST", "/opt/libdragon"), "bin", "audioconv64")
MODULES = {".mod", ".xm", ".it", ".s3m", ".mptm", ".669", ".mtm", ".stm", ".med", ".okt"}
AUDIO = {".mp3", ".wav", ".ogg", ".flac", ".m4a", ".aac", ".opus", ".wma"}


def song_name(path, menu):
    name = os.path.splitext(os.path.basename(path))[0]
    # keep names simple for the FAT file system and the in-game track list
    name = re.sub(r"[^A-Za-z0-9_\-]+", "_", name).strip("_") or "song"
    if not (name.startswith("MENU_") or name.startswith("GAME_")):
        name = ("MENU_" if menu else "GAME_") + name
    return name


def to_wav(src, dst, tmp):
    ext = os.path.splitext(src)[1].lower()
    if ext in MODULES:
        # openmpt123 writes <input>.wav next to its input
        copy = os.path.join(tmp, "module")
        shutil.copy(src, copy)
        subprocess.run(["openmpt123", "--quiet", "--render", "--force",
                        "--samplerate", "32000", "--no-float", copy],
                       check=True, stdout=subprocess.DEVNULL)
        shutil.move(copy + ".wav", dst)
    else:
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", src,
                        "-ac", "2", "-ar", "32000", "-sample_fmt", "s16", dst],
                       check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("inputs", nargs="+", help="music files or folders")
    parser.add_argument("-o", "--output", required=True, help="output folder")
    parser.add_argument("--menu", action="store_true",
                        help="songs without a MENU_/GAME_ prefix play in menus")
    args = parser.parse_args()

    files = []
    for item in args.inputs:
        if os.path.isdir(item):
            for root, _, names in os.walk(item):
                files += [os.path.join(root, n) for n in sorted(names)]
        else:
            files.append(item)
    files = [f for f in files if os.path.splitext(f)[1].lower() in MODULES | AUDIO]
    if not files:
        sys.exit("no music files found")

    os.makedirs(args.output, exist_ok=True)
    failed = 0
    for src in files:
        name = song_name(src, args.menu)
        with tempfile.TemporaryDirectory() as tmp:
            wav = os.path.join(tmp, name + ".wav")
            try:
                to_wav(src, wav, tmp)
                subprocess.run([sys.executable, os.path.join(HERE, "n64_level_wav.py"), wav],
                               check=True, stdout=subprocess.DEVNULL)
                subprocess.run([AUDIOCONV, "--wav-compress", "2", "--wav-loop", "true",
                                "-o", args.output, wav], check=True, stdout=subprocess.DEVNULL)
            except (subprocess.CalledProcessError, OSError) as e:
                print(f"FAILED  {src}: {e}")
                failed += 1
                continue
        size = os.path.getsize(os.path.join(args.output, name + ".wav64"))
        print(f"ok      {src} -> {name}.wav64 ({size // 1024} KB)")

    print(f"\n{len(files) - failed} converted, {failed} failed. Copy the .wav64 files to "
          "sd:/apotris/music/ on the SD card.")


if __name__ == "__main__":
    main()
