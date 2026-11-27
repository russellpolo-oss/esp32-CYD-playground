#!/usr/bin/env python3
"""Generate random animal-sorting boards and save boards with legal solutions.

Each saved line contains the floating-point difficulty score, shortest move
count, and nine box contents. Animal IDs are 1-8; digits within a box are
ordered bottom-to-top. An empty box is encoded as 0.
"""

import argparse
import random
from collections import deque
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

ANIMAL_TYPES = 8
COPIES_PER_ANIMAL = 4
BOX_COUNT = 9
BOX_CAPACITY = 4
MAX_DEPTH = 1000

Stack = Tuple[int, ...]
Board = Tuple[Stack, ...]
Move = Tuple[int, int, int]
SolveStats = Tuple[int, int, int]


def is_complete(stack: Stack) -> bool:
    return (
        len(stack) == BOX_CAPACITY
        and all(animal == stack[0] for animal in stack)
    )


def is_solved(board: Board) -> bool:
    complete_animals: Set[int] = set()
    empty_boxes = 0

    for stack in board:
        if not stack:
            empty_boxes += 1
            continue
        if not is_complete(stack) or stack[0] in complete_animals:
            return False
        complete_animals.add(stack[0])

    return (
        empty_boxes == 1
        and len(complete_animals) == ANIMAL_TYPES
    )


def inventory_is_valid(board: Board) -> bool:
    if len(board) != BOX_COUNT:
        return False

    totals = [0] * (ANIMAL_TYPES + 1)
    for stack in board:
        if len(stack) > BOX_CAPACITY:
            return False
        for animal in stack:
            if animal < 1 or animal > ANIMAL_TYPES:
                return False
            totals[animal] += 1

    return all(
        totals[animal] == COPIES_PER_ANIMAL
        for animal in range(1, ANIMAL_TYPES + 1)
    )


def random_starting_board(rng: random.Random) -> Board:
    """Distribute all animals randomly, leaving four random slots empty."""
    empty_slot_boxes = [
        box
        for box in range(BOX_COUNT)
        for _ in range(BOX_CAPACITY)
    ]
    rng.shuffle(empty_slot_boxes)

    box_sizes = [BOX_CAPACITY] * BOX_COUNT
    for box in empty_slot_boxes[:COPIES_PER_ANIMAL]:
        box_sizes[box] -= 1

    animals = [
        animal
        for animal in range(1, ANIMAL_TYPES + 1)
        for _ in range(COPIES_PER_ANIMAL)
    ]
    rng.shuffle(animals)

    board: List[Stack] = []
    next_animal = 0
    for size in box_sizes:
        board.append(tuple(animals[next_animal:next_animal + size]))
        next_animal += size

    result = tuple(board)
    if not inventory_is_valid(result):
        raise RuntimeError("Random board has an invalid animal inventory")
    return result


def random_easy_starting_board(rng: random.Random) -> Board:
    """Distribute matching animals in pairs across eight full boxes."""
    pairs = [
        (animal, animal)
        for animal in range(1, ANIMAL_TYPES + 1)
        for _ in range(COPIES_PER_ANIMAL // 2)
    ]

    while True:
        rng.shuffle(pairs)
        if all(
            pairs[box * 2][0] != pairs[box * 2 + 1][0]
            for box in range(BOX_COUNT - 1)
        ):
            break

    box_order = list(range(BOX_COUNT))
    rng.shuffle(box_order)
    stacks: List[List[int]] = [[] for _ in range(BOX_COUNT)]
    for pair_index, pair in enumerate(pairs):
        stacks[box_order[pair_index // 2]].extend(pair)

    result = tuple(tuple(stack) for stack in stacks)
    if not inventory_is_valid(result):
        raise RuntimeError("Easy board has an invalid animal inventory")
    return result


def random_baby_starting_board(rng: random.Random) -> Board:
    """Make each full box contain a triple and one different animal."""
    triple_animals = list(range(1, ANIMAL_TYPES + 1))
    single_animals = list(range(1, ANIMAL_TYPES + 1))
    rng.shuffle(triple_animals)

    while True:
        rng.shuffle(single_animals)
        if all(
            triple != single
            for triple, single in zip(triple_animals, single_animals)
        ):
            break

    stacks: List[Stack] = []
    for triple, single in zip(triple_animals, single_animals):
        if rng.choice((True, False)):
            stacks.append((triple, triple, triple, single))
        else:
            stacks.append((single, triple, triple, triple))

    stacks.append(())
    rng.shuffle(stacks)
    result = tuple(stacks)
    if not inventory_is_valid(result):
        raise RuntimeError("Baby board has an invalid animal inventory")
    return result


def random_medium_starting_board(rng: random.Random) -> Board:
    """Disrupt an easy board with two or three individual animal swaps."""
    stacks = [
        list(stack)
        for stack in random_easy_starting_board(rng)
    ]
    positions = [
        (box, slot)
        for box, stack in enumerate(stacks)
        for slot in range(len(stack))
    ]

    for _ in range(rng.randint(2, 3)):
        candidates = [
            (first, second)
            for index, first in enumerate(positions)
            for second in positions[index + 1:]
            if first[0] != second[0]
            and stacks[first[0]][first[1]] != stacks[second[0]][second[1]]
        ]
        first, second = rng.choice(candidates)
        first_box, first_slot = first
        second_box, second_slot = second
        stacks[first_box][first_slot], stacks[second_box][second_slot] = (
            stacks[second_box][second_slot],
            stacks[first_box][first_slot],
        )

    result = tuple(tuple(stack) for stack in stacks)
    if not inventory_is_valid(result):
        raise RuntimeError("Medium board has an invalid animal inventory")
    return result


def apply_move(board: Board, source: int, destination: int) -> Optional[Tuple[Board, int]]:
    source_stack = board[source]
    destination_stack = board[destination]

    if (
        source == destination
        or not source_stack
        or len(destination_stack) == BOX_CAPACITY
        or is_complete(source_stack)
        or is_complete(destination_stack)
    ):
        return None

    animal = source_stack[-1]
    if destination_stack and destination_stack[-1] != animal:
        return None

    group_size = 1
    while (
        group_size < len(source_stack)
        and source_stack[-group_size - 1] == animal
    ):
        group_size += 1

    amount = min(group_size, BOX_CAPACITY - len(destination_stack))
    next_board = list(board)
    next_board[source] = source_stack[:-amount]
    next_board[destination] = destination_stack + (animal,) * amount
    return tuple(next_board), amount


def animal_run_count(board: Board) -> int:
    return sum(
        1
        for stack in board
        for index, animal in enumerate(stack)
        if index == 0 or animal != stack[index - 1]
    )


def legal_successors(board: Board) -> List[Tuple[int, Board, Move]]:
    successors: List[Tuple[int, Board, Move]] = []
    for source in range(BOX_COUNT):
        if not board[source] or is_complete(board[source]):
            continue
        for destination in range(BOX_COUNT):
            moved = apply_move(board, source, destination)
            if moved is None:
                continue
            next_board, amount = moved
            score = animal_run_count(next_board) * 4
            if not board[destination]:
                score += 1
            successors.append((
                score,
                next_board,
                (source, destination, amount),
            ))

    successors.sort(key=lambda candidate: candidate[0])
    return successors


def canonical_board(board: Board) -> Board:
    """Treat identical stacks as interchangeable during the search."""
    return tuple(sorted(board))


def solve(board: Board, max_depth: int) -> Optional[SolveStats]:
    """Find shortest solutions and count unique reachable terminal states."""
    initial_key = canonical_board(board)
    queue = deque([(board, 0)])
    distances: Dict[Board, int] = {initial_key: 0}
    dead_ends: Set[Board] = set()
    solutions: Set[Board] = set()
    shortest_moves: Optional[int] = None

    while queue:
        state, depth = queue.popleft()
        key = canonical_board(state)
        if is_solved(state):
            solutions.add(key)
            if shortest_moves is None or depth < shortest_moves:
                shortest_moves = depth
            continue

        successors = legal_successors(state)
        if not successors:
            dead_ends.add(key)
            continue
        if depth >= max_depth:
            continue

        next_depth = depth + 1
        for _, next_state, _ in successors:
            next_key = canonical_board(next_state)
            known_depth = distances.get(next_key)
            if known_depth is None:
                distances[next_key] = next_depth
                queue.append((next_state, next_depth))

    if shortest_moves is None:
        return None
    return shortest_moves, len(dead_ends), len(solutions)


def encode_stack(stack: Stack) -> int:
    if not stack:
        return 0
    return int("".join(str(animal) for animal in stack))


def encode_board(board: Board) -> Tuple[int, ...]:
    return tuple(encode_stack(stack) for stack in board)


def load_saved_boards(path: Path) -> Set[Tuple[int, ...]]:
    saved: Set[Tuple[int, ...]] = set()
    if not path.exists():
        return saved

    with path.open("r", encoding="utf-8") as solutions:
        for line in solutions:
            fields = [value.strip() for value in line.split(",")]
            if len(fields) == BOX_COUNT + 2:
                board_fields = fields[2:]
            elif len(fields) == BOX_COUNT + 1:
                board_fields = fields[1:]
            else:
                continue
            try:
                board = tuple(int(value) for value in board_fields)
            except ValueError:
                continue
            saved.add(board)
    return saved


def save_solution(
    path: Path,
    difficulty_score: float,
    move_count: int,
    encoded_board: Sequence[int],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    record = format_solution_record(
        difficulty_score, move_count, encoded_board
    )
    with path.open("a", encoding="utf-8") as solutions:
        solutions.write(record + "\n")
        solutions.flush()


def format_solution_record(
    difficulty_score: float,
    move_count: int,
    encoded_board: Sequence[int],
) -> str:
    return ",".join(
        str(value)
        for value in (float(difficulty_score), move_count, *encoded_board)
    )


def parse_arguments() -> argparse.Namespace:
    default_output = (
        Path(__file__).resolve().parent.parent
        / "animal_puzzle_solutions.txt"
    )
    parser = argparse.ArgumentParser(
        description="Generate and solve random animal-sorting puzzles."
    )
    parser.add_argument(
        "--max-depth",
        type=int,
        default=MAX_DEPTH,
        help="maximum number of legal moves to search (default: 1000)",
    )
    parser.add_argument(
        "--attempts",
        type=int,
        default=0,
        help="number of random boards to try; 0 runs continuously",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=default_output,
        help="solution file (default: animal_puzzle_solutions.txt)",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=None,
        help="optional random seed for repeatable test runs",
    )
    board_modes = parser.add_mutually_exclusive_group()
    board_modes.add_argument(
        "--easy",
        action="store_true",
        help="start with matching animals grouped into pairs",
    )
    board_modes.add_argument(
        "--baby",
        action="store_true",
        help="start with triples and one different animal in each box",
    )
    board_modes.add_argument(
        "--medium",
        action="store_true",
        help="start with paired animals and make two or three individual swaps",
    )
    args = parser.parse_args()
    if args.max_depth < 0:
        parser.error("--max-depth must be non-negative")
    if args.attempts < 0:
        parser.error("--attempts must be non-negative")
    return args


def main() -> None:
    args = parse_arguments()
    rng = random.Random(args.seed)
    saved_boards = load_saved_boards(args.output)
    attempt = 0

    try:
        while args.attempts == 0 or attempt < args.attempts:
            attempt += 1
            if args.baby:
                make_starting_board = random_baby_starting_board
            elif args.medium:
                make_starting_board = random_medium_starting_board
            elif args.easy:
                make_starting_board = random_easy_starting_board
            else:
                make_starting_board = random_starting_board
            board = make_starting_board(rng)
            while is_solved(board) or any(is_complete(stack) for stack in board):
                board = make_starting_board(rng)

            encoded = encode_board(board)
            stats = solve(board, args.max_depth)
            if stats is None:
                continue

            shortest_moves, dead_end_count, solution_count = stats
            difficulty_score = shortest_moves * max(
                1.0, dead_end_count / solution_count
            )
            signature = encoded
            if signature not in saved_boards:
                save_solution(
                    args.output, difficulty_score, shortest_moves, encoded
                )
                saved_boards.add(signature)
            print(
                format_solution_record(
                    difficulty_score, shortest_moves, encoded
                ),
                flush=True,
            )
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
