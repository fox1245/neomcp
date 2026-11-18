#!/usr/bin/env python3
"""Fail when a program of the README differs from the file that is compiled for it.

    python3 -I tests/consumer/check_readme.py [README]

The README is read line by line: a line that starts with a code fence opens a block (the rest of the line names its
language) and a line that is exactly a fence closes it, a line that starts with "## " outside a block starts a
section. Each section below must hold exactly one cpp block, and that block must equal the file next to this script
byte for byte. The section's cmake block must hold one target_link_libraries line, and the CMakeLists.txt next to
this script must hold the same line. Files are compared as they are checked out, with "\n" line ends.
"""

import difflib
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
FENCE = "```"

# README section -> the file that holds its program.
PROGRAMS = {
    "A first server": "calc.cpp",
    "Serving over HTTP": "calc_http.cpp",
}


def read(path):
    with open(path, encoding="utf-8", newline="") as handle:
        return handle.read()


def blocks_by_section(text):
    """{(section, language): [block text, ...]} for every fenced block of the README."""
    found = {}
    section = None
    language = None
    body = []
    for line in text.split("\n"):
        if language is None:
            if line.startswith(FENCE):
                language = line[len(FENCE):].strip()
                body = []
            elif line.startswith("## "):
                section = line[3:].strip()
        elif line == FENCE:
            found.setdefault((section, language), []).append("\n".join(body) + "\n")
            language = None
        else:
            body.append(line)
    if language is not None:
        raise SystemExit("README: a code block is never closed")
    return found


def only_block(found, section, language):
    blocks = found.get((section, language), [])
    if len(blocks) != 1:
        raise SystemExit(f'README: section "{section}" must hold exactly one {language} block, it holds {len(blocks)}')
    return blocks[0]


def main():
    readme = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE.parent.parent / "README.md"
    found = blocks_by_section(read(readme))
    cmake_lines = read(HERE / "CMakeLists.txt").split("\n")
    failed = False
    for section, name in PROGRAMS.items():
        expected = only_block(found, section, "cpp")
        actual = read(HERE / name)
        if actual != expected:
            failed = True
            print(f'{name} differs from the cpp block of the README section "{section}":')
            sys.stdout.writelines(
                difflib.unified_diff(expected.splitlines(keepends=True), actual.splitlines(keepends=True),
                                     "README.md", name))
            print()
        links = [line for line in only_block(found, section, "cmake").split("\n")
                 if line.startswith("target_link_libraries(")]
        if len(links) != 1:
            raise SystemExit(f'README: the cmake block of "{section}" must hold one target_link_libraries line')
        if links[0] not in cmake_lines:
            failed = True
            print(f'CMakeLists.txt lacks the line the README gives for "{section}": {links[0]}')
    if failed:
        return 1
    print("README programs match: " + ", ".join(PROGRAMS.values()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
