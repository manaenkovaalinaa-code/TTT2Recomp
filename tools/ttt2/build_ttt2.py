#!/usr/bin/env python3
"""Build the Tekken Tag Tournament 2 port from your own dump of the game.

usage: build_ttt2.py --game GAME_DIR [--work DIR] [--jobs N] [--clean]

Steps: (1) on Windows, download the pinned tools into <repo>/.tools (llvm-mingw, CMake, Ninja;
SHA-256 checked, no administrator rights); (2) translate code/Tekken.rpx to C (recompile.py);
(3) compile the runtime and the translated game with CMake; (4) put the game into
<work>/TTT2-port with run_TTT2.bat (Windows) or run_TTT2.sh.

Everything generated here comes from your copy of the game and must not be redistributed.
Standard library only (Windows: the build-windows.bat next to this file supplies Python).
"""
import argparse, hashlib, os, platform, shutil, subprocess, sys, tarfile, time, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
WINDOWS = os.name == "nt"

# pinned downloads (Windows x86-64): the same llvm-mingw as tools/installer/toolchains.json
TOOLS = {
    "llvm-mingw": ("https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip",
                   "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666",
                   "llvm-mingw-20260922-ucrt-x86_64/bin"),
    "cmake": ("https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip",
              "d163cd3ab4959b0a53fa8988f2ddbd2e6c501658201e6a154386bad9dbe4f836",
              "cmake-3.31.6-windows-x86_64/bin"),
    "ninja": ("https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip",
              "f550fec705b6d6ff58f2db3c374c2277a37691678d6aba463adcbb129108467a",
              "."),
}


def log(msg):
    print("== " + msg, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# Vulkan headers and the loader's export list (an import library is made from it), as in the release CI
VULKAN_HEADERS = ("https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz",
                  "cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68")
VULKAN_DEF = ("https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/vulkan-sdk-1.4.363.0/loader/vulkan-1.def",
              "9ba339b7f5ee2df28487698a6840ecf095fac58b415c2c89ad9f161e9d316378")


def download(url, digest, path):
    if os.path.exists(path) and sha256(path) == digest:
        return
    log("downloading " + url)
    tmp = path + ".part"
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    got = sha256(tmp)
    if got != digest:
        os.remove(tmp)
        sys.exit("%s: SHA-256 %s, expected %s; try again" % (url, got, digest))
    os.replace(tmp, path)


def fetch_vulkan(tools_dir, llvm_bin):
    dest = os.path.join(tools_dir, "vulkan")
    lib = os.path.join(dest, "libvulkan-1.a")
    if os.path.exists(lib) and os.path.exists(os.path.join(dest, "include", "vulkan", "vulkan.h")):
        return dest
    os.makedirs(dest, exist_ok=True)
    tgz = os.path.join(tools_dir, "vulkan-headers.tar.gz")
    download(*VULKAN_HEADERS, tgz)
    with tarfile.open(tgz) as t:
        for m in t.getmembers():
            parts = m.name.split("/", 1)
            if len(parts) == 2 and parts[1].startswith("include/") and (m.isfile() or m.isdir()):
                m.name = parts[1]
                t.extract(m, dest)
    os.remove(tgz)
    deffile = os.path.join(dest, "vulkan-1.def")
    download(*VULKAN_DEF, deffile)
    run([os.path.join(llvm_bin, "llvm-dlltool.exe"), "-m", "i386:x86-64", "-d", deffile, "-l", lib])
    return dest


def fetch_tool(name, tools_dir):
    url, digest, bindir = TOOLS[name]
    dest = os.path.join(tools_dir, name)
    stamp = os.path.join(dest, ".ok")
    if os.path.exists(stamp) and open(stamp).read().strip() == digest:
        return os.path.normpath(os.path.join(dest, bindir))
    os.makedirs(tools_dir, exist_ok=True)
    archive = os.path.join(tools_dir, os.path.basename(url))
    if not os.path.exists(archive) or sha256(archive) != digest:
        log("downloading %s (%s)" % (name, url))
        tmp = archive + ".part"
        with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        os.replace(tmp, archive)
    got = sha256(archive)
    if got != digest:
        os.remove(archive)
        sys.exit("%s: SHA-256 %s, expected %s; download it again" % (archive, got, digest))
    log("unpacking %s" % name)
    shutil.rmtree(dest, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(dest)
    os.remove(archive)
    with open(stamp, "w") as f:
        f.write(digest)
    return os.path.normpath(os.path.join(dest, bindir))


def run(cmd, env=None):
    print("$ " + " ".join('"%s"' % c if " " in c else c for c in cmd), flush=True)
    r = subprocess.run(cmd, env=env)
    if r.returncode:
        sys.exit("failed (exit code %d): %s" % (r.returncode, cmd[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="the extracted game folder (code, content, meta)")
    ap.add_argument("--work", default=os.path.join(REPO, "build-ttt2"), help="build folder")
    ap.add_argument("--jobs", type=int, default=0, help="parallel compile jobs (default: all cores)")
    ap.add_argument("--recompile-jobs", type=int, default=0, help="translator processes (about 2 GB of memory each; default 2)")
    ap.add_argument("--clean", action="store_true", help="translate and compile everything again")
    a = ap.parse_args()

    game = os.path.abspath(a.game)
    work = os.path.abspath(a.work)
    gen = os.path.join(work, "gen")
    cmake_dir = os.path.join(work, "cmake")
    if a.clean:
        shutil.rmtree(gen, ignore_errors=True)
        shutil.rmtree(cmake_dir, ignore_errors=True)
    os.makedirs(work, exist_ok=True)
    env = dict(os.environ)
    t0 = time.time()

    # 1. tools
    if WINDOWS:
        if platform.machine().lower() not in ("amd64", "x86_64"):
            sys.exit("this script builds for x86-64 Windows; this PC is %s" % platform.machine())
        tools_dir = os.path.join(REPO, ".tools")
        paths = [fetch_tool(n, tools_dir) for n in ("llvm-mingw", "cmake", "ninja")]
        env["PATH"] = os.pathsep.join(paths + [env.get("PATH", "")])
        triple = os.path.join(paths[0], "x86_64-w64-mingw32-")
        cc, cxx, rc = triple + "clang.exe", triple + "clang++.exe", triple + "windres.exe"
        cmake = os.path.join(paths[1], "cmake.exe")
        ninja = os.path.join(paths[2], "ninja.exe")
        vulkan = fetch_vulkan(tools_dir, paths[0])
    else:
        cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
        cc, cxx = shutil.which("clang"), shutil.which("clang++")
        if not (cmake and ninja and cc and cxx):
            sys.exit("needs cmake, ninja, clang and clang++ on PATH (and the Linux build dependencies in README.md)")

    # 2. translate the game code
    stamp = os.path.join(gen, ".translated")
    if not os.path.exists(stamp):
        log("translating the game code to C (10-20 minutes)")
        cmd = [sys.executable, os.path.join(HERE, "recompile.py"), game, gen]
        if a.recompile_jobs:
            cmd += ["--jobs", str(a.recompile_jobs)]
        run(cmd, env)
        with open(stamp, "w") as f:
            f.write("ok\n")
    else:
        log("game code already translated (%s); --clean translates again" % gen)

    # 3. compile
    log("configuring")
    cfg = [cmake, "-S", REPO, "-B", cmake_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_MAKE_PROGRAM=" + ninja, "-DCMAKE_C_COMPILER=" + cc, "-DCMAKE_CXX_COMPILER=" + cxx,
           "-DGEN_DIR=" + gen, "-DWWHD_SETUP_GUI=OFF"]
    if WINDOWS:
        cfg += ["-DCMAKE_RC_COMPILER=" + rc, "-DWWHD_BUNDLED_DEPS=ON", "-DCMAKE_EXE_LINKER_FLAGS=-static",
                "-DVulkan_INCLUDE_DIR=" + os.path.join(vulkan, "include"),
                "-DVulkan_LIBRARY=" + os.path.join(vulkan, "libvulkan-1.a")]
    run(cfg, env)
    log("compiling (the game code is about 1000 large files: 20-90 minutes, once)")
    build = [cmake, "--build", cmake_dir, "--target", "wwhd"]
    if a.jobs:
        build += ["--parallel", str(a.jobs)]
    run(build, env)

    # 4. package
    out = os.path.join(work, "TTT2-port")
    os.makedirs(out, exist_ok=True)
    exe = "wwhd.exe" if WINDOWS else "wwhd"
    shutil.copy2(os.path.join(cmake_dir, exe), os.path.join(out, "TTT2.exe" if WINDOWS else "ttt2"))
    for dll in ("SDL3.dll",):
        if os.path.exists(os.path.join(cmake_dir, dll)):
            shutil.copy2(os.path.join(cmake_dir, dll), out)
    with open(os.path.join(out, "portable.txt"), "w", encoding="utf-8") as f:
        f.write("Portable: saves, settings, shader caches and logs stay in this folder.\n")
    if WINDOWS:
        # UTF-8 with chcp 65001: the game folder may have a non-ASCII name (e.g. in a Cyrillic user folder)
        with open(os.path.join(out, "run_TTT2.bat"), "w", newline="\r\n", encoding="utf-8") as f:
            f.write('@echo off\nchcp 65001 >nul\ncd /d "%%~dp0"\nset "GAME=%s"\nif not "%%~1"=="" set "GAME=%%~1"\n'
                    "set WWHD_PRO_CONTROLLER=1\nset WWHD_VK_VERTEX_COPY_WINDOW=1\n"
                    'TTT2.exe --game "%%GAME%%"\nif errorlevel 1 pause\n' % game)
    else:
        path = os.path.join(out, "run_TTT2.sh")
        with open(path, "w", encoding="utf-8") as f:
            f.write('#!/bin/sh\ncd "$(dirname "$0")"\nGAME="${1:-%s}"\n'
                    "WWHD_PRO_CONTROLLER=1 WWHD_VK_VERTEX_COPY_WINDOW=1 exec ./ttt2 --game \"$GAME\"\n" % game)
        os.chmod(path, 0o755)
    log("done in %.0f min: %s" % ((time.time() - t0) / 60, out))


if __name__ == "__main__":
    main()
