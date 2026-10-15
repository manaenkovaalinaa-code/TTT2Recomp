# In-game button prompts that follow your controller

With **Controls → Face buttons** set to **by label** (or **Automatic** on an Xbox-style pad), the
game's own button prompts match the controller in your hands:

- **Colours:** dialog icons and the HUD buttons use **green A, red B, blue X, yellow Y**.
- **Positions:** the HUD's four-button cluster follows the Xbox layout (**A bottom, B right, X left,
  Y top**). Whole buttons move, with their item art, text and A's parry effect.
- **Shoulder buttons:** prompts read **R1 / R2** for R / ZR and **L1 / L2** for L / ZL, in the game's
  own HUD lettering.

The letters themselves never change: *by label* already makes the Xbox A button do the game's A, so
a prompt that says A still means the button printed A. **By position** and **custom** keep the game's
original look. Switching takes effect immediately, without a restart, and also applies to text the
game had already drawn. Nothing changes while you have never chosen *by label* in a session.

No Nintendo artwork or font is shipped: the colours are applied while the game draws, and the
R1/R2/L1/L2 captions are composed at runtime from your own copy's HUD font.

## How it works (technical notes)

Three function hooks (`tools/recomp/hooks_glyphs.txt`), plus wrappers around the existing aspect
(`0x028766CC`) and RTL text (`0x028785C8`) hooks:

- `0x0286E9F0` (`CharWriter::Print`) marks button glyphs (A/B/X/Y: U+E000–E003; L/R/ZL/ZR:
  U+E083–E086) in unused bits of the cached glyph quad's byte `+0x2F`.
- `0x028F7C7C` (TextBox vertex generation) tints marked quads and swaps shoulder captions to the
  composed atlas for the vertex build, then restores everything it changed.
- `0x02874D54` (`Picture::DrawSelf`) tints the HUD button backgrounds (`P_A_00`, `P_B_00`, `P_X_00`,
  `P_Y_00`, `P_ASpecial_00`) and restores their authored vertex colours afterwards.
- The pane-matrix hook moves the complete CommandGuide cluster (`N_All_00`) for the Xbox positions;
  only the computed global matrices keep the new layout, the authored locals are restored.

**Untouched until used:** none of the hooks write to game memory until *by label* has been selected
once in the session; the first activation rebuilds each cached text box once so its glyphs get marked.
A loaded save state clears that rebuild list.

**Why the marker bits are safe:** each cached glyph quad is 0x30 bytes, and its last word (`+0x2C`) is
only touched by three functions in the game code: the emitter `0x0286E4FC` writes the 16-bit value at
`+0x2C` and the page byte at `+0x2E`, stores zero to `+0x2F` and then sets only bit 0 (colour font);
batch grouping `0x028F7A4C` reads `+0x2F` masked with `& 1`; vertex generation `0x028F7C7C` reads
`+0x2C`/`+0x2E` and never `+0x2F`. Bits 1–7 of `+0x2F` are therefore free, and every new build of a
quad clears them.

**Shoulder captions:** the four characters L, R, 1, 2 are read from the player's `CKingMain` font
through the game's `Font::GetGlyph`; its BC4 (or plain alpha) sheets are decoded on the CPU and
composed into a small host-side atlas, keyed by font. If the font isn't available, the original
ZR/ZL label is kept unchanged.

The hook behaviour is covered by `runtime/tools/button_glyphs_test.cpp` (synthetic guest memory: face
and shoulder marking, colour restoration, live switches, the untouched-until-used gate, *Automatic*,
HUD positions including 21:9, font fallback). The EU build maps all three hooks. After a change to the
hook list, regenerate the game code (setup does this on update).
