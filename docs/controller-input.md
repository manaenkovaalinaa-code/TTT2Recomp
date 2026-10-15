# Controller directions and Wind Waker controls

The physical controller layout and the emulated Wii U controller are separate
choices. Controls selects whether your inputs act as a Wii U GamePad or Pro
Controller. Select that controller in the game's Controller Selection screen too.
The face-button preset changes A/B/X/Y; it does not change D-pad or stick directions.

**Stick dead zone** defaults to **15%** for both controller sticks. This filters
small resting-stick drift, which can otherwise keep the Picto Box zooming and its
picture blurred. Older controls files with the former 0% default automatically
load as 15%; other saved values are kept. Saving controls writes an
`options.format` marker of `2`, so a deliberate 0% setting saved in the new format
is preserved on subsequent loads.

**Stick range** is set per stick (**Left stick range**, **Right stick range**),
from 100% to 150%; the default 100% changes nothing. Raise it if a stick can't
reach full deflection, for example when the Wind Waker baton's directions don't
register because the game needs an almost fully tilted stick. The range applies
after the dead zone: the stick's tilt is multiplied by the range and capped at
full deflection, keeping its direction, so diagonals keep their angle. A stick
that only reaches 90% needs about 112% to reach full deflection. Both sliders
sit below the dead zone in the Controls tab and in the macOS Controls window, take
effect immediately and are saved as `left_stick_range` and `right_stick_range`
in the controls file's `options` (stored as factors, 1.0 to 1.5). Files without
these keys load at 100%; values outside the range are clamped.

While conducting, the **right stick supplies the notes**, including down. The
**left stick controls the meter**: hold left or right to change the beat count.
The D-pad does not substitute for the note stick in the original game. For example,
the Song of Passing uses right, left, down on the right stick in three-beat meter.

D-pad up/down can move the Quest Log cursor in File Selection in both controller
modes. D-pad directions also navigate the pause-menu sidebar and item grid in
both modes. In GamePad mode, the inventory appears on the GamePad screen. Other
screens have their own controls; follow the game's stick/button prompts. The port does not turn D-pad presses into stick movement automatically.
If down fails, check its binding in Controls and test D-pad down, Left Stick Down
and Right Stick Down separately.

## Input diagnostics

For a reproducible controller report, these optional environment variables log
the packets written for the game:

- `WWHD_VPAD_TRACE=path`: GamePad reads.
- `WWHD_KPAD_TRACE=path`: Pro Controller reads on channel 0.

Each line contains `logic_step repeat hold trigger release lx ly rx ry`.
Button fields are hexadecimal; sticks are signed decimal values, with positive Y
pointing up. `repeat=1` identifies an interpolation/repeated input sample. The
existing four-column `WWHD_PAD_TRACE` format is unchanged.

D-pad down is `00000100` in VPAD and `00004000` in the Pro Controller's extended
button fields. In Pro Controller mode, the GamePad remains connected for its
screen and touch, but its buttons and sticks are idle. Read the KPAD trace for
controller input in that mode.

The game-free `sdl_input` test drives virtual Xbox and Switch Pro gamepads through
SDL polling and the configured bindings. `pad_delivery` checks the actual VPAD
and KPAD writers, including direction bits, press/release edges, both sticks,
interpolation repeats and fresh analog samples. Neither test reads game data or a save, or uses a physical controller. These
checks do not replace testing a particular physical device and its driver.
