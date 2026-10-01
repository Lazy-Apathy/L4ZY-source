"""Ou vivent les fichiers d'une installation L4ZY.

Deux familles, jamais melangees :
  - distribues (remplaces par les mises a jour) : ROOT/bin64, data, packages,
    runtime, service, docs, L4ZY.exe ;
  - personnels (jamais touches par une mise a jour) : reglages de traduction,
    corrections apprises, cle API, modeles installes, journaux.

Le lanceur passe tous les chemins en arguments. Sans lanceur (developpement),
on relit l4zy.ini avec les memes regles que lui.
"""

from __future__ import annotations

import configparser
import os
from pathlib import Path

# abspath, not resolve(): keep the install path even when service is a junction (workshop)
CODE = Path(os.path.abspath(__file__)).parent
ROOT = CODE.parent
STATE = ROOT / "state"
DATA = None      # reglages + corrections + secrets + journaux de traduction
MODELS = None    # modeles GGUF installes par le joueur
PROFILE = None   # profil du jeu (config.cfg...), seulement pour information
LOGS = None


def _documents():
    if os.name == "nt":
        try:
            import ctypes
            from ctypes import wintypes

            class GUID(ctypes.Structure):
                _fields_ = [
                    ("Data1", wintypes.DWORD),
                    ("Data2", wintypes.WORD),
                    ("Data3", wintypes.WORD),
                    ("Data4", ctypes.c_ubyte * 8),
                ]

            # FOLDERID_Documents {FDD39AD0-238F-46AF-ADB4-6C85480369C7}
            fid = GUID(0xFDD39AD0, 0x238F, 0x46AF,
                       (ctypes.c_ubyte * 8)(0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7))
            out = ctypes.c_wchar_p()
            shell32 = ctypes.windll.shell32
            if shell32.SHGetKnownFolderPath(ctypes.byref(fid), 0, None, ctypes.byref(out)) == 0:
                path = out.value
                ctypes.windll.ole32.CoTaskMemFree(out)
                if path:
                    return Path(path)
        except Exception:  # noqa: BLE001
            pass
    return Path.home() / "Documents"


def _local_appdata():
    val = os.environ.get("LOCALAPPDATA")
    if val:
        return Path(val)
    return Path.home() / "AppData" / "Local"


def read_ini(root=None):
    root = Path(root) if root else ROOT
    ini = configparser.ConfigParser(interpolation=None)
    path = root / "l4zy.ini"
    if path.is_file():
        try:
            ini.read(path, encoding="utf-8-sig")
        except (OSError, configparser.Error):
            pass
    return ini


def default_dirs(root=None, ini=None):
    """Meme logique que le lanceur (launcher.cpp : resolve_paths)."""
    root = Path(root) if root else ROOT
    ini = ini if ini is not None else read_ini(root)
    portable = ini.get("paths", "portable", fallback="0").strip() in ("1", "yes", "true")
    profile = ini.get("paths", "profile", fallback="").strip()
    userdata = ini.get("paths", "userdata", fallback="").strip()
    if portable:
        base = root / "userdata"
        prof = Path(profile) if profile else base / "profile"
        user = Path(userdata) if userdata else base / "local"
    else:
        prof = Path(profile) if profile else _documents() / "My Games" / "L4ZY"
        user = Path(userdata) if userdata else _local_appdata() / "L4ZY"
    if not prof.is_absolute():
        prof = root / prof
    if not user.is_absolute():
        user = root / user
    return prof, user


def configure(data_dir=None, models_dir=None, state_dir=None, profile_dir=None):
    global DATA, MODELS, STATE, PROFILE, LOGS
    prof, user = default_dirs()
    PROFILE = Path(profile_dir) if profile_dir else prof
    DATA = Path(data_dir) if data_dir else user / "translation"
    MODELS = Path(models_dir) if models_dir else user / "models"
    if state_dir:
        STATE = Path(state_dir)
    LOGS = DATA / "logs"
    for d in (DATA, MODELS, STATE, LOGS):
        d.mkdir(parents=True, exist_ok=True)


def ensure():
    if DATA is None:
        configure()
