"""Mises a jour L4ZY : recherche, telechargement, verification, preparation.

Ce module ne remplace JAMAIS un fichier de l'installation active. Il prepare
tout dans state/update/stage (fichiers verifies + plan.txt), puis le lanceur,
une fois le jeu ferme, confie le plan a une copie de lui-meme qui l'applique
avec sauvegarde et retour arriere (launcher.cpp, mode --apply).

Canal : un pointeur latest.json (HTTPS) -> manifeste d'une version
(empreinte verifiee) -> archives de composants (empreinte + taille verifiees,
nommees par leur contenu, donc immuables). Seuls les composants dont
l'identifiant change sont telecharges : un correctif du jeu ne retelecharge ni
les cartes ni les modeles (les modeles ne font d'ailleurs partie d'aucun
composant : ce sont des donnees du joueur).

Protocole jeu (GET/POST /update), lignes cle=valeur ASCII :
  state avail progress msg ver remote installed size channel restart
avail : 0 rien, 1 disponible, 2 telechargement, 3 pret (redemarrer).
Compatible avec les anciens clients (1.2) qui n'envoient pas d'action.
"""

from __future__ import annotations

import hashlib
import http.client
import json
import os
import re
import shutil
import ssl
import threading
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath
from urllib.parse import urljoin, urlparse

import sauerrt_paths as paths

PRODUCT = "l4zy"
PLATFORM = "windows-x64"
FORMAT = 1
PLAN_FORMAT = 1
USER_AGENT = "l4zy-updater/1"
MAX_POINTER = 64 * 1024
MAX_MANIFEST = 32 * 1024 * 1024
MAX_ARCHIVE = 3 * 1024 * 1024 * 1024
CHECK_EVERY = 30 * 60
DISK_MARGIN = 200 * 1024 * 1024

# Ce qu'un composant a le droit d'ecrire dans l'installation. Tout le reste
# (state/, userdata/, l4zy.ini, fichiers inconnus) est hors d'atteinte.
ALLOWED_TOP_DIRS = {"bin64", "data", "packages", "runtime", "service", "docs"}
ALLOWED_ROOT_FILES = {"L4ZY.exe", "autoexec.cfg", "traduction.cfg", "LISEZMOI.txt", "README.txt"}
RESERVED = re.compile(r"^(con|prn|aux|nul|com[0-9]|lpt[0-9]|conin\$|conout\$)(\..*)?$", re.I)
VERSION_RE = re.compile(r"^\d{1,6}(\.\d{1,6}){1,4}$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")

_lock = threading.RLock()
_st = {
    "state": "idle",      # idle checking uptodate available downloading ready error
    "msg": "",
    "progress": 0,
    "remote": "",
    "size": 0,
    "checked": 0.0,
    "restart": False,
}
_manifest = None
_pointer = None
_worker = None
LOG_MAX = 512 * 1024
UNCHANGED = "Your game is unchanged."


class PortalError(RuntimeError):
    """The network answered with a web page (hotel/school sign-in page)."""


# --------------------------------------------------------------------------
# journal (state/logs/updater.log, next to launcher.log)

def log(text):
    """One dated line per event; errors carry their technical detail."""
    try:
        path = state_dir() / "logs" / "updater.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.is_file() and path.stat().st_size > LOG_MAX:
            os.replace(path, path.with_name("updater.old.log"))
        line = time.strftime("%Y-%m-%d %H:%M:%S ") + str(text).replace("\r", " ").replace("\n", " | ")
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError:
        pass


def _detail(exc):
    """Technical description for the journal (never shown in the game)."""
    parts = [type(exc).__name__]
    reason = getattr(exc, "reason", None)
    if reason is not None:
        parts.append(f"reason={type(reason).__name__}: {reason}")
    code = getattr(exc, "code", None)
    if code is not None:
        parts.append(f"code={code}")
    parts.append(str(exc)[:400])
    return " ".join(parts)


# --------------------------------------------------------------------------
# chemins et etat installe

def state_dir():
    paths.ensure()
    return paths.STATE


def upd_dir():
    return state_dir() / "update"


def stage_dir():
    return upd_dir() / "stage"


def dl_dir():
    return upd_dir() / "downloads"


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return default


def write_atomic(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def installed():
    data = read_json(state_dir() / "installed.json", {}) or {}
    if not isinstance(data, dict):
        data = {}
    return data


def installed_version():
    return str(installed().get("version") or "0")


def product_info():
    return read_json(paths.CODE / "product.json", {}) or {}


def channel():
    ini = paths.read_ini()
    ch = ini.get("install", "channel", fallback="").strip()
    return ch or str(installed().get("channel") or product_info().get("default_channel") or "stable")


def channel_url():
    env = (os.environ.get("L4ZY_UPDATE_URL") or "").strip()
    if env:
        return env
    ini = paths.read_ini()
    url = ini.get("install", "channel_url", fallback="").strip()
    if url:
        return url
    return str((product_info().get("channels") or {}).get(channel()) or "")


# --------------------------------------------------------------------------
# versions, chemins, validation

def parse_version(text):
    text = str(text or "").strip()
    if not VERSION_RE.match(text):
        return None
    return tuple(int(x) for x in text.split("."))


def is_newer(remote, local):
    a, b = parse_version(remote), parse_version(local) or (0,)
    if a is None:
        return False
    n = max(len(a), len(b))
    return a + (0,) * (n - len(a)) > b + (0,) * (n - len(b))


def check_rel(rel):
    """Chemin relatif d'un fichier distribue. Refuse tout ce qui sort du cadre."""
    if not isinstance(rel, str) or not rel or len(rel) > 240:
        raise ValueError(f"bad path {rel!r}")
    if "\\" in rel or ":" in rel or rel.startswith("/") or "\x00" in rel:
        raise ValueError(f"bad path {rel!r}")
    parts = rel.split("/")
    for part in parts:
        if part in ("", ".", "..") or part != part.rstrip(" .") or RESERVED.match(part):
            raise ValueError(f"bad path {rel!r}")
        if any(ord(c) < 32 for c in part) or any(c in '<>"|?*' for c in part):
            raise ValueError(f"bad path {rel!r}")
    if len(parts) == 1:
        if parts[0] not in ALLOWED_ROOT_FILES:
            raise ValueError(f"file not allowed at install root: {rel}")
    elif parts[0] not in ALLOWED_TOP_DIRS:
        raise ValueError(f"folder not allowed: {rel}")
    return rel


def _is_reparse(path: Path):
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        return False
    attrs = getattr(st, "st_file_attributes", 0)
    return bool(attrs & 0x400) or os.path.islink(path)


def safe_target(root: Path, rel):
    """ROOT/rel, en refusant les jonctions/liens sur le chemin (pas de sortie
    du dossier d'installation par une jonction, meme preexistante)."""
    check_rel(rel)
    root = Path(root)
    cur = root
    for part in rel.split("/"):
        cur = cur / part
        if _is_reparse(cur):
            raise ValueError(f"refusing to write through a junction or link: {rel}")
    real_root = os.path.realpath(root)
    real = os.path.realpath(cur)
    if os.path.commonpath([real_root.lower(), real.lower()]) != real_root.lower():
        raise ValueError(f"path leaves the install folder: {rel}")
    return cur


def validate_manifest(man, expect_version=None):
    if not isinstance(man, dict):
        raise ValueError("manifest is not an object")
    if man.get("product") != PRODUCT:
        raise ValueError("manifest is not for L4ZY")
    if man.get("platform") != PLATFORM:
        raise ValueError("manifest is not for Windows x64")
    if int(man.get("format") or 0) != FORMAT:
        raise ValueError("unsupported manifest format, reinstall from the website")
    ver = str(man.get("version") or "")
    if parse_version(ver) is None:
        raise ValueError("manifest has a bad version")
    if expect_version and ver != expect_version:
        raise ValueError("manifest version does not match the channel")
    comps = man.get("components")
    if not isinstance(comps, dict) or not comps:
        raise ValueError("manifest has no components")
    seen = {}
    for name, comp in comps.items():
        if not re.match(r"^[a-z0-9][a-z0-9._-]{0,63}$", str(name)):
            raise ValueError(f"bad component name {name!r}")
        if not isinstance(comp, dict):
            raise ValueError(f"bad component {name}")
        if not SHA_RE.match(str(comp.get("id") or "")) or not SHA_RE.match(str(comp.get("archive_sha256") or "")):
            raise ValueError(f"component {name} has no valid hash")
        size = int(comp.get("archive_size") or 0)
        if size <= 0 or size > MAX_ARCHIVE:
            raise ValueError(f"component {name} has a bad size")
        if not str(comp.get("url") or ""):
            raise ValueError(f"component {name} has no url")
        files = comp.get("files")
        if not isinstance(files, dict) or not files:
            raise ValueError(f"component {name} lists no files")
        for rel, meta in files.items():
            check_rel(rel)
            if not (isinstance(meta, list) and len(meta) == 2 and int(meta[0]) >= 0 and SHA_RE.match(str(meta[1]))):
                raise ValueError(f"bad file entry {rel}")
            key = rel.lower()
            if key in seen:
                raise ValueError(f"file {rel} appears twice ({seen[key]}, {name})")
            seen[key] = name
    return man


# --------------------------------------------------------------------------
# reseau

def _ssl_context():
    return ssl.create_default_context()


def _check_url(url):
    p = urlparse(url)
    if p.scheme == "https":
        return
    # Tests locaux uniquement : http vers cette machine, sur demande explicite.
    if (p.scheme == "http" and p.hostname in ("127.0.0.1", "localhost")
            and os.environ.get("L4ZY_TEST_ALLOW_LOCAL_HTTP") == "1"):
        return
    raise RuntimeError("update address must use https")


def _is_html(resp, head=b""):
    ctype = ""
    if resp is not None:
        try:
            ctype = (resp.headers.get("Content-Type") or "").lower()
        except AttributeError:
            pass
    start = head.lstrip()[:15].lower()
    return "text/html" in ctype or start.startswith(b"<!doctype html") or start.startswith(b"<html")


def _open(url, headers=None, timeout=30):
    _check_url(url)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    resp = urllib.request.urlopen(req, timeout=timeout, context=_ssl_context()) \
        if url.startswith("https") else urllib.request.urlopen(req, timeout=timeout)
    final = resp.geturl()
    try:
        _check_url(final)  # une redirection ne doit pas quitter https
    except RuntimeError:
        resp.close()
        # Typical of a hotel/school network sending every request to its sign-in page.
        raise PortalError(f"redirected to a non-secure page: {final[:120]}")
    if _is_html(resp):
        resp.close()
        raise PortalError(f"got a web page (text/html) instead of an update file from {url[:120]}")
    return resp


def fetch_small(url, limit):
    with _open(url, timeout=20) as resp:
        data = resp.read(limit + 1)
    if len(data) > limit:
        raise RuntimeError("update file too large")
    if _is_html(None, data[:64]):
        raise PortalError(f"got a web page instead of an update file from {url[:120]}")
    return data


def _friendly(exc):
    """Short English sentence for the Updates menu (ASCII, one line)."""
    text = str(exc)
    if isinstance(exc, PortalError):
        return "The network showed a web page instead of the update (hotel/school wifi sign-in page?)."
    if isinstance(exc, urllib.error.HTTPError):
        return f"The update server answered with an error (HTTP {exc.code}). Try again later."
    reason = getattr(exc, "reason", None)
    if isinstance(exc, ssl.SSLError) or isinstance(reason, ssl.SSLError) or "CERTIFICATE" in text.upper():
        return ("Secure connection to the update server failed (a hotel/school network or an antivirus "
                "may be intercepting it).")
    if isinstance(exc, (urllib.error.URLError, TimeoutError, ConnectionError)) or "timed out" in text:
        return "Could not reach the update server (no internet, or a hotel/school network blocks it)."
    if isinstance(exc, ValueError) and "JSON" in type(exc).__name__ + text:
        return "The update server sent something unreadable (hotel/school network?)."
    if "not enough disk space" in text:
        return "Not enough disk space for the update (" + text.split(": ", 1)[-1][:60] + "). Free some space and try again."
    if "hash mismatch" in text or "corrupted" in text or "failed verification" in text:
        return "A downloaded file was damaged (checksum mismatch) and was deleted. Try again."
    if "incomplete" in text:
        return "The download was interrupted. Click again to resume where it stopped."
    return "Update failed: " + text[:120] + "."


def _fail(what, exc, **kw):
    msg = f"{_friendly(exc)} {UNCHANGED}"
    log(f"ERROR {what}: {_detail(exc)}")
    log(f"  shown: {msg}")
    _set(state="error", msg=msg, **kw)


# --------------------------------------------------------------------------
# etat affiche

def _set(**kw):
    with _lock:
        _st.update(kw)


def _ascii(text, limit=220):
    out = "".join(c for c in str(text or "") if 32 <= ord(c) < 127)
    return out[:limit].strip()


def human_size(n):
    n = float(n or 0)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit in ("B", "KB") else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} GB"


# --------------------------------------------------------------------------
# recherche

def _changed_components(man):
    inst = installed().get("components") or {}
    out = []
    for name, comp in man["components"].items():
        cur = inst.get(name) or {}
        if cur.get("id") != comp["id"]:
            out.append(name)
    return out


def check(force=False):
    """Interroge le canal. Ne telecharge que le pointeur et le manifeste."""
    global _manifest, _pointer
    with _lock:
        if _st["state"] in ("downloading", "ready", "checking"):
            return
        if not force and _st["checked"] and time.time() - _st["checked"] < CHECK_EVERY:
            return
        _st.update(state="checking", msg="checking for updates...", progress=0)
    url = channel_url()
    local = installed_version()
    log(f"check: installed {local}, channel {url or '(none)'}")
    if not url:
        # Copie de travail (atelier) : pas de canal, rien a chercher.
        _set(state="uptodate", remote="", size=0, checked=time.time(),
             msg="workshop copy: no update channel (versions are published from here)")
        return
    try:
        pointer = json.loads(fetch_small(url, MAX_POINTER).decode("utf-8-sig"))
        if not isinstance(pointer, dict) or pointer.get("product") != PRODUCT or pointer.get("platform") != PLATFORM:
            raise RuntimeError("the channel is not a L4ZY channel")
        remote = str(pointer.get("version") or "")
        if parse_version(remote) is None:
            raise RuntimeError("the channel has a bad version")
        if not is_newer(remote, local):
            _manifest = None
            _set(state="uptodate", remote=remote, size=0, checked=time.time(),
                 msg=f"L4ZY {local} is up to date (checked {time.strftime('%H:%M')})")
            log(f"check: up to date (channel has {remote})")
            return
        murl = urljoin(url, str(pointer.get("manifest") or ""))
        msha = str(pointer.get("manifest_sha256") or "").lower()
        if not SHA_RE.match(msha):
            raise RuntimeError("the channel has no manifest hash")
        raw = fetch_small(murl, MAX_MANIFEST)
        if hashlib.sha256(raw).hexdigest() != msha:
            raise RuntimeError("manifest hash mismatch")
        man = validate_manifest(json.loads(raw.decode("utf-8-sig")), expect_version=remote)
        man["_url"] = murl
        man["_raw_sha"] = msha
        _manifest, _pointer = man, pointer
        changed = _changed_components(man)
        size = sum(int(man["components"][n]["archive_size"]) for n in changed)
        notes = _ascii(man.get("notes") or "", 120)
        msg = f"L4ZY {remote} is available ({human_size(size)} to download)"
        if notes:
            msg += f" - {notes}"
        _set(state="available", remote=remote, size=size, checked=time.time(), msg=msg)
        log(f"check: {remote} available, {len(changed)} components changed ({', '.join(changed)}), {human_size(size)}")
        # Intention deja donnee (telechargement interrompu par une fermeture) :
        # on reprend tout seul.
        if (stage_dir() / "APPLY").is_file() and not (stage_dir() / "READY").is_file():
            log("check: a download was interrupted earlier, resuming it")
            start_download(restart=_read_apply().get("restart", False))
    except Exception as exc:  # noqa: BLE001
        _fail("check failed", exc, checked=time.time())


# --------------------------------------------------------------------------
# telechargement et preparation

def _download_archive(url, dest: Path, size, sha, progress):
    """Reprise (.part + Range), puis controle taille + SHA-256."""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.is_file():
        if dest.stat().st_size == size and _sha256(dest) == sha:
            progress(size)
            return
        dest.unlink()
    part = dest.with_name(dest.name + ".part")
    have = part.stat().st_size if part.is_file() else 0
    if have > size:
        part.unlink()
        have = 0
    headers = {"Range": f"bytes={have}-"} if have else {}
    try:
        resp = _open(url, headers=headers, timeout=60)
    except urllib.error.HTTPError as exc:
        if have and exc.code == 416:
            part.unlink(missing_ok=True)
            have = 0
            resp = _open(url, timeout=60)
        else:
            raise
    with resp:
        status = getattr(resp, "status", 200)
        if have:
            log(f"download: resuming {dest.name} at {have} bytes (server status {status})")
        if have and status != 206:
            have = 0  # le serveur ignore Range : on repart de zero
        mode = "ab" if have else "wb"
        written = have
        progress(written)
        with open(part, mode) as fh:
            while True:
                try:
                    chunk = resp.read(1024 * 1024)
                except (OSError, http.client.HTTPException) as exc:
                    # Connection lost mid-file: what arrived stays in .part
                    # and the next attempt resumes from there.
                    fh.flush()
                    raise RuntimeError(f"download incomplete (network interrupted?): {type(exc).__name__} "
                                       f"after {written} of {size} bytes") from exc
                if not chunk:
                    break
                written += len(chunk)
                if written > size:
                    raise RuntimeError("download larger than announced")
                fh.write(chunk)
                progress(written)
    if part.stat().st_size != size:
        raise RuntimeError(f"download incomplete (network interrupted?): {part.stat().st_size} of {size} bytes")
    if _sha256(part) != sha:
        part.unlink(missing_ok=True)
        raise RuntimeError(f"downloaded file is corrupted (hash mismatch), deleted: {dest.name}")
    os.replace(part, dest)


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _extract_component(zpath: Path, comp, files_dir: Path):
    expected = {rel: (int(m[0]), m[1]) for rel, m in comp["files"].items()}
    got = set()
    with zipfile.ZipFile(zpath) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            rel = info.filename
            if rel not in expected:
                raise RuntimeError(f"archive contains an unexpected file: {_ascii(rel, 80)}")
            if rel in got:
                raise RuntimeError(f"archive repeats {rel}")
            size, sha = expected[rel]
            if info.file_size != size:
                raise RuntimeError(f"archive size mismatch for {rel}")
            target = files_dir / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            h = hashlib.sha256()
            n = 0
            with zf.open(info) as src, open(target, "wb") as out:
                while True:
                    chunk = src.read(1024 * 1024)
                    if not chunk:
                        break
                    n += len(chunk)
                    if n > size:
                        raise RuntimeError(f"archive entry too large: {rel}")
                    h.update(chunk)
                    out.write(chunk)
            if n != size or h.hexdigest() != sha:
                raise RuntimeError(f"file {rel} failed verification")
            got.add(rel)
    missing = set(expected) - got
    if missing:
        raise RuntimeError(f"archive is incomplete ({len(missing)} files missing)")


def _read_apply():
    out = {}
    try:
        for line in (stage_dir() / "APPLY").read_text(encoding="utf-8").splitlines():
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    except OSError:
        pass
    out["restart"] = out.get("restart") == "1"
    return out


def _write_apply(restart):
    stage_dir().mkdir(parents=True, exist_ok=True)
    write_atomic(stage_dir() / "APPLY", f"restart={'1' if restart else '0'}\nrequested={int(time.time())}\n")


def _game_env_port(man):
    return "1" if (man.get("features") or {}).get("service_port_env") else "0"


def _prepare(man, restart):
    root = paths.ROOT
    stage = stage_dir()
    version = man["version"]
    changed = _changed_components(man)
    comps = man["components"]
    inst = installed()
    inst_comps = inst.get("components") or {}

    # Nouveau depart propre si un ancien staging traine pour une autre version.
    # Also when it was prepared from another installed version.
    old_ready = (stage / "READY")
    if old_ready.is_file() and (old_ready.read_text(encoding="utf-8").strip() != version
                                or _plan_from(stage) != installed_version()):
        log("prepare: discarding an old staged update")
        shutil.rmtree(stage, ignore_errors=True)
    apply_intent = (stage / "APPLY").is_file()
    shutil.rmtree(stage / "files", ignore_errors=True)
    (stage / "READY").unlink(missing_ok=True)
    stage.mkdir(parents=True, exist_ok=True)
    if apply_intent or restart is not None:
        _write_apply(bool(restart))

    total = sum(int(comps[n]["archive_size"]) for n in changed)
    unpacked = sum(int(m[0]) for n in changed for m in comps[n]["files"].values())
    free = shutil.disk_usage(state_dir()).free
    if os.environ.get("L4ZY_TEST_FREE_BYTES", "").isdigit() and os.environ.get("L4ZY_TEST_ALLOW_LOCAL_HTTP") == "1":
        free = int(os.environ["L4ZY_TEST_FREE_BYTES"])  # local tests only, like local http
    log(f"prepare: {version} from {installed_version()}, download {human_size(total)}, free {human_size(free)}")
    if free < total + unpacked + DISK_MARGIN:
        raise RuntimeError(f"not enough disk space: need {human_size(total + unpacked + DISK_MARGIN)}, "
                           f"{human_size(free)} free")

    # Cibles sures avant d'ecrire quoi que ce soit.
    for n in changed:
        for rel in comps[n]["files"]:
            safe_target(root, rel)

    done = [0]
    base_done = 0

    for n in changed:
        comp = comps[n]
        size = int(comp["archive_size"])
        zpath = dl_dir() / f"{comp['archive_sha256']}.zip"

        def prog(w, base=base_done):
            pct = int(90.0 * (base + w) / total) if total else 90
            _set(progress=max(1, min(90, pct)),
                 msg=f"downloading {human_size(base + w)} / {human_size(total)}")

        _download_archive(urljoin(man["_url"], comp["url"]), zpath, size, comp["archive_sha256"], prog)
        base_done += size

    _set(progress=92, msg="verifying files...")
    files_dir = stage / "files"
    for n in changed:
        comp = comps[n]
        _extract_component(dl_dir() / f"{comp['archive_sha256']}.zip", comp, files_dir)

    # Plan : fichiers des composants changes, suppression de ceux qui
    # disparaissent. Les composants inchanges ne sont pas touches.
    inst_files = {}
    for cur in inst_comps.values():
        inst_files.update(cur.get("files") or {})
    new_files = {}
    for n in changed:
        for rel, meta in comps[n]["files"].items():
            old = inst_files.get(rel)
            tgt = root / rel
            # Fichier identique deja en place : on ne le reecrit pas.
            if old and list(old) == list(meta) and tgt.is_file() and tgt.stat().st_size == int(meta[0]):
                continue
            new_files[rel] = meta
    all_new = {rel.lower() for c in comps.values() for rel in c["files"]}
    dels = []
    for n, cur in inst_comps.items():
        if n in comps and n not in changed:
            continue
        for rel in (cur.get("files") or {}):
            if rel.lower() not in all_new:
                try:
                    safe_target(root, rel)
                except ValueError:
                    continue
                dels.append(rel)

    lines = [f"l4zy-plan {PLAN_FORMAT}", f"version {version}", f"from {installed_version()}"]
    # Le lanceur est remplace en dernier : si l'application s'arrete avant,
    # l'ancien lanceur (qui sait reprendre) est encore en place.
    order = sorted(new_files, key=lambda r: (r == "L4ZY.exe", r))
    for rel in order:
        size, sha = new_files[rel]
        lines.append(f"PUT\t{rel}\t{int(size)}\t{sha}")
    for rel in sorted(dels):
        lines.append(f"DEL\t{rel}")
    lines.append("END")

    new_inst = {
        "format": FORMAT,
        "product": PRODUCT,
        "platform": PLATFORM,
        "version": version,
        "channel": man.get("channel") or channel(),
        "label": man.get("label") or "",
        "manifest_sha256": man.get("_raw_sha"),
        "installed_from": "update",
        "components": {n: {"id": c["id"], "files": c["files"]} for n, c in comps.items()},
    }
    write_atomic(stage / "installed.json.new", json.dumps(new_inst, indent=1, sort_keys=True))
    write_atomic(stage / "installed.ini.new",
                 f"version={version}\nchannel={new_inst['channel']}\nservice_port_env={_game_env_port(man)}\n")
    write_atomic(stage / "manifest.json", json.dumps({k: v for k, v in man.items() if not k.startswith("_")}, sort_keys=True))
    write_atomic(stage / "plan.txt", "\n".join(lines) + "\n")
    write_atomic(stage / "READY", version + "\n")


def _download_worker(man, restart):
    global _worker
    try:
        _prepare(man, restart)
        _set(state="ready", progress=100, restart=_read_apply().get("restart", False),
             msg=f"L4ZY {man['version']} is ready: restart the game to install it")
        log(f"prepare: {man['version']} ready to install")
    except Exception as exc:  # noqa: BLE001
        _fail("download failed", exc, progress=0)
    finally:
        with _lock:
            _worker = None


def start_download(restart=True):
    global _worker
    with _lock:
        if _st["state"] == "ready":
            if restart:
                _write_apply(True)
                _st["restart"] = True
            return
        if _worker is not None:
            return
        man = _manifest
        if man is None or _st["state"] not in ("available", "error"):
            raise RuntimeError("no update waiting")
        if not is_newer(man["version"], installed_version()):
            raise RuntimeError("no update waiting")
        _st.update(state="downloading", progress=1, msg="starting download...")
        log(f"download: starting {man['version']} (restart={'1' if restart else '0'})")
        _worker = threading.Thread(target=_download_worker, args=(man, restart), daemon=True)
        _worker.start()


def request_restart():
    """Le joueur a clique "redemarrer" : le lanceur appliquera a la fermeture."""
    with _lock:
        if _st["state"] != "ready":
            raise RuntimeError("update is not ready yet")
        _write_apply(True)
        _st["restart"] = True
        _st["msg"] = "closing the game to install the update..."
        log("restart requested: the launcher installs the update when the game closes")


# --------------------------------------------------------------------------
# demarrage : reprendre ce qui existe sur le disque

def _last_result():
    path = upd_dir() / "last-result.txt"
    if not path.is_file():
        return None
    data = {}
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            k, _, v = line.partition("=")
            data[k.strip()] = v.strip()
    except OSError:
        return None
    try:
        path.replace(path.with_name("last-result.seen.txt"))
    except OSError:
        pass
    return data


def _plan_from(stage):
    """Version a staged plan was prepared from ("" if unknown)."""
    try:
        for line in (Path(stage) / "plan.txt").read_text(encoding="utf-8").splitlines():
            if line.startswith("from "):
                return line[5:].strip()
    except OSError:
        pass
    return ""


def startup():
    local = installed_version()
    res = _last_result()
    stage = stage_dir()
    ready = (stage / "READY")
    log(f"service start: installed {local}")
    if ready.is_file():
        ver = ready.read_text(encoding="utf-8").strip()
        src = _plan_from(stage)
        if is_newer(ver, local) and src == local:
            _set(state="ready", remote=ver, progress=100, restart=_read_apply().get("restart", False),
                 msg=f"L4ZY {ver} is ready: restart the game to install it")
            log(f"startup: {ver} is staged and ready")
        else:
            # Prepared for another installed version (go back to the previous
            # version, reinstall): never applied, downloaded again if needed.
            log(f"startup: staged update {ver} (prepared from {src or '?'}) does not fit installed {local}, discarded")
            shutil.rmtree(stage, ignore_errors=True)
    elif (stage / "APPLY").is_file():
        log("startup: an earlier download was interrupted; it resumes after the next check")
    if res:
        if res.get("ok") == "1":
            _set(msg=f"updated to L4ZY {res.get('version', local)}")
            log(f"startup: last update installed ({res.get('version', local)})")
            shutil.rmtree(dl_dir(), ignore_errors=True)
        elif "rolled back" in res.get("message", ""):
            log(f"startup: went back to the previous version ({local})")
            _set(msg=f"Back to the previous version (L4ZY {local}).")
        else:
            log(f"startup: last update result: {res.get('message', '?')}")
            _set(state="error" if _st["state"] != "ready" else "ready",
                 msg=_ascii(f"The last update could not be installed ({res.get('message', '?')}). "
                            f"Your previous version was kept.", 220))
    threading.Thread(target=check, daemon=True).start()


# --------------------------------------------------------------------------
# reponses au jeu

def snapshot():
    with _lock:
        st = dict(_st)
    state = st["state"]
    avail = {"available": 1, "downloading": 2, "ready": 3}.get(state, 0)
    if state == "error" and _manifest is not None and _worker is None:
        avail = 1  # on peut reessayer
    lines = [
        "ok=1",
        "proto=2",
        f"state={state}",
        f"avail={avail}",
        f"installed={installed_version()}",
        f"local={installed_version()}",
        f"channel={_ascii(channel(), 16)}",
        f"remote={st['remote']}",
        f"ver={st['remote']}",
        f"size={human_size(st['size']) if st['size'] else ''}",
        f"progress={st['progress']}",
        f"restart={'1' if st['restart'] else '0'}",
        f"msg={_ascii(st['msg'])}",
    ]
    return "\n".join(lines) + "\n"


def handle_get(params):
    check(force=False)
    return snapshot()


def handle_post(params):
    action = (params.get("action", [""])[0] or "").strip().lower()
    if action == "check":
        with _lock:
            busy = _st["state"] in ("downloading", "ready")
        if not busy:
            check(force=True)
        return snapshot()
    if action == "restart":
        request_restart()
        return snapshot()
    if action in ("download", ""):
        # Ancien client (sans action) : son menu dit "Quit to Finish Update",
        # donc le clic vaut telechargement + installation au prochain quit.
        check(force=False)
        start_download(restart=True)
        return snapshot()
    raise ValueError("unknown update action")
