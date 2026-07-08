#!/usr/bin/env python3
"""Convert PNG to RGB565 C array for 284x76 TFT display."""
from pathlib import Path
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: PIL not installed. Run: pip install Pillow")
    sys.exit(1)

PROJECT_DIR = Path(__file__).resolve().parent
PNG_PATH = PROJECT_DIR / "background.png"
OUT_PATH = PROJECT_DIR / "main" / "boards" / "zhimu-2.25tft-wifi" / "bg_image.c"
NAME = "bg_image"
WIDTH, HEIGHT = 284, 76

def main():
    img = Image.open(PNG_PATH)
    print(f"Input: {img.size}, mode={img.mode}")

    # Resize to 284x76 (LANCZOS for quality)
    img = img.resize((WIDTH, HEIGHT), Image.LANCZOS)
    img = img.convert("RGB")
    print(f"Resized: {img.size}")

    pixels = list(img.getdata())

    # Convert to RGB565
    data = []
    for r, g, b in pixels:
        rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        data.append(rgb565 & 0xFF)       # low byte
        data.append((rgb565 >> 8) & 0xFF) # high byte

    # Write C file
    with open(OUT_PATH, "w", encoding="utf-8") as f:
        f.write(f'#include "lvgl.h"\n\n')
        f.write(f'static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {NAME}_map[] = {{\n')

        for i, byte in enumerate(data):
            if i % 16 == 0:
                f.write("\n    ")
            f.write(f"0x{byte:02x}, ")

        f.write(f'\n}};\n\n')
        f.write(f'const lv_image_dsc_t {NAME} = {{\n')
        f.write(f'  .header = {{\n')
        f.write(f'    .magic = LV_IMAGE_HEADER_MAGIC,\n')
        f.write(f'    .cf = LV_COLOR_FORMAT_RGB565,\n')
        f.write(f'    .flags = 0,\n')
        f.write(f'    .w = {WIDTH},\n')
        f.write(f'    .h = {HEIGHT},\n')
        f.write(f'    .stride = {WIDTH * 2},\n')
        f.write(f'    .reserved_2 = 0,\n')
        f.write(f'  }},\n')
        f.write(f'  .data_size = sizeof({NAME}_map),\n')
        f.write(f'  .data = {NAME}_map,\n')
        f.write(f'  .reserved = NULL,\n')
        f.write(f'}};\n')

    size_kb = len(data) / 1024
    print(f"Output: {OUT_PATH} ({size_kb:.1f} KB)")
    print("Done!")

if __name__ == "__main__":
    main()
