
#!/usr/bin/env python3

### creates artwork.h from woman.jpg and clothes*.png files
### files are scaled down to 32% of original size and converted to RGB565



from PIL import Image
from pathlib import Path
import re

SCALE = 0.32
TRANSPARENT_COLOR = 0xF81F  # RGB565 magenta

OUTPUT = "artwork.h"


def rgb565(r, g, b):
    """Convert 8-bit RGB to 16-bit RGB565."""
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    # Avoid accidentally using the transparency color.
    if value == TRANSPARENT_COLOR:
        value = 0xF81E

    return value


def load_image(filename, use_alpha=False):
    img = Image.open(filename)

    new_width = max(1, round(img.width * SCALE))
    new_height = max(1, round(img.height * SCALE))

    img = img.resize(
        (new_width, new_height),
        Image.Resampling.LANCZOS
    )

    if use_alpha:
        img = img.convert("RGBA")
    else:
        img = img.convert("RGB")

    pixels = []

    for pixel in img.getdata():

        if use_alpha:
            r, g, b, a = pixel

            if a < 128:
                pixels.append(TRANSPARENT_COLOR)
                continue
        else:
            r, g, b = pixel

        pixels.append(rgb565(r, g, b))

    return new_width, new_height, pixels


def format_bitmap(values):
    lines = []

    for i in range(0, len(values), 12):
        chunk = values[i:i + 12]
        lines.append(
            "    " + ", ".join(f"0x{v:04X}" for v in chunk)
        )

    return ",\n".join(lines)


def natural_sort_key(path):
    return [
        int(part) if part.isdigit() else part.lower()
        for part in re.split(r"(\d+)", path.name)
    ]


def main():

    files = sorted(
        Path(".").glob("clothes*.png"),
        key=natural_sort_key
    )

    if not files:
        print("No clothes*.png files found!")
        return

    print("Generating artwork.h")

    with open(OUTPUT, "w") as out:

        out.write("// Automatically generated artwork\n")
        out.write("// RGB565 bitmaps stored in program memory\n\n")

        out.write("#pragma once\n")
        out.write("#include <Arduino.h>\n\n")

        out.write(f"#define ARTWORK_SCALE {SCALE}f\n")
        out.write(
            f"#define ARTWORK_TRANSPARENT 0x{TRANSPARENT_COLOR:04X}\n\n"
        )

        # Generate the woman bitmap.

        width, height, pixels = load_image(
            "woman.jpg",
            use_alpha=False
        )

        out.write("// Main character\n")
        out.write(f"#define WOMAN_WIDTH {width}\n")
        out.write(f"#define WOMAN_HEIGHT {height}\n\n")

        out.write(
            "const uint16_t womanBitmap[] PROGMEM = {\n"
        )
        out.write(format_bitmap(pixels))
        out.write("\n};\n\n")

        # Generate individual clothing bitmaps.

        clothing_entries = []

        for index, filename in enumerate(files):

            name = f"clothes{index + 1:02d}Bitmap"

            width, height, pixels = load_image(
                filename,
                use_alpha=True
            )

            print(
                f"{filename}: {width} x {height} "
                f"({len(pixels)} pixels)"
            )

            out.write(f"// {filename}\n")
            out.write(
                f"const uint16_t {name}[] PROGMEM = {{\n"
            )
            out.write(format_bitmap(pixels))
            out.write("\n};\n\n")

            clothing_entries.append(
                (width, height, name, filename.name)
            )

        # Clothing structure.

        out.write("// Clothing metadata\n")
        out.write("struct ClothingItem {\n")
        out.write("    uint16_t width;\n")
        out.write("    uint16_t height;\n")
        out.write("    const uint16_t *bitmap;\n")
        out.write("};\n\n")

        out.write(
            "const ClothingItem clothes[] PROGMEM = {\n"
        )

        for width, height, name, filename in clothing_entries:

            out.write(
                f"    {{ {width}, {height}, {name} }},"
                f" // {filename}\n"
            )

        out.write("};\n\n")

        out.write(
            f"#define CLOTHING_COUNT {len(clothing_entries)}\n"
        )

    print()
    print(f"Created {OUTPUT}")
    print(f"Clothing items: {len(files)}")
    print(f"Scale: {SCALE * 100:.0f}%")


if __name__ == "__main__":
    main()
