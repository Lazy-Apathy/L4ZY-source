"""Plan des menus pour l'assistant des reglages (data/assistant-menus.txt).

Le fichier est genere a chaque version par distrib/tools/menus_plan.py depuis
data/menus.cfg et traduction.cfg. Ici on le lit et on choisit le petit
morceau utile a une question : la position exacte des reglages du catalogue
(page, section, ligne, voisins, condition d'affichage) et, si la question
porte sur la disposition (ou, sous, a cote, au-dessus...), la section ou la
page autour. Jamais tout le plan : le contexte du modele est petit (4096).

Bibliotheque standard uniquement.
"""

from __future__ import annotations

import re
import unicodedata
from pathlib import Path

ITEM_RE = re.compile(r'\b(checkbox|slider|field|flag|colours)\s+(?:"([^"]*)"\s+)?([A-Za-z_]\w*)')
CHOICE_RE = re.compile(r'(?:"([^"]*)"\s+)?choice\s+([A-Za-z_]\w*):')
BUTTON_RE = re.compile(r'\bbutton\s+"([^"]*)"\s+>\s+page\s+(\w+)')

# mots d'une question sur la disposition des menus (francais, anglais), sans accents
LAYOUT_WORDS = {
    "ou", "where", "sous", "dessous", "dessus", "under", "below", "above", "beneath", "cote", "next", "beside",
    "voisin", "voisins", "avant", "apres", "before", "after", "menu", "menus", "page", "pages", "section",
    "sections", "ligne", "lignes", "line", "lines", "trouve", "trouver", "find", "ordre", "order", "autour",
    "around", "liste", "list", "gauche", "droite", "left", "right", "haut", "bas", "top", "bottom", "premier",
    "derniere", "dernier", "first", "last", "contient", "contains", "sousmenu", "submenu", "bouton", "button",
    "onglet", "tab", "disposition", "layout", "range", "rangee", "colonne", "column", "affiche", "visible",
    "apparait", "appear", "appears", "cache", "hidden", "grise", "greyed", "chemin", "path", "acceder", "reach",
}


def norm(s):
    s = unicodedata.normalize("NFKD", s)
    s = "".join(c for c in s if not unicodedata.combining(c)).lower()
    return " ".join(re.sub(r"[^a-z0-9]+", " ", s).split())


class Section:
    def __init__(self, name, head):
        self.name, self.head, self.lines = name, head, []


class Page:
    def __init__(self, name, title, route):
        self.name, self.title, self.route = name, title, route
        self.also = []
        self.sections = []


class MenuPlan:
    def __init__(self, text):
        self.pages = {}
        self.order = []
        self.where = {}      # var -> [(page, section index, line index, label)]
        cur = sec = None
        for raw in text.splitlines():
            line = raw.rstrip()
            if not line or (line.startswith("#") and not line.startswith("## ")):
                continue
            if line.startswith("## "):
                parts = [p.strip() for p in line[3:].split("|", 2)]
                while len(parts) < 3:
                    parts.append("")
                cur = Page(*parts)
                self.pages[cur.name] = cur
                self.order.append(cur.name)
                sec = None
                continue
            if cur is None:
                continue
            if line.startswith("also: "):
                cur.also.append(line[6:])
                continue
            if line.startswith("== "):
                head = line[3:]
                name = head.split(" {", 1)[0]
                sec = Section(name, head)
                cur.sections.append(sec)
                continue
            m = re.match(r"^(\d+)\. (.*)$", line)
            if m and sec is not None:
                sec.lines.append(m.group(2))
                si, li = len(cur.sections) - 1, len(sec.lines) - 1
                for mm in ITEM_RE.finditer(m.group(2)):
                    self._add(mm.group(3), cur.name, si, li, mm.group(2) or "")
                for mm in CHOICE_RE.finditer(m.group(2)):
                    self._add(mm.group(2), cur.name, si, li, mm.group(1) or "")

    def _add(self, var, page, si, li, label):
        lst = self.where.setdefault(var, [])
        if (page, si, li) not in [(p, s, l) for p, s, l, _ in lst]:
            lst.append((page, si, li, label))

    # ---------------------------------------------------------------- lookups

    def page_route(self, page):
        p = self.pages.get(page)
        return p.route if p else ""

    def position(self, var):
        """one line: where the setting is, what is around it and when it is shown"""
        hits = self.where.get(var)
        if not hits:
            return ""
        page, si, li, label = hits[0]
        p = self.pages[page]
        sec = p.sections[si]
        name = f'"{label}"' if label else var
        if not label:
            # radio buttons without a title: their labels
            for line in p.sections[si].lines[li:li + 1]:
                m = re.search(r"choice %s: ((?:\"[^\"]*\"=[^ ,]+(?:, )?)+)" % re.escape(var), line)
                if m:
                    name = " / ".join(f'"{x}"' for x in re.findall(r'"([^"]*)"', m.group(1)))
        where = p.route + (f' > section "{sec.name}"' if sec.name != "(top)" else " (top of the page, before any section title)")
        out = f"{name} ({var}, page {page}): {where}"
        if " {only if " in sec.head:
            out += "; the whole section shows only if " + sec.head.split(" {only if ", 1)[1].rstrip("}")
        has = re.compile(r"\b%s\b" % re.escape(var))
        cells = line_cells(sec.lines[li])
        k = next((n for n, (c, _) in enumerate(cells) if has.search(c)), 0)
        mine, cond = cells[k] if cells else ("", "")
        column = None
        if mine.startswith("[ ") and mine.endswith(" ]"):
            # a column of a line ("[ a / b / c ] | [ d / e ]"): neighbours are above / below in it
            column = [x for x in split_top(mine[2:-2], " / ")]
            j = next((n for n, c in enumerate(column) if has.search(c)), 0)
            mine = column[j]
            m = re.match(r"^\{if (.*?)\} (.*)$", mine)
            if m:
                cond, mine = m.group(1), m.group(2)
        if cond:
            out += f"; it shows only if {cond}"
        if " choice " in " " + mine:
            opts = re.findall(r'"([^"]*)"=', mine)
            if opts:
                out += "; its choices: " + ", ".join(opts)
        if column is not None:
            ups = [line_name(c) for c in column[:j] if line_name(c) and not c.startswith("note:")]
            downs = [line_name(c) for c in column[j + 1:] if line_name(c) and not c.startswith("note:")]
            out += f"; it is in a column of {len(column)} items"
            if ups:
                out += ", under " + ups[-1]
            if downs:
                out += ", above " + downs[0]
            side = [line_name(split_top(c[2:-2] if c.startswith("[ ") else c, " / ")[0]) for c, _ in cells if not has.search(c)]
            if side:
                out += "; the other column(s) of that line start with: " + ", ".join(x for x in side if x)
        else:
            left = [cell_name(c, w) for c, w in cells[:k] if cell_name(c, w)]
            right = [cell_name(c, w) for c, w in cells[k + 1:] if cell_name(c, w)]
            if left:
                out += "; on the same line, to its left: " + ", ".join(left)
            if right:
                out += "; on the same line, to its right: " + ", ".join(right)
        up = self._neighbour(sec.lines, li, -1)
        down = self._neighbour(sec.lines, li, 1)
        if up:
            out += f"; the line above: {up}"
        elif sec.name != "(top)":
            out += f'; it is the first line under the section title "{sec.name}"'
        if down:
            out += f"; the line below: {down}"
        elif si + 1 < len(p.sections):
            out += f'; it is the last line of the section; next comes the section "{p.sections[si + 1].name}"'
        if len(hits) > 1:
            others = [f"{self.pages[h[0]].route}" for h in hits[1:3] if h[0] != page]
            if others:
                out += "; also in: " + ", ".join(others)
        return out

    @staticmethod
    def _neighbour(lines, li, step):
        """the next line holding a control or a title (grey notes between are counted)"""
        notes = 0
        k = li + step
        while 0 <= k < len(lines):
            if not lines[k].startswith(("note:", "status:")):
                names = [cell_name(c, w) for c, w in line_cells(lines[k])]
                names = [n for n in names if n]
                if names:
                    tail = f" (with {notes} grey hint line{'s' if notes > 1 else ''} between)" if notes else ""
                    return ", ".join(names[:4]) + (" ..." if len(names) > 4 else "") + tail
            notes += 1
            k += step
        return ""

    def section_text(self, page, si, focus=None, maxlines=40):
        p = self.pages[page]
        sec = p.sections[si]
        lines = sec.lines
        lo, hi = 0, len(lines)
        if focus is not None and len(lines) > maxlines:
            lo = max(0, focus - maxlines // 2)
            hi = min(len(lines), lo + maxlines)
        out = [f"== {sec.head}"]
        if lo > 0:
            out.append(f"(lines 1-{lo} not shown)")
        for k in range(lo, hi):
            out.append(f"{k + 1}. {short(lines[k], 220)}")
        if hi < len(lines):
            out.append(f"(lines {hi + 1}-{len(lines)} not shown)")
        return "\n".join(out)

    def page_head(self, page):
        p = self.pages[page]
        names = ", ".join(s.head.replace("(top)", "(top, no title)") for s in p.sections)
        out = f'Page "{p.title}" (route: {p.route}; page {p.name}). Sections, top to bottom: {names}'
        for a in p.also:
            out += f"\nAlso reached by: {a}"
        return out

    # ---------------------------------------------------------------- context for one question

    def context(self, question, catalog, budget=1500):
        """text for the model: positions of the catalogue's settings, plus the
        section (or the page) around the best one when the question is about
        the layout. "" when nothing fits."""
        qn = norm(question)
        qwords = set(qn.split())
        layout = bool(qwords & LAYOUT_WORDS) or "a cote" in qn or "au dessus" in qn or "en dessous" in qn
        cvars = catalog_vars(catalog)
        # score every place: settings of the catalogue (by rank) and labels named in the question
        scores = {}
        for rank, var in enumerate(cvars[:8]):
            for page, si, li, _ in self.where.get(var, [])[:1]:
                scores[(page, si, li)] = scores.get((page, si, li), 0) + max(1, 10 - rank)
        for page in self.order:
            p = self.pages[page]
            for si, sec in enumerate(p.sections):
                sn = set(w for w in norm(sec.name).split() if len(w) >= 4)
                for li, line in enumerate(sec.lines):
                    labels = " ".join(re.findall(r'"([^"]*)"', line))
                    lw = set(w for w in norm(labels).split() if len(w) >= 4)
                    hit = len(qwords & lw)
                    if hit:
                        scores[(page, si, li)] = scores.get((page, si, li), 0) + 4 * hit + (2 if qwords & sn else 0)
        if not scores:
            return ""
        best = max(scores.items(), key=lambda kv: kv[1])[0]
        page, si, li = best
        parts = []
        used = set()
        # the setting at the best place first; others only from the same section
        # (a far away one only confuses a small model)
        ranked = sorted((v for v in cvars[:8] if v in self.where),
                        key=lambda v: (self.where[v][0][:2] != (page, si), cvars.index(v)))
        for var in ranked[:(3 if layout else 1)]:
            if used and self.where[var][0][:2] != (page, si):
                continue
            pos = self.position(var)
            if pos and var not in used:
                parts.append("- " + pos)
                used.add(var)
        if not parts:
            # no setting of the catalogue in the plan: the line the question names
            sec = self.pages[page].sections[si]
            parts.append(f"- {self.pages[page].route} > section \"{sec.name}\", line {li + 1}: {short(sec.lines[li], 160)}")
        head = "MENU POSITIONS (exact, from the game's menus):\n" + "\n".join(parts)
        if not layout:
            return clip(head, budget)
        block = [head, "", "MENU LAYOUT around it (lines top to bottom; ' | ' = side by side left to right; {if ...} = shown only then):",
                 self.page_head(page), self.section_text(page, si, focus=li)]
        text = "\n".join(block)
        if len(text) > budget:
            # the section alone is too long: keep the lines near the setting
            block[-1] = self.section_text(page, si, focus=li, maxlines=8)
            text = "\n".join(block)
        if len(text) > budget:
            block[-2] = f'Page "{self.pages[page].title}" (route: {self.pages[page].route}; page {page})'
            text = "\n".join(block)
        return clip(text, budget)

    def rewrite_catalog(self, catalog):
        """[menu: ...] of each catalogue line -> the exact route from the plan"""
        out = []
        for line in catalog.splitlines():
            m = re.match(r"^([A-Za-z_]\w*) = ", line)
            if m and m.group(1) in self.where and "[menu:" in line:
                page, si, li, _ = self.where[m.group(1)][0]
                p = self.pages[page]
                sec = p.sections[si].name
                path = p.route + (f" > {sec}" if sec != "(top)" else "")
                line = re.sub(r"\[menu: [^\]]*\]", f"[menu: {path} | showgui {page}]", line)
            out.append(line)
        return "\n".join(out)

    def overview(self, maxlabels=3):
        """the Options page, button by button, with each page's sections (for the prompt)"""
        opt = self.pages.get("options")
        if not opt:
            return ""
        out = []
        for sec in opt.sections:
            for line in sec.lines:
                m = BUTTON_RE.search(line)
                if not m or m.group(2) not in self.pages or m.group(2) in ("settingsearch", "assistant"):
                    continue
                p = self.pages[m.group(2)]
                secs = []
                for s in p.sections:
                    labels = []
                    for l in s.lines:
                        if l.startswith("{if") or len(labels) >= maxlabels:
                            continue        # shown only sometimes: not for the overview
                        for mm in re.finditer(r'(?:checkbox|slider|field|colours|key)\s+"([^"]*)"|"([^"]*)"\s+choice|choice \w+: ((?:"[^"]*"=[^ ,]+(?:, )?)+)', l):
                            if mm.group(3):
                                t = "/".join(re.findall(r'"([^"]*)"', mm.group(3))[:3])
                            else:
                                t = mm.group(1) or mm.group(2)
                            if t and "<" not in t and t not in labels and len(labels) < maxlabels:
                                labels.append(t if len(t) <= 30 else t[:28] + "..")
                    subs = []
                    for l in s.lines:
                        for mm in re.finditer(r'button "([^"<]{2,})" > page (\w+)', l):
                            if mm.group(1) == "image":
                                continue
                            t = self.pages[mm.group(2)].title if mm.group(2) in self.pages else mm.group(2)
                            if len(subs) < 6:
                                subs.append(f"{mm.group(1)} > {t} page")
                    labels += subs
                    name = s.name if s.name != "(top)" else "top"
                    cond = " (RT only)" if "Ray tracing" in s.head.split(" {only if ", 1)[-1] and " {only if " in s.head else ""
                    cond = re.sub(r'"([^"]*)" chosen \([^)]*\)', r"\1 chosen", cond)
                    secs.append(f"{name}{cond}: " + ", ".join(labels) if labels else name + cond)
                out.append(f'- {m.group(1)} (showgui {p.name}): ' + "; ".join(secs))
        return "\n".join(out)


def split_top(s, sep=" | "):
    """split on sep outside quotes, brackets, parentheses and braces"""
    out, depth, quote, start, i = [], 0, False, 0, 0
    while i < len(s):
        c = s[i]
        if c == '"':
            quote = not quote
        elif not quote and c in "([{":
            depth += 1
        elif not quote and c in ")]}":
            depth -= 1
        elif not quote and depth == 0 and s.startswith(sep, i):
            out.append(s[start:i])
            i += len(sep)
            start = i
            continue
        i += 1
    out.append(s[start:])
    return out


def line_cells(line):
    """[(cell, condition)] of one plan line, left to right ("{if X} (a | b)" opened)"""
    cells = []
    for part in split_top(line):
        cond = ""
        m = re.match(r"^\{if (.*?)\} (.*)$", part)
        if m and split_top(m.group(1), "}")[0] == m.group(1):
            cond, part = m.group(1), m.group(2)
        if cond and part.startswith("(") and part.endswith(")"):
            for sub in split_top(part[1:-1]):
                cells.append((sub, cond))
        else:
            cells.append((part, cond))
    return cells


def cell_name(cell, cond=""):
    name = line_name(cell)
    if not name:
        return ""
    if cell.startswith("note:"):
        return ""
    return name + (f" (shown only if {cond})" if cond else "")


def line_name(line):
    """what a player calls a line: its first label, or its first control"""
    body = re.sub(r"^\{if [^}]*\} ", "", line)
    m = re.search(r'"([^"]*)"', body)
    if m:
        return f'"{m.group(1)}"'
    m = re.search(r"\b(choice|slider|field|checkbox|colours|flag) ([A-Za-z_]\w*)", body)
    return f"{m.group(1)} {m.group(2)}" if m else ""


def catalog_vars(catalog):
    out = []
    for line in (catalog or "").splitlines():
        m = re.match(r"^([A-Za-z_]\w*) = ", line)
        if m and m.group(1) not in out:
            out.append(m.group(1))
    return out


def short(s, n):
    return s if len(s) <= n else s[: n - 3] + "..."


def clip(s, n):
    if len(s) <= n:
        return s
    cut = s.rfind("\n", 0, n)
    return s[: cut if cut > n // 2 else n]


_CACHE = {"path": None, "mtime": None, "plan": None}


def load(path):
    """the plan, re-read when the file changes; None if it is missing"""
    path = Path(path)
    try:
        mtime = path.stat().st_mtime
    except OSError:
        return None
    if _CACHE["path"] != str(path) or _CACHE["mtime"] != mtime:
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            return None
        _CACHE.update(path=str(path), mtime=mtime, plan=MenuPlan(text))
    return _CACHE["plan"]
