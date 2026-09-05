import re
from pathlib import Path


def expand_5(value):
    return (value << 3) | (value >> 2)


def expand_6(value):
    return (value << 2) | (value >> 4)


def convert(path):
    content = path.read_text(encoding="utf-8")
    if "LV_IMAGE_HEADER_MAGIC" in content:
        return False

    data_match = re.search(r"(\w+)_data\[\]\s*=\s*\{(.*?)\};", content, re.DOTALL)
    width_match = re.search(r"\.header\.w\s*=\s*(\d+)", content)
    height_match = re.search(r"\.header\.h\s*=\s*(\d+)", content)
    if not data_match or not width_match or not height_match:
        raise ValueError(f"Cannot parse {path.name}")

    name = data_match.group(1)
    width = int(width_match.group(1))
    height = int(height_match.group(1))
    source = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", data_match.group(2))]
    has_alpha = "LV_IMG_CF_TRUE_COLOR_ALPHA" in content
    source_bpp = 3 if has_alpha else 2
    expected = width * height * source_bpp
    if len(source) != expected:
        raise ValueError(f"{path.name}: expected {expected} bytes, found {len(source)}")

    rgba = bytearray()
    for offset in range(0, len(source), source_bpp):
        # The LVGL 8 project stored wire-order BGR565 followed by alpha.
        pixel = (source[offset] << 8) | source[offset + 1]
        blue = expand_5((pixel >> 11) & 0x1F)
        green = expand_6((pixel >> 5) & 0x3F)
        red = expand_5(pixel & 0x1F)
        alpha = source[offset + 2] if has_alpha else 0xFF
        rgba.extend((red, green, blue, alpha))

    rows = []
    for offset in range(0, len(rgba), 16):
        rows.append("    " + ", ".join(f"0x{value:02X}" for value in rgba[offset:offset + 16]) + ",")

    output = f'''#include "lvgl.h"

const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_data[] = {{
{chr(10).join(rows)}
}};

const lv_image_dsc_t {name} = {{
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.flags = 0,
    .header.w = {width},
    .header.h = {height},
    .header.stride = {width * 4},
    .data_size = sizeof({name}_data),
    .data = {name}_data,
}};
'''
    path.write_text(output, encoding="utf-8", newline="\n")
    print(f"{path.name}: {width}x{height} -> ARGB8888")
    return True


if __name__ == "__main__":
    image_dir = Path(__file__).resolve().parent / "ui_ref"
    converted = sum(convert(path) for path in sorted(image_dir.glob("ui_img_*.c")))
    print(f"Converted {converted} image files")
