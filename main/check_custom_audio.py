#!/usr/bin/env python3
import sys
import wave
from pathlib import Path


MAX_SIZE = 700_000


def fail(message: str) -> None:
    print(f"custom_audio.wav error: {message}", file=sys.stderr)
    print(
        "Please use a short WAV file: 16-bit PCM, mono/stereo, 8-48 kHz, "
        "preferably mono 16 kHz and under 700 KB.",
        file=sys.stderr,
    )
    raise SystemExit(1)


def main() -> None:
    if len(sys.argv) != 2:
        fail("missing audio file path")

    audio_file = Path(sys.argv[1])
    if not audio_file.exists():
        fail(f"{audio_file} does not exist")

    size = audio_file.stat().st_size
    if size > MAX_SIZE:
        fail(f"{audio_file} is too large ({size} bytes)")

    try:
        with wave.open(str(audio_file), "rb") as wav:
            channels = wav.getnchannels()
            sample_width = wav.getsampwidth()
            sample_rate = wav.getframerate()
    except wave.Error as exc:
        fail(f"invalid WAV file: {exc}")

    if sample_width != 2:
        fail(f"unsupported sample width: {sample_width * 8} bit")
    if channels not in (1, 2):
        fail(f"unsupported channel count: {channels}")
    if sample_rate < 8000 or sample_rate > 48000:
        fail(f"unsupported sample rate: {sample_rate} Hz")


if __name__ == "__main__":
    main()
