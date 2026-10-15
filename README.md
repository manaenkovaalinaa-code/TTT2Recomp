# Tekken Tag Tournament 2 Wii U Edition — native PC port

**[Русский](#русский) · [English](#english)**

An unofficial static recompilation of *Tekken Tag Tournament 2 Wii U Edition* for Windows (x86-64,
Vulkan), built on the runtime of the [Wind Waker HD native port](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp).
The game's PowerPC code is translated to C ahead of time, the Cafe OS libraries are reimplemented
natively, and GX2 graphics go straight to Vulkan. There is no emulator at run time.

---

## Русский

### Важно (правовая информация)

Это неофициальный фанатский проект, не связанный с Bandai Namco или Nintendo. «Tekken» и «Wii U» —
товарные знаки их владельцев, здесь они только описывают совместимость.

**В репозитории нет игры**: ни кода игры (исходного, перекомпилированного или дизассемблированного),
ни ресурсов, ни ключей. Только инструменты и среда выполнения этого проекта. Нужна **своя законно
полученная копия игры**, снятая со своей консоли. Всё, что создаётся из неё при сборке
(`build-ttt2/`), остаётся у вас и не должно распространяться. Поэтому готового `.exe` здесь нет:
игра собирается на вашем компьютере из вашей копии.

### Что нужно

- Windows 10/11 x64, видеокарта с Vulkan 1.3 (или 1.1/1.2 с `VK_KHR_dynamic_rendering`)
- около 15 ГБ свободного места, 8 ГБ ОЗУ, интернет при первой сборке
- распакованная игра **Tekken Tag Tournament 2 Wii U Edition [AKNPAF]** (Европа, без обновлений):
  папка с `code`, `content`, `meta` (например, из dumpling или Cemu). Проверяется файл
  `code/Tekken.rpx`, SHA-256 `c3afc72f30aa1af84901bfc0775aeae7a359d396923a387cfbe71034175886ab`.

### Сборка (Windows)

1. Скачайте репозиторий: зелёная кнопка **Code → Download ZIP**, распакуйте (путь лучше без кириллицы
   и пробелов, например `C:\TTT2Recomp`). Или `git clone`.
2. Запустите `tools\ttt2\build-windows.bat`. Перетащите в окно папку с игрой и нажмите Enter.
3. Скрипт сам скачает всё нужное в папку `.tools` (компилятор llvm-mingw, CMake, Ninja и, если
   Python не установлен, Python), проверит SHA-256 и соберёт игру. Права администратора не нужны.
   Перевод кода занимает 10–20 минут, компиляция — от 20 до 90 минут, один раз.
4. Готовая игра: `build-ttt2\TTT2-port`. Запуск — `run_TTT2.bat`. Папку `TTT2-port` можно перенести
   куда угодно. Сохранения и настройки лежат в ней же.

Если что-то пошло не так, запустите `build-windows.bat` ещё раз: готовые шаги повторно не выполняются.
Полностью заново: `python tools\ttt2\build_ttt2.py --game "ПАПКА_ИГРЫ" --clean`.

### Управление

- Режим Pro Controller: экран геймпада не нужен.
- **F1** — меню настроек, вкладка **Controls**: переназначение клавиш и кнопок геймпада.
- Клавиатура по умолчанию:

| Действие | Клавиша |
|---|---|
| Движение (крестовина) | WASD или стрелки |
| Левая рука / правая рука | U / I |
| Левая нога / правая нога | J / K (или пробел) |
| Смена персонажа (Tag) | O |
| L / ZL / ZR | Y / H / P |
| Start (+) / Select (−) | Enter / Tab |

- Геймпады (Xbox, PlayStation, Switch Pro и др.) работают сразу через SDL.

### Состояние

Играбельно: бои, меню и раскраска костюмов работают, 60 к/с на RTX 3050 Laptop. При первом
появлении новых персонажей и арен возможны короткие подтормаживания, пока собираются шейдеры.

Сообщения об ошибках — в Issues. Приложите `TTT2-port\captures\wwhd.log`.

---

## English

### Legal notice

An unofficial fan project, not affiliated with Bandai Namco or Nintendo. "Tekken" and "Wii U" are
trademarks of their owners, used only to describe compatibility.

**This repository contains no game code, no game assets and no keys.** You need your own, legally
obtained copy of the game dumped from your own console. Everything built from it (`build-ttt2/`)
stays on your machine and must not be redistributed. That is why there is no prebuilt `.exe`: the
game is built locally from your copy.

### Requirements

- Windows 10/11 x64, a GPU with Vulkan 1.3 (or 1.1/1.2 with `VK_KHR_dynamic_rendering`)
- about 15 GB of free disk space, 8 GB of RAM, internet access for the first build
- the extracted game **Tekken Tag Tournament 2 Wii U Edition [AKNPAF]** (Europe, no update): the
  folder with `code`, `content`, `meta`. `code/Tekken.rpx` must have SHA-256
  `c3afc72f30aa1af84901bfc0775aeae7a359d396923a387cfbe71034175886ab`.

### Build (Windows)

1. **Code → Download ZIP** and unzip it (a short path without spaces is best, e.g. `C:\TTT2Recomp`), or `git clone`.
2. Run `tools\ttt2\build-windows.bat` and drag the game folder into the window.
3. It downloads pinned tools into `.tools` (llvm-mingw, CMake, Ninja, and Python if none is
   installed), checks their SHA-256 and builds. No administrator rights are needed. Translation
   takes 10–20 minutes and compiling 20–90 minutes, once.
4. The game is in `build-ttt2\TTT2-port`. Start `run_TTT2.bat`. The folder is portable.

Linux (experimental): install the Linux build dependencies listed in [README.WWHD.md](README.WWHD.md),
then `python3 tools/ttt2/build_ttt2.py --game /path/to/game`.

### Controls

Pro Controller mode, so no GamePad screen. **F1** opens the settings, where the **Controls** tab
remaps keys and gamepad buttons. Default keys: WASD or the arrow keys move; U and I are the
left and right punch; J and K (or Space) are the left and right kick; O tags; Enter is Start.
SDL gamepads work out of the box.

### Status

Playable: fights, menus and costume colours work, 60 fps on an RTX 3050 Laptop GPU. Expect short
hitches the first time new characters and stages appear, while their shaders are built.

### How it was made

`tools/ttt2/recompile.py` runs the Wind Waker HD port's recompiler (`tools/recomp`) on
`Tekken.rpx` with an identity address map and no hooks. TTT2-specific runtime work includes
GX2 stream-out (GPU skinning), GX2R buffers and display lists, LINEAR_SPECIAL surface copies,
primitive restart and Pro Controller input. It also adopts BC1/BC3 blocks that the game encodes
on the GPU into its costume textures (`adopt_rendered_blocks` in `runtime/src/gfx/vulkan/surfaces.cpp`).
The shader decompiler carries upstream Cemu's `ALU_BREAK` fix, which those encoders need.

### Credits and license

- Runtime, recompiler and tools: [ZeldaWWHDRecomp](https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp)
  (MPL-2.0). Its original README is [README.WWHD.md](README.WWHD.md).
- GPU address library and shader decompiler: [Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0).
- SDL3, glslang, zstd, lz4, zlib and the other third-party code listed in README.WWHD.md, under their own licenses.

This project is distributed under the Mozilla Public License 2.0 ([LICENSE](LICENSE)).
