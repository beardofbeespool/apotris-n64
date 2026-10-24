#!/usr/bin/env python3
"""Bring a rendered song to a common loudness, in place.

Usage: n64_level_wav.py <song.wav>

Every song ends up with the same average (RMS) level, so the music does not
jump in volume between tracks, and its peaks stay well below full scale,
leaving room for the sound effects mixed on top (without that, loud songs
clipped as soon as an effect played).
"""

import array
import math
import sys
import wave

TARGET_RMS_DB = -21.0
PEAK_LIMIT_DB = -1.5


def main():
    path = sys.argv[1]
    with wave.open(path, "rb") as w:
        params = w.getparams()
        assert params.sampwidth == 2, "16 bit WAV expected"
        samples = array.array("h", w.readframes(params.nframes))
    if sys.byteorder == "big":
        samples.byteswap()

    peak = max(1, max(samples), -min(samples))
    rms = math.sqrt(sum(v * v for v in samples) / max(1, len(samples))) or 1.0

    rms_db = 20 * math.log10(rms / 32768)
    peak_db = 20 * math.log10(peak / 32768)
    gain_db = min(TARGET_RMS_DB - rms_db, PEAK_LIMIT_DB - peak_db)
    gain = 10 ** (gain_db / 20)

    out = array.array("h", (max(-32768, min(32767, round(v * gain))) for v in samples))
    if sys.byteorder == "big":
        out.byteswap()
    with wave.open(path, "wb") as w:
        w.setparams(params)
        w.writeframes(out.tobytes())
    print(f"{path}: {gain_db:+.1f} dB")


if __name__ == "__main__":
    main()
