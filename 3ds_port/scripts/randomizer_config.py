#!/usr/bin/env python3
"""Emit settings.txt lines for the seeded species randomizer.

The game reads rand_wild, rand_starters, rand_trainers (0 or 1) and rand_seed
(0..2147483647) from sdmc:/3ds/emerald3ds/settings.txt (include/3ds_randomizer.h).
Put the lines there, replacing any earlier ones, BEFORE starting a new game:
a changed seed changes only the Pokemon met afterwards.

  python scripts/randomizer_config.py --seed 12345 --wild --starters --trainers
  python scripts/randomizer_config.py --seed-text "my run" --wild
  python scripts/randomizer_config.py --off

--seed-text hashes a word the way CtrRandomizer_SeedFromText does
(FNV-1a, 31 bits), so the same word is the same seed everywhere.
"""
import argparse
import sys

SEED_MAX = 0x7FFFFFFF


def seed_from_text(text: str) -> int:
    value = 0x811C9DC5
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 0x01000193) & 0xFFFFFFFF
    return value & SEED_MAX


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--seed", type=int, help="seed, 0..2147483647")
    source.add_argument("--seed-text", help="a word to make the seed from")
    parser.add_argument("--wild", action="store_true", help="randomize wild Pokemon")
    parser.add_argument("--starters", action="store_true", help="randomize the starters")
    parser.add_argument("--trainers", action="store_true", help="randomize ordinary trainers' Pokemon")
    parser.add_argument("--off", action="store_true", help="every kind off")
    args = parser.parse_args(argv)

    if args.seed is not None and not 0 <= args.seed <= SEED_MAX:
        parser.error("--seed must be 0..%d" % SEED_MAX)
    if args.off and (args.wild or args.starters or args.trainers):
        parser.error("--off with a kind turned on")
    seed = args.seed if args.seed is not None else seed_from_text(args.seed_text) if args.seed_text is not None else 0

    lines = [
        "rand_wild=%d" % (args.wild and not args.off),
        "rand_starters=%d" % (args.starters and not args.off),
        "rand_trainers=%d" % (args.trainers and not args.off),
        "rand_seed=%d" % seed,
    ]
    sys.stdout.write("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
