#!/usr/bin/env python3
"""Construit une version L4ZY publiable, depuis n'importe quel PC de dev.

  python sauerrt_build.py fetch
      Recupere les composants tiers epingles (pins.json), verifie leurs SHA-256.
  python sauerrt_build.py release recettes/<version>.json [--reuse <manifest>]
      Assemble l'arborescence, cree les composants (zip nommes par contenu),
      le manifeste, le pointeur de canal candidat et l'installateur .exe.

Aucun chemin personnel : tout est relatif au depot (dossier parent de
distrib/) ou donne dans la recette. Aucun secret de publication ici : la
publication est un autre outil (sauerrt_publish.py) et rien de ce dossier
n'est embarque dans le client, a part service/ et launcher/ compiles.

Bibliotheque standard uniquement (tourne avec le Python embarque epingle).
"""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
DISTRIB = TOOLS.parent
REPO = DISTRIB.parent
CACHE = DISTRIB / ".cache"
OUT = DISTRIB / "out"
PINS = DISTRIB / "pins.json"
sys.path.insert(0, str(DISTRIB / "service"))
import updater as upd  # noqa: E402  (memes regles de chemins que le client)

ZIP_DATE = (2020, 1, 1, 0, 0, 0)
STORE_EXT = {".ogg", ".jpg", ".jpeg", ".png", ".gz", ".zip", ".wav", ".mp3", ".webp", ".dds", ".ogz"}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def log(msg):
    print(msg, flush=True)


def die(msg):
    print("ERREUR : " + msg, file=sys.stderr)
    sys.exit(1)


# ------------------------------------------------------------------ fetch

def download(url, dest, sha):
    dest = Path(dest)
    if dest.is_file() and sha256_file(dest) == sha:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    log(f"  telechargement {url}")
    tmp = dest.with_name(dest.name + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "l4zy-build"})
    with urllib.request.urlopen(req, timeout=120) as resp, open(tmp, "wb") as fh:
        shutil.copyfileobj(resp, fh, 1 << 20)
    got = sha256_file(tmp)
    if got != sha:
        tmp.unlink()
        die(f"empreinte inattendue pour {url}\n  attendu {sha}\n  obtenu  {got}")
    os.replace(tmp, dest)
    return dest


def fetch_vcredist(pin):
    """Runtime Visual C++ (MSVCP140...) depuis le paquet officiel Microsoft
    'CRT.Redist' du canal Visual Studio. Redistribuable app-local."""
    dest = CACHE / "vcredist"
    want = pin["files"]
    if all((dest / n).is_file() and sha256_file(dest / n) == s for n, s in want.items()):
        return dest
    log("  runtime Visual C++ : lecture du canal Visual Studio")
    ch = json.load(urllib.request.urlopen(pin["channel"], timeout=60))
    item = next(x for x in ch["channelItems"] if x["id"] == "Microsoft.VisualStudio.Manifests.VisualStudio")
    vs = json.load(urllib.request.urlopen(item["payloads"][0]["url"], timeout=120))
    pkg = None
    for p in vs["packages"]:
        if p["id"].lower() == pin["package"].lower():
            pkg = p
            break
    if pkg is None:
        die(f"paquet {pin['package']} absent du canal Visual Studio (version retiree ?)")
    pay = pkg["payloads"][0]
    raw = urllib.request.urlopen(pay["url"], timeout=120).read()
    if hashlib.sha256(raw).hexdigest() != pay["sha256"].lower():
        die("paquet CRT.Redist : empreinte Microsoft incorrecte")
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(raw)) as zf:
        for info in zf.infolist():
            name = info.filename.replace("\\", "/").split("/")[-1]
            if name.lower() in {n.lower() for n in want} and "/x64/" in info.filename.replace("\\", "/").lower():
                (dest / name.lower()).write_bytes(zf.read(info))
    for n, s in want.items():
        if not (dest / n).is_file() or sha256_file(dest / n) != s:
            die(f"vcredist {n} : empreinte differente de pins.json")
    return dest


def cmd_fetch(args):
    pins = json.loads(PINS.read_text(encoding="utf-8"))
    for key, pin in pins.items():
        if key.startswith("_"):
            continue
        if key == "vcredist":
            fetch_vcredist(pin)
        else:
            download(pin["url"], CACHE / pin["file"], pin["sha256"])
        log(f"  ok {key}")
    return pins


# ---------------------------------------------------------------- assemble

def component_of(rel):
    parts = rel.split("/")
    if rel == "L4ZY.exe":
        return "launcher"
    top = parts[0]
    if top == "bin64":
        if len(parts) > 2 and parts[1].startswith("ngx"):
            return "ngx"
        if len(parts) > 2 and parts[1] == "nrd":
            return "nrd"
        if len(parts) > 2 and parts[1] == "fsr":
            return "fsr"
        return "game-bin"
    if top == "data" or len(parts) == 1:
        return "game-data"
    if top == "packages":
        if len(parts) == 2:
            return "pkg-root"
        return "pkg-" + parts[1].lower().replace("_", "-")
    if top == "runtime":
        return parts[1] if len(parts) > 2 else "runtime"
    return top


class Tree:
    def __init__(self, root):
        self.root = Path(root)
        self.files = {}

    def add(self, rel, src, expect=None):
        upd.check_rel(rel)
        src = Path(src)
        if not src.is_file():
            die(f"source manquante pour {rel} : {src}")
        if expect and sha256_file(src) != expect.lower():
            die(f"{rel} : empreinte source differente de la recette ({src})")
        if rel.lower() in {k.lower() for k in self.files}:
            die(f"{rel} ajoute deux fois")
        self.files[rel] = src

    def add_dir(self, prefix, src_dir, exclude=()):
        src_dir = Path(src_dir)
        if not src_dir.is_dir():
            die(f"dossier source manquant : {src_dir}")
        n = 0
        for path in sorted(src_dir.rglob("*")):
            if not path.is_file():
                continue
            rel_src = path.relative_to(src_dir).as_posix()
            if any(fnmatch.fnmatch(rel_src.lower(), pat.lower()) for pat in exclude):
                continue
            self.add(f"{prefix}/{rel_src}" if prefix else rel_src, path)
            n += 1
        return n

    def materialize(self):
        if self.root.exists():
            shutil.rmtree(self.root)
        for rel, src in sorted(self.files.items()):
            dst = self.root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)


def src_path(p):
    p = Path(p)
    return p if p.is_absolute() else REPO / p


def build_launcher(out_exe):
    bat = DISTRIB / "launcher" / "build.bat"
    subprocess.run(["cmd", "/c", str(bat), str(out_exe)], check=True)
    return out_exe


def extract_zip_members(zpath, dest, names):
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zpath) as zf:
        have = {i.filename.split("/")[-1]: i for i in zf.infolist() if not i.is_dir()}
        for n in names:
            if n not in have:
                die(f"{n} absent de {zpath.name}")
            (dest / n).write_bytes(zf.read(have[n]))


def assemble(recipe, pins, work):
    t = Tree(work / "tree")
    tmp = work / "tmp"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir(parents=True)

    # Lanceur, compile depuis distrib/launcher.
    exe = build_launcher(tmp / "L4ZY.exe")
    t.add("L4ZY.exe", exe)

    # Entrees du jeu : fichiers et dossiers decrits par la recette.
    for item in recipe.get("files", []):
        t.add(item["dest"], src_path(item["src"]), item.get("sha256"))
    for item in recipe.get("dirs", []):
        n = t.add_dir(item["dest"], src_path(item["src"]), item.get("exclude", []))
        log(f"  {item['dest']}: {n} fichiers")

    # Python embarque officiel, avec un ._pth qui ajoute ../../service.
    py = tmp / "python"
    with zipfile.ZipFile(CACHE / pins["python"]["file"]) as zf:
        zf.extractall(py)
    pth = next(py.glob("python3*._pth"))
    pth.write_text(
        "python314.zip\n.\n..\\..\\service\n# Python prive de L4ZY : isole, aucun site-packages.\n",
        encoding="utf-8",
    )
    t.add_dir("runtime/python", py)

    # llama.cpp Vulkan : seulement ce que llama-server charge reellement.
    ll = tmp / "llama"
    extract_zip_members(CACHE / pins["llama"]["file"], ll, pins["llama"]["keep"])
    shutil.copyfile(CACHE / pins["llama_license"]["file"], ll / "LICENSE-llama.cpp.txt")
    for n in pins["vcredist"]["files"]:
        shutil.copyfile(fetch_vcredist(pins["vcredist"]) / n, ll / n)
    t.add_dir("runtime/llama", ll)

    # Service (code distribue seulement ; aucun fichier personnel).
    svc = tmp / "service"
    svc.mkdir()
    for p in sorted((DISTRIB / "service").iterdir()):
        if p.suffix in (".py", ".txt") and p.is_file():
            shutil.copyfile(p, svc / p.name)
    product = {
        "product": "l4zy",
        "default_channel": recipe["channel"],
        "channels": recipe.get("channel_urls") or json.loads((DISTRIB / "channels.json").read_text(encoding="utf-8")),
    }
    (svc / "product.json").write_text(json.dumps(product, indent=1, sort_keys=True), encoding="utf-8")
    t.add_dir("service", svc)

    # Documents et licences.
    t.add_dir("docs", DISTRIB / "docs")
    for item in recipe.get("docs", []):
        t.add(item["dest"], src_path(item["src"]))
    return t


# ------------------------------------------------------------- composants

def file_table(tree):
    table = {}
    for rel in sorted(tree.files):
        p = tree.root / rel
        table[rel] = [p.stat().st_size, sha256_file(p)]
    return table


def comp_id(files):
    h = hashlib.sha256()
    for rel in sorted(files):
        size, sha = files[rel]
        h.update(f"{rel}\t{size}\t{sha}\n".encode("utf-8"))
    return h.hexdigest()


def write_zip(path, root, files):
    tmp = path.with_name(path.name + ".tmp")
    with zipfile.ZipFile(tmp, "w", allowZip64=True) as zf:
        for rel in sorted(files):
            info = zipfile.ZipInfo(rel, ZIP_DATE)
            ext = os.path.splitext(rel)[1].lower()
            info.compress_type = zipfile.ZIP_STORED if ext in STORE_EXT else zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(root / rel, "rb") as src, zf.open(info, "w", force_zip64=True) as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
    os.replace(tmp, path)


def load_reuse(path):
    if not path:
        return {}
    if str(path).startswith("https://"):
        raw = urllib.request.urlopen(path, timeout=60).read()
        base = str(path)
    else:
        raw = Path(path).read_bytes()
        base = None
    man = json.loads(raw.decode("utf-8"))
    out = {}
    for name, c in man.get("components", {}).items():
        out[c["id"]] = dict(c, _base=base)
    return out


def cmd_release(args):
    pins = cmd_fetch(args)
    recipe_path = Path(args.recipe).resolve()
    recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
    version = recipe["version"]
    if upd.parse_version(version) is None:
        die(f"version invalide {version} (attendu ex. 2026.9.25.1)")
    work = OUT / version
    site = work / "site"
    if (site / "releases" / version / "manifest.json").exists() and not args.force:
        die(f"{version} existe deja dans {work}. Une version publiee est immuable : choisis un nouveau numero.")
    log(f"L4ZY {version} ({recipe['channel']})")
    tree = assemble(recipe, pins, work)
    log("  copie de l'arborescence...")
    tree.materialize()
    table = file_table(tree)
    groups = {}
    for rel, meta in table.items():
        groups.setdefault(component_of(rel), {})[rel] = meta

    reuse = load_reuse(args.reuse)
    comp_dir = site / "components"
    comp_dir.mkdir(parents=True, exist_ok=True)
    comps = {}
    for name in sorted(groups):
        files = groups[name]
        cid = comp_id(files)
        entry = {"id": cid, "files": files}
        prev = reuse.get(cid)
        if prev:
            entry.update(archive_sha256=prev["archive_sha256"], archive_size=prev["archive_size"],
                         url=prev["url"] if not prev.get("_base") else prev["url"])
            log(f"  {name}: inchange (reutilise {prev['url']})")
        else:
            zname = f"{name}-{cid[:16]}.zip"
            zpath = comp_dir / zname
            if not zpath.exists():
                write_zip(zpath, tree.root, files)
            entry.update(archive_sha256=sha256_file(zpath), archive_size=zpath.stat().st_size,
                         url=f"../../components/{zname}")
            log(f"  {name}: {len(files)} fichiers, {zpath.stat().st_size / 1e6:.1f} Mo")
        comps[name] = entry

    git = ""
    try:
        git = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        pass
    man = {
        "format": upd.FORMAT,
        "product": upd.PRODUCT,
        "platform": upd.PLATFORM,
        "version": version,
        "channel": recipe["channel"],
        "label": recipe.get("label", ""),
        "notes": recipe.get("notes", ""),
        "released": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "features": recipe.get("features", {}),
        "build": {
            "recipe": recipe_path.name,
            "recipe_sha256": sha256_file(recipe_path),
            "git_head": git,
            "base": recipe.get("base", {}),
        },
        "components": comps,
    }
    upd.validate_manifest(json.loads(json.dumps(man)))
    rel_dir = site / "releases" / version
    rel_dir.mkdir(parents=True, exist_ok=True)
    raw = json.dumps(man, indent=1, sort_keys=True).encode("utf-8")
    (rel_dir / "manifest.json").write_bytes(raw)
    msha = hashlib.sha256(raw).hexdigest()
    pointer = {
        "format": upd.FORMAT,
        "product": upd.PRODUCT,
        "platform": upd.PLATFORM,
        "channel": recipe["channel"],
        "version": version,
        "manifest": f"../../releases/{version}/manifest.json",
        "manifest_sha256": msha,
        "manifest_size": len(raw),
    }
    ch_dir = site / "channels" / recipe["channel"]
    ch_dir.mkdir(parents=True, exist_ok=True)
    (ch_dir / "latest.json").write_text(json.dumps(pointer, indent=1, sort_keys=True), encoding="utf-8")

    # Etat installe livre par l'installateur (meme format que les mises a jour).
    state = work / "state"
    state.mkdir(exist_ok=True)
    inst = {
        "format": upd.FORMAT, "product": upd.PRODUCT, "platform": upd.PLATFORM,
        "version": version, "channel": recipe["channel"], "label": recipe.get("label", ""),
        "manifest_sha256": msha, "installed_from": "installer",
        "components": {n: {"id": c["id"], "files": c["files"]} for n, c in comps.items()},
    }
    (state / "installed.json").write_text(json.dumps(inst, indent=1, sort_keys=True), encoding="utf-8", newline="\n")
    envp = "1" if recipe.get("features", {}).get("service_port_env") else "0"
    (state / "installed.ini").write_text(
        f"version={version}\nchannel={recipe['channel']}\nservice_port_env={envp}\n", encoding="utf-8", newline="\n")

    if not args.no_installer:
        build_installer(recipe, pins, work, version)

    summary = [f"L4ZY {version} canal {recipe['channel']}", f"manifeste {msha}", ""]
    for n, c in sorted(comps.items()):
        summary.append(f"{c['archive_sha256']}  {c['archive_size']:>12}  {n}  {c['url']}")
    (work / "EMPREINTES.txt").write_text("\n".join(summary) + "\n", encoding="utf-8")
    log(f"OK : {work}")


# --------------------------------------------------------------- installeur

def build_installer(recipe, pins, work, version):
    nsis_dir = CACHE / "nsis"
    if not (nsis_dir / "makensis.exe").is_file():
        with zipfile.ZipFile(CACHE / pins["nsis"]["file"]) as zf:
            zf.extractall(CACHE / "nsis-tmp")
        inner = next((CACHE / "nsis-tmp").iterdir())
        if nsis_dir.exists():
            shutil.rmtree(nsis_dir)
        shutil.move(str(inner), str(nsis_dir))
        shutil.rmtree(CACHE / "nsis-tmp", ignore_errors=True)
    inst_dir = work / "installer"
    inst_dir.mkdir(exist_ok=True)
    out = inst_dir / f"L4ZY-Setup-{version}.exe"
    starter = recipe.get("starter_model", "small")
    defs = [
        f"/DVERSION={version}",
        f"/DCHANNEL={recipe['channel']}",
        f"/DSTARTER={starter}",
        f"/DTREE={work / 'tree'}",
        f"/DSTATE={work / 'state'}",
        f"/DOUTFILE={out}",
        f"/DICON={DISTRIB / 'launcher' / 'l4zy.ico'}",
        f"/DLABEL={recipe.get('label', '')}",
    ]
    log("  installateur NSIS...")
    subprocess.run([str(nsis_dir / "makensis.exe"), "/V2", "/INPUTCHARSET", "UTF8"] + defs
                   + [str(DISTRIB / "installer" / "l4zy.nsi")], check=True)
    log(f"  {out.name} : {out.stat().st_size / 1e6:.0f} Mo, sha256 {sha256_file(out)}")


def cmd_seed(args):
    """Autre PC de dev : recupere depuis un canal publie les composants dont on
    n'a pas les fichiers (ex. pkg-* = cartes/textures), verifies comme le client."""
    if args.allow_local_http:
        os.environ["L4ZY_TEST_ALLOW_LOCAL_HTTP"] = "1"
    from urllib.parse import urljoin
    ptr = json.loads(upd.fetch_small(args.channel, upd.MAX_POINTER).decode("utf-8-sig"))
    murl = urljoin(args.channel, ptr["manifest"])
    raw = upd.fetch_small(murl, upd.MAX_MANIFEST)
    if hashlib.sha256(raw).hexdigest() != ptr["manifest_sha256"]:
        die("empreinte du manifeste fausse")
    man = upd.validate_manifest(json.loads(raw.decode("utf-8")), expect_version=ptr["version"])
    dest = Path(args.dest).resolve()
    pats = [p.strip() for p in args.components.split(",") if p.strip()]
    for name, c in sorted(man["components"].items()):
        if not any(fnmatch.fnmatch(name, p) for p in pats):
            continue
        z = CACHE / "seed" / f"{c['archive_sha256']}.zip"
        upd._download_archive(urljoin(murl, c["url"]), z, int(c["archive_size"]), c["archive_sha256"], lambda n: None)
        for rel in c["files"]:
            upd.safe_target(dest, rel)
            if (dest / rel).exists() and not args.overwrite:
                die(f"{rel} existe deja (ajouter --overwrite pour remplacer)")
        upd._extract_component(z, c, dest)
        log(f"  {name}: {len(c['files'])} fichiers")
    log(f"OK : composants de {ptr['version']} poses dans {dest}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fetch")
    s = sub.add_parser("seed", help="recuperer des composants publies (ex. les cartes) dans le depot")
    s.add_argument("--channel", required=True, help="URL du pointeur latest.json")
    s.add_argument("--components", default="pkg-*")
    s.add_argument("--dest", default=str(REPO))
    s.add_argument("--overwrite", action="store_true")
    s.add_argument("--allow-local-http", action="store_true")
    r = sub.add_parser("release")
    r.add_argument("recipe")
    r.add_argument("--reuse", help="manifeste precedent (chemin ou URL https) : composants inchanges non reconstruits")
    r.add_argument("--no-installer", action="store_true")
    r.add_argument("--force", action="store_true", help="reconstruire une version locale NON publiee")
    args = ap.parse_args()
    if args.cmd == "fetch":
        cmd_fetch(args)
    elif args.cmd == "seed":
        cmd_seed(args)
    else:
        cmd_release(args)


if __name__ == "__main__":
    main()
