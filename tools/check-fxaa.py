"""Validate the fxaa postfx shader in data/glsl.cfg the way the engine builds it.

shader.cpp prepends a version header plus the legacy-name defines, and the
fsps macro in glsl.cfg wraps the body in main() with `color` already sampled.
Reassembling both here means glslang sees the same text the driver would,
so a typo shows up before the shader is loaded in a live game.
"""

import os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, "data", "glsl.cfg")

def find_glslang():
    for sub in ("glslang-bin", "glslang"):
        exe = os.path.join(ROOT, "tools", "_deps", sub, "bin",
                           "glslang" + (".exe" if os.name == "nt" else ""))
        if os.path.exists(exe):
            return exe
    return None

def extract(name):
    src = open(CFG, encoding="utf-8", errors="replace").read()
    head = 'lazyshader 0 "%s" (fsvs) (fsps [' % name
    i = src.index(head) + len(head)
    j = src.index("\n] [", i)
    body = src[i:j]
    k = src.index("]", j + 4)
    decls = src[j + 4:k]
    return body, decls

def assemble(body, decls, version):
    parts = ["#version %d\n" % version]
    if version >= 130:
        parts.append("#define varying in\n")
        if version < 150:
            parts.append("precision highp float;\n")
        parts.append("out vec4 cube2_FragColor;\n"
                     "#define gl_FragColor cube2_FragColor\n")
        parts.append("#define texture2D(sampler, coords) texture(sampler, coords)\n")
    parts.append("uniform sampler2D tex0;\n"
                 "varying vec2 texcoord0;\n")
    parts.append(decls + "\n")
    parts.append("void main(void)\n{\n"
                 "    vec4 color = texture2D(tex0, texcoord0);\n")
    parts.append(body)
    parts.append("\n}\n")
    return "".join(parts)

def main():
    glslang = find_glslang()
    if not glslang:
        print("glslang introuvable dans tools/_deps")
        return 1

    body, decls = extract("fxaa")
    print("corps extrait : %d octets, declarations : %r" % (len(body), decls.strip()))
    for ch in "[]@$":
        if ch in body:
            print("ATTENTION : le corps contient %r, que CubeScript interprete" % ch)
            return 1
    print("aucun caractere que CubeScript intercepte dans le corps")

    bad = 0
    for version in (120, 330):
        text = assemble(body, decls, version)
        path = os.path.join(tempfile.gettempdir(), "fxaa_%d.frag" % version)
        open(path, "w", encoding="utf-8", newline="\n").write(text)
        r = subprocess.run([glslang, path], capture_output=True, text=True)
        ok = r.returncode == 0
        print("\n--- #version %d : %s" % (version, "OK" if ok else "ECHEC"))
        out = (r.stdout + r.stderr).strip()
        if not ok or out.count("\n") > 1:
            print(out)
        if not ok:
            bad += 1
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
