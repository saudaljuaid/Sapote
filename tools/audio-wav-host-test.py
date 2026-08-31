#!/usr/bin/env python3
"""Verify deterministic Ring 3 native-audio and SDL QEMU WAV artifacts."""

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
SDL_FRAMES = 4_096
SDL_CHUNKS = SDL_FRAMES // CHUNK_FRAMES
SDL_PROOF_RUNS = 2
SDL_EXPECTED_SHA256 = "5f69c57a2899917978544f1c8ed66b6f826c7e67b8302d281ea206f40852ef56"


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


def expected_sdl() -> bytes:
    samples: list[int] = []
    for frame in range(SDL_FRAMES):
        sample = 7_000 if (frame // 48) % 2 == 0 else -7_000
        samples.extend((sample, sample // 2))
    return struct.pack(f"<{len(samples)}h", *samples)


def read_wave(path: Path) -> tuple[int, bytes]:
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
    return frames, payload


def verify_native_audio(frames: int, payload: bytes) -> dict[str, int | str]:
    expected = expected_mix()
    frame_bytes = CHANNELS * SAMPLE_BYTES
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
        "duration_ns": frames * 1_000_000_000 // RATE,
        "nonzero_samples": nonzero,
        "match_frame": offset // frame_bytes,
        "matched_frames": len(matched) // frame_bytes,
        "complete_chunk": complete,
        "matched_sha256": matched_digest,
        "fixture_sha256": EXPECTED_SHA256,
    }


def nonzero_runs(payload: bytes) -> list[bytes]:
    frame_bytes = CHANNELS * SAMPLE_BYTES
    silence = bytes(frame_bytes)
    runs: list[bytes] = []
    start: int | None = None

    for offset in range(0, len(payload), frame_bytes):
        if payload[offset:offset + frame_bytes] != silence:
            if start is None:
                start = offset
        elif start is not None:
            runs.append(payload[start:offset])
            start = None
    if start is not None:
        runs.append(payload[start:])
    return runs


def match_sdl_run(payload: bytes, chunks: list[bytes]) -> int:
    frame_bytes = CHANNELS * SAMPLE_BYTES
    positions = {0}

    for segment in range(SDL_CHUNKS * SDL_PROOF_RUNS):
        chunk = chunks[segment % SDL_CHUNKS]
        next_positions: set[int] = set()
        for position in positions:
            remaining_frames = (len(payload) - position) // frame_bytes
            limit = min(CHUNK_FRAMES, remaining_frames)
            matched = 0
            while matched < limit:
                start = matched * frame_bytes
                if payload[position + start:
                        position + start + frame_bytes] != \
                        chunk[start:start + frame_bytes]:
                    break
                matched += 1
            for frames in range(MIN_CAPTURE_FRAMES, matched + 1):
                next_positions.add(position + frames * frame_bytes)
        positions = next_positions
        if len(payload) in positions and (segment + 1) % SDL_CHUNKS == 0:
            return segment + 1
        if not positions:
            break
    return 0


def verify_sdl(frames: int, payload: bytes) -> dict[str, int | str]:
    expected = expected_sdl()
    digest = hashlib.sha256(expected).hexdigest()
    if digest != SDL_EXPECTED_SHA256:
        raise VerificationError("SDL host waveform fixture hash drifted")
    chunks = [expected[offset:offset + CHUNK_FRAMES * CHANNELS * SAMPLE_BYTES]
              for offset in range(0, len(expected),
                                  CHUNK_FRAMES * CHANNELS * SAMPLE_BYTES)]
    runs = nonzero_runs(payload)
    segment_counts = [match_sdl_run(run, chunks) for run in runs]
    if not runs or any(count == 0 for count in segment_counts):
        raise VerificationError("unrecognized SDL non-silent run")
    segments = sum(segment_counts)
    required = SDL_CHUNKS * SDL_PROOF_RUNS
    if segments != required:
        raise VerificationError(
            f"SDL waveform segments {segments}, expected {required}")
    matched = b"".join(runs)
    nonzero = sum(sample != 0 for (sample,) in
                  struct.iter_unpack("<h", matched))
    if nonzero < required * MIN_CAPTURE_FRAMES:
        raise VerificationError("SDL matched waveform is unexpectedly silent")
    return {
        "frames": frames,
        "duration_ns": frames * 1_000_000_000 // RATE,
        "audio_frames": len(matched) // (CHANNELS * SAMPLE_BYTES),
        "segments": segments,
        "runs": len(runs),
        "nonzero_samples": nonzero,
        "matched_sha256": hashlib.sha256(matched).hexdigest(),
        "fixture_sha256": SDL_EXPECTED_SHA256,
    }


def verify(path: Path, profile: str = "native-audio") -> dict[str, int | str]:
    frames, payload = read_wave(path)
    if profile == "native-audio":
        return verify_native_audio(frames, payload)
    if profile == "sdl":
        return verify_sdl(frames, payload)
    raise VerificationError(f"unknown WAV profile: {profile}")


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
        sdl = expected_sdl()
        sdl_fixture = root / "sdl.wav"
        prefixes = (440, 439, 440, 439, 438, 441, 437, 442)
        chunks = [sdl[offset:offset + CHUNK_FRAMES * CHANNELS * SAMPLE_BYTES]
                  for offset in range(0, len(sdl),
                                      CHUNK_FRAMES * CHANNELS * SAMPLE_BYTES)]
        sdl_payload = bytes(31 * CHANNELS * SAMPLE_BYTES)
        for index, frames in enumerate(prefixes):
            if index == SDL_CHUNKS:
                sdl_payload += bytes(23 * CHANNELS * SAMPLE_BYTES)
            sdl_payload += chunks[index % SDL_CHUNKS][:
                frames * CHANNELS * SAMPLE_BYTES]
        sdl_payload += bytes(17 * CHANNELS * SAMPLE_BYTES)
        with wave.open(str(sdl_fixture), "wb") as output:
            output.setnchannels(CHANNELS)
            output.setsampwidth(SAMPLE_BYTES)
            output.setframerate(RATE)
            output.writeframes(sdl_payload)
        sdl_report = verify(sdl_fixture, "sdl")
        if sdl_report["segments"] != SDL_CHUNKS * SDL_PROOF_RUNS or \
                sdl_report["runs"] != SDL_PROOF_RUNS:
            raise VerificationError("SDL fixture geometry changed")
        try:
            verify(sdl_fixture, "native-audio")
        except VerificationError:
            pass
        else:
            raise VerificationError("SDL fixture passed the native profile")
        invalid_sdl = {
            "one-run": bytes(31 * CHANNELS * SAMPLE_BYTES) + b"".join(
                chunks[index][:prefixes[index] * CHANNELS * SAMPLE_BYTES]
                for index in range(SDL_CHUNKS)),
            "corrupted": bytearray(sdl_payload),
        }
        invalid_sdl["corrupted"][
            31 * CHANNELS * SAMPLE_BYTES + 128] ^= 0x01
        sdl_refused = 0
        for name, invalid_payload in invalid_sdl.items():
            invalid_path = root / f"sdl-{name}.wav"
            with wave.open(str(invalid_path), "wb") as output:
                output.setnchannels(CHANNELS)
                output.setsampwidth(SAMPLE_BYTES)
                output.setframerate(RATE)
                output.writeframes(invalid_payload)
            try:
                verify(invalid_path, "sdl")
            except VerificationError:
                sdl_refused += 1
        if sdl_refused != len(invalid_sdl):
            raise VerificationError("negative SDL WAV controls were accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path, nargs="?")
    parser.add_argument("--profile", choices=("native-audio", "sdl"),
                        default="native-audio")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        print("Sapote audio WAV host self-test passed")
        return 0
    if arguments.wav is None:
        parser.error("wav is required without --self-test")
    try:
        report = verify(arguments.wav, arguments.profile)
    except VerificationError as error:
        print(f"Sapote audio WAV refused: {error}")
        return 1
    label = "AUDIO" if arguments.profile == "native-audio" else "SDL"
    print(f"SAPOTE {label} WAV PASS " + " ".join(
        f"{name}={value}" for name, value in report.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
