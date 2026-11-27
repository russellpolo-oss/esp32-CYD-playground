#!/usr/bin/env python3
"""Generate animal_levels.h from scored animal puzzle solutions."""

import math
from pathlib import Path
from typing import List, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
SOLUTIONS_FILE = PROJECT_ROOT / "animal_puzzle_solutions.txt"
OUTPUT_FILE = PROJECT_ROOT / "include" / "animal_levels.h"

LEVEL_COUNT = 100
NO_DEAD_END_LEVEL_COUNT = 3
BOX_COUNT = 9

Level = Tuple[float, int, Tuple[int, ...]]


def parse_solutions(path: Path) -> List[Level]:
    """Read score, shortest moves, and box placements from each row."""
    solutions: List[Level] = []
    with path.open("r", encoding="utf-8") as source:
        for line_no, line in enumerate(source, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            parts = [part.strip() for part in line.split(",")]
            if len(parts) != BOX_COUNT + 2:
                print("Warning: skipping malformed line {}: {}".format(
                    line_no, line
                ))
                continue

            try:
                score = float(parts[0])
                moves = int(parts[1])
                placements = tuple(int(value) for value in parts[2:])
            except ValueError as error:
                print("Warning: invalid data on line {}: {}".format(
                    line_no, error
                ))
                continue

            if (
                not math.isfinite(score)
                or moves <= 0
                or score < moves
                or any(value < 0 or value > 9999 for value in placements)
            ):
                print("Warning: invalid score or placements on line {}".format(
                    line_no
                ))
                continue

            solutions.append((score, moves, placements))

    return solutions


def select_evenly_spaced(levels: List[Level], count: int) -> List[Level]:
    """Select a sorted sample spanning the full difficulty range."""
    if len(levels) < count:
        raise ValueError(
            "Need at least {} scored levels above the no-dead-end levels; "
            "found {}".format(count, len(levels))
        )
    if count == 1:
        return [levels[0]]

    last_index = len(levels) - 1
    return [
        levels[index * last_index // (count - 1)]
        for index in range(count)
    ]


def select_levels(solutions: List[Level]) -> List[Level]:
    """Select three no-dead-end starts, then spread levels by score."""
    sorted_solutions = sorted(
        set(solutions),
        key=lambda level: (level[0], level[1], level[2]),
    )
    no_dead_end = [
        level for level in sorted_solutions
        if level[0] == level[1]
    ]
    with_dead_ends = [
        level for level in sorted_solutions
        if level[0] > level[1]
    ]

    if len(no_dead_end) < NO_DEAD_END_LEVEL_COUNT:
        raise ValueError(
            "Need at least {} levels with no dead ends; found {}".format(
                NO_DEAD_END_LEVEL_COUNT, len(no_dead_end)
            )
        )

    initial_levels = no_dead_end[:NO_DEAD_END_LEVEL_COUNT]
    remaining_count = LEVEL_COUNT - NO_DEAD_END_LEVEL_COUNT
    remaining_levels = select_evenly_spaced(with_dead_ends, remaining_count)

    if initial_levels[-1][0] > remaining_levels[0][0]:
        raise ValueError(
            "The three no-dead-end levels cannot precede all harder levels "
            "in score order"
        )

    levels = initial_levels + remaining_levels
    if any(
        previous[0] > current[0]
        for previous, current in zip(levels, levels[1:])
    ):
        raise ValueError("Selected difficulty scores are not progressive")
    return levels


def write_header(levels: List[Level], out_path: Path) -> None:
    """Write the selected levels and annotate each row with its score."""
    if len(levels) != LEVEL_COUNT:
        raise ValueError(
            "Expected {} levels, got {}".format(LEVEL_COUNT, len(levels))
        )
    if any(len(placements) != BOX_COUNT for _, _, placements in levels):
        raise ValueError("Every level must contain {} box placements".format(
            BOX_COUNT
        ))

    lines = [
        "// animal_levels.h",
        "// Auto-generated from animal_puzzle_solutions.txt",
        "// {} levels selected by difficulty score".format(len(levels)),
        "// Sorted by increasing difficulty; first 3 levels have no dead ends",
        "",
        "#ifndef ANIMAL_LEVELS_H",
        "#define ANIMAL_LEVELS_H",
        "",
        "#include <Arduino.h>",
        "",
        "struct AnimalLevel {",
        "  uint8_t moves;",
        "  uint16_t placements[{}];".format(BOX_COUNT),
        "};",
        "",
        "const AnimalLevel animal_levels[] PROGMEM = {",
    ]

    for score, moves, placements in levels:
        placement_text = ", ".join(str(value) for value in placements)
        lines.append(
            "  {{{}, {{{}}}}}, // Difficulty score: {}".format(
                moves, placement_text, score
            )
        )

    lines += [
        "};",
        "",
        "constexpr size_t NUM_ANIMAL_LEVELS = {};".format(len(levels)),
        "",
        "#endif // ANIMAL_LEVELS_H",
        "",
    ]

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")
    print("Wrote {} levels to {}".format(len(levels), out_path))


def main() -> int:
    if not SOLUTIONS_FILE.exists():
        print("Error: solutions file not found: {}".format(SOLUTIONS_FILE))
        return 1

    solutions = parse_solutions(SOLUTIONS_FILE)
    print("Loaded {} valid solutions".format(len(solutions)))
    if not solutions:
        print("Error: no valid solutions found")
        return 1

    try:
        levels = select_levels(solutions)
    except ValueError as error:
        print("Error: {}".format(error))
        return 1

    write_header(levels, OUTPUT_FILE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
