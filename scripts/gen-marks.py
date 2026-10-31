#!/usr/bin/env python3
"""Write src/marks_data.inc from @emoji-mart/data set 15.

The dock ships the generated table, so a normal build does not need this
script or the JSON. Run it again only when the emoji set should change.
"""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT = Path.home() / "Projects/mira/node_modules/@emoji-mart/data/sets/15/native.json"


def cstr(text: str) -> str:
    out = ['"']
    for ch in text:
        code = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif code < 32 or code == 127:
            out.append("\\x%02x" % code)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def main() -> None:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    data = json.loads(src.read_text(encoding="utf-8"))
    cat_index = {cat["id"]: i for i, cat in enumerate(data["categories"])}
    lines = [
        "/* Generated from @emoji-mart/data set 15. Regenerate with scripts/gen-marks.py */",
        "static const MarkRec kMarks[] = {",
    ]
    for cat in data["categories"]:
        for eid in cat["emojis"]:
            entry = data["emojis"][eid]
            keys = " ".join(word.lower() for word in entry.get("keywords", []))
            natives = "|".join(skin["native"] for skin in entry["skins"])
            lines.append(
                "    {%s, %s, %s, %s, %d},"
                % (cstr(eid), cstr(entry["name"]), cstr(keys), cstr(natives), cat_index[cat["id"]])
            )
    lines.append("};")
    dest = ROOT / "src" / "marks_data.inc"
    dest.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {dest}")


if __name__ == "__main__":
    main()
