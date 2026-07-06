"""
Build and run the hypex_proto host tests with whatever C compiler is around.

Tries, in order: gcc/clang on PATH, cl.exe on PATH, then MSVC located via
vcvars64.bat under common Visual Studio install roots.

Usage:  python run_tests.py
"""

import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROTO_DIR = os.path.join(HERE, "..", "components", "hypex_proto")
SOURCES = [
    os.path.join(HERE, "test_hypex_proto.c"),
    os.path.join(PROTO_DIR, "hypex_proto.c"),
]
INCLUDE = os.path.join(PROTO_DIR, "include")
BUILD = os.path.join(HERE, "build")
EXE = os.path.join(BUILD, "test_hypex_proto.exe" if os.name == "nt"
                   else "test_hypex_proto")


def run(cmd, **kw):
    print("+", cmd if isinstance(cmd, str) else " ".join(cmd))
    return subprocess.run(cmd, **kw).returncode


def try_gcc_like(cc):
    path = shutil.which(cc) or os.environ.get(cc.upper())
    if not path:
        return None
    flags = (["-Wall"] if cc == "tcc"
             else ["-std=c99", "-Wall", "-Wextra", "-Werror"])
    rc = run([path, *flags, "-I", INCLUDE, *SOURCES, "-o", EXE])
    return EXE if rc == 0 else False


def find_vcvars64():
    roots = [
        r"C:\Program Files\Microsoft Visual Studio",
        r"C:\Program Files (x86)\Microsoft Visual Studio",
    ]
    hits = []
    for root in roots:
        hits += glob.glob(os.path.join(
            root, "*", "*", "VC", "Auxiliary", "Build", "vcvars64.bat"))
    return sorted(hits, reverse=True)[0] if hits else None


def try_msvc():
    vcvars = find_vcvars64()
    if not shutil.which("cl") and not vcvars:
        return None
    srcs = " ".join(f'"{s}"' for s in SOURCES)
    # A generated batch file avoids cmd.exe nested-quoting problems with
    # `call "...vcvars64.bat" && cl ...`.
    lines = ["@echo off"]
    if not shutil.which("cl"):
        lines.append(f'call "{vcvars}" >NUL')
    lines.append(f'cl /nologo /W4 /I "{INCLUDE}" {srcs} '
                 f'/Fe:"{EXE}" /Fo:"{BUILD}\\\\"')
    bat = os.path.join(BUILD, "build.bat")
    with open(bat, "w") as f:
        f.write("\r\n".join(lines) + "\r\n")
    rc = run([bat])
    return EXE if rc == 0 else False


def main():
    os.makedirs(BUILD, exist_ok=True)
    exe = None
    # tcc: set env var TCC=<path-to-tcc.exe> if it isn't on PATH
    for attempt in (lambda: try_gcc_like("gcc"), lambda: try_gcc_like("clang"),
                    lambda: try_gcc_like("tcc"), try_msvc):
        result = attempt()
        if result:
            exe = result
            break
        if result is False:
            sys.exit("compiler found but the build failed (see above)")
    if not exe:
        sys.exit("no C compiler found (tried gcc, clang, cl/vcvars64). "
                 "Install VS Build Tools or MinGW and re-run.")
    rc = run([exe])
    sys.exit(rc)


if __name__ == "__main__":
    main()
