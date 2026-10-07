#!/usr/bin/env python3
"""Plan of the game menus for the settings assistant (data/assistant-menus.txt).

Reads data/menus.cfg and traduction.cfg (the menu files the game loads) and
writes, for every page (newgui): how to reach it, its sections in order, and
its lines in order with the setting behind each control, the exact labels,
the submenus opened by the buttons and the main display conditions (if).

    python distrib/tools/menus_plan.py            (writes data/assistant-menus.txt)
    python distrib/tools/menus_plan.py --check    (exit 1 if the file is not up to date)
    python distrib/tools/menus_plan.py --root <repo> --out <file>

Run by `sauerrt.ps1 release` before every build, so the plan follows any
change of the menus. Standard library only. Also imported by
check_settings_coverage.py (list of the settings shown in the menus).

What is resolved: menu aliases used as building blocks (HUD_gap, GFX_name,
friendhudtitle, TRAD_*...) are expanded with their arguments; macros
(@(resbutton 320 240)) are expanded; guilistsplit over a known list (Keys
page) is expanded item by item; looplist over a literal list is expanded.
What is not (shown as such in the plan): loops over a number known only in
game (friends, recordings, models) appear once, marked "repeated"; labels
computed in game appear as <function>; conditions are written as readable
formulas, with the menu label of the setting when there is one.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import OrderedDict, deque
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MENU_FILES = ("data/menus.cfg", "traduction.cfg")
OUT_REL = "data/assistant-menus.txt"
# pages that only exist to work (search, assistant, file editor): not settings
NOT_SETTINGS = {"settingsearch", "assistant", "optionscfg", "options"}

# ------------------------------------------------------------------ tokens


class Tok:
    __slots__ = ("kind", "text", "at")

    def __init__(self, kind, text, at=0):
        self.kind, self.text, self.at = kind, text, at   # kind: str blk exp word

    def __repr__(self):
        return f"{self.kind}:{self.text[:30]!r}"


def _skip_str(s, i):
    j = i + 1
    n = len(s)
    while j < n and s[j] not in '"\n':
        if s[j] == "^":
            j += 1
        j += 1
    return j + 1 if j < n and s[j] == '"' else j


def _skip_group(s, i):
    """index of the bracket/paren closing the one at s[i]"""
    close = "]" if s[i] == "[" else ")"
    j, n = i + 1, len(s)
    while j < n:
        c = s[j]
        if c == '"':
            j = _skip_str(s, j)
            continue
        if s.startswith("//", j):
            k = s.find("\n", j)
            j = n if k < 0 else k
            continue
        if c in "[(":
            j = _skip_group(s, j) + 1
            continue
        if c == close:
            return j
        j += 1
    return n


def commands(src):
    """cubescript text -> list of commands, each a list of Tok"""
    out, cur = [], []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in " \t\r":
            i += 1
            continue
        if c in "\n;":
            if cur:
                out.append(cur)
                cur = []
            i += 1
            continue
        if src.startswith("//", i):
            k = src.find("\n", i)
            i = n if k < 0 else k
            continue
        at = 0
        while i < n and src[i] == "@":
            at += 1
            i += 1
        if i >= n:
            break
        c = src[i]
        if c == '"':
            j = _skip_str(src, i)
            end = j - 1 if j - 1 < n and src[j - 1] == '"' and j - 1 > i else j
            cur.append(Tok("str", src[i + 1:end], at))
            i = j
        elif c in "[(":
            j = _skip_group(src, i)
            cur.append(Tok("blk" if c == "[" else "exp", src[i + 1:j], at))
            i = j + 1
        elif c in ")]":
            i += 1
        else:
            j = i
            while j < n and src[j] not in " \t\r\n;\"[]()" and not src.startswith("//", j):
                j += 1
            cur.append(Tok("word", src[i:j], at))
            i = j
    if cur:
        out.append(cur)
    return out


def clean(s):
    """menu text as the player reads it: no colour codes, tabs or escapes"""
    s = re.sub(r"\^f(<[^>]*>|.)", "", s)
    s = s.replace("^t", " ").replace("^n", " ").replace('^"', '"').replace("^^", "^")
    s = s.replace("\f", "")
    return " ".join(s.split())


def colour_of(raw):
    m = re.match(r"\^f(.)", raw)
    return m.group(1) if m else ""


# ------------------------------------------------------------------ model


class Item:
    def __init__(self, kind, **kw):
        self.kind = kind          # check radio slider field flag key button text note msg colours section ...
        self.label = ""
        self.var = ""
        self.value = None
        self.extra = ""
        self.target = ""          # page opened (showgui)
        self.cmd = ""
        self.dynamic = False
        self.conds = ()
        self.options = []         # radio group: [(label, value)]
        self.horizontal = True
        self.__dict__.update(kw)


class Box:
    def __init__(self, horizontal, conds=()):
        self.horizontal = horizontal
        self.children = []
        self.conds = conds


class Page:
    def __init__(self, name, body, src):
        self.name, self.body, self.src = name, body, src
        self.title = ""
        self.root = Box(False)
        self.edges = []           # (target, label, item)
        self.repeated = False


class Menus:
    def __init__(self):
        self.pages = OrderedDict()
        self.aliases = {}         # name -> Tok
        self.macros = {}
        self.unknown = {}         # command head -> count (ignored, for the report)
        self.notes = []
        self.vars = set()         # game variables declared in src/ (empty: src/ not there)

    # -- definitions --
    def load(self, text, src):
        self._collect(commands(text), src)

    def _collect(self, cmds, src):
        for cmd in cmds:
            w0 = cmd[0]
            if w0.kind == "word" and w0.text == "newgui" and len(cmd) >= 3 and cmd[2].kind == "blk":
                name = cmd[1].text
                if name in self.pages:
                    del self.pages[name]   # the last definition wins, as in the game
                self.pages[name] = Page(name, cmd[2].text, src)
            elif len(cmd) >= 3 and cmd[1].kind == "word" and cmd[1].text == "=" and w0.kind == "word":
                self.aliases[w0.text] = cmd[2]
            elif w0.kind == "word" and w0.text == "alias" and len(cmd) >= 3:
                self.aliases[cmd[1].text] = cmd[2]
            elif w0.kind == "word" and w0.text == "macro" and len(cmd) >= 3:
                self.macros[cmd[1].text] = cmd[2].text
            elif w0.kind == "word" and w0.text in ("if", "nodebug", "do"):
                for t in cmd[1:]:
                    if t.kind == "blk":
                        self._collect(commands(t.text), src)

    def alias_list(self, name):
        t = self.aliases.get(name)
        if t is None:
            return None
        return listitems(t.text)


def listitems(text):
    """cubescript list -> items (quoted items keep their spaces)"""
    out = []
    for cmd in commands(text.replace("\n", " ")):
        for t in cmd:
            out.append(t.text)
    return out


# ------------------------------------------------------------------ evaluation of labels


def lookup(name, env, menus):
    if name in env:
        v = env[name]
        if v.startswith("$") and re.match(r"^\$\w+$", v):
            return f"<{v[1:]}>", True
        return v, False
    t = menus.aliases.get(name)
    if t is not None and t.kind in ("str", "word", "blk"):
        return t.text, False
    return f"<{name}>", True


def evaltok(t, env, menus):
    """(text, dynamic) of an argument"""
    if t.kind == "str":
        return subst(t.text, env), False
    if t.kind == "word":
        if t.text.startswith("$"):
            return lookup(t.text[1:], env, menus)
        return t.text, False
    if t.kind == "exp":
        return evalexp(t.text, env, menus)
    return t.text, True


def evalexp(text, env, menus):
    cmds = commands(text)
    if not cmds:
        return "", False
    cmd = cmds[0]
    fn = cmd[0].text if cmd[0].kind == "word" else ""
    args = cmd[1:]
    vals = [evaltok(a, env, menus) for a in args]
    dyn = any(d for _, d in vals)
    v = [x for x, _ in vals]
    if fn == "concatword":
        return "".join(v), dyn
    if fn == "concat":
        return " ".join(v), dyn
    if fn == "format" and v:
        out = v[0]
        for k in range(1, len(v)):
            out = out.replace(f"%{k}", v[k])
        return out, dyn
    if fn == "?" and len(v) >= 3:
        return v[2], vals[2][1]      # what the menu shows when the test fails (the usual look)
    if fn == "tabify" and v:
        return v[0], dyn
    if fn == "at" and len(v) >= 2:
        try:
            return listitems(v[0])[int(v[1])], False
        except (ValueError, IndexError):
            return f"<{clean(text)}>", True
    if fn in ("+", "-") and len(v) == 2:
        try:
            a, b = int(v[0]), int(v[1])
            return str(a + b if fn == "+" else a - b), False
        except ValueError:
            pass
    if cmd[0].kind == "word" and fn.startswith("$") and not args:
        return lookup(fn[1:], env, menus)
    return f"<{clean(text)}>", True


def subst(text, env):
    """$arg1 / @arg1 / @@n -> their value (display of actions and labels)"""
    if not env:
        return text

    def rep(m):
        name = m.group(2)
        return env[name] if name in env else m.group(0)
    return re.sub(r"([@$]+)(\w+)", rep, text)


# ------------------------------------------------------------------ walking a page

LAYOUT_ONLY = {"guistrut", "guibar", "guispring", "guibanner", "guibannersplit", "guibannercenter", "guibackdrop",
               "guiscrollbottom", "playerpreviewtint", "guionclear", "cleargui", "tmodelsync", "texamplesync",
               "friendsync", "tupdatesync", "guicolor", "echo", "initservers", "result", "settinglock"}
# menu aliases that draw one colour picker for one setting (as the game's settings index sees them)
PICKERS = {
    "friendcolorpicks": "colour dots (none + 10 colours)",
    "crosshaircolourpicks": "hex field + white/green/cyan/yellow/pink",
    "trailcolourpicks": "colour shown, hex field, same as All / stock, white/cyan/green/yellow/red",
}


class Walker:
    def __init__(self, menus):
        self.m = menus

    def page(self, page):
        self.cur = page
        self.walk(page.body, page.root, {}, (), 0)

    def add(self, box, item, conds):
        item.conds = conds
        box.children.append(item)
        return item

    def walk(self, text, box, env, conds, depth):
        if depth > 8:
            self.m.notes.append(f"{self.cur.name}: nesting too deep, cut")
            return
        for cmd in commands(text):
            self.command(cmd, box, env, conds, depth)

    def command(self, cmd, box, env, conds, depth):
        m = self.m
        head = cmd[0]
        args = cmd[1:]
        # @(macro args) or @@@@(resbutton 320 240)
        if head.kind == "exp":
            inner = commands(head.text)
            if inner and inner[0][0].kind == "word" and inner[0][0].text in m.macros:
                body = m.macros[inner[0][0].text]
                for k, a in enumerate(inner[0][1:], 1):
                    body = body.replace(f"%{k}", a.text)
                self.walk(body, box, env, conds, depth + 1)
            return
        if head.kind != "word":
            return
        w = head.text
        if len(args) >= 2 and args[0].kind == "word" and args[0].text == "=":
            val, dyn = evaltok(args[1], env, m)
            env[w] = val if not dyn else f"<{w}>"
            if args[1].kind == "word" and args[1].text.startswith("$"):
                env["__src_" + w] = args[1].text[1:]
            return
        a = lambda k: args[k] if k < len(args) else None   # noqa: E731

        def text(k):
            t = a(k)
            if t is None:
                return "", False
            return evaltok(t, env, m)

        if w in ("guititle", "guisection", "guitab"):
            lab, dyn = text(0)
            lab = clean(lab)
            if w == "guititle" and not self.cur.title and box is self.cur.root:
                self.cur.title = lab
                return
            self.add(box, Item("section", label=("tab: " + lab) if w == "guitab" else lab, dynamic=dyn), conds)
            return
        if w in ("guitext", "guitextbox"):
            t = a(0)
            if t is None:
                return
            lab, dyn = evaltok(t, env, m)
            raw = t.text if t.kind == "str" else lab
            col = colour_of(raw)
            kind = "note" if col == "4" else ("msg" if col in ("0", "2", "3") and (conds or dyn) else "text")
            if dyn and t.kind == "exp" and not clean(lab).strip("<>"):
                kind = "text"
            self.add(box, Item(kind, label=clean(lab), dynamic=dyn), conds)
            return
        if w == "guibutton" or w == "guiimage":
            if w == "guiimage":
                act = a(1)
                lab = "image"
            else:
                lab, dyn = text(0)
                act = a(1)
            cmdtext = subst(act.text, env) if act is not None else ""
            target = self.showgui_target(cmdtext, depth)
            it = Item("button", label=clean(lab), cmd=" ".join(cmdtext.split()), target=target,
                      dynamic=(w == "guibutton" and text(0)[1]))
            self.add(box, it, conds)
            if target:
                self.cur.edges.append((target, it, box))
            return
        if w == "guicheckbox":
            lab, dyn = text(0)
            it = Item("check", label=clean(lab), var=text(1)[0], dynamic=dyn)
            it.on = text(2)[0] if a(2) is not None and a(2).kind != "blk" else None
            it.off = text(3)[0] if a(3) is not None and a(3).kind != "blk" else None
            self.add(box, it, conds)
            return
        if w == "guiradio":
            lab, dyn = text(0)
            self.add(box, Item("radio", label=clean(lab), var=text(1)[0], value=text(2)[0], dynamic=dyn), conds)
            return
        if w == "guibitfield":
            lab, dyn = text(0)
            self.add(box, Item("flag", label=clean(lab), var=text(1)[0], value=text(2)[0]), conds)
            return
        if w in ("guislider", "guinameslider"):
            it = Item("slider", var=text(0)[0])
            if a(1) is not None and a(1).kind != "blk" and a(2) is not None and a(2).kind != "blk":
                it.extra = f"{text(1)[0]}..{text(2)[0]}"
            self.add(box, it, conds)
            return
        if w == "guilistslider":
            vals = text(1)[0] if a(1) is not None else ""
            self.add(box, Item("slider", var=text(0)[0], extra="values " + " ".join(listitems(vals))), conds)
            return
        if w in ("guifield", "guiinputfield"):
            name = text(0)[0]
            act = a(2)
            var, cmdname = self.field_target(name, act, env)
            self.add(box, Item("field", var=var, cmd=cmdname, extra=name if name != var else ""), conds)
            return
        if w == "guikeyfield":
            act = a(2)
            action = ""
            if act is not None:
                mm = re.search(r"bind \$j \[([^\]]*)\]", subst(act.text, env))
                action = mm.group(1) if mm else ""
            self.add(box, Item("key", var="", cmd=action), conds)
            return
        if w == "guicolorswatch":
            self.add(box, Item("colours", var=text(0)[0], extra="colour swatch"), conds)
            return
        if w == "guiplayerpreview":
            act = a(3)
            self.add(box, Item("preview", label="player preview", cmd=clean(act.text) if act is not None else ""), conds)
            return
        if w == "guiservers":
            self.add(box, Item("text", label="<server list>", dynamic=True), conds)
            return
        if w == "guilist":
            if a(0) is not None and a(0).kind == "blk":
                sub = Box(not box.horizontal, conds)
                self.walk(a(0).text, sub, env, conds, depth + 1)
                if sub.children:
                    box.children.append(sub)
            return
        if w in ("guistayopen", "guinoautotab", "nodebug"):
            if a(0) is not None and a(0).kind == "blk":
                self.walk(a(0).text, box, env, conds, depth + 1)
            return
        if w == "guifloat":
            blk = next((t for t in args if t.kind == "blk"), None)
            if blk is not None:
                sub = Box(False, conds)
                self.walk(blk.text, sub, env, conds, depth + 1)
                if sub.children:
                    self.add(box, Item("section", label="floating panel"), conds)
                    box.children.append(sub)
            return
        if w == "if":
            cond = a(0)
            if cond is None:
                return
            c = ("if", cond.kind, subst(cond.text, env))
            if a(1) is not None and a(1).kind == "blk":
                self.walk(a(1).text, box, env, conds + (c,), depth + 1)
            if a(2) is not None and a(2).kind == "blk":
                self.walk(a(2).text, box, env, conds + (("not",) + c[1:],), depth + 1)
            return
        if w in ("loop", "looplist", "loopsublist"):
            blk = args[-1] if args and args[-1].kind == "blk" else None
            if blk is None or len(args) < 3:
                return
            var = args[0].text
            if w == "looplist":
                lst, dyn = evaltok(args[1], env, m)
                items = listitems(lst) if not dyn else None
                if items is not None and len(items) <= 40:
                    for v in items:
                        e2 = dict(env)
                        e2[var] = v
                        self.walk(blk.text, box, e2, conds, depth + 1)
                    return
            n, _ = evaltok(args[1], env, m)
            e2 = dict(env)
            e2[var] = f"<{var}>"
            self.cur.repeated = True
            self.walk(blk.text, box, e2, conds + (("repeat", var, clean(n)),), depth + 1)
            return
        if w == "guilistsplit" and len(args) >= 4:
            var = args[0].text
            ncol, _ = evaltok(args[1], env, m)
            lst, dyn = evaltok(args[2], env, m)
            items = listitems(lst) if not dyn else []
            try:
                ncol = max(1, int(ncol))
            except ValueError:
                ncol = 1
            row = Box(not box.horizontal, conds)
            per = (len(items) + ncol - 1) // ncol if items else 0
            for c in range(ncol):
                col = Box(box.horizontal, conds)
                for v in items[c * per:(c + 1) * per]:
                    env[var] = v
                    self.walk(args[3].text, col, env, conds, depth + 1)
                if col.children:
                    row.children.append(col)
            if row.children:
                box.children.append(row)
            return
        if w == "do":
            t = a(0)
            if t is not None:
                self.add(box, Item("text", label=f"<generated by {clean(t.text)}>", dynamic=True), conds)
            return
        if w == "showfileeditor":
            self.add(box, Item("text", label=f"<text editor of {clean(text(0)[0])}>", dynamic=True), conds)
            return
        if w in PICKERS:
            var = text(0)[0]
            lab = clean(text(1)[0]) if a(1) is not None else var
            self.add(box, Item("colours", var=var, label=lab, extra=PICKERS[w]), conds)
            return
        if w in LAYOUT_ONLY:
            return
        if w in m.aliases and m.aliases[w].kind == "blk":
            e2 = {"numargs": str(len(args))}
            for k, t in enumerate(args, 1):
                v, dyn = evaltok(t, env, m)
                if dyn and t.kind == "word" and t.text.startswith("$"):
                    v = t.text if t.text[1:] not in env else env[t.text[1:]]
                e2[f"arg{k}"] = v
            self.walk(m.aliases[w].text, box, e2, conds, depth + 1)
            return
        m.unknown[w] = m.unknown.get(w, 0) + 1

    def field_target(self, name, act, env):
        """guifield NAME N [VAR $NAME] edits VAR; [command $NAME] runs a command"""
        if act is None or act.kind != "blk":
            return name, ""
        cmds = commands(act.text)
        if len(cmds) == 1 and len(cmds[0]) >= 2:
            first = cmds[0][0].text
            refs = [t.text for t in cmds[0][1:]]
            if any(r in (f"${name}", f"@{name}") for r in refs) or any(name in r for r in refs):
                known = self.m.vars
                if first in self.m.aliases or not re.match(r"^[a-z_]\w*$", first, re.I) or (known and first not in known):
                    return "", first
                return first, ""
        return name, " ".join(act.text.split())

    def showgui_target(self, cmdtext, depth):
        mm = re.search(r"\bshowgui\s+\"?([\w-]+)\"?", cmdtext)
        if mm:
            return mm.group(1)
        # a button running a menu alias that opens a page (chooseplayermodel)
        first = cmdtext.split()[0] if cmdtext.split() else ""
        t = self.m.aliases.get(first)
        if t is not None and depth < 6:
            mm = re.search(r"\bshowgui\s+\"?([\w-]+)\"?", t.text)
            if mm:
                return mm.group(1)
        return ""


# ------------------------------------------------------------------ tidy up: labels, radio groups

GROUPABLE = ("slider", "field", "key", "colours", "radiogroup", "radio", "check")


def tidy(box):
    """merge a plain text with the control it names; group radio buttons of one setting"""
    kids = []
    for c in box.children:
        if isinstance(c, Box):
            tidy(c)
        kids.append(c)
    # radio groups (consecutive radios of the same setting, same conditions)
    out = []
    for c in kids:
        if isinstance(c, Item) and c.kind == "radio":
            last = out[-1] if out else None
            if isinstance(last, Item) and last.kind == "radiogroup" and last.var == c.var and last.conds == c.conds:
                last.options.append((c.label, c.value))
                continue
            g = Item("radiogroup", var=c.var, conds=c.conds, horizontal=box.horizontal)
            g.options = [(c.label, c.value)]
            out.append(g)
            continue
        out.append(c)
    # one thing alone in a nested list: lift it ("Upscaling:" + choices merge, GFX_name)
    for k, c in enumerate(out):
        if isinstance(c, Box) and len(c.children) == 1 and isinstance(c.children[0], Item):
            out[k] = c.children[0]
    # the same line in both branches of an if (only its colour changes): written once
    dedup = []
    for c in out:
        prev = dedup[-1] if dedup else None
        if (isinstance(c, Item) and isinstance(prev, Item) and c.kind == prev.kind and c.label == prev.label
                and c.var == prev.var and c.conds and prev.conds and c.conds[:-1] == prev.conds[:-1]
                and c.conds[-1][0] != prev.conds[-1][0] and c.conds[-1][1:] == prev.conds[-1][1:]):
            prev.conds = prev.conds[:-1]
            continue
        dedup.append(c)
    out = dedup
    # "Master Volume" + slider, "Upscaling:" + radios, "mouse DPI: " + field
    merged = []
    for c in out:
        prev = merged[-1] if merged else None
        if (isinstance(c, Item) and c.kind in GROUPABLE and not c.label and isinstance(prev, Item)
                and prev.kind == "text" and prev.conds == c.conds) or (
                isinstance(c, Item) and c.kind == "radiogroup" and isinstance(prev, Item) and prev.kind == "text"
                and prev.conds == c.conds and not getattr(c, "label", "")):
            c.label = prev.label.rstrip(": ").strip()
            c.dynamic = c.dynamic or prev.dynamic
            merged[-1] = c
            continue
        merged.append(c)
    box.children = merged


def first_label(node, skip=None):
    if isinstance(node, Item):
        if node is skip or node.kind in ("section",):
            return ""
        if node.kind == "radiogroup":
            return node.label or (node.options[0][0] if node.options else "")
        return node.label if not node.dynamic else ""
    for c in node.children:
        lab = first_label(c, skip)
        if lab:
            return lab
    return ""


# ------------------------------------------------------------------ conditions in clear

FRIENDLY = {
    "(>= (getvarmax hwrtngxmode) 4)": "DLSS/FSR built in",
    "(= (getvarmax hwrtngxmode) 8)": "FSR built in",
    "(= (getvarmax hwrtnrd) 1)": "NRD built in",
    "(hwrtlighteffective)": "RT lighting running",
    "(isconnected)": "connected to a server",
    "(hdroutwindowshdr)": "Windows HDR on for this screen",
    "(= (hdrouteffectif) 2)": "Native HDR in use",
    "(= (hdrouteffectif) 1)": "HDR preview in use",
}
CMP = {"=": "=", "!=": "!=", "<": "<", ">": ">", "<=": "<=", ">=": ">=", "=f": "=", "!=f": "!=", "<f": "<", ">f": ">",
       "=s": "=", "!=s": "!="}


class CondText:
    def __init__(self, pages):
        self.radio = {}    # var -> {value: label}
        self.check = {}    # var -> label
        for p in pages:
            for it in iter_items(p.root):
                if it.kind == "radiogroup":
                    for lab, val in it.options:
                        self.radio.setdefault(it.var, {}).setdefault(val, lab)
                elif it.kind == "check" and it.label:
                    self.check.setdefault(it.var, it.label)

    def truth(self, var):
        r = self.radio.get(var)
        if r:
            nz = [(v, lab) for v, lab in r.items() if v not in ("0", "0.0")]
            if len(nz) == 1 and "0" in r:
                return f'"{nz[0][1]}" chosen ({var} {nz[0][0]})'
        if var in self.check and len(self.check[var]) > 3:
            return f'"{self.check[var]}" ticked ({var})'
        return f"{var} on"

    def expr(self, t):
        if t.kind == "word":
            if t.text.startswith("$"):
                return self.truth(t.text[1:])
            return t.text
        if t.kind == "str":
            return f'"{clean(t.text)}"' if t.text else '""'
        if t.kind != "exp":
            return clean(t.text)
        key = "(" + " ".join(t.text.split()) + ")"
        if key in FRIENDLY:
            return FRIENDLY[key]
        cmds = commands(t.text)
        if not cmds:
            return ""
        fn = cmds[0][0].text
        args = cmds[0][1:]
        if fn == "!" and args:
            return "not " + self.expr(args[0])
        if fn in ("&&", "||"):
            return (" and " if fn == "&&" else " or ").join(self.expr(x) for x in args)
        if fn in CMP and len(args) == 2:
            a, b = args
            if a.kind == "word" and a.text.startswith("$") and b.kind == "word" and fn in ("=", "=s"):
                var = a.text[1:]
                lab = self.radio.get(var, {}).get(b.text)
                if lab:
                    return f'"{lab}" chosen ({var} {b.text})'
            left = a.text[1:] if a.kind == "word" and a.text.startswith("$") else self.expr(a)
            right = b.text[1:] if b.kind == "word" and b.text.startswith("$") else self.expr(b)
            return f"{left} {CMP[fn]} {right}"
        if not args:
            return f"{fn}()"
        return f"{fn}(" + " ".join(clean(x.text) if x.kind != "exp" else self.expr(x) for x in args) + ")"

    def cond(self, c):
        if c[0] == "repeat":
            return f"repeated for each {c[1]} (count known in game: {c[2]})"
        kind, raw = c[1], c[2]
        txt = self.expr(Tok(kind, raw))
        return ("not (" + txt + ")") if c[0] == "not" else txt


def iter_items(node):
    if isinstance(node, Item):
        yield node
        return
    for c in node.children:
        yield from iter_items(c)


# ------------------------------------------------------------------ routes


def routes(menus):
    """page -> list of routes (lists of button labels), Options first"""
    pages = menus.pages
    edges = {}
    for p in pages.values():
        lst = []
        labels = {}
        for target, it, box in p.edges:
            labels[it.label] = labels.get(it.label, 0) + 1
        # buttons with a fixed label first: they make the clearest route
        for target, it, box in sorted(p.edges, key=lambda e: (bool(e[1].dynamic), len(e[1].conds))):
            lab = it.label or (f"<{it.cmd.split()[0]}>" if it.cmd else "button")
            if it.label in ("image", "") or it.dynamic or labels.get(it.label, 0) > 1:
                near = ""
                for sib in box.children:
                    if sib is not it:
                        near = first_label(sib, it)
                        if near:
                            break
                if near:
                    lab = f"{lab} (next to {near})"
            lst.append((target, lab))
        edges[p.name] = lst
    out = {}
    for start, prefix in (("options", ["Options"]), ("main", [])):
        if start not in pages:
            continue
        seen = {start: prefix}
        dq = deque([start])
        while dq:
            cur = dq.popleft()
            for target, lab in edges.get(cur, []):
                if target in pages and target not in seen and target != "main":
                    seen[target] = seen[cur] + [lab]
                    dq.append(target)
        for k, v in seen.items():
            r = " > ".join(v) if v else "main menu"
            out.setdefault(k, [])
            if r not in out[k]:
                out[k].append(r)
    return out


# ------------------------------------------------------------------ text output


def render_item(it, ct):
    lab = f'"{it.label}"' if it.label else ""
    k = it.kind
    if k == "check":
        s = f"checkbox {lab} {it.var}".replace("  ", " ")
        if it.on not in (None, "1", ""):
            s += f" on={it.on}"
        if it.off not in (None, "0", ""):
            s += f" off={it.off}"
        return s
    if k == "radiogroup":
        opts = ", ".join(f'"{lab_}"={v}' for lab_, v in it.options)
        tail = " (one under the other)" if not it.horizontal and len(it.options) > 1 else ""
        return (f"{lab} " if lab else "") + f"choice {it.var}: {opts}{tail}"
    if k == "radio":
        return f"radio {lab} {it.var}={it.value}"
    if k == "slider":
        return f"slider {lab} {it.var}".replace("  ", " ") + (f" ({it.extra})" if it.extra else "")
    if k == "field":
        if it.cmd and it.var:
            return f"field {lab} {it.var} (Enter runs {it.cmd})".replace("  ", " ")
        if it.cmd:
            return f"field {lab} -> {it.cmd}".replace("  ", " ")
        return f"field {lab} {it.var}".replace("  ", " ")
    if k == "flag":
        return f"flag {lab} {it.var} {it.value}"
    if k == "key":
        return f"key {lab} -> {it.cmd}".replace("  ", " ")
    if k == "colours":
        return f"colours {lab} {it.var}".replace("  ", " ")
    if k == "button":
        if it.target:
            return f"button {lab} > page {it.target}".replace("  ", " ")
        c = it.cmd if len(it.cmd) <= 48 else it.cmd[:45] + "..."
        return f"button {lab} : {c}".replace("  ", " ")
    if k == "preview":
        return "player preview" + (f" (click: {it.cmd})" if it.cmd else "")
    if k == "note":
        t = it.label if len(it.label) <= 110 else it.label[:107] + "..."
        return f"note: {t}"
    if k == "msg":
        return f"status: {it.label}"
    if k == "text":
        return f"text {lab}" if not it.dynamic or it.label.startswith("<") else f"text {lab} (computed)"
    return f"{k} {lab}"


def render_node(node, ct, base):
    """one cell: an item, or a nested list"""
    if isinstance(node, Item):
        extra = [c for c in node.conds[len(base):]]
        s = render_item(node, ct)
        if extra:
            s = "{if " + " and ".join(ct.cond(c) for c in extra) + "} " + s
        return s
    kids = [c for c in node.children if not (isinstance(c, Item) and c.kind == "section")]
    sep = " | " if node.horizontal else " / "
    # neighbours under the same condition are written once: {if X} (a | b)
    groups = []
    for k in kids:
        kc = common_prefix(node_conds(k))
        kc = kc if len(kc) > len(base) else tuple(base)
        if groups and groups[-1][0] == kc:
            groups[-1][1].append(k)
        else:
            groups.append([kc, [k]])
    parts = []
    shown = 0
    for kc, members in groups:
        sub = [render_node(m, ct, kc) for m in members]
        sub = [x for x in sub if x]
        if not sub:
            continue
        shown += len(sub)
        s = sep.join(sub)
        if len(kc) > len(base):
            lead = "{if " + " and ".join(ct.cond(c) for c in kc[len(base):]) + "} "
            s = lead + ("(" + s + ")" if len(sub) > 1 else s)
        parts.append(s)
    if not parts:
        return ""
    if node.horizontal:
        return " | ".join(parts)
    return "[ " + " / ".join(parts) + " ]" if shown > 1 else parts[0]


def has_section(node):
    if isinstance(node, Item):
        return node.kind == "section"
    return any(has_section(c) for c in node.children)


def flat_lines(box, side=False):
    """root sequence; lists holding a section title are opened (their parts become lines)"""
    for c in box.children:
        if isinstance(c, Box) and has_section(c):
            yield ("cols", None)
            yield from flat_lines(c, True)
            yield ("endcols", None)
        else:
            yield ("node", c)


def common_prefix(seqs):
    if not seqs:
        return ()
    p = seqs[0]
    for s in seqs[1:]:
        n = 0
        while n < len(p) and n < len(s) and p[n] == s[n]:
            n += 1
        p = p[:n]
    return tuple(p)


def node_conds(node):
    if isinstance(node, Item):
        return [node.conds]
    out = [node.conds]
    for c in node.children:
        out += node_conds(c)
    return out


def render_page(p, ct, route_list):
    lines = []
    title = p.title or p.name
    r = route_list or [f"not reached by a button (console: /showgui {p.name})"]
    lines.append(f"## {p.name} | {title} | {r[0]}")
    for extra in r[1:]:
        lines.append(f"also: {extra}")
    sections = [["(top)", (), []]]
    cols = 0
    for kind, node in flat_lines(p.root):
        if kind in ("cols", "endcols"):
            cols += 1 if kind == "cols" else -1
            continue
        if isinstance(node, Item) and node.kind == "section":
            sections.append([node.label, node.conds, [], cols > 0])
            continue
        sections[-1][2].append(node)
    for sec in sections:
        name, sconds, nodes = sec[0], sec[1], sec[2]
        if not nodes and name == "(top)":
            continue
        allc = [c for n in nodes for c in node_conds(n)] or [sconds]
        base = common_prefix(allc + [sconds]) if nodes else sconds
        head = f"== {name}"
        if base:
            head += " {only if " + " and ".join(ct.cond(c) for c in base) + "}"
        if len(sec) > 3 and sec[3]:
            head += " {in columns side by side}"
        lines.append(head)
        n = 0
        for node in nodes:
            s = render_node(node, ct, base)
            if not s:
                continue
            n += 1
            lines.append(f"{n}. {s}")
    return lines


HEADER = """\
# L4ZY menu plan for the settings assistant. GENERATED by distrib/tools/menus_plan.py
# from data/menus.cfg and traduction.cfg at every release: do not edit by hand.
# "## page | title | route" starts a page; the route starts at the main menu (Escape).
# "== name" starts a section (its title in the page). Lines are numbered top to bottom.
# On one line, " | " = side by side, left to right; "[ a / b ]" = a column, top to bottom.
# {if ...} = only shown when the condition holds. "> page X" = the button opens page X.
# checkbox / choice (radio buttons, "label"=value, side by side unless "one under the other") / slider / field /
# flag / key / colours: the setting follows. "note:" = grey hint line, "status:" = coloured message.
# Not resolved: loops over a count known only in game are shown once ("repeated");
# labels computed in game show as <function>.
"""


def game_vars(src_root):
    """{name: saved} for every variable declared in the game sources (VARP... = saved)"""
    out = {}
    src = Path(src_root) / "src"
    if not src.is_dir():
        return out
    for f in sorted(list(src.rglob("*.cpp")) + list(src.rglob("*.h"))):
        if "third_party" in f.parts:
            continue
        text = f.read_text(encoding="latin-1")
        for m in re.finditer(r"\b([A-Z]*VAR[A-Z]*)\s*\(\s*([A-Za-z_]\w*)\s*,", text):
            macro, name = m.group(1), m.group(2)
            saved = "P" in macro.replace("VAR", "", 1)
            out[name] = out.get(name, False) or saved
    return out


def build(root=ROOT, src_root=None, files=None):
    """files: {"data/menus.cfg": path...} to read other menu files than <root>/..."""
    menus = Menus()
    menus.vars = set(game_vars(src_root or root))
    for rel in MENU_FILES:
        f = Path((files or {}).get(rel) or Path(root) / rel)
        if f.is_file():
            menus.load(f.read_text(encoding="utf-8", errors="replace"), rel)
    w = Walker(menus)
    for p in menus.pages.values():
        w.page(p)
        tidy(p.root)
    return menus


def settings_pages(menus):
    """pages reached from Options (the settings), plus the player model page"""
    rt = routes(menus)
    names = [n for n, r in rt.items() if any(x.startswith("Options") for x in r) and n not in NOT_SETTINGS]
    for extra in ("playermodel",):
        if extra in menus.pages and extra not in names:
            names.append(extra)
    return names


def menu_settings(menus):
    """[(var, page, label, kind)] for every control of the settings pages, menu order"""
    out = []
    for name in settings_pages(menus):
        for it in iter_items(menus.pages[name].root):
            if it.kind in ("check", "radiogroup", "slider", "field", "flag", "colours") and it.var:
                if re.match(r"^[A-Za-z_]\w*$", it.var):
                    label = it.label
                    if not label and it.kind == "radiogroup":
                        label = " / ".join(lab for lab, _ in it.options)
                    out.append((it.var, name, label, it.kind))
    # the main menu holds "My colour" and the name
    if "main" in menus.pages:
        for it in iter_items(menus.pages["main"].root):
            if it.kind == "colours" and it.var:
                out.append((it.var, "main", it.label, it.kind))
    return out


def render(menus):
    rt = routes(menus)
    ct = CondText(menus.pages.values())
    order = [n for n in rt if n in menus.pages]
    first = [n for n in ("options",) if n in menus.pages] + settings_pages(menus)
    rest = [n for n in menus.pages if n not in first]
    out = [HEADER]
    out.append("# --- settings pages (Options) ---")
    for n in first:
        out += render_page(menus.pages[n], ct, rt.get(n))
        out.append("")
    out.append("# --- other pages ---")
    for n in rest:
        out += render_page(menus.pages[n], ct, rt.get(n))
        out.append("")
    _ = order
    return "\n".join(out).rstrip() + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default=str(ROOT), help="repository root (menus are read there)")
    ap.add_argument("--src", default=None, help="folder holding src/ (default: --root), to tell settings from commands")
    ap.add_argument("--out", default=None, help=f"output file (default <root>/{OUT_REL})")
    ap.add_argument("--check", action="store_true", help="only check that the file is up to date")
    ap.add_argument("--report", action="store_true", help="also print what was not resolved")
    args = ap.parse_args(argv)
    root = Path(args.root)
    out = Path(args.out) if args.out else root / OUT_REL
    menus = build(root, args.src)
    text = render(menus)
    if args.check:
        cur = out.read_text(encoding="utf-8") if out.is_file() else ""
        if cur != text:
            print(f"{out}: not up to date with the menus (run distrib/tools/menus_plan.py)")
            return 1
        print(f"{out}: up to date")
        return 0
    old = out.read_text(encoding="utf-8") if out.is_file() else None
    if old != text:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8", newline="\n")
    npages = len(menus.pages)
    nset = len(menu_settings(menus))
    print(f"menu plan: {npages} pages, {nset} settings controls, {len(text)} characters"
          f" -> {out}{' (unchanged)' if old == text else ''}")
    if args.report:
        if menus.unknown:
            print("commands ignored (not drawn or unknown): " + ", ".join(f"{k} x{v}" for k, v in sorted(menus.unknown.items())))
        for n in menus.notes:
            print("note: " + n)
        print("pages with repeated parts: " + ", ".join(p.name for p in menus.pages.values() if p.repeated))
    return 0


if __name__ == "__main__":
    sys.exit(main())
