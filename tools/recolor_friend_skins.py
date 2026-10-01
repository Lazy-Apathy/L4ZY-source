# Recolor FFA player skins onto friend hues.
# Always starts from the FFA albedo (never blue/red) so clothes stay identical
# across colors. Only the "body identity" pixels are shifted; value/shading
# come from FFA so bright folds stay brighter than dark ones.

import colorsys
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MODELS = ROOT / "packages" / "models"

# Same RGB as friends.cpp friendtexcolor[]
FRIEND_RGB = {
    0: (0x40, 0xFF, 0x80),  # green
    1: (0x60, 0xA0, 0xFF),  # blue
    2: (0xFF, 0xC0, 0x40),  # yellow
    3: (0xFF, 0x40, 0x40),  # red
    4: (0x80, 0x80, 0x80),  # gray
    5: (0xC0, 0x40, 0xC0),  # magenta
    6: (0xFF, 0x80, 0x00),  # orange
    7: (0xFF, 0xFF, 0xFF),  # white
    8: (0x60, 0xF0, 0xFF),  # cyan
    9: (0xFF, 0x80, 0xC8),  # rose
}

# hue_lo/hi are 0-1. Pixels outside stay FFA (vest, pants, metal, eyes).
CHARACTERS = [
    {
        "name": "mrfixit",
        "cfgname": "md5.cfg",
        # Olive orc skin. Brown vest / gloves / belt stay FFA.
        "maps": [
            {"ffa": "Head.png", "out": "Headc{idx}.png", "hue": (0.155, 0.25)},
            {"ffa": "Body.png", "out": "Bodyc{idx}.png", "hue": (0.155, 0.25)},
        ],
        "cfg": """\
md5dir "mrfixit"
md5load "mrfixit.md5mesh" mrfixit
exec "packages/models/mrfixit/ragdoll.cfg"
md5tag Weapon tag_weapon
md5skin Head "Headc{idx}.png" "<dds>headmasks.png" .9 .8
md5bumpmap Head "<dds>headnorm.png"
md5envmap Head socksky/mars
md5skin Body "Bodyc{idx}.png" "<dds>bodymasks.png" .4 .2
md5bumpmap Body "<dds>bodynorm.png"
md5envmap Body socksky/desert
exec "packages/models/mrfixit/anims.cfg"
mdlscale 800
mdlspec 175
""",
    },
    {
        "name": "snoutx10k",
        "cfgname": "md5.cfg",
        # Green armor plates. Black undersuit has almost no saturation.
        "maps": [
            {"ffa": "upper.png", "out": "upper_c{idx}.png", "hue": (0.248, 0.34)},
            {"ffa": "lower.png", "out": "lower_c{idx}.png", "hue": (0.248, 0.34)},
        ],
        "cfg": """\
md5dir "snoutx10k"
md5load "snoutx10k.md5mesh" mrfixit
exec "packages/models/mrfixit/ragdoll.cfg"
md5tag Weapon tag_weapon
md5skin Upper "upper_c{idx}.png" "<dds>upper_mask.png" .4 .1
md5bumpmap Upper "<dds>upper_normals.png"
md5skin Lower "lower_c{idx}.png" "<dds>lower_mask.png" .4 .1
md5bumpmap Lower "<dds>lower_normals.png"
exec "packages/models/mrfixit/anims.cfg"
mdlscale 800
mdlspec 175
mdlenvmap 0 0 skyboxes/morning
""",
    },
    {
        "name": "ogro2",
        "cfgname": "iqm.cfg",
        # Green armor / pants / warpaint. Brown skin stays FFA.
        "maps": [
            {"ffa": "green.jpg", "out": "greenc{idx}.png", "hue": (0.27, 0.38), "smin": 0.12},
        ],
        "cfg": """\
iqmdir "ogro2"
iqmload "ogro.iqm" ogro
exec "packages/models/ogro2/ragdoll.cfg"
iqmskin * "greenc{idx}.png" "masks.jpg"
iqmtag Tag.L tag_weapon
iqmtag Forearm.R tag_shield 0.05 -0.4 0 -80 0 -90
iqmtag Chest tag_powerup -0.55 -0.25 -0.25 70 170 -25
exec "packages/models/ogro2/anims.cfg"
mdlspec 150
mdlambient 50
mdlglare 1 0
mdltrans 0 0 1.43
mdlscale 1800
mdlextendbb 0 6 1
""",
    },
    {
        "name": "inky",
        "cfgname": "md5.cfg",
        # Green creature body. Eyes / suckers / wings stay FFA.
        "maps": [
            {"ffa": "inky.png", "out": "inky_c{idx}.png", "hue": (0.165, 0.28)},
        ],
        "cfg": """\
md5dir "inky"
md5load "inky.md5mesh" inky
exec "packages/models/inky/ragdoll.cfg"
md5skin inkymesh "inky_c{idx}.png" "<dds>inky_mask.png"
md5bumpmap inkymesh "<dds>inky_normals.png"
md5skin wings "<dds>inky_wings.png" "<dds>inky_wings_mask.png"
md5bumpmap wings "<dds>inky_wings_norms.png"
md5skin wingarms "<dds>inky_wings.png" "<dds>inky_wings_mask.png"
md5bumpmap wingarms "<dds>inky_wings_norms.png"
md5tag Weapon tag_weapon
exec "packages/models/inky/anims.cfg"
mdlglow 200
mdlspec 30
mdltrans 0.5 0 0
mdlscale 1900
""",
    },
    {
        "name": "captaincannon",
        "cfgname": "md5.cfg",
        # Green jumpsuit. Orange bib / mask / gloves and brown skin stay FFA.
        "maps": [
            {"ffa": "cc_head.png", "out": "cc_head_c{idx}.png", "hue": (0.328, 0.40)},
            {"ffa": "cc_body.png", "out": "cc_body_c{idx}.png", "hue": (0.328, 0.40)},
        ],
        "cfg": """\
md5dir "captaincannon"
md5load "captaincannon.md5mesh" captaincannon 70
exec "packages/models/captaincannon/ragdoll.cfg"
md5tag Weapon tag_weapon
md5skin head "cc_head_c{idx}.png" "<dds>cc_head_mask.png"
md5bumpmap head "<dds>cc_head_normals.png"
md5skin body "cc_body_c{idx}.png" "<dds>cc_body_mask.png"
md5bumpmap body "<dds>cc_body_normals.png"
exec "packages/models/captaincannon/anims.cfg"
mdlscale 1225
mdlspec 60
""",
    },
]

# First-person hands. Mr. Fixit's FPS skin is greener than the 3rd-person olive
# body (hue ~0.30 vs ~0.19). Leather gauntlet stays FFA (brown, ~0.08).
HANDS = [
    {
        "ffa": MODELS / "mrfixit" / "hudguns" / "fixit_hands.png",
        "out": "mrfixit/hudguns/fixit_hands_c{idx}.png",
        "hue": (0.26, 0.36),
    },
    {
        "ffa": MODELS / "snoutx10k" / "hudguns" / "snout_hands.png",
        "out": "snoutx10k/hudguns/snout_hands_c{idx}.png",
        "hue": (0.248, 0.34),
    },
]

HUDGUN_CHARS = ("mrfixit", "snoutx10k", "inky", "captaincannon")
HUDGUN_GUNS = ("fist", "chaing", "gl", "pistol", "rifle", "rocket", "shotg")


def target_hsv(rgb):
    return colorsys.rgb_to_hsv(rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0)


def hue_in(h, lo, hi):
    if lo <= hi:
        return lo <= h <= hi
    return h >= lo or h <= hi


def recolor_ffa(ffa_path: Path, out_path: Path, target_rgb, hue, smin=0.12, vmin=0.10):
    ffa = Image.open(ffa_path).convert("RGB")
    th, ts, tv = target_hsv(target_rgb)
    hue_lo, hue_hi = hue
    fp = ffa.tobytes()
    n = len(fp) // 3
    out = bytearray(len(fp))
    changed = 0
    for i in range(n):
        o = i * 3
        fr, fg, fb = fp[o], fp[o + 1], fp[o + 2]
        fh, fs, fv = colorsys.rgb_to_hsv(fr / 255.0, fg / 255.0, fb / 255.0)
        if not (fs >= smin and fv >= vmin and hue_in(fh, hue_lo, hue_hi)):
            out[o:o + 3] = fr, fg, fb
            continue
        if ts < 0.08:
            ns = min(fs * 0.15, 0.12)
            v = fv * 0.4 + 0.58 if tv >= 0.95 else fv * 0.65 + 0.18
        else:
            ns = min(1.0, fs * 0.50 + ts * 0.50)
            v = fv
        nr, ng, nb = colorsys.hsv_to_rgb(th, ns, v)
        out[o] = int(nr * 255.0 + 0.5)
        out[o + 1] = int(ng * 255.0 + 0.5)
        out[o + 2] = int(nb * 255.0 + 0.5)
        changed += 1
    Image.frombytes("RGB", ffa.size, bytes(out)).save(out_path, "PNG")
    pct = 100.0 * changed / n
    print(f"  {out_path.name}: {changed}/{n} pixels ({pct:.1f}%)")


def write_cfg(char, idx: int):
    d = MODELS / char["name"] / f"c{idx}"
    d.mkdir(parents=True, exist_ok=True)
    (d / char["cfgname"]).write_text(char["cfg"].format(idx=idx), encoding="ascii")


def write_hudgun_cfgs(indices):
    for char in HUDGUN_CHARS:
        if only_filter and char != only_filter:
            continue
        for gun in HUDGUN_GUNS:
            blue = MODELS / char / "hudguns" / gun / "blue" / "md5.cfg"
            if not blue.is_file():
                raise SystemExit(f"missing {blue}")
            text = blue.read_text(encoding="ascii")
            for idx in indices:
                dest = MODELS / char / "hudguns" / gun / f"c{idx}"
                dest.mkdir(parents=True, exist_ok=True)
                (dest / "md5.cfg").write_text(text, encoding="ascii")
            print(f"  hudguns {char}/{gun}/c0-c{indices[-1] if indices else 9}")


only_filter = None


def run(indices, only=None):
    global only_filter
    only_filter = only
    for char in CHARACTERS:
        if only and char["name"] != only:
            continue
        folder = MODELS / char["name"]
        for spec in char["maps"]:
            src = folder / spec["ffa"]
            if not src.is_file():
                raise SystemExit(f"missing {src}")
        for idx in indices:
            rgb = FRIEND_RGB[idx]
            print(f"{char['name']}/c{idx} from FFA -> {rgb}")
            for spec in char["maps"]:
                out = folder / spec["out"].format(idx=idx)
                recolor_ffa(
                    folder / spec["ffa"],
                    out,
                    rgb,
                    spec["hue"],
                    spec.get("smin", 0.12),
                    spec.get("vmin", 0.10),
                )
            write_cfg(char, idx)
    for spec in HANDS:
        if only and only not in spec["out"]:
            continue
        if not spec["ffa"].is_file():
            raise SystemExit(f"missing {spec['ffa']}")
        for idx in indices:
            rgb = FRIEND_RGB[idx]
            out = MODELS / spec["out"].format(idx=idx)
            print(f"hands {out.relative_to(MODELS)} -> {rgb}")
            recolor_ffa(spec["ffa"], out, rgb, spec["hue"])
    write_hudgun_cfgs(indices)


if __name__ == "__main__":
    only = None
    hands_only = False
    idxs = []
    for a in sys.argv[1:]:
        if a.startswith("--only="):
            only = a.split("=", 1)[1]
        elif a == "--hands":
            hands_only = True
        else:
            idxs.append(int(a))
    idxs = idxs or list(range(10))
    if hands_only:
        only_filter = only
        for spec in HANDS:
            if only and only not in spec["out"]:
                continue
            if not spec["ffa"].is_file():
                raise SystemExit(f"missing {spec['ffa']}")
            for idx in idxs:
                rgb = FRIEND_RGB[idx]
                out = MODELS / spec["out"].format(idx=idx)
                print(f"hands {out.relative_to(MODELS)} -> {rgb}")
                recolor_ffa(spec["ffa"], out, rgb, spec["hue"])
        write_hudgun_cfgs(idxs)
    else:
        run(idxs, only)
