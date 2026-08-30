#!/usr/bin/env python3
"""Convert Quick Look RGBA PNG thumbnails into LVGL alpha-only image C data."""

import argparse
import struct
import zlib
from pathlib import Path


def read_png(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos = 8
    width = height = bit_depth = color_type = None
    compressed = bytearray()
    while pos < len(data):
        length = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", body
            )
            if bit_depth != 8 or compression != 0 or filtering != 0 or interlace != 0:
                raise ValueError(f"unsupported PNG format: {path}")
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
    if color_type not in (2, 6):
        raise ValueError(f"expected RGB/RGBA PNG: {path}")
    raw = zlib.decompress(compressed)
    channels = 4 if color_type == 6 else 3
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    cursor = 0
    for _ in range(height):
        filter_type = raw[cursor]
        cursor += 1
        encoded = raw[cursor : cursor + stride]
        cursor += stride
        row = bytearray(stride)
        for i, value in enumerate(encoded):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if filter_type == 0:
                result = value
            elif filter_type == 1:
                result = (value + left) & 0xFF
            elif filter_type == 2:
                result = (value + up) & 0xFF
            elif filter_type == 3:
                result = (value + ((left + up) // 2)) & 0xFF
            elif filter_type == 4:
                estimate = left + up - upper_left
                pa = abs(estimate - left)
                pb = abs(estimate - up)
                pc = abs(estimate - upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
                result = (value + predictor) & 0xFF
            else:
                raise ValueError(f"unsupported PNG filter {filter_type}: {path}")
            row[i] = result
        rows.append(row)
        previous = row
    pixels = []
    for row in rows:
        for offset in range(0, len(row), channels):
            rgb = tuple(row[offset : offset + 3])
            source_alpha = row[offset + 3] if channels == 4 else 255
            pixels.append((*rgb, source_alpha))
    return width, height, pixels


def trim_alpha(width, height, alpha):
    nonzero = [(x, y) for y in range(height) for x in range(width) if alpha[y * width + x]]
    if not nonzero:
        return 1, 1, [0]
    min_x = min(x for x, _ in nonzero)
    max_x = max(x for x, _ in nonzero)
    min_y = min(y for _, y in nonzero)
    max_y = max(y for _, y in nonzero)
    trimmed = []
    for y in range(min_y, max_y + 1):
        trimmed.extend(alpha[y * width + min_x : y * width + max_x + 1])
    return max_x - min_x + 1, max_y - min_y + 1, trimmed


def infer_alpha(pixels, foreground, opacity):
    if foreground is None:
        return [pixel[3] for pixel in pixels]
    result = []
    for red, green, blue, source_alpha in pixels:
        channels = []
        for value, target in zip((red, green, blue), foreground):
            difference = 255 - target
            if difference:
                channels.append((255 - value) * 255 / difference / opacity)
        inferred = max(channels) if channels else 255
        result.append(max(0, min(255, round(inferred * source_alpha / 255))))
    return result


def crop_pixels(width, height, pixels, crop_width, crop_height):
    if crop_width > width or crop_height > height:
        raise ValueError("crop is larger than source PNG")
    cropped = []
    for y in range(crop_height):
        cropped.extend(pixels[y * width : y * width + crop_width])
    return crop_width, crop_height, cropped


def write_image(out, symbol, width, height, alpha):
    out.write(f"const lv_img_dsc_t {symbol} = {{\n")
    out.write("    .header = {\n")
    out.write("        .cf = LV_IMG_CF_ALPHA_8BIT,\n")
    out.write("        .always_zero = 0,\n        .reserved = 0,\n")
    out.write(f"        .w = {width},\n        .h = {height},\n")
    out.write("    },\n")
    out.write(f"    .data_size = sizeof({symbol}_data),\n")
    out.write(f"    .data = {symbol}_data,\n}};\n\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("images", nargs="+")
    parser.add_argument("--trim", action="append", default=[])
    parser.add_argument("--foreground", action="append", default=[])
    parser.add_argument("--opacity", action="append", default=[])
    parser.add_argument("--crop", action="append", default=[])
    args = parser.parse_args()
    foregrounds = {}
    for spec in args.foreground:
        symbol, value = spec.split("=", 1)
        foregrounds[symbol] = tuple(int(value[index : index + 2], 16) for index in (0, 2, 4))
    opacities = {symbol: float(value) for symbol, value in (item.split("=", 1) for item in args.opacity)}
    crops = {}
    for spec in args.crop:
        symbol, size = spec.split("=", 1)
        crop_width, crop_height = (int(value) for value in size.lower().split("x", 1))
        crops[symbol] = (crop_width, crop_height)
    images = []
    for spec in args.images:
        symbol, raw_path = spec.split("=", 1)
        width, height, pixels = read_png(Path(raw_path))
        if symbol in crops:
            width, height, pixels = crop_pixels(width, height, pixels, *crops[symbol])
        alpha = infer_alpha(pixels, foregrounds.get(symbol), opacities.get(symbol, 1.0))
        if symbol in args.trim:
            width, height, alpha = trim_alpha(width, height, alpha)
        images.append((symbol, width, height, alpha))
    with args.output.open("w", encoding="utf-8") as out:
        out.write('#include "lvgl.h"\n\n')
        for symbol, _, _, alpha in images:
            out.write(f"static const uint8_t {symbol}_data[] = {{\n")
            for offset in range(0, len(alpha), 16):
                row = alpha[offset : offset + 16]
                out.write("    " + ", ".join(str(value) for value in row) + ",\n")
            out.write("};\n\n")
        for symbol, width, height, alpha in images:
            write_image(out, symbol, width, height, alpha)


if __name__ == "__main__":
    main()
