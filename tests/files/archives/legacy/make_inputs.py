#!/usr/bin/env python3
# This file is part of the dosbox-automation Project.
# License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
#
"""Inputs for the PKZIP 1.x fixtures: text and binary, small and large."""

import os
import pathlib
import random

WORDS = ("disk drive sector track cluster archive member window literal "
         "distance length code tree table stream buffer prompt batch").split()

# 1990-03-15 12:00:00 UTC, PKZIP 1.10's release month; PKZIP stores it as
# DOS local time, so the run pins TZ=UTC.
STAMP = 637502400


def text(rng: random.Random, size: int) -> bytes:
    out = []
    length = 0
    while length < size:
        line = " ".join(rng.choice(WORDS) for _ in range(rng.randint(4, 12)))
        out.append(line + "\r\n")
        length += len(line) + 2
    return "".join(out).encode("ascii")[:size]


def binary(rng: random.Random, size: int) -> bytes:
    # Runs and noise, so implode finds matches and still needs literals.
    data = bytearray()
    while len(data) < size:
        if rng.random() < 0.5:
            data += bytes([rng.randrange(256)]) * rng.randint(3, 40)
        else:
            data += bytes(rng.randrange(256) for _ in range(rng.randint(1, 30)))
    return bytes(data[:size])


def main() -> None:
    rng = random.Random(1990)
    out = pathlib.Path(__file__).with_name("inputs")
    out.mkdir(exist_ok=True)
    files = {
        "TINY.TXT": text(rng, 200),
        "SMALL.TXT": text(rng, 3000),
        "LARGE.TXT": text(rng, 60000),
        "SMALL.BIN": binary(rng, 3000),
        "LARGE.BIN": binary(rng, 60000),
        "EMPTY.DAT": b"",
    }
    for name, data in files.items():
        path = out / name
        path.write_bytes(data)
        os.utime(path, (STAMP, STAMP))


if __name__ == "__main__":
    main()
