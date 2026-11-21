#!/usr/bin/env python3
"""LUNE-830 spike: link extra objects into Black 2/White 2's ARM9 main module.

dsd writes the linker script and object list from the delinked configs. This
adds each extra object's sections at the end of the main module's text,
read-only data, data and BSS, and appends the objects to the link's list, so
the overlays and the arena after them move up by the added size.
usage: link.py LCF OBJECTS_FILE OBJECT...
"""

from pathlib import Path
import sys

SECTIONS = {"ARM9_TEXT_END": ".text", "ARM9_RODATA_END": ".rodata", "ARM9_DATA_END": ".data",
            "ARM9_BSS_END": ".bss"}


def main():
    lcf, objects_file, *objects = sys.argv[1:]
    lines = Path(lcf).read_text().splitlines()
    names = [Path(name).name for name in objects]
    output, inserted = [], set()
    for line in lines:
        marker = line.strip().removesuffix(" = .;")
        if marker in SECTIONS and marker not in inserted:
            indent = line[:len(line) - len(line.lstrip())]
            # Before the ALIGN that precedes the marker, so the marker keeps its alignment.
            at = len(output) - 1 if output and output[-1].strip().startswith(". = ALIGN") else len(output)
            output[at:at] = [f"{indent}{name} ({SECTIONS[marker]})" for name in names]
            inserted.add(marker)
        output.append(line)
    if inserted != set(SECTIONS):
        sys.exit(f"{lcf}: missing {sorted(set(SECTIONS) - inserted)}")
    Path(lcf).write_text("\n".join(output) + "\n")
    with open(objects_file, "a") as listing:
        listing.writelines(f'"{name}"\n' for name in objects)


if __name__ == "__main__":
    main()
