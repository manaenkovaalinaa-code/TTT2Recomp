#!/usr/bin/env python3
"""The AppImage's AppRun: per-user folders by default, a "<AppImage>.data" folder when it exists (issue #55)."""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import appimage  # noqa: E402


def run(env_extra):
    with tempfile.TemporaryDirectory() as temporary:
        appdir = os.path.join(temporary, "AppDir")
        os.makedirs(appdir)
        apprun = os.path.join(appdir, "AppRun")
        with open(apprun, "w") as f:
            f.write(appimage.APPRUN)
        launcher = os.path.join(appdir, "wind-waker-hd")
        with open(launcher, "w") as f:  # stands in for the launcher: report what it was given
            f.write('#!/bin/sh\necho "data=$XDG_DATA_HOME config=$XDG_CONFIG_HOME cache=$XDG_CACHE_HOME args=$*"\n')
        os.chmod(apprun, 0o755)
        os.chmod(launcher, 0o755)
        image = os.path.join(temporary, "Wind Waker [HD].AppImage")
        env = {k: v for k, v in os.environ.items() if not k.startswith("XDG_") and k != "APPIMAGE"}
        env.update({k: v.replace("@IMAGE@", image) for k, v in env_extra.items()})
        if env_extra.get("make_data"):
            os.makedirs(image + ".data")
        env.pop("make_data", None)
        out = subprocess.run(["sh", apprun, "--setup"], env=env, capture_output=True, text=True, check=True).stdout.strip()
        return out, image


def main():
    out, _ = run({})
    assert out == "data= config= cache= args=--setup", out  # outside an AppImage: nothing changes
    out, _ = run({"APPIMAGE": "@IMAGE@"})
    assert out == "data= config= cache= args=--setup", out  # no .data folder: per-user folders
    out, image = run({"APPIMAGE": "@IMAGE@", "make_data": "1"})
    want = "data=%s.data/share config=%s.data/config cache=%s.data/cache args=--setup" % (image, image, image)
    assert out == want, (out, want)  # portable: everything under the .data folder (spaces/brackets in the path work)
    print("AppRun: per-user folders by default, <AppImage>.data when present")


if __name__ == "__main__":
    main()
