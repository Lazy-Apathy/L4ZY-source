#!/usr/bin/env python3
"""Controle avant publication : chaque reglage des menus est connu de l'assistant.

Compare les reglages affiches dans les pages de reglages (data/menus.cfg,
traduction.cfg, lus par menus_plan.py) avec :
  - data/assistant-settings.txt (ce que fait chaque reglage, ou "-" = jamais propose) ;
  - data/settings-keywords.txt (mots de recherche en plus du libelle : francais, synonymes).
Et l'inverse : les entrees de ces deux fichiers qui ne correspondent plus a
aucun reglage du jeu (renomme ou supprime) = orphelines.

    python distrib/tools/check_settings_coverage.py [--root <depot>] [--src <dossier de src/>]

Code de sortie 0 = complet, 1 = il manque quelque chose (chaque manque est
liste, avec une ligne a completer). Appele par `sauerrt.ps1 release` : la
version n'est pas construite tant que la liste n'est pas vide.

Une entree de settings-keywords.txt "variable: -" dit expres "pas de mots en
plus, le libelle suffit" (le jeu l'ignore).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import menus_plan  # noqa: E402

SETTINGS_REL = "data/assistant-settings.txt"
KEYWORDS_REL = "data/settings-keywords.txt"


def read_entries(path, keywords=False):
    """{variable: texte} des lignes "variable: texte" (pas les commentaires ni les =synonymes)"""
    out = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        key, val = line.split(":", 1)
        key = key.strip()
        if keywords and key.startswith("="):
            continue
        if re.match(r"^[A-Za-z_]\w*$", key):
            out.setdefault(key, val.strip())
    return out


def cfg_aliases(root, files=None):
    """noms definis dans les fichiers de menus (alias, pas des reglages du jeu)"""
    names = set()
    for rel in menus_plan.MENU_FILES + ("data/stdlib.cfg", "data/defaults.cfg", "data/game_fps.cfg"):
        f = Path((files or {}).get(rel) or Path(root) / rel)
        if f.is_file():
            names.update(re.findall(r"^\s*([A-Za-z_]\w*)\s*=", f.read_text(encoding="utf-8", errors="replace"), re.M))
    return names


def check(root, src=None, files=None):
    root = Path(root)
    menus = menus_plan.build(root, src, files)
    gvars = menus_plan.game_vars(src or root)
    if not gvars:
        raise SystemExit(f"src/ introuvable sous {src or root} : impossible de distinguer reglages et commandes")
    descs = read_entries(root / SETTINGS_REL)
    words = read_entries(root / KEYWORDS_REL, keywords=True)
    aliases = cfg_aliases(root, files)

    seen = {}
    for var, page, label, kind in menus_plan.menu_settings(menus):
        seen.setdefault(var, (page, label, kind))
    menu_vars = {v: w for v, w in seen.items() if v in gvars}
    not_game = sorted(v for v in seen if v not in gvars)

    no_desc = [(v, *menu_vars[v]) for v in menu_vars if v not in descs]
    no_words = [(v, *menu_vars[v]) for v in menu_vars if v not in words]
    hidden = [(v, *menu_vars[v]) for v in menu_vars if descs.get(v) == "-"]
    known = set(gvars) | aliases
    orphan_desc = sorted(v for v in descs if v not in known)
    orphan_words = sorted(v for v in words if v not in known)
    return {
        "menus": menus, "menu_vars": menu_vars, "not_game": not_game, "no_desc": no_desc, "no_words": no_words,
        "hidden": hidden, "orphan_desc": orphan_desc, "orphan_words": orphan_words,
    }


def where(menus, page):
    rt = menus_plan.routes(menus).get(page)
    return rt[0] if rt else page


def report(r, out=print):
    menus = r["menus"]
    out(f"{len(r['menu_vars'])} reglages du jeu dans les pages de reglages")
    if r["not_game"]:
        out("  (alias des menus, pas des reglages du jeu, non controles : " + ", ".join(r["not_game"]) + ")")
    problems = 0
    if r["no_desc"]:
        problems += len(r["no_desc"])
        out(f"\nMANQUE dans {SETTINGS_REL} ({len(r['no_desc'])}) : ce que fait le reglage, ou \"-\" s'il ne doit jamais etre propose")
        for v, page, label, kind in r["no_desc"]:
            out(f"  {v}: ...        <- {kind} \"{label}\", {where(menus, page)}")
    if r["no_words"]:
        problems += len(r["no_words"])
        out(f"\nMANQUE dans {KEYWORDS_REL} ({len(r['no_words'])}) : mots de recherche (francais, synonymes), ou \"-\" si le libelle suffit")
        for v, page, label, kind in r["no_words"]:
            out(f"  {v}: ...        <- {kind} \"{label}\", {where(menus, page)}")
    if r["orphan_desc"]:
        problems += len(r["orphan_desc"])
        out(f"\nORPHELINES dans {SETTINGS_REL} ({len(r['orphan_desc'])}) : aucun reglage du jeu de ce nom (renomme ? supprime ?)")
        for v in r["orphan_desc"]:
            out(f"  {v}")
    if r["orphan_words"]:
        problems += len(r["orphan_words"])
        out(f"\nORPHELINES dans {KEYWORDS_REL} ({len(r['orphan_words'])}) : aucun reglage du jeu de ce nom")
        for v in r["orphan_words"]:
            out(f"  {v}")
    if r["hidden"]:
        out(f"\nA noter (pas bloquant) : reglages des menus marques \"-\" (jamais proposes par l'assistant) : "
            + ", ".join(v for v, *_ in r["hidden"]))
    out("\n" + (f"{problems} manque(s) : completer les fichiers ci-dessus avant de publier." if problems
                else "complet : chaque reglage des menus est connu de l'assistant et de la recherche."))
    return problems


def main(argv=None):
    ap = argparse.ArgumentParser(description="reglages des menus inconnus de l'assistant / de la recherche")
    ap.add_argument("--root", default=str(menus_plan.ROOT), help="depot (data/, traduction.cfg)")
    ap.add_argument("--src", default=None, help="dossier qui contient src/ (defaut : --root)")
    args = ap.parse_args(argv)
    r = check(args.root, args.src)
    return 1 if report(r) else 0


if __name__ == "__main__":
    sys.exit(main())
