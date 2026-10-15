#!/usr/bin/env python3
"""Translate Tekken Tag Tournament 2 (Wii U) to C with this project's recompiler.

usage: recompile.py GAME_DIR_OR_RPX OUTDIR [--jobs N]

GAME_DIR_OR_RPX is your own dump of the game: the folder with code/, content/ and meta/, or its
code/Tekken.rpx. The output (code_*.c, table.c, imports.c, funcs.h) is generated from your copy and
must not be redistributed. The Wind Waker HD runtime refers to a few functions TTT2 does not have;
wwhd_stubs.c (copied into OUTDIR as code_wwhdstubs.c) stops with a message if one is ever reached.
"""
import argparse, collections, hashlib, os, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
RPX_SHA256 = "c3afc72f30aa1af84901bfc0775aeae7a359d396923a387cfbe71034175886ab"
PER_FILE = 30000  # instructions per generated file


def find_rpx(path):
    if os.path.isfile(path):
        return path
    code = os.path.join(path, "code")
    if os.path.isdir(code):
        for name in os.listdir(code):
            if name.lower() == "tekken.rpx":
                return os.path.join(code, name)
    sys.exit("no code/Tekken.rpx in %s: choose the extracted game folder (with code, content, meta)" % path)


def worker(rpx, outdir, wi, nw):
    sys.path.insert(0, os.path.join(TOOLS, "recomp"))
    sys.path.insert(0, TOOLS)
    import builds as gb
    ttt2 = gb.Build({"name": "TTT2", "title_id": "000500001010F800", "rpx_sha256": RPX_SHA256,
                     "code_bounds": ["02000000", "0A000000"], "data_bounds": ["10000000", "11000000"]})
    gb.hook_files = lambda directory=None: []  # no Wind Waker HD hooks: identity address map
    import recomp
    t = time.time()
    r = recomp.Recompiler(rpx, ttt2)
    print("[%d] analysis %.0fs, %d functions" % (wi, time.time() - t, len(r.sorted_entries)), flush=True)
    r.unhandled = collections.Counter()
    r.cr_stats = [0, 0]
    r.leaf_count = r.nonleaf_count = 0
    r.used_imports = set()
    r.ordinal = {e: i for i, e in enumerate(r.sorted_entries)}
    chunks, cur, n = [], [], 0
    for s in r.sorted_entries:
        cur.append(s)
        n += (r.func_end(s) - s) // 4
        if n >= PER_FILE:
            chunks.append(cur)
            cur, n = [], 0
    if cur:
        chunks.append(cur)
    for i, ch in enumerate(chunks):
        if i % nw != wi:
            continue
        tmp = os.path.join(outdir, "code_%04d.c.tmp" % i)
        with open(tmp, "w") as f:
            f.write('#include "funcs.h"\n\n')
            for s in ch:
                src, _ = r.emit_function(s)
                f.write(src)
                f.write("\n\n")
        os.replace(tmp, os.path.join(outdir, "code_%04d.c" % i))
        if i % 50 < nw:
            print("[%d] file %d/%d %.0fs" % (wi, i, len(chunks), time.time() - t), flush=True)
    if wi == 0:
        r.write_headers(outdir)
    print("[%d] done %.0fs" % (wi, time.time() - t), flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("outdir")
    ap.add_argument("--jobs", type=int, default=2, help="worker processes (each needs about 2 GB of memory)")
    ap.add_argument("--worker", type=int, help=argparse.SUPPRESS)
    a = ap.parse_args()
    rpx = find_rpx(a.game)
    os.makedirs(a.outdir, exist_ok=True)
    if a.worker is not None:
        worker(rpx, a.outdir, a.worker, a.jobs)
        return
    digest = hashlib.sha256(open(rpx, "rb").read()).hexdigest()
    if digest != RPX_SHA256:
        sys.exit("%s is not the supported Tekken.rpx (SHA-256 %s, expected %s): use the EUR disc/eShop release "
                 "[AKNPAF] without updates" % (rpx, digest, RPX_SHA256))
    for name in os.listdir(a.outdir):
        if name.startswith("code_") or name in ("table.c", "imports.c", "funcs.h", "imports.json"):
            os.remove(os.path.join(a.outdir, name))
    t = time.time()
    procs = [subprocess.Popen([sys.executable, os.path.abspath(__file__), rpx, a.outdir, "--jobs", str(a.jobs),
                               "--worker", str(i)]) for i in range(a.jobs)]
    if any(p.wait() for p in procs):
        sys.exit("the recompiler failed (see above)")
    shutil.copyfile(os.path.join(HERE, "wwhd_stubs.c"), os.path.join(a.outdir, "code_wwhdstubs.c"))
    print("translated in %.0f s into %s" % (time.time() - t, a.outdir))


if __name__ == "__main__":
    main()
