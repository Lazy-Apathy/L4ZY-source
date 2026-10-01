#!/usr/bin/env python3
"""Publie une version L4ZY construite par sauerrt_build.py.

  python sauerrt_publish.py publish --version 2026.9.25.1 --channel test --target <dossier>
  python sauerrt_publish.py verify  --url https://.../channels/test/latest.json

Regles (identiques depuis n'importe quel PC de dev) :
  1. une version publiee est immuable : si releases/<v>/manifest.json existe
     deja avec un autre contenu, refus ; composants nommes par leur contenu ;
  2. jamais de retour en arriere d'un canal : si le canal annonce deja une
     version >= celle-ci, refus (deux PC qui publient ne s'ecrasent pas) ;
  3. fichiers d'abord, pointeur latest.json EN DERNIER ;
  4. aucune cle ni jeton dans ce depot : le dossier cible est un dossier
     synchronise/monte (hebergement statique), ou un backend a ajouter qui lit
     son jeton dans une variable d'environnement au moment de publier.

`verify` refait exactement le parcours du client (HTTPS, empreintes, tailles,
chemins) : c'est la preuve que le canal publie est telechargeable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import urllib.error
import urllib.request
from pathlib import Path
from urllib.parse import urljoin

TOOLS = Path(__file__).resolve().parent
DISTRIB = TOOLS.parent
OUT = DISTRIB / "out"
sys.path.insert(0, str(DISTRIB / "service"))
import updater as upd  # noqa: E402


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def die(msg):
    print("REFUS : " + msg, file=sys.stderr)
    sys.exit(1)


def find_local_component(url_rel):
    name = url_rel.split("/")[-1]
    for d in sorted(OUT.glob("*/site/components")):
        p = d / name
        if p.is_file():
            return p
    return None


def copy_immutable(src, dst):
    dst = Path(dst)
    if dst.exists():
        if sha256_file(dst) != sha256_file(src):
            die(f"{dst} existe deja avec un autre contenu (fichier publie immuable)")
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + ".uploading")
    shutil.copyfile(src, tmp)
    os.replace(tmp, dst)
    return True


def cmd_publish(a):
    ver = a.version
    local = OUT / ver
    man_path = local / "site" / "releases" / ver / "manifest.json"
    if not man_path.is_file():
        die(f"{man_path} absent : construire d'abord la version")
    raw = man_path.read_bytes()
    man = upd.validate_manifest(json.loads(raw.decode("utf-8")))
    if man["version"] != ver:
        die("manifeste incoherent")
    target = Path(a.target)
    target.mkdir(parents=True, exist_ok=True)

    # 2. canal : jamais en arriere, jamais la meme version republiee autrement
    ptr_path = target / "channels" / a.channel / "latest.json"
    if ptr_path.is_file():
        cur = json.loads(ptr_path.read_text(encoding="utf-8"))
        cv = str(cur.get("version") or "0")
        if not upd.is_newer(ver, cv) and not (a.repoint and cv == ver):
            die(f"le canal {a.channel} annonce deja {cv} (>= {ver}). Choisis un numero plus recent.")
    # 1. manifeste immuable
    man_dst = target / "releases" / ver / "manifest.json"
    if man_dst.is_file() and man_dst.read_bytes() != raw:
        die(f"releases/{ver} deja publie avec un autre contenu")

    # composants : tous presents et exacts avant d'ecrire le moindre pointeur
    sent = 0
    for name, c in sorted(man["components"].items()):
        src = find_local_component(c["url"])
        dst = target / "components" / c["url"].split("/")[-1]
        if dst.is_file() and dst.stat().st_size == c["archive_size"] and sha256_file(dst) == c["archive_sha256"]:
            continue
        if src is None:
            die(f"archive du composant {name} introuvable localement ({c['url']})")
        if src.stat().st_size != c["archive_size"] or sha256_file(src) != c["archive_sha256"]:
            die(f"archive locale du composant {name} differente du manifeste")
        if copy_immutable(src, dst):
            sent += 1
            print(f"  + {dst.relative_to(target)}")
    copy_immutable(man_path, man_dst)
    inst = local / "installer" / f"L4ZY-Setup-{ver}.exe"
    if inst.is_file() and not a.no_installer:
        if copy_immutable(inst, target / "installers" / inst.name):
            print(f"  + installers/{inst.name}")

    # 3. pointeur en dernier
    msha = hashlib.sha256(raw).hexdigest()
    pointer = {
        "format": upd.FORMAT, "product": upd.PRODUCT, "platform": upd.PLATFORM,
        "channel": a.channel, "version": ver,
        "manifest": f"../../releases/{ver}/manifest.json",
        "manifest_sha256": msha, "manifest_size": len(raw),
    }
    if man.get("channel") != a.channel:
        print(f"  note : version construite pour '{man.get('channel')}', annoncee sur '{a.channel}'")
    ptr_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = ptr_path.with_name("latest.json.tmp")
    tmp.write_text(json.dumps(pointer, indent=1, sort_keys=True), encoding="utf-8")
    os.replace(tmp, ptr_path)
    print(f"OK : {a.channel} -> {ver} ({sent} archive(s) nouvelle(s))")


# ------------------------------------------------------------------ GitHub

GH_API = "https://api.github.com"


def gh_token():
    """Jeton du gestionnaire d'identifiants Windows (celui de git), lu en
    memoire au moment de publier. Jamais ecrit, jamais affiche."""
    import subprocess
    out = subprocess.run(["git", "credential", "fill"], input="protocol=https\nhost=github.com\n\n",
                         capture_output=True, text=True, env={**os.environ, "GIT_TERMINAL_PROMPT": "0"})
    tok = user = ""
    for line in out.stdout.splitlines():
        if line.startswith("password="):
            tok = line[9:]
        elif line.startswith("username="):
            user = line[9:]
    if not tok:
        die("aucun acces GitHub enregistre sur ce PC (se connecter une fois avec git ou GitHub Desktop)")
    return user, tok


def gh(method, url, tok, data=None, headers=None, raw=None, length=None):
    h = {"Authorization": f"Bearer {tok}", "Accept": "application/vnd.github+json",
         "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "l4zy-publish"}
    h.update(headers or {})
    body = raw
    if data is not None:
        body = json.dumps(data).encode("utf-8")
        h["Content-Type"] = "application/json"
    if length is not None:
        h["Content-Length"] = str(length)
    req = urllib.request.Request(url if url.startswith("http") else GH_API + url, data=body, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            txt = r.read().decode("utf-8")
            return json.loads(txt) if txt else {}
    except urllib.error.HTTPError as e:
        if e.code == 404 and method == "GET":
            return None
        die(f"GitHub {method} {url} : {e.code} {e.read()[:300]!r}")


def gh_assets(repo, tok):
    found = {}
    page = 1
    while True:
        rels = gh("GET", f"/repos/{repo}/releases?per_page=100&page={page}", tok) or []
        for r in rels:
            for asset in r.get("assets", []):
                found[asset["name"]] = (asset["browser_download_url"], asset["size"])
        if len(rels) < 100:
            return found
        page += 1


def gh_upload(repo, rel, path, tok, name=None):
    name = name or Path(path).name
    size = Path(path).stat().st_size
    print(f"  envoi {name} ({size / 1e6:.1f} Mo)")
    with open(path, "rb") as fh:
        res = gh("POST", f"https://uploads.github.com/repos/{repo}/releases/{rel['id']}/assets?name={name}",
                 tok, raw=fh, length=size, headers={"Content-Type": "application/octet-stream"})
    return res["browser_download_url"]


def cmd_publish_github(a):
    import base64
    import urllib.error  # noqa: F401
    ver, repo, channel = a.version, a.repo, a.channel
    local = OUT / ver
    man = upd.validate_manifest(json.loads((local / "site" / "releases" / ver / "manifest.json").read_text(encoding="utf-8")))
    user, tok = gh_token()
    print(f"compte GitHub : {user}")

    # canal : jamais en arriere
    ptr_path = f"channels/{channel}/latest.json"
    cur = gh("GET", f"/repos/{repo}/contents/{ptr_path}", tok)
    cur_sha = None
    if cur:
        cur_sha = cur["sha"]
        cv = json.loads(base64.b64decode(cur["content"]).decode("utf-8")).get("version", "0")
        if not upd.is_newer(ver, cv):
            die(f"le canal {channel} annonce deja {cv} (>= {ver})")

    tag = f"v{ver}"
    rel = gh("GET", f"/repos/{repo}/releases/tags/{tag}", tok)
    existing = gh_assets(repo, tok)
    manifest_name = f"manifest-{ver}.json"
    if a.pointer_only:
        # Version deja publiee : on annonce seulement ce meme manifeste sur un
        # autre canal (ex. test en plus de stable). Rien n'est reenvoye.
        if not rel or manifest_name not in existing:
            die(f"{tag} n'est pas encore publiee")
        murl = existing[manifest_name][0]
        raw = urllib.request.urlopen(urllib.request.Request(murl, headers={"User-Agent": "l4zy-publish"}), timeout=60).read()
        upd.validate_manifest(json.loads(raw.decode("utf-8")), expect_version=ver)
        write_pointer(repo, tok, channel, ver, murl, raw, cur_sha, ptr_path)
        return
    if rel and any(x["name"] == manifest_name for x in rel.get("assets", [])):
        die(f"{tag} est deja publiee (immuable). Choisis un numero plus recent.")
    if not rel:
        rel = gh("POST", f"/repos/{repo}/releases", tok, data={
            "tag_name": tag, "target_commitish": "main", "name": f"L4ZY {ver}",
            "body": (man.get("notes") or "") + f"\n\nChannel: {channel}. Install with L4ZY-Setup-{ver}.exe; updates come through the in-game Updates menu.",
            "prerelease": channel != "stable"})

    # composants : reutilises s'ils existent deja dans une release, sinon envoyes ici
    hosted = json.loads(json.dumps(man))
    for name, c in sorted(hosted["components"].items()):
        zname = c["url"].split("/")[-1]
        if zname in existing and existing[zname][1] == c["archive_size"]:
            c["url"] = existing[zname][0]
            continue
        src = find_local_component(c["url"])
        if src is None or src.stat().st_size != c["archive_size"] or sha256_file(src) != c["archive_sha256"]:
            die(f"archive locale du composant {name} absente ou differente")
        c["url"] = gh_upload(repo, rel, src, tok, zname)
    raw = json.dumps(hosted, indent=1, sort_keys=True).encode("utf-8")
    mfile = local / manifest_name
    mfile.write_bytes(raw)
    inst = local / "installer" / f"L4ZY-Setup-{ver}.exe"
    if not inst.is_file():
        inst = REPO_SHOTS_INSTALLER(ver)
    if inst and inst.is_file() and not a.no_installer:
        gh_upload(repo, rel, inst, tok)
    murl = gh_upload(repo, rel, mfile, tok)

    # pointeur en dernier
    write_pointer(repo, tok, channel, ver, murl, raw, cur_sha, ptr_path)
    push_readme(repo, tok, f"README: L4ZY {ver}")


def write_pointer(repo, tok, channel, ver, murl, raw, cur_sha, ptr_path):
    import base64
    pointer = {"format": upd.FORMAT, "product": upd.PRODUCT, "platform": upd.PLATFORM, "channel": channel,
               "version": ver, "manifest": murl, "manifest_sha256": hashlib.sha256(raw).hexdigest(),
               "manifest_size": len(raw)}
    body = {"message": f"{channel}: L4ZY {ver}", "branch": "main",
            "content": base64.b64encode(json.dumps(pointer, indent=1, sort_keys=True).encode("utf-8")).decode("ascii")}
    if cur_sha:
        body["sha"] = cur_sha
    gh("PUT", f"/repos/{repo}/contents/{ptr_path}", tok, data=body)
    print(f"OK : {channel} -> {ver}  (https://github.com/{repo}/releases/tag/v{ver})")


def push_readme(repo, tok, message):
    """Met la page d'accueil du depot a jour depuis distrib/github/README.md
    (versionne avec les sources : on le complete a chaque nouvelle version)."""
    import base64
    src = DISTRIB / "github" / "README.md"
    if not src.is_file():
        return
    new = src.read_bytes()
    cur = gh("GET", f"/repos/{repo}/contents/README.md", tok)
    if cur and base64.b64decode(cur["content"]) == new:
        print("  README deja a jour")
        return
    body = {"message": message, "branch": "main", "content": base64.b64encode(new).decode("ascii")}
    if cur:
        body["sha"] = cur["sha"]
    gh("PUT", f"/repos/{repo}/contents/README.md", tok, data=body)
    print("  README mis a jour")


def cmd_readme_github(a):
    user, tok = gh_token()
    push_readme(a.repo, tok, "README update")


def REPO_SHOTS_INSTALLER(ver):
    for p in (DISTRIB.parent / "shots").glob(f"*/livraison/L4ZY-Setup-{ver}.exe"):
        return p
    return None


def cmd_verify(a):
    """Parcours client complet sur le canal publie (telecharge tout)."""
    if a.allow_local_http:
        os.environ["L4ZY_TEST_ALLOW_LOCAL_HTTP"] = "1"
    ptr = json.loads(upd.fetch_small(a.url, upd.MAX_POINTER).decode("utf-8-sig"))
    if ptr.get("product") != upd.PRODUCT:
        die("pointeur etranger")
    murl = urljoin(a.url, ptr["manifest"])
    raw = upd.fetch_small(murl, upd.MAX_MANIFEST)
    if hashlib.sha256(raw).hexdigest() != ptr["manifest_sha256"]:
        die("empreinte du manifeste fausse")
    man = upd.validate_manifest(json.loads(raw.decode("utf-8")), expect_version=ptr["version"])
    total = 0
    for name, c in sorted(man["components"].items()):
        with upd._open(urljoin(murl, c["url"]), timeout=60) as resp:
            h = hashlib.sha256()
            n = 0
            while True:
                chunk = resp.read(1 << 20)
                if not chunk:
                    break
                h.update(chunk)
                n += len(chunk)
        ok = n == c["archive_size"] and h.hexdigest() == c["archive_sha256"]
        total += n
        print(f"  {'ok ' if ok else 'KO '} {name} {n} octets")
        if not ok:
            die(f"composant {name} incorrect sur le canal")
    print(f"OK : canal {ptr['channel']} version {ptr['version']}, {total / 1e6:.0f} Mo verifies")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("publish")
    p.add_argument("--version", required=True)
    p.add_argument("--channel", required=True, choices=["test", "stable"])
    p.add_argument("--target", required=True, help="dossier racine du site publie (synchronise ou monte)")
    p.add_argument("--no-installer", action="store_true")
    p.add_argument("--repoint", action="store_true", help="reecrire le pointeur vers la MEME version (reparation)")
    g = sub.add_parser("publish-github")
    g.add_argument("--version", required=True)
    g.add_argument("--channel", required=True, choices=["test", "stable"])
    g.add_argument("--repo", default="Lazy-Apathy/L4ZY")
    g.add_argument("--no-installer", action="store_true")
    g.add_argument("--pointer-only", action="store_true", help="annoncer une version deja publiee sur un autre canal")
    rd = sub.add_parser("readme-github", help="publier seulement distrib/github/README.md")
    rd.add_argument("--repo", default="Lazy-Apathy/L4ZY")
    v = sub.add_parser("verify")
    v.add_argument("--url", required=True)
    v.add_argument("--allow-local-http", action="store_true", help="essais sur 127.0.0.1 uniquement")
    a = ap.parse_args()
    {"publish": cmd_publish, "publish-github": cmd_publish_github, "readme-github": cmd_readme_github, "verify": cmd_verify}[a.cmd](a)


if __name__ == "__main__":
    main()
