#!/usr/bin/env python3
"""View StudyBoard raw BGR24 video sent by studyboard-ncnn-face-lcd.

Run on the Windows PC, not on the Zynq board. Requires numpy and a desktop
OpenCV build (the existing Ultralytics training environment normally has both).
"""

import argparse
import socket
import struct
import time


HEADER = struct.Struct("!4sHH4sII")
MAX_FRAME_BYTES = 1920 * 1080 * 3


def read_exact(connection, size):
    data = bytearray(size)
    view = memoryview(data)
    received = 0
    while received < size:
        count = connection.recv_into(view[received:])
        if count == 0:
            raise EOFError("board disconnected")
        received += count
    return data


def read_frame(connection):
    magic, width, height, fourcc, sequence, length = HEADER.unpack(
        read_exact(connection, HEADER.size)
    )
    if magic != b"SBV1" or fourcc != b"BGR3":
        raise ValueError("unexpected video protocol or pixel format")
    if not width or not height or length != width * height * 3:
        raise ValueError("invalid frame dimensions or length")
    if length > MAX_FRAME_BYTES:
        raise ValueError("frame is too large")
    return width, height, sequence, read_exact(connection, length)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.10.2", help="board IPv4 address")
    parser.add_argument("--port", type=int, default=5000)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be 1..65535")

    try:
        import cv2
        import numpy as np
    except ImportError as exc:
        parser.error(f"install numpy and desktop opencv-python: {exc}")

    cv2.namedWindow("StudyBoard camera (Q or Esc to quit)", cv2.WINDOW_NORMAL)
    frames = 0
    since = time.monotonic()
    while True:
        try:
            print(f"Connecting to {args.host}:{args.port} ...", flush=True)
            with socket.create_connection((args.host, args.port), timeout=3) as connection:
                connection.settimeout(3)
                print("Connected. Q or Esc closes the viewer.", flush=True)
                while True:
                    width, height, sequence, payload = read_frame(connection)
                    image = np.frombuffer(payload, dtype=np.uint8).reshape(
                        (height, width, 3)
                    )
                    cv2.imshow("StudyBoard camera (Q or Esc to quit)", image)
                    frames += 1
                    now = time.monotonic()
                    if now - since >= 5:
                        print(
                            f"PC display: {frames / (now - since):.2f} fps, "
                            f"last source sequence={sequence}",
                            flush=True,
                        )
                        frames = 0
                        since = now
                    if cv2.waitKey(1) & 0xFF in (ord("q"), 27):
                        return
        except (EOFError, OSError, ValueError) as exc:
            print(f"Video connection: {exc}; retrying in 2 seconds", flush=True)
            if cv2.waitKey(1) & 0xFF in (ord("q"), 27):
                return
            time.sleep(2)
        except KeyboardInterrupt:
            return


if __name__ == "__main__":
    main()
