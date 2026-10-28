#!/usr/bin/env python3
"""Convert a tightly packed 24-bit StudyBoard frame to a PPM image.

The kernel snapshot is deliberately exported as raw bytes: it contains no
width, height or colour-order header.  This small host-side teaching tool adds
a PPM header and, when requested, swaps the first and third byte of every
pixel.  It uses only the Python standard library.

Examples:
    python3 raw_to_ppm.py frame0.raw frame0.ppm
    python3 raw_to_ppm.py frame0.raw frame0-rgb-diagnostic.ppm --order rgb
"""

import argparse
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="convert packed RGB888/BGR888 raw pixels to binary PPM"
    )
    parser.add_argument("input", type=Path, help="input raw frame")
    parser.add_argument("output", type=Path, help="output PPM image")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument(
        "--order",
        choices=("rgb", "bgr"),
        default="bgr",
        help="byte order in the input file (default: bgr, measured on this design)",
    )
    parser.add_argument(
        "--flip-y",
        action="store_true",
        help="reverse the row order if the captured image is upside down",
    )
    return parser.parse_args()


def convert_pixels(
    source: bytes, width: int, height: int, order: str, flip_y: bool
) -> bytes:
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")

    row_bytes = width * 3
    expected_bytes = row_bytes * height
    if len(source) != expected_bytes:
        raise ValueError(
            f"input has {len(source)} bytes, expected {expected_bytes} "
            f"for {width}x{height}x3"
        )

    pixels = bytearray(source)

    # PPM P6 always stores R, G, B.  Convert B,G,R input one pixel at a time.
    if order == "bgr":
        for offset in range(0, len(pixels), 3):
            pixels[offset], pixels[offset + 2] = (
                pixels[offset + 2],
                pixels[offset],
            )

    if flip_y:
        flipped = bytearray(len(pixels))
        for output_row in range(height):
            input_row = height - 1 - output_row
            source_start = input_row * row_bytes
            output_start = output_row * row_bytes
            flipped[output_start : output_start + row_bytes] = pixels[
                source_start : source_start + row_bytes
            ]
        pixels = flipped

    return bytes(pixels)


def main() -> int:
    args = parse_args()
    source = args.input.read_bytes()
    pixels = convert_pixels(
        source, args.width, args.height, args.order, args.flip_y
    )

    # P6 is the binary PPM variant: ASCII header followed by raw RGB bytes.
    header = f"P6\n{args.width} {args.height}\n255\n".encode("ascii")
    args.output.write_bytes(header + pixels)

    print(
        f"wrote {args.output}: {args.width}x{args.height}, "
        f"input-order={args.order}, flip-y={args.flip_y}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
