"""Catalogue Qwen officiel, telechargement, custom GGUF, cle API.

Bibliotheque standard uniquement. Le client parle via GET/POST /models*.
La cle API n'est jamais ecrite dans les logs ni dans runtime.cfg.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import threading
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from urllib.parse import urlparse

import llama_runtime
import sauerrt_paths as paths

# Chemins personnels : fixes par init() a partir de sauerrt_paths (le lanceur
# les donne). Les modeles ne sont JAMAIS dans les fichiers distribues : une
# mise a jour du jeu ne les retelecharge ni ne les efface.
TRAD = None
MODELS = None
CUSTOM_DIR = None
SECRETS = None
DOWNLOAD_STATE = None
STARTER_FLAG = None
USER_AGENT = "l4zy-models"


def _bind_paths():
    global TRAD, MODELS, CUSTOM_DIR, SECRETS, DOWNLOAD_STATE, STARTER_FLAG
    paths.ensure()
    TRAD = paths.DATA
    MODELS = paths.MODELS
    CUSTOM_DIR = MODELS / "custom"
    SECRETS = TRAD / "api.secrets"
    DOWNLOAD_STATE = MODELS / ".download.json"
    STARTER_FLAG = TRAD / "starter-model.txt"

# Un fichier par cran, Q4_K_M documente par l'organisation Hugging Face Qwen.
# Pas de 14B : cache dans le menu. Pas d'URL collable pour le catalogue.
CATALOG = {
    "small": {
        "label": "Small",
        "file": "Qwen3-4B-Q4_K_M.gguf",
        "url": "https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf",
        "sha256": "7485fe6f11af29433bc51cab58009521f205840f5b4ae3a32fa7f92e8534fdf5",
        "bytes": 2497280256,
        "about": "Qwen3-4B ~2.5 GB, ~4 GB VRAM",
    },
    "normal": {
        "label": "Normal",
        "file": "Qwen3-8B-Q4_K_M.gguf",
        "url": "https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf",
        "sha256": "d98cdcbd03e17ce47681435b5150e34c1417f50b5c0019dd560e4882c5745785",
        "bytes": 5027783488,
        "about": "Qwen3-8B ~5 GB, ~6-7 GB VRAM",
    },
    "large": {
        "label": "Large",
        "file": "Qwen3-14B-Q4_K_M.gguf",
        "url": "https://huggingface.co/Qwen/Qwen3-14B-GGUF/resolve/main/Qwen3-14B-Q4_K_M.gguf",
        "sha256": "500a8806e85ee9c83f3ae08420295592451379b4f8cf2d0f41c15dffeb6b81f0",
        "bytes": 9001752960,
        "about": "Qwen3-14B ~9 GB, ~10-11 GB VRAM",
    },
}

_lock = threading.RLock()
_cfg = {}
_cfg_dir = TRAD
_api_key = ""
_api_url = ""
_api_model = ""
_busy = ""  # "", downloading, loading
_progress = 0
_msg = ""
_dl_id = ""
_error = ""
_cancel = False
_started = False
_hash_ok = {}


def _set_msg(text):
    global _msg
    _msg = (text or "")[:240]


def _gguf_path(entry):
    return MODELS / entry["file"]


def _rel_model(path: Path):
    """runtime.cfg garde un chemin relatif au dossier personnel (portable)."""
    try:
        return path.resolve().relative_to(MODELS.parent.resolve()).as_posix()
    except ValueError:
        return str(path)


def _sha256_file(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _official_ok(entry):
    path = _gguf_path(entry)
    if not path.is_file():
        return False
    try:
        st = path.stat()
    except OSError:
        return False
    if st.st_size != entry["bytes"]:
        return False
    key = str(path.resolve())
    prev = _hash_ok.get(key)
    if prev == (st.st_mtime_ns, st.st_size):
        return True
    if _sha256_file(path) != entry["sha256"]:
        return False
    _hash_ok[key] = (st.st_mtime_ns, st.st_size)
    return True


def _safe_custom_name(name):
    if not name:
        return None
    base = Path(name).name
    if base != name or base in (".", "..") or "/" in name or "\\" in name:
        return None
    if not base.lower().endswith(".gguf"):
        return None
    if base.startswith("."):
        return None
    return base


def _custom_path(name):
    safe = _safe_custom_name(name)
    if not safe:
        return None
    return CUSTOM_DIR / safe


def _disk_free(path: Path):
    try:
        path.mkdir(parents=True, exist_ok=True)
        return shutil.disk_usage(str(path)).free
    except OSError:
        return 0


def _active_gguf():
    raw = (_cfg.get("gguf") or "").strip()
    resolved = llama_runtime.resolve_path(raw, _cfg_dir) if raw else None
    if resolved and resolved.is_file():
        return resolved.resolve()
    return None


def _active_id():
    active = _active_gguf()
    if not active:
        return ""
    for mid, entry in CATALOG.items():
        if _gguf_path(entry).resolve() == active:
            return mid
    try:
        active.relative_to(CUSTOM_DIR.resolve())
        return "custom:" + active.name
    except ValueError:
        return "custom:" + active.name


def _load_secrets():
    global _api_key, _api_url, _api_model
    _api_key = _api_url = _api_model = ""
    if not SECRETS.is_file():
        return
    try:
        raw = SECRETS.read_text(encoding="utf-8")
    except OSError:
        return
    for line in raw.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip().lower()
        val = val.strip()
        if key == "api_key":
            _api_key = val
        elif key in ("llm_url", "api_url"):
            _api_url = val
        elif key in ("api_model", "model"):
            _api_model = val


def _save_secrets():
    lines = [
        "# Ne pas partager. Pas dans git. La cle n'apparait pas dans les logs.",
        f"api_url = {_api_url}",
        f"api_model = {_api_model}",
        f"api_key = {_api_key}",
        "",
    ]
    try:
        SECRETS.write_text("\n".join(lines), encoding="utf-8")
        try:
            os.chmod(SECRETS, 0o600)
        except OSError:
            pass
    except OSError as exc:
        raise RuntimeError(f"cannot save API key ({exc})") from exc


def _normalize_api_url(url):
    url = (url or "").strip().rstrip("/")
    if not url:
        return ""
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https"):
        return ""
    path = parsed.path or ""
    if path.endswith("/chat/completions"):
        return url
    if path.endswith("/v1") or path.endswith("/v1/"):
        return url.rstrip("/") + "/chat/completions"
    if not path or path == "/":
        return url + "/v1/chat/completions"
    return url


def infer_model(url, explicit):
    if (explicit or "").strip():
        return explicit.strip()
    host = (urlparse(url).hostname or "").lower()
    if host in ("127.0.0.1", "localhost", "::1"):
        return "qwen3"
    return "gpt-4o-mini"


def using_api():
    with _lock:
        return bool(_api_key.strip())


def is_loading():
    with _lock:
        return _busy == "loading"


def endpoint():
    """URL, nom de modele, cle (cle vide = llama local)."""
    with _lock:
        if _api_key.strip():
            url = _normalize_api_url(_api_url) or "https://api.openai.com/v1/chat/completions"
            return url, infer_model(url, _api_model), _api_key.strip()
        local = (_cfg.get("llm_url") or "").strip()
        if local:
            return local.rstrip("/"), "qwen3", ""
    return llama_runtime.llm_url({}), "qwen3", ""


def _backend_label():
    m = llama_runtime.mode()
    if m == "gpu":
        devs = llama_runtime.list_devices()
        name = devs[0].split(":", 1)[1].split("(")[0].strip() if devs else "GPU"
        return f"GPU Vulkan {name}"[:60]
    if m == "cpu":
        return "CPU (slow, no GPU acceleration)"
    return ""


def _catalog_state(mid):
    entry = CATALOG[mid]
    path = _gguf_path(entry)
    with _lock:
        downloading = _busy == "downloading" and _dl_id == mid
    if downloading:
        return "downloading"
    if path.is_file():
        return "installed"
    return "missing"


def _file_mb(path: Path):
    try:
        return path.stat().st_size / 1e6
    except OSError:
        return 0


def status_text():
    with _lock:
        busy = _busy
        msg = _msg
        err = _error
        progress = _progress
        api = bool(_api_key.strip())
        dl = _dl_id
    if err and busy != "downloading":
        return f"error: {err}"
    if busy == "downloading":
        label = CATALOG.get(dl, {}).get("label", dl or "model")
        return msg or f"downloading {label} {progress}%"
    if busy == "loading":
        return msg or "loading model..."
    if api:
        return "using internet API - chat leaves this PC"
    if (_cfg.get("translation") or "on").strip().lower() in ("off", "0", "no"):
        return "local translation is off (runtime.cfg translation = off)"
    active = _active_id()
    backend = _backend_label()
    where = f" on {backend}" if backend else ""
    if active and not llama_runtime.is_up() and llama_runtime.last_error():
        return f"model not loaded: {llama_runtime.last_error()}"[:230]
    if active.startswith("custom:"):
        return f"using custom {active.split(':', 1)[1]}{where}"
    if active in CATALOG:
        return f"using local {CATALOG[active]['label']}{where} ({CATALOG[active]['about']})"
    if not _active_gguf():
        return "no local model. install Small/Normal, or paste an API key"
    return f"using local model{where}"


def snapshot():
    CUSTOM_DIR.mkdir(parents=True, exist_ok=True)
    MODELS.mkdir(parents=True, exist_ok=True)
    active = _active_id()
    with _lock:
        api = bool(_api_key.strip())
        busy = _busy
        lines = [
            f"ok=1",
            f"backend={'api' if api else ('loading' if busy == 'loading' else ('downloading' if busy == 'downloading' else 'local'))}",
            f"active={active}",
            f"api={'1' if api else '0'}",
            f"apiurl={_normalize_api_url(_api_url)}",
            f"apimodel={_api_model}",
            f"apikey={'1' if api else '0'}",
            f"progress={_progress}",
            f"dl={_dl_id}",
            f"msg={status_text()}",
            f"disk={int(_disk_free(MODELS) / 1e9)}",
        ]
    for mid, entry in CATALOG.items():
        st = _catalog_state(mid)
        if not api and busy != "loading" and active == mid and st == "installed":
            st = "active"
        lines.append(f"{mid}={st}")
        lines.append(f"{mid}about={entry['about']}")
    customs = []
    if CUSTOM_DIR.is_dir():
        for path in sorted(CUSTOM_DIR.glob("*.gguf")):
            if path.is_file():
                customs.append(path)
    lines.append(f"customcount={min(len(customs), 12)}")
    for i, path in enumerate(customs[:12]):
        st = "installed"
        if not api and busy != "loading" and active == "custom:" + path.name:
            st = "active"
        lines.append(f"custom{i}={path.name}")
        lines.append(f"custom{i}size={_file_mb(path):.1f}")
        lines.append(f"custom{i}state={st}")
    return "\n".join(lines) + "\n"


RUNTIME_TEMPLATE = """\
# Reglages de traduction de CE joueur. Jamais remplace par une mise a jour.
# Le modele se choisit dans le jeu : Chat Translation -> model.
gguf =
# auto = carte graphique (Vulkan) puis repli processeur ; gpu ; cpu
device = auto
# 99 = tout sur la carte. Petite carte : baisser (20, 12).
gpu_layers = 99
ctx = 4096
parallel = 1
# off = ne jamais charger de modele local (le jeu et les mises a jour marchent)
translation = on
# Serveur compatible OpenAI deja lance ailleurs (optionnel, choix explicite).
llm_url =
extra =
"""


def ensure_runtime_cfg():
    path = TRAD / "runtime.cfg"
    if not path.is_file():
        path.write_text(RUNTIME_TEMPLATE, encoding="utf-8")
    return path


def update_runtime(updates: dict):
    path = ensure_runtime_cfg()
    lines = []
    if path.is_file():
        lines = path.read_text(encoding="utf-8").splitlines()
    seen = set()
    out = []
    for line in lines:
        stripped = line.strip()
        if stripped and not stripped.startswith("#") and "=" in stripped:
            key = stripped.split("=", 1)[0].strip().lower()
            if key in updates:
                out.append(f"{key} = {updates[key]}")
                seen.add(key)
                continue
        out.append(line)
    for key, val in updates.items():
        if key not in seen:
            out.append(f"{key} = {val}")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    _reload_cfg()


def _reload_cfg():
    global _cfg, _cfg_dir
    _cfg, _cfg_dir = llama_runtime.load_config(TRAD / "runtime.cfg")


def init(cfg=None, cfg_dir=None):
    global _cfg, _cfg_dir, _started
    _bind_paths()
    ensure_runtime_cfg()
    if cfg is None:
        _reload_cfg()
    else:
        _cfg = cfg
        _cfg_dir = cfg_dir or TRAD
    MODELS.mkdir(parents=True, exist_ok=True)
    CUSTOM_DIR.mkdir(parents=True, exist_ok=True)
    _load_secrets()
    with _lock:
        _started = True
        if not _msg:
            _set_msg(status_text())
    return using_api()


def download_file(url, dest: Path, expected_sha256=None, expected_bytes=None, progress_cb=None):
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    headers = {"User-Agent": USER_AGENT}
    existing = part.stat().st_size if part.exists() else 0
    if dest.is_file() and expected_sha256:
        if (expected_bytes is None or dest.stat().st_size == expected_bytes) and _sha256_file(dest) == expected_sha256:
            if progress_cb:
                progress_cb(100, dest.stat().st_size, expected_bytes or dest.stat().st_size)
            return
        dest.unlink()
    elif dest.is_file() and not expected_sha256:
        return
    if existing:
        headers["Range"] = f"bytes={existing}-"
    req = urllib.request.Request(url, headers=headers)
    try:
        resp = urllib.request.urlopen(req, timeout=60)
    except Exception as exc:
        if existing and "Range" in headers:
            existing = 0
            part.unlink(missing_ok=True)
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            resp = urllib.request.urlopen(req, timeout=60)
        else:
            raise RuntimeError(str(exc)) from exc
    total = resp.headers.get("Content-Length")
    total = int(total) + existing if total else expected_bytes
    mode = "ab" if existing else "wb"
    written = existing
    last_cb = 0
    with part.open(mode) as fh, resp:
        while True:
            if _cancel:
                raise RuntimeError("download cancelled")
            chunk = resp.read(1024 * 1024)
            if not chunk:
                break
            fh.write(chunk)
            written += len(chunk)
            if progress_cb and written - last_cb >= 2 * 1024 * 1024:
                pct = int(100.0 * written / total) if total else 0
                progress_cb(min(99, pct), written, total)
                last_cb = written
    part.replace(dest)
    if expected_sha256:
        got = _sha256_file(dest)
        if got != expected_sha256:
            dest.unlink(missing_ok=True)
            raise RuntimeError("file hash mismatch, download deleted")
        try:
            st = dest.stat()
            _hash_ok[str(dest.resolve())] = (st.st_mtime_ns, st.st_size)
        except OSError:
            pass
    if progress_cb:
        size = dest.stat().st_size
        progress_cb(100, size, expected_bytes or size)


def ensure_llama_server():
    # Le moteur est livre avec le jeu (runtime/llama). Plus aucun
    # telechargement d'outil en cours de partie.
    if llama_runtime.find_server(_cfg, _cfg_dir):
        return
    raise RuntimeError("translation engine missing (runtime/llama). Reinstall L4ZY")


def maybe_start_starter(starter_id):
    """Premier lancement : installe le modele de depart choisi a l'installation.

    Une seule fois par joueur (fichier starter-model.txt). Ne remplace jamais
    un modele deja choisi, ni une cle API. Tourne en fond : le jeu n'attend pas.
    """
    starter_id = (starter_id or "").strip().lower()
    if starter_id not in CATALOG or STARTER_FLAG.is_file():
        return False
    if using_api() or _active_gguf():
        STARTER_FLAG.write_text("skipped: a model or API key is already set\n", encoding="utf-8")
        return False
    STARTER_FLAG.write_text(f"{starter_id}\n", encoding="utf-8")
    try:
        start_install(starter_id, activate=True)
    except RuntimeError as exc:
        _set_msg(f"error: {exc}")
        return False
    return True


def _save_download_state(mid, activate):
    entry = CATALOG[mid]
    payload = {
        "id": mid,
        "url": entry["url"],
        "file": entry["file"],
        "sha256": entry["sha256"],
        "bytes": entry["bytes"],
        "activate": bool(activate),
    }
    DOWNLOAD_STATE.write_text(json.dumps(payload), encoding="utf-8")


def _clear_download_state():
    DOWNLOAD_STATE.unlink(missing_ok=True)


def _download_worker(mid, activate):
    global _busy, _progress, _dl_id, _error, _cancel
    entry = CATALOG[mid]
    dest = _gguf_path(entry)
    try:
        need = entry["bytes"] + 80 * 1024 * 1024
        free = _disk_free(MODELS)
        already = dest.stat().st_size if dest.is_file() else 0
        part = dest.with_name(dest.name + ".part")
        already = part.stat().st_size if part.is_file() else already
        if free + already < need:
            raise RuntimeError("not enough disk space")

        def progress_cb(pct, written, total):
            global _progress
            with _lock:
                _progress = int(pct)
                if total:
                    _set_msg(
                        f"downloading {entry['label']} {written / 1e9:.1f} / {total / 1e9:.1f} GB"
                    )

        download_file(
            entry["url"],
            dest,
            expected_sha256=entry["sha256"],
            expected_bytes=entry["bytes"],
            progress_cb=progress_cb,
        )
        _clear_download_state()
        with _lock:
            _busy = ""
            _dl_id = ""
            _progress = 100
            _error = ""
            _set_msg(f"{entry['label']} installed")
        if activate:
            activate_model(mid, None)
    except Exception as exc:  # noqa: BLE001
        with _lock:
            _busy = ""
            _dl_id = ""
            _error = str(exc)
            _set_msg(f"error: {exc}")
            if _cancel:
                _error = ""
                _set_msg("download cancelled")
                _cancel = False


def start_install(mid, activate=True):
    global _busy, _progress, _dl_id, _error, _cancel
    if mid not in CATALOG:
        raise ValueError("unknown model")
    entry = CATALOG[mid]
    dest = _gguf_path(entry)
    try:
        complete = dest.is_file() and dest.stat().st_size == entry["bytes"]
    except OSError:
        complete = False
    if complete:
        if activate:
            return activate_model(mid, None)
        return snapshot()
    with _lock:
        if _busy == "downloading":
            raise RuntimeError("a download is already running")
        _cancel = False
        _busy = "downloading"
        _dl_id = mid
        _progress = 0
        _error = ""
        _set_msg(f"downloading {entry['label']}...")
    _save_download_state(mid, activate)
    threading.Thread(target=_download_worker, args=(mid, activate), daemon=True).start()
    return snapshot()


def resume_download_if_any():
    if not DOWNLOAD_STATE.is_file():
        return
    try:
        data = json.loads(DOWNLOAD_STATE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        _clear_download_state()
        return
    mid = data.get("id")
    if mid not in CATALOG:
        _clear_download_state()
        return
    try:
        start_install(mid, activate=bool(data.get("activate", True)))
    except RuntimeError:
        pass


def _kill_llama():
    llama_runtime.stop()
    if llama_runtime.is_up(_cfg, timeout=0.3):
        llama_runtime.kill_server()
        for _ in range(30):
            if not llama_runtime.is_up(_cfg, timeout=0.2):
                break
            time.sleep(0.2)


def _load_local():
    global _busy, _error
    with _lock:
        _busy = "loading"
        _set_msg("loading model...")
    try:
        active = _active_gguf()
        if active:
            for entry in CATALOG.values():
                if _gguf_path(entry).resolve() == active.resolve():
                    _set_msg(f"checking {entry['label']}...")
                    if not _official_ok(entry):
                        active.unlink(missing_ok=True)
                        raise RuntimeError(
                            f"{entry['label']} hash mismatch, file deleted. install again"
                        )
                    break
        ensure_llama_server()
        _reload_cfg()
        _kill_llama()
        if llama_runtime.is_up(_cfg, timeout=0.4):
            raise RuntimeError("could not stop the old model")
        llama_runtime.start_if_needed(
            _cfg, _cfg_dir, logpath=str(paths.LOGS / "llama.log")
        )
        if not llama_runtime.is_up(_cfg, timeout=1.0):
            raise RuntimeError("llama-server did not start. see the llama.log file")
        with _lock:
            _busy = ""
            _error = ""
            _set_msg(status_text())
    except Exception as exc:  # noqa: BLE001
        with _lock:
            _busy = ""
            _error = str(exc)
            _set_msg(f"error: {exc}")


def activate_model(mid, custom_name):
    if mid in CATALOG:
        entry = CATALOG[mid]
        path = _gguf_path(entry)
        if not path.is_file():
            raise RuntimeError(f"{entry['label']} is not installed")
        try:
            if path.stat().st_size != entry["bytes"]:
                raise RuntimeError(f"{entry['label']} is incomplete, install again")
        except OSError as exc:
            raise RuntimeError(f"{entry['label']} is unreadable") from exc
        rel = _rel_model(path)
        update_runtime({"gguf": rel, "llm_url": ""})
        new_id = mid
    elif mid == "custom":
        path = _custom_path(custom_name)
        if not path or not path.is_file():
            raise RuntimeError("custom file not found")
        rel = _rel_model(path)
        update_runtime({"gguf": rel, "llm_url": ""})
        new_id = "custom:" + path.name
    else:
        raise ValueError("unknown model")
    if using_api():
        _set_msg("local file ready. clear the API key to use it (key still wins)")
        return snapshot()
    if _active_id() == new_id and llama_runtime.is_up(_cfg, timeout=0.3):
        _set_msg(status_text())
        return snapshot()
    with _lock:
        _busy = "loading"
        _set_msg("loading model...")
    threading.Thread(target=_load_local, daemon=True).start()
    return snapshot()


def delete_model(mid, custom_name):
    global _cancel
    active = _active_id()
    if mid in CATALOG:
        if not using_api() and active == mid:
            raise RuntimeError("stop using it first (switch or use API)")
        with _lock:
            if _busy == "downloading" and _dl_id == mid:
                _cancel = True
        entry = CATALOG[mid]
        path = _gguf_path(entry)
        part = path.with_name(path.name + ".part")
        path.unlink(missing_ok=True)
        part.unlink(missing_ok=True)
        if DOWNLOAD_STATE.is_file():
            try:
                data = json.loads(DOWNLOAD_STATE.read_text(encoding="utf-8"))
                if data.get("id") == mid:
                    _clear_download_state()
            except (OSError, json.JSONDecodeError):
                pass
        _set_msg(f"{entry['label']} deleted")
        return snapshot()
    if mid == "custom":
        if not using_api() and active == "custom:" + (custom_name or ""):
            raise RuntimeError("stop using it first")
        path = _custom_path(custom_name)
        if not path or not path.is_file():
            raise RuntimeError("custom file not found")
        path.unlink()
        _set_msg(f"{path.name} deleted")
        return snapshot()
    raise ValueError("unknown model")


def save_api(url, key, model):
    global _api_url, _api_key, _api_model
    url = (url or "").strip()
    key = (key or "").strip()
    model = (model or "").strip()
    if key and not _normalize_api_url(url) and not url:
        url = "https://api.openai.com/v1"
    if url and not _normalize_api_url(url):
        raise ValueError("API address must start with http:// or https://")
    with _lock:
        if url:
            _api_url = url
        if model or model == "":
            _api_model = model
        if key:
            _api_key = key
        if not _api_key:
            raise ValueError("paste an API key, or use the local buttons")
        _save_secrets()
        _set_msg("using internet API — chat leaves this PC")
    threading.Thread(target=_kill_llama, daemon=True).start()
    return snapshot()


def clear_api():
    global _api_key
    with _lock:
        _api_key = ""
        _save_secrets()
    if _active_gguf():
        threading.Thread(target=_load_local, daemon=True).start()
        _set_msg("API key cleared, loading local model...")
    else:
        _set_msg("API key cleared. install a local model")
    return snapshot()
