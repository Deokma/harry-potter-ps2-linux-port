#!/usr/bin/env python3
"""Resolves a git merge conflict in a notes file by keeping both sides: python3 -I scripts/merge_keep_both.py FILE...

Ours first, then a blank line, then theirs. For files where both branches added paragraphs at the same place (NOTES.md).
"""
import sys

for path in sys.argv[1:]:
    with open(path, encoding="utf-8", newline="") as f:
        lines = f.readlines()
    out = []
    conflicts = 0
    for line in lines:
        if line.startswith("<<<<<<< "):
            conflicts += 1
        elif line.startswith("======="):
            if out and out[-1].strip():
                out.append("\r\n" if line.endswith("\r\n") else "\n")
        elif line.startswith(">>>>>>> "):
            pass
        else:
            out.append(line)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.writelines(out)
    print(f"{path}: {conflicts} conflicts kept both ways")
