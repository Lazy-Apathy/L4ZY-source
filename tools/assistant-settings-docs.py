#!/usr/bin/env python3
"""Completes data/assistant-settings.txt with the descriptions of the
Sauerbraten docs (docs/config.html, docs/game.html) for the saved settings
(VARP/FVARP/SVARP/HVARP in src/) that the hand-written part does not cover.

Everything above the "# --- from the Sauerbraten docs ---" line is kept
as it is; the part below is rewritten. Run it again after adding settings.
"""

import glob
import html
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "assistant-settings.txt"
MARK = "# --- from the Sauerbraten docs ---"


def saved_settings():
    names = set()
    for pattern in ("src/engine/*.cpp", "src/engine/*.h", "src/engine/hwrt/*.cpp", "src/fpsgame/*.cpp", "src/fpsgame/*.h"):
        for f in glob.glob(str(ROOT / pattern)):
            text = Path(f).read_text(encoding="latin-1")
            names.update(m.group(1) for m in re.finditer(r"\bH?[FS]?VARF?P\s*\(\s*(\w+)\s*,", text))
    return names


def doc_descriptions():
    out = {}
    for name in ("config.html", "game.html"):
        text = (ROOT / "docs" / name).read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'<pre id="([^"]+)">.*?</pre>\s*<p>(.*?)</p>', text, re.S):
            desc = html.unescape(re.sub(r"<[^>]+>", "", m.group(2)))
            desc = re.sub(r"\s*\(default:[^)]*\)", "", desc)   # the game shows the current value
            desc = " ".join(desc.split())
            if desc:
                out[m.group(1)] = desc[:220]
    return out


def main():
    text = OUT.read_text(encoding="utf-8")
    lines_in = text.splitlines()
    cut = lines_in.index(MARK) if MARK in lines_in else len(lines_in)   # the mark alone on its line
    head = "\n".join(lines_in[:cut]).rstrip() + "\n\n"
    known = {line.split(":", 1)[0].strip() for line in head.splitlines() if ":" in line and not line.startswith("#")}
    saved = saved_settings()
    docs = doc_descriptions()
    lines = [f"{name}: {docs[name]}" for name in sorted(saved) if name in docs and name not in known]
    OUT.write_text(head + MARK + "\n" + "\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(lines)} descriptions from the docs, {len(known)} hand-written")


if __name__ == "__main__":
    main()
