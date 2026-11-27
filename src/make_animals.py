
#!/usr/bin/env python3

### Creates animals.h from animal*.png files
### Original image dimensions are preserved.
### Images are converted to RGB565 and stored in PROGMEM.

from PIL import Image
from pathlib import Path
import re

TRANSPARENT_COLOR = 0xF81F
OUTPUT = "animals.h"


def rgb565(r, g, b):
    """Convert 8-bit RGB to 16-bit RGB565."""

    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    # Avoid accidentally using the transparency color.
    if value == TRANSPARENT_COLOR:
        value = 0xF81E

    return value


def load_image(filename):

    img = Image.open(filename).convert("RGBA")

    width, height = img.size

    pixels = []

    for r, g, b, a in img.getdata():

        # Transparent pixels become the transparency color.
        if a < 128:
            pixels.append(TRANSPARENT_COLOR)
        else:
            pixels.append(rgb565(r, g, b))

    return width, height, pixels


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
        Path(".").glob("animal*.png"),
        key=natural_sort_key
    )

    if not files:
        print("No animal*.png files found!")
        return

    print("Generating animals.h")

    with open(OUTPUT, "w") as out:

        out.write("// Automatically generated animal artwork\n")
        out.write("// RGB565 bitmaps stored in program memory\n\n")

        out.write("#pragma once\n")
        out.write("#include <Arduino.h>\n\n")

        out.write(
            f"#define ANIMAL_TRANSPARENT 0x{TRANSPARENT_COLOR:04X}\n\n"
        )

        animal_entries = []

        for index, filename in enumerate(files):

            name = f"animal{index + 1:02d}Bitmap"

            width, height, pixels = load_image(filename)

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

            animal_entries.append(
                (width, height, name, filename.name)
            )

        # Animal metadata structure.

        out.write("// Animal metadata\n")

        out.write("struct AnimalItem {\n")
        out.write("    uint16_t width;\n")
        out.write("    uint16_t height;\n")
        out.write("    const uint16_t *bitmap;\n")
        out.write("};\n\n")

        out.write(
            "const AnimalItem animals[] PROGMEM = {\n"
        )

        for width, height, name, filename in animal_entries:

            out.write(
                f"    {{ {width}, {height}, {name} }},"
                f" // {filename}\n"
            )

        out.write("};\n\n")

        out.write(
            f"#define ANIMAL_COUNT {len(animal_entries)}\n"
        )

    print()
    print(f"Created {OUTPUT}")
    print(f"Animals: {len(files)}")


if __name__ == "__main__":
    main()
