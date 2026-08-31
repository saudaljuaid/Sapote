#!/usr/bin/env python3
"""Verify the deterministic Ring 3 native-audio QEMU WAV artifact."""

from __future__ import annotations

import argparse
import hashlib
import struct
import tempfile
import wave
from pathlib import Path


RATE = 48_000
CHANNELS = 2
SAMPLE_BYTES = 2
CHUNK_FRAMES = 1_024
MIN_CAPTURE_FRAMES = 256
MAX_CAPTURE_SECONDS = 180
EXPECTED_SHA256 = "5864c13557496ba86294adbbfe8078e9f2c0b5e808e4d0c4f49738fd465d1261"


class VerificationError(RuntimeError):
    """The capture did not contain the promised public-ABI waveform."""


def expected_mix() -> bytes:
    samples: list[int] = []
    for frame in range(CHUNK_FRAMES):
        first = 8_192 if (frame // 32) % 2 == 0 else -8_192
        second = 4_096 if (frame // 64) % 2 == 0 else -4_096
        samples.extend((first + second // 2, first // 2 + second))
    return struct.pack(f"<{len(samples)}h", *samples)


def canceled_mix() -> bytes:
    samples = [value for _ in range(CHUNK_FRAMES)
               for value in (30_000, -15_000)]
    return struct.pack(f"<{len(samples)}h", *samples)


def verify(path: Path) -> dict[str, int | str]:
    expected = expected_mix()
    digest = hashlib.sha256(expected).hexdigest()
    if digest != EXPECTED_SHA256:
        raise VerificationError("host waveform fixture hash drifted")
    try:
        with wave.open(str(path), "rb") as capture:
            channels = capture.getnchannels()
            width = capture.getsampwidth()
            rate = capture.getframerate()
            frames = capture.getnframes()
            compression = capture.getcomptype()
            payload = capture.readframes(frames)
    except (EOFError, OSError, wave.Error) as error:
        raise VerificationError(f"invalid WAV: {error}") from error
    if ((channels, width, rate, compression) !=
            (CHANNELS, SAMPLE_BYTES, RATE, "NONE")):
        raise VerificationError(
            f"format {rate}Hz/{channels}ch/{width * 8}bit/{compression}")
    if frames < MIN_CAPTURE_FRAMES or frames > RATE * MAX_CAPTURE_SECONDS:
        raise VerificationError(f"frame count out of bounds: {frames}")
    if len(payload) != frames * channels * width:
        raise VerificationError("WAV payload length disagrees with header")
    frame_bytes = channels * width
    offset = payload.find(expected)
    if offset >= 0 and offset % frame_bytes == 0:
        matched = expected
        complete = "yes"
    elif frames < CHUNK_FRAMES and expected.startswith(payload):
        # QEMU's HDA codec has a staging ring in front of the WAV backend.
        # Stopping the completed controller stream deactivates that ring, so
        # the backend can persist only the exact prefix already delivered by
        # its timer.  The serial proof independently requires the complete
        # 1,024-frame DMA drain; this branch authenticates the audible bytes.
        offset = 0
        matched = payload
        complete = "no"
    else:
        raise VerificationError("deterministic mixed waveform not found")
    matched_digest = hashlib.sha256(matched).hexdigest()
    if complete == "yes" and matched_digest != EXPECTED_SHA256:
        raise VerificationError("captured mixed waveform hash changed")
    if canceled_mix() in payload:
        raise VerificationError("canceled chunk reached the WAV backend")
    nonzero = sum(sample != 0 for (sample,) in
                  struct.iter_unpack("<h", matched))
    if nonzero < len(matched) // frame_bytes:
        raise VerificationError("matched waveform is unexpectedly silent")
    return {
        "frames": frames,
        "duration_ns": frames * 1_000_000_000 // rate,
        "nonzero_samples": nonzero,
        "match_frame": offset // frame_bytes,
        "matched_frames": len(matched) // frame_bytes,
        "complete_chunk": complete,
        "matched_sha256": matched_digest,
        "fixture_sha256": EXPECTED_SHA256,
    }


def self_test() -> None:
    prefix = bytes(37 * CHANNELS * SAMPLE_BYTES)
    suffix = bytes(19 * CHANNELS * SAMPLE_BYTES)
    with tempfile.TemporaryDirectory(prefix="sapote-audio-") as directory:
        root = Path(directory)
        fixture = root / "proof.wav"
        with wave.open(str(fixture), "wb") as output:
            output.setnchannels(CHANNELS)
            output.setsampwidth(SAMPLE_BYTES)
            output.setframerate(RATE)
            output.writeframes(prefix + expected_mix() + suffix)
        report = verify(fixture)
        if report["frames"] != 37 + CHUNK_FRAMES + 19 or \
                report["match_frame"] != 37 or \
                report["complete_chunk"] != "yes":
            raise VerificationError("self-test fixture geometry changed")
        partial = root / "partial.wav"
        with wave.open(str(partial), "wb") as output:
            output.setnchannels(CHANNELS)
            output.setsampwidth(SAMPLE_BYTES)
            output.setframerate(RATE)
            output.writeframes(expected_mix()[:437 * CHANNELS * SAMPLE_BYTES])
        partial_report = verify(partial)
        if partial_report["matched_frames"] != 437 or \
                partial_report["complete_chunk"] != "no":
            raise VerificationError("partial fixture geometry changed")
        canceled = root / "canceled.wav"
        with wave.open(str(canceled), "wb") as output:
            output.setnchannels(CHANNELS)
            output.setsampwidth(SAMPLE_BYTES)
            output.setframerate(RATE)
            output.writeframes(expected_mix() + canceled_mix())
        wrong_rate = root / "wrong-rate.wav"
        with wave.open(str(wrong_rate), "wb") as output:
            output.setnchannels(CHANNELS)
            output.setsampwidth(SAMPLE_BYTES)
            output.setframerate(44_100)
            output.writeframes(expected_mix())
        too_short = root / "too-short.wav"
        with wave.open(str(too_short), "wb") as output:
            output.setnchannels(CHANNELS)
            output.setsampwidth(SAMPLE_BYTES)
            output.setframerate(RATE)
            output.writeframes(expected_mix()[:
                (MIN_CAPTURE_FRAMES - 1) * CHANNELS * SAMPLE_BYTES])
        corrupted = root / "corrupted.wav"
        corrupt_payload = bytearray(
            expected_mix()[:437 * CHANNELS * SAMPLE_BYTES])
        corrupt_payload[128] ^= 0x01
        with wave.open(str(corrupted), "wb") as output:
            output.setnchannels(CHANNELS)
            output.setsampwidth(SAMPLE_BYTES)
            output.setframerate(RATE)
            output.writeframes(corrupt_payload)
        refused = 0
        for invalid in (canceled, wrong_rate, too_short, corrupted):
            try:
                verify(invalid)
            except VerificationError:
                refused += 1
        if refused != 4:
            raise VerificationError("negative WAV controls were accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path, nargs="?")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        print("Sapote audio WAV host self-test passed")
        return 0
    if arguments.wav is None:
        parser.error("wav is required without --self-test")
    try:
        report = verify(arguments.wav)
    except VerificationError as error:
        print(f"Sapote audio WAV refused: {error}")
        return 1
    print("SAPOTE AUDIO WAV PASS " + " ".join(
        f"{name}={value}" for name, value in report.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
