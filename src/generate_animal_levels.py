#!/usr/bin/env python3
"""
Generate animal_levels.h from animal_puzzle_solutions.txt

Reads solutions, groups by move count, selects up to 10 random
solutions per move count, and emits a PROGMEM array for ESP32/Arduino.
"""

import os
import random
import collections
from pathlib import Path

# Path relative to this script (or adjust as needed)
SCRIPT_DIR = Path(__file__).resolve().parent
SOLUTIONS_FILE = SCRIPT_DIR / ".." / "animal_puzzle_solutions.txt"
OUTPUT_FILE = SCRIPT_DIR / "animal_levels.h"

# Number of placement values expected after the move count
# (will be detected automatically if consistent)
MAX_PER_MOVE = 6


def parse_solutions(path: Path):
    """Return list of (moves, placements_tuple)"""
    solutions = []
    with open(path, "r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split(",") if p.strip()]
            if len(parts) < 2:
                print(f"Warning: skipping malformed line {line_no}: {line}")
                continue
            try:
                moves = int(parts[0])
                placements = tuple(int(x) for x in parts[1:])
            except ValueError as e:
                print(f"Warning: non-integer data on line {line_no}: {e}")
                continue
            solutions.append((moves, placements))
    return solutions


def select_levels(solutions):
    """Group by moves, sort groups, pick up to MAX_PER_MOVE randomly from each."""
    grouped = collections.defaultdict(list)
    for moves, placements in solutions:
        grouped[moves].append(placements)

    selected = []
    for moves in sorted(grouped.keys()):
        candidates = grouped[moves]
        count = min(MAX_PER_MOVE, len(candidates))
        chosen = random.sample(candidates, count)
        # Keep a stable order within the group after sampling
        for placements in chosen:
            selected.append((moves, placements))
    return selected


def write_header(levels, out_path: Path):
    """Emit animal_levels.h with PROGMEM data."""
    if not levels:
        raise RuntimeError("No levels to write")

    # Determine placement length (assume constant)
    place_len = len(levels[0][1])
    for moves, placements in levels:
        if len(placements) != place_len:
            raise RuntimeError(
                f"Inconsistent placement length: expected {place_len}, got {len(placements)} "
                f"for moves={moves}"
            )

    lines = [
        "// animal_levels.h",
        "// Auto-generated from animal_puzzle_solutions.txt",
        f"// {len(levels)} levels selected (up to {MAX_PER_MOVE} per move count)",
        "// Sorted by increasing move count",
        "",
        "#ifndef ANIMAL_LEVELS_H",
        "#define ANIMAL_LEVELS_H",
        "",
        "#include <Arduino.h>",
        "",
        "struct AnimalLevel {",
        "  uint8_t moves;",
        f"  uint16_t placements[{place_len}];",
        "};",
        "",
        "const AnimalLevel animal_levels[] PROGMEM = {",
    ]

    for moves, placements in levels:
        place_str = ", ".join(str(p) for p in placements)
        lines.append(f"  {{{moves}, {{{place_str}}}}},")

    lines += [
        "};",
        "",
        f"constexpr size_t NUM_ANIMAL_LEVELS = {len(levels)};",
        "",
        "#endif // ANIMAL_LEVELS_H",
        "",
    ]

    out_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {len(levels)} levels to {out_path}")


def main():
    if not SOLUTIONS_FILE.exists():
        print(f"Error: solutions file not found: {SOLUTIONS_FILE}")
        print("Place animal_puzzle_solutions.txt one directory above this script,")
        print("or edit the SOLUTIONS_FILE path in the script.")
        return 1

    random.seed()  # or set a fixed seed for reproducibility: random.seed(42)

    solutions = parse_solutions(SOLUTIONS_FILE)
    print(f"Loaded {len(solutions)} solutions")

    if not solutions:
        print("No valid solutions found.")
        return 1

    levels = select_levels(solutions)
    write_header(levels, OUTPUT_FILE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())