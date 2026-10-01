"""Validate the looktaa shader the way the engine builds it.

The git-tracked copy lives in data/hwrt.cfg as shader 0 "looktaa".
data/glsl.cfg may redefine the same name next to lookssao.
"""

import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HWRT = os.path.join(ROOT, "data", "hwrt.cfg")
GLSL = os.path.join(ROOT, "data", "glsl.cfg")

def find_glslang():
    for sub in ("glslang-bin", "glslang"):
        exe = os.path.join(ROOT, "tools", "_deps", sub, "bin",
                           "glslang" + (".exe" if os.name == "nt" else ""))
        if os.path.exists(exe):
            return exe
    return None

def extract_shader0(path, name):
    src = open(path, encoding="utf-8", errors="replace").read()
    head = 'shader 0 "%s" [' % name
    i = src.index(head) + len(head)
    j = src.index("\n] [", i)
    vs = src[i:j]
    k = src.index("\n]", j + 4)
    fs = src[j + 4:k]
    return vs, fs

def assemble_fs(fs, version):
    parts = ["#version %d\n" % version]
    if version >= 130:
        parts.append("#define varying in\n")
        if version < 150:
            parts.append("precision highp float;\n")
        parts.append("out vec4 cube2_FragColor;\n"
                     "#define gl_FragColor cube2_FragColor\n")
        parts.append("#define texture2D(sampler, coords) texture(sampler, coords)\n")
    parts.append(fs)
    if "void main" not in fs:
        raise SystemExit("fragment is missing main()")
    return "".join(parts)

def compile_one(glslang, text, tag):
    path = os.path.join(tempfile.gettempdir(), "looktaa_%s.frag" % tag)
    open(path, "w", encoding="utf-8", newline="\n").write(text)
    r = subprocess.run([glslang, path], capture_output=True, text=True)
    ok = r.returncode == 0
    print("\n--- %s : %s" % (tag, "OK" if ok else "ECHEC"))
    out = (r.stdout + r.stderr).strip()
    if not ok or out.count("\n") > 1:
        print(out)
    return ok

def main():
    glslang = find_glslang()
    if not glslang:
        print("glslang introuvable dans tools/_deps")
        return 1

    vs, fs = extract_shader0(HWRT, "looktaa")
    print("hwrt.cfg looktaa: vs %d octets, fs %d octets" % (len(vs), len(fs)))
    for ch in "[]@$":
        # The closing brackets of the CubeScript shader form are outside fs.
        if ch in fs:
            print("ATTENTION : le fragment hwrt.cfg contient %r" % ch)
            return 1
    print("aucun caractere que CubeScript intercepte dans le fragment")

    bad = 0
    for version in (120, 330):
        if not compile_one(glslang, assemble_fs(fs, version), "hwrt_%d" % version):
            bad += 1

    if os.path.exists(GLSL) and 'lazyshader 0 "looktaa"' in open(GLSL, encoding="utf-8", errors="replace").read():
        print("\nglsl.cfg redefine aussi looktaa (copie look-system), non verifie ici")

    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
