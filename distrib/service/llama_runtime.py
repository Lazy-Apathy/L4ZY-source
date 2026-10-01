"""Demarre / arrete le llama-server de CETTE installation. Bibliotheque standard.

Regles L4ZY autonome :
  - llama-server vient uniquement de ROOT/runtime/llama (livre avec le jeu),
    ou d'un chemin explicitement ecrit dans runtime.cfg. Jamais du PATH.
  - le port est choisi libre au demarrage : deux installations (ou l'ancien
    client) peuvent tourner sans se voler le modele.
  - on n'arrete que le processus qu'on a lance nous-memes. Aucun taskkill
    par nom : un llama-server d'un autre logiciel n'est jamais touche.
  - GPU (Vulkan) d'abord ; s'il refuse de demarrer, repli CPU explicite.
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path

import sauerrt_paths as paths

TRAD_DIR = paths.CODE
GAME_DIR = paths.ROOT
RUNTIME_DIR = paths.ROOT / "runtime" / "llama"

_proc = None
_lock = threading.Lock()
_port = 0
_mode = ""        # "", "gpu", "cpu"
_devices = None   # cache de --list-devices
_last_error = ""

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
BELOW_NORMAL = 0x00004000


def cfg_path():
    paths.ensure()
    return paths.DATA / "runtime.cfg"


def load_config(path=None):
    cfg = {
        "gguf": "",
        "llama_server": "",
        "gpu_layers": "99",
        "ctx": "4096",
        # 1 = un slot. llama-server passe tout seul a 4 sinon (cache du
        # prefixe perdu, pic GPU, ecran qui scintille, trad lente).
        "parallel": "1",
        "llm_url": "",
        "extra": "",
        # auto = GPU Vulkan puis repli CPU ; gpu ; cpu
        "device": "auto",
        # off = ne jamais charger de modele local (jeu + mises a jour seuls)
        "translation": "on",
    }
    cfg_file = Path(path) if path else cfg_path()
    if cfg_file.is_file():
        for raw in cfg_file.read_text(encoding="utf-8-sig").splitlines():
            line = raw.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            key = key.strip().lower()
            val = val.strip().strip('"').strip("'")
            if key in cfg:
                cfg[key] = val
    return cfg, cfg_file.parent


def _resolve(value, cfg_dir):
    if not value:
        return None
    p = Path(value)
    if p.is_absolute():
        return p
    for base in (cfg_dir, paths.MODELS.parent if paths.MODELS else None, paths.ROOT):
        if base is None:
            continue
        cand = (Path(base) / value)
        if cand.is_file():
            return cand.resolve()
    return (Path(cfg_dir) / value).resolve()


def resolve_path(value, cfg_dir):
    return _resolve(value, cfg_dir)


def find_server(cfg, cfg_dir):
    raw = (cfg.get("llama_server") or "").strip()
    if raw:
        resolved = _resolve(raw, cfg_dir)
        if resolved and resolved.is_file():
            return resolved
        return None
    local = RUNTIME_DIR / "llama-server.exe"
    if local.is_file():
        return local
    return None


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]
    finally:
        s.close()


def port():
    with _lock:
        return _port


def mode():
    with _lock:
        return _mode


def last_error():
    with _lock:
        return _last_error


def llm_url(cfg):
    if cfg.get("llm_url"):
        return cfg["llm_url"].rstrip("/")
    return f"http://127.0.0.1:{port() or 1}/v1/chat/completions"


def health_url(cfg=None):
    return f"http://127.0.0.1:{port() or 1}/health"


def _running():
    with _lock:
        proc = _proc
    return proc is not None and proc.poll() is None


def is_up(cfg=None, timeout=0.4):
    """Vrai seulement si NOTRE llama-server tourne et repond."""
    if not _running():
        return False
    try:
        urllib.request.urlopen(health_url(), timeout=timeout).read()
        return True
    except (urllib.error.URLError, TimeoutError, OSError):
        return False


def wait_health(cfg=None, seconds=180):
    deadline = time.time() + seconds
    last_err = None
    while time.time() < deadline:
        with _lock:
            proc = _proc
        if proc is None or proc.poll() is not None:
            code = proc.returncode if proc is not None else "?"
            raise RuntimeError(f"llama-server stopped (code {code}), see llama.log")
        try:
            urllib.request.urlopen(health_url(), timeout=1.5).read()
            return True
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            last_err = exc
        time.sleep(0.4)
    raise RuntimeError(f"llama-server does not answer: {last_err}")


def _child_env(server):
    env = {}
    for key, val in os.environ.items():
        up = key.upper()
        if up.startswith("PYTHON") or up in ("VIRTUAL_ENV", "CONDA_PREFIX"):
            continue
        env[key] = val
    # DLL du runtime a cote de l'exe ; rien d'autre ajoute au PATH.
    env["PATH"] = str(server.parent) + os.pathsep + env.get("PATH", "")
    return env


def list_devices(server=None):
    """Peripheriques vus par llama.cpp (Vulkan). Cache pour la session."""
    global _devices
    with _lock:
        if _devices is not None:
            return list(_devices)
    server = server or (RUNTIME_DIR / "llama-server.exe")
    found = []
    if Path(server).is_file():
        try:
            out = subprocess.run(
                [str(server), "--list-devices"],
                cwd=str(Path(server).parent),
                env=_child_env(Path(server)),
                capture_output=True, timeout=25,
                creationflags=NO_WINDOW,
            )
            text = (out.stdout or b"").decode("utf-8", "replace") + (out.stderr or b"").decode("utf-8", "replace")
            for line in text.splitlines():
                line = line.strip()
                if line.lower().startswith("vulkan") and ":" in line:
                    found.append(line)
        except (OSError, subprocess.SubprocessError):
            pass
    with _lock:
        _devices = found
    return list(found)


def _launch(server, gguf, cfg, gpu, logpath):
    global _proc, _port, _mode
    ctx = str(cfg.get("ctx") or "4096")
    n_parallel = str(cfg.get("parallel") or "1")
    extra = (cfg.get("extra") or "").strip().split()
    ngl = str(cfg.get("gpu_layers") or "99") if gpu else "0"
    myport = free_port()
    base = [
        str(server),
        "-m", str(gguf),
        "--host", "127.0.0.1",
        "--port", str(myport),
        "-c", ctx,
        "-ngl", ngl,
        "-np", n_parallel,
        "--jinja",
        "--alias", "qwen3",
    ]
    if not gpu:
        base += ["--device", "none"]
    # -kvu : sans ca, llama-server ignore -np 1 et ouvre 4 slots.
    # --reasoning off : Qwen3 ne "reflechit" pas 5 s sur une ligne de chat.
    flag_sets = [
        ["-kvu", "--reasoning", "off", "--no-webui"],
        ["-kvu", "--chat-template-kwargs", '{"enable_thinking": false}', "--reasoning-budget", "0", "--no-webui"],
        ["--reasoning", "off", "--no-webui"],
        ["--chat-template-kwargs", '{"enable_thinking": false}', "--no-webui"],
        [],
    ]
    last_err = None
    for flags in flag_sets:
        cmd = base + flags + extra
        logf = open(logpath, "w", encoding="utf-8", errors="replace")
        logf.write("cmd: " + " ".join(cmd) + "\n")
        logf.flush()
        proc = subprocess.Popen(
            cmd,
            stdout=logf,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            cwd=str(server.parent),
            env=_child_env(server),
            creationflags=NO_WINDOW | BELOW_NORMAL,
        )
        with _lock:
            _proc = proc
            _port = myport
            _mode = "gpu" if gpu else "cpu"
        time.sleep(0.6)
        if proc.poll() is not None:
            last_err = f"exit code {proc.returncode}"
            logf.close()
            continue
        try:
            wait_health(cfg)
            logf.close()
            return True
        except Exception as exc:  # noqa: BLE001
            last_err = exc
            _stop_proc(proc)
            logf.close()
    with _lock:
        _proc = None
        _mode = ""
    raise RuntimeError(str(last_err))


def start_if_needed(cfg, cfg_dir, logpath=None):
    """Demarre notre llama-server si un GGUF existe. Retourne True si lance."""
    global _last_error
    if (cfg.get("translation") or "on").strip().lower() in ("off", "0", "no"):
        print("  traduction locale desactivee (runtime.cfg translation = off)")
        return False
    if is_up():
        return False
    gguf = _resolve(cfg.get("gguf") or "", cfg_dir)
    if not gguf or not gguf.is_file():
        print("  aucun modele local installe : le jeu et les mises a jour restent disponibles.")
        return False
    server = find_server(cfg, cfg_dir)
    if not server:
        with _lock:
            _last_error = "translation engine missing, reinstall L4ZY"
        raise RuntimeError("translation engine files missing (runtime/llama). Reinstall L4ZY.")
    logpath = logpath or str((paths.LOGS or paths.DATA) / "llama.log")
    want = (cfg.get("device") or "auto").strip().lower()
    attempts = []
    if want == "auto" and not list_devices(server):
        # Aucune carte Vulkan utilisable : on ne pretend pas "GPU".
        print("  aucune carte graphique Vulkan vue par llama.cpp : mode processeur.")
    elif want in ("auto", "gpu"):
        attempts.append(True)
    if want in ("auto", "cpu"):
        attempts.append(False)
    errors = []
    for gpu in attempts:
        label = "GPU (Vulkan)" if gpu else "CPU"
        print(f"  demarrage llama-server : {gguf.name}  {label}  ctx={cfg.get('ctx')}")
        try:
            _launch(server, gguf, cfg, gpu, logpath if gpu else logpath.replace(".log", "-cpu.log"))
            print(f"  llama-server pret sur 127.0.0.1:{port()} ({label}).")
            with _lock:
                _last_error = ""
            return True
        except Exception as exc:  # noqa: BLE001
            errors.append(f"{label}: {exc}")
            print(f"  echec {label} : {exc}")
    with _lock:
        _last_error = "; ".join(errors)
    raise RuntimeError("llama-server did not start (" + "; ".join(errors) + ")")


def _stop_proc(proc):
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            pass


def stop():
    global _proc, _mode
    with _lock:
        proc = _proc
        _proc = None
        _mode = ""
    _stop_proc(proc)


def kill_server():
    """Changement de modele : arrete NOTRE serveur, jamais celui d'un autre."""
    stop()


def pid_alive(pid):
    if not pid:
        return False
    if sys.platform == "win32":
        import ctypes
        SYNCHRONIZE = 0x00100000
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
        if not h:
            return False
        try:
            return k32.WaitForSingleObject(h, 0) == 0x102  # WAIT_TIMEOUT
        finally:
            k32.CloseHandle(h)
    try:
        os.kill(int(pid), 0)
        return True
    except OSError:
        return False


def watch_pid(pid, on_exit, poll=1.0):
    """Appelle on_exit quand le lanceur de cette installation disparait."""

    def loop():
        while pid_alive(pid):
            time.sleep(poll)
        print("  lanceur ferme, arret du service.")
        try:
            on_exit()
        except Exception as exc:  # noqa: BLE001
            print(f"  arret : {exc}", file=sys.stderr)

    threading.Thread(target=loop, daemon=True).start()
