"""Offline CarLife head-unit demo with a simulated phone and real H.264."""

import argparse
import shutil
import struct
import subprocess
from io import BytesIO
from pathlib import Path
from tempfile import mkdtemp

HU_VERSION, VERSION_MATCH = 0x18001, 0x10002
HU_INFO, VIDEO_INIT, VIDEO_INIT_DONE = 0x18003, 0x18007, 0x10008
VIDEO_START, VIDEO_DATA, TOUCH = 0x18009, 0x20001, 0x68001
MAX_FRAME = 4 * 1024 * 1024

# Proto2 payloads for the simulated peer. Video mode is 640 × 360 at 30 fps.
VERSION = bytes.fromhex("08 01 10 00")
ACCEPTED = bytes.fromhex("08 01")
DEVICE_INFO = b"\x0a\x05Linux\x22\x0cNBT Evo Demo"
VIDEO_MODE = bytes.fromhex("08 80 05 10 e8 02 18 1e")
TOUCH_DOWN = bytes.fromhex("08 00 10 c0 02 18 b4 01")
TOUCH_UP = bytes.fromhex("08 01 10 c0 02 18 b4 01")


def send(wire, message, payload=b"", timestamp=0):
    channel = message >> 16
    if channel == 2:
        header = struct.pack(">III", len(payload), timestamp, message)
    elif channel in (1, 6):
        header = struct.pack(">HHI", len(payload), 0, message)
    else:
        raise ValueError(f"unsupported demo channel {channel}")

    length = len(header) + len(payload)
    if length > MAX_FRAME:
        raise ValueError("frame exceeds demo limit")
    wire.write(struct.pack(">II", channel, length))
    wire.write(header)
    wire.write(payload)


def read_exact(wire, length):
    # Deliberately fragment reads to exercise stream framing.
    data = bytearray()
    while len(data) < length:
        chunk = wire.read(min(31, length - len(data)))
        if not chunk:
            raise ValueError("truncated frame")
        data.extend(chunk)
    return bytes(data)


def receive(wire, expected_message):
    channel, length = struct.unpack(">II", read_exact(wire, 8))
    header_size = 12 if channel == 2 else 8
    if channel != expected_message >> 16 or not header_size <= length <= MAX_FRAME:
        raise ValueError("unexpected channel or invalid frame size")

    header = read_exact(wire, header_size)
    if channel == 2:
        payload_size, _, message = struct.unpack(">III", header)
    else:
        payload_size, reserved, message = struct.unpack(">HHI", header)
        if reserved:
            raise ValueError("nonzero reserved field")

    if message != expected_message or payload_size != length - header_size:
        raise ValueError(f"invalid frame for message {expected_message:#07x}")
    return read_exact(wire, payload_size)


def exchange(message, payload=b"", timestamp=0):
    wire = BytesIO()
    send(wire, message, payload, timestamp)
    wire.seek(0)
    return receive(wire, message)


def validate(received, expected):
    if received != expected:
        raise ValueError("unexpected negotiation or touch payload")


def run(ffmpeg, output):
    print("CarLife HU demo | simulated phone, offline", flush=True)
    validate(exchange(HU_VERSION, VERSION), VERSION)
    validate(exchange(VERSION_MATCH, ACCEPTED), ACCEPTED)
    print("HU → MD  version 1.0  |  MD → HU  accepted")

    validate(exchange(HU_INFO, DEVICE_INFO), DEVICE_INFO)
    validate(exchange(VIDEO_INIT, VIDEO_MODE), VIDEO_MODE)
    validate(exchange(VIDEO_INIT_DONE, VIDEO_MODE), VIDEO_MODE)
    validate(exchange(VIDEO_START), b"")
    print("HU ↔ MD  video negotiated and started  |  640 × 360 @ 30 fps")

    common = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    generated = subprocess.check_output(common + [
        "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30",
        "-frames:v", "1", "-c:v", "libx264", "-preset", "ultrafast",
        "-f", "h264", "pipe:1",
    ])
    video = exchange(VIDEO_DATA, generated, timestamp=33)
    if not video:
        raise ValueError("empty video frame")
    preview = output / "first-frame.png"
    subprocess.run(common + [
        "-n", "-f", "h264", "-i", "pipe:0", "-frames:v", "1",
        "-update", "1", str(preview),
    ], input=video, check=True)
    print(f"MD → HU  decoded H.264 first frame  |  {len(video)} bytes")

    validate(exchange(TOUCH, TOUCH_DOWN), TOUCH_DOWN)
    validate(exchange(TOUCH, TOUCH_UP), TOUCH_UP)
    print("HU → MD  touch DOWN / UP  |  x=320 y=180")
    print(f"Preview: {preview.resolve()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="new directory for the decoded PNG")
    args = parser.parse_args()
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        parser.error("ffmpeg with libx264 is required")

    try:
        if args.output is None:
            output = Path(mkdtemp(prefix="carlife-demo-"))
        else:
            output = args.output
            output.mkdir()
        run(ffmpeg, output)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Demo failed: {error}\n")


if __name__ == "__main__":
    main()
