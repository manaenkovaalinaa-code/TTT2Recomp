# Shader template maintenance

The release template lives in the private `ZeldaWWHDRecomp/shader-template` repository.
`template.ref` pins its exact commit and the SHA-256 of `template.bin`. Update both after publishing a
new private template. The public branch must never track `template.bin`, recordings or generated caches.

Release packaging fetches the pinned commit using the read-only `SHADER_TEMPLATE_DEPLOY_KEY` secret,
verifies the hash, and runs the independent release guard. Fork and pull-request builds without the secret print a notice and package without a template.
Upstream tag, development-dispatch and nightly builds require the secret. Fetch or verification failure
stops packaging. GitHub's SSH host key is pinned rather than learned at fetch time.
The workflow keeps its temporary SSH key private and removes it even when a step fails.

## Local recording

Use Python 3. The archive decoder builds a small local C helper with Clang/GCC (`CC` overrides it),
or uses a Python fallback. Recordings carry game-derived programs; keep them and generated headstarts
out of commits, uploads and reports. Only independently guarded code-free templates may be packaged.

Windows (PowerShell, from the repository root):

```powershell
$env:WWHD_RENDERER_RUNTIME = "vulkan"
$env:WWHD_VK_RECIPE_RECORD = "$PWD\game\shadercache\vulkan-recorded.bin"
.\build\cmake\wwhd.exe --game game
Remove-Item Env:WWHD_VK_RECIPE_RECORD
py -3 tools/shaderprep.py template game/shadercache/vulkan-recorded.bin game/shadercache/template.bin
py -3 tools/shaderprep.py build game/shadercache/headstart.bin --template game/shadercache/template.bin --max 0
.\build\cmake\wwhd.exe --game game --warm-shaders
```

Linux:

```sh
WWHD_RENDERER_RUNTIME=vulkan \
WWHD_VK_RECIPE_RECORD="$PWD/game/shadercache/vulkan-recorded.bin" \
  ./build/cmake/wwhd --game game
python3 tools/shaderprep.py template game/shadercache/vulkan-recorded.bin game/shadercache/template.bin
python3 tools/shaderprep.py build game/shadercache/headstart.bin --template game/shadercache/template.bin --max 0
WWHD_RENDERER_RUNTIME=vulkan ./build/cmake/wwhd --game game --warm-shaders
```

Play the scenes to cover and exit normally before conversion. Recording is opt-in and bounded to
128 MiB; forced termination can lose the recording. Add `--merge game/shadercache/template.bin` to
`template` to retain existing states. `--max 0` resolves known states only; `--no-pipelines` excludes
pipeline recipes. Vulkan and Metal skip the other renderer's typed recipes.

`WWHD_HEADSTART=<file>` selects a local built headstart (`0` disables it).
`WWHD_VK_SHADER_CACHE=<directory>` selects SPIR-V storage; `WWHD_VK_PIPELINE_CACHE=<file>` selects the
device cache. `WWHD_VK_PIPELINE_CACHE_DIR=<directory>` selects device-specific
`vulkan-<vendor>-<device>-<uuid>.bin` files and adjacent `.recipes` files; an
explicit file override wins over the directory. Normal setup and runtime use
`shader-prebuild/live/`. On first activation, old default Vulkan caches and the
macOS Metal shader cache are copied there once without replacing existing files.
Sources stay intact; copy errors are logged and do not prevent play.
Never pass `spirv.bin` to `shaderprep.py template`: conversion requires the GX2 recording.

The stripped template schema is documented at the top of `tools/shaderprep.py`. The independent
`tools/release/guard.py` permits only that schema at the packaged template path. Packaging tests generate
small synthetic fixtures; they do not depend on the private template.

For a local template, run setup with `--shader-template /path/to/template.bin` or set
`WWHD_SHADER_TEMPLATE` to that file. The command-line option takes precedence over
the environment variable; otherwise setup uses the optional bundled template.
The selected path is saved for later cache rebuilds. Without a template, setup
skips preparation and shaders compile during play.

Setup/repair is the only preparation entry point. Ordinary game startup checks the saved
input metadata and selects an already-ready head start; it never runs Python, assembles
a head start, or performs offline warming. Full input hashes are computed during setup.
An opt-out is bound to the game and template, so a runtime update does not undo it.
Failed preparations are remembered for those inputs until they change or a rebuild is requested.

Prepared head starts belong to `shader-prebuild/generation-*`; player shader and driver
caches belong to `shader-prebuild/live` and persist across generations. Successful publication
removes superseded generations only when they have no active reader lease. Explicit cache
environment overrides retain precedence. Local Vulkan recipes also carry a source-derived
runtime fingerprint; a warm recipe must match the current serialized pipeline state before use.
