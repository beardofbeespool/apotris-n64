#!/usr/bin/env python3
"""Write the extra N64 songs as 4-channel ProTracker modules.

Usage: n64_tetris_music.py <menuet.mid> <output dir>

- Korobeiniki: the 19th century folk melody behind the Tetris "A" theme
  (public domain), in an arrangement made for this port: chords played as
  chiptune arpeggios, an octave bass and drums. Normal and turbo versions.
- Menuet from Bach's French Suite No. 3, BWV 814 (the Tetris "C" theme).
  The notes come from the Mutopia Project transcription by Knute Snortum
  (CC BY-SA 4.0, see assets-n64/CREDITS.md), so this arrangement is
  CC BY-SA 4.0 as well.

The instruments are synthesized here (square, pulse, triangle and noise),
so the modules depend on nothing but this script and the MIDI file.
"""

import math
import os
import random
import struct
import sys

# ProTracker periods for C-1 .. B-3
PERIODS = [
    [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453],
    [428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226],
    [214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113],
]

# Paula plays a sample at 3546895 / period Hz (PAL clock), so C-2 (428)
# plays a 32 sample cycle at C4 and a 64 sample cycle at C3.
C2_RATE = 3546895 / 428


def square(length, duty, amp=56):
    high = int(length * duty)
    return [amp if i < high else -amp for i in range(length)]


def triangle(length, amp=100):
    half = length // 2
    up = [int(-amp + 2 * amp * i / half) for i in range(half)]
    return up + [-v for v in up]


def kick():
    out, phase = [], 0.0
    n = int(C2_RATE * 0.16)
    for i in range(n):
        t = i / C2_RATE
        freq = 45 + 110 * math.exp(-t * 30)
        phase += 2 * math.pi * freq / C2_RATE
        out.append(int(110 * math.sin(phase) * math.exp(-t * 18)))
    return out


def noise(seconds, decay, amp, seed):
    rng = random.Random(seed)
    n = int(C2_RATE * seconds)
    return [int(amp * (rng.random() * 2 - 1) * math.exp(-i / C2_RATE * decay))
            for i in range(n)]


# sample number: (name, data, volume, looped)
SAMPLES = {
    1: ("square lead", square(32, 0.5), 40, True),     # C-1 = C3
    2: ("square lead high", square(16, 0.5), 36, True),  # C-1 = C4
    3: ("pulse lead", square(32, 0.25), 38, True),     # C-1 = C3
    4: ("pulse chord", square(32, 0.125), 22, True),   # C-1 = C3
    5: ("triangle bass", triangle(64), 52, True),      # C-1 = C2
    6: ("kick", kick(), 56, False),
    7: ("snare", noise(0.14, 22, 90, 1), 40, False),
    8: ("hat", noise(0.035, 90, 70, 2), 22, False),
}
SAMPLE_BASE = {1: 48, 2: 60, 3: 48, 4: 48, 5: 36,   # MIDI note of C-1
               6: 48, 7: 48, 8: 48}             # drums play at C-2 (note 60)


def period(sample, midi):
    rel = midi - SAMPLE_BASE[sample]
    assert 0 <= rel < 36, f"note {midi} out of range for sample {sample}"
    return PERIODS[rel // 12][rel % 12]


class Song:
    """Rows of 4 channels; a cell is (sample, midi note, effect, param)."""

    def __init__(self):
        self.rows = []

    def ensure(self, n):
        while len(self.rows) < n:
            self.rows.append([None, None, None, None])

    def put(self, row, ch, sample=0, note=None, effect=0, param=0):
        self.ensure(row + 1)
        old = self.rows[row][ch] or (0, None, 0, 0)
        if effect == 0 and param == 0:
            effect, param = old[2], old[3]
        self.rows[row][ch] = (sample or old[0], note if note is not None else old[1],
                              effect, param)

    def note(self, row, ch, sample, midi, length, cut=True):
        self.put(row, ch, sample, midi)
        # a short gap at the end keeps repeated notes apart
        if cut and length > 1:
            self.put(row + length - 1, ch, effect=0xE, param=0xC3)


def write_mod(path, title, song, speed, bpm):
    rows = song.rows
    # speed and tempo on the first row, a pattern break after the last one
    song.put(0, 3, effect=0xF, param=speed)
    song.put(1, 3, effect=0xF, param=bpm)
    if len(rows) % 64:
        song.put(len(rows) - 1, 2, effect=0xD, param=0)
    patterns = (len(rows) + 63) // 64
    song.ensure(patterns * 64)

    out = bytearray(title.encode()[:20].ljust(20, b"\0"))
    for n in range(1, 32):
        if n in SAMPLES:
            name, data, vol, looped = SAMPLES[n]
            words = len(data) // 2
            out += name.encode()[:22].ljust(22, b"\0")
            out += struct.pack(">HBBHH", words, 0, vol, 0, words if looped else 1)
        else:
            out += bytes(22) + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
    out += bytes([patterns, 127])
    out += bytes(range(patterns)).ljust(128, b"\0")
    out += b"M.K."
    for row in song.rows:
        for cell in row:
            if cell is None:
                out += bytes(4)
                continue
            sample, midi, effect, param = cell
            per = period(sample, midi) if midi is not None else 0
            out += bytes([(sample & 0xF0) | (per >> 8), per & 0xFF,
                          ((sample & 0x0F) << 4) | effect, param])
    for n in sorted(SAMPLES):
        data = SAMPLES[n][1][: len(SAMPLES[n][1]) // 2 * 2]
        out += bytes((v + 256) % 256 for v in data)
    with open(path, "wb") as f:
        f.write(out)


# --- Korobeiniki ------------------------------------------------------------

E4, F4, G4, A4, B4 = 64, 65, 67, 69, 71
C5, D5, E5, F5, G5, A5 = 72, 74, 76, 77, 79, 81
REST = None

# the folk melody, in sixteenth note rows (4 per beat, 16 per bar)
MELODY = [
    (E5, 4), (B4, 2), (C5, 2), (D5, 4), (C5, 2), (B4, 2),
    (A4, 4), (A4, 2), (C5, 2), (E5, 4), (D5, 2), (C5, 2),
    (B4, 6), (C5, 2), (D5, 4), (E5, 4),
    (C5, 4), (A4, 4), (A4, 4), (REST, 4),
    (REST, 2), (D5, 4), (F5, 2), (A5, 4), (G5, 2), (F5, 2),
    (E5, 6), (C5, 2), (E5, 4), (D5, 2), (C5, 2),
    (B4, 4), (B4, 2), (C5, 2), (D5, 4), (E5, 4),
    (C5, 4), (A4, 4), (A4, 4), (REST, 4),
]

# my own harmony, one chord per half bar: (root, arpeggio 0xy)
MINOR, MAJOR = 0x37, 0x47
CHORDS = [
    (E4, MINOR), (G4, MAJOR), (A4, MINOR), (A4 - 9 + 12, MAJOR),   # Em G Am C
    (E4, MINOR), (62, MAJOR), (A4, MINOR), (A4, MINOR),            # Em D Am Am
    (62, MINOR), (62, MINOR), (60, MAJOR), (60, MAJOR),            # Dm Dm C C
    (G4, MAJOR), (E4, MINOR), (A4, MINOR), (A4, MINOR),            # G Em Am Am
]


def korobeiniki_section(song, start, lead, chords=True, drums="full"):
    row = start
    for midi, length in MELODY:
        if midi is not None:
            song.note(row, 0, lead, midi, length)
        row += length

    for i, (root, arp) in enumerate(CHORDS):
        r = start + i * 8
        bass = root - 24 if root - 24 >= 36 else root - 12
        for k in range(4):   # octave bouncing eighths
            song.note(r + k * 2, 2, 5, bass + (12 if k % 2 else 0), 2, cut=False)
        if chords:
            song.put(r, 1, 4, root)
            for k in range(8):
                song.put(r + k, 1, effect=0x0, param=arp)

    for bar in range(8):
        r = start + bar * 16
        song.note(r, 3, 6, 60, 1)
        song.note(r + 8, 3, 6, 60, 1)
        if drums == "full":
            song.note(r + 4, 3, 7, 60, 1)
            song.note(r + 12, 3, 7, 60, 1)
            for k in (2, 6, 10, 14):
                song.note(r + k, 3, 8, 60, 1)
    return start + 8 * 16


def korobeiniki(lead_low, lead_high):
    song = Song()
    # one bar of bass and drums to count in
    for k in range(8):
        song.note(k * 2, 2, 5, 40 + (12 if k % 2 else 0), 2, cut=False)
    for k in (0, 8):
        song.note(k, 3, 6, 60, 1)
    for k in (4, 12):
        song.note(k, 3, 7, 60, 1)
    row = 16
    row = korobeiniki_section(song, row, lead_low)
    row = korobeiniki_section(song, row, lead_high)
    row = korobeiniki_section(song, row, 3, chords=False, drums="kick")
    song.ensure(row)
    return song


# --- Bach, BWV 814 Menuet -------------------------------------------------

def read_midi_notes(path):
    """Notes per track as (start tick, end tick, note), plus ticks per beat."""
    d = open(path, "rb").read()
    _, ntracks, division = struct.unpack(">HHH", d[8:14])
    pos, tracks = 14, []

    def vlq(p):
        v = 0
        while True:
            b = d[p]
            p += 1
            v = (v << 7) | (b & 0x7F)
            if b < 0x80:
                return v, p

    for _ in range(ntracks):
        length = struct.unpack(">I", d[pos + 4:pos + 8])[0]
        p, end = pos + 8, pos + 8 + length
        pos = end
        tick, status, on, notes = 0, None, {}, []
        while p < end:
            dt, p = vlq(p)
            tick += dt
            if d[p] == 0xFF:
                n, p2 = vlq(p + 2)
                p = p2 + n
                continue
            if d[p] in (0xF0, 0xF7):
                n, p2 = vlq(p + 1)
                p = p2 + n
                continue
            if d[p] & 0x80:
                status = d[p]
                p += 1
            kind = status & 0xF0
            if kind in (0xC0, 0xD0):
                p += 1
                continue
            a, b = d[p], d[p + 1]
            p += 2
            if kind == 0x90 and b > 0:
                on[a] = tick
            elif kind in (0x80, 0x90) and a in on:
                notes.append((on.pop(a), tick, a))
        tracks.append(sorted(notes))
    return tracks, division


def bach_menuet(midi_path):
    tracks, division = read_midi_notes(midi_path)
    upper, lower = tracks[1], tracks[2]
    per_row = division // 4                 # sixteenth note rows
    bar = 12                                 # 3/4 time
    part_two = 16 * bar                      # the second repeat starts at bar 17
    total = max(n[1] for n in upper + lower) // per_row

    # the MIDI plays each half once; the score repeats both
    sections = [(0, part_two), (0, part_two), (part_two, total), (part_two, total)]

    song = Song()
    out = 0
    for first, last in sections:
        for channel, sample, notes in ((0, 3, upper), (1, 5, lower)):
            for start, end, midi in notes:
                r0, r1 = start // per_row, end // per_row
                if first <= r0 < last:
                    song.note(out + r0 - first, channel, sample, midi, max(1, r1 - r0))
        for r in range(first, last, 4):      # a soft hat on every beat
            song.note(out + r - first, 3, 8, 60, 1)
        out += last - first
    song.ensure(out)
    return song


def main():
    midi_path, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    write_mod(os.path.join(out_dir, "GAME_g_korobeiniki.mod"), "korobeiniki",
              korobeiniki(1, 2), speed=6, bpm=150)
    write_mod(os.path.join(out_dir, "GAME_h_korobeiniki_turbo.mod"), "korobeiniki turbo",
              korobeiniki(3, 2), speed=6, bpm=200)
    write_mod(os.path.join(out_dir, "GAME_i_bach_menuet.mod"), "bach menuet bwv 814",
              bach_menuet(midi_path), speed=6, bpm=140)


if __name__ == "__main__":
    main()
