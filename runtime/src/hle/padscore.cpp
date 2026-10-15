#include <algorithm>
#include "mods/mods.h"
// padscore: Wii Remote / Pro Controller (WPAD, KPAD). Only a Pro Controller on channel 0 exists,
// and only while the keyboard/host controllers are set to act as one (Input menu). Struct layouts
// and constants follow Cemu's padscore.
#include "../crashrec.h"
#include "../runtime.h"
#include "../input.h"
#include "../motion/motion.h"
#include "../rumble.h"

namespace interp { bool repeat_input(); bool fresh_sticks(); uint64_t logic_steps(); }

namespace {
constexpr int32_t kWpadErrNone = 0, kWpadErrNoController = -1;
constexpr int32_t kKpadErrNone = 0, kKpadErrNoController = -2;
constexpr uint8_t kDevURCC = 31, kDevNone = 253, kFormatURCC = 22;

bool connected(uint32_t chan) { return chan == 0 && input::pro_controller(); }

// VPAD button bits (input::PadState) -> Pro Controller (URCC) bits
uint32_t pro_buttons(uint32_t v) {
    static const uint32_t map[][2] = {
        {input::kUp, 0x1},     {input::kLeft, 0x2},    {input::kZR, 0x4},     {input::kX, 0x8},
        {input::kA, 0x10},     {input::kY, 0x20},      {input::kB, 0x40},     {input::kZL, 0x80},
        {input::kR, 0x200},    {input::kPlus, 0x400},  {input::kHome, 0x800}, {input::kMinus, 0x1000},
        {input::kL, 0x2000},   {input::kDown, 0x4000}, {input::kRight, 0x8000},
        {input::kStickR, 0x10000}, {input::kStickL, 0x20000},
    };
    uint32_t out = 0;
    for (auto& m : map)
        if (v & m[0]) out |= m[1];
    return out;
}
}  // namespace

HLE(padscore, KPADInitEx) {}
HLE(padscore, KPADGetMplsWorkSize) { ret(c, 0x5FE0); }
HLE(padscore, KPADSetMplsWorkarea) {}
HLE(padscore, WPADEnableURCC) {}
HLE(padscore, WPADEnableWiiRemote) {}
HLE(padscore, WPADDisconnect) {}
// The Pro Controller motor: cmd 0 stops it, 1 runs it until the game stops it. The Pro
// Controller is only connected while the keyboard/controllers act as one (see connected).
HLE(padscore, WPADControlMotor) {
    uint32_t chan = arg(c, 0), cmd = arg(c, 1);
    if (connected(chan)) {
        TRACE("[pad] WPADControlMotor(%u, %u) -> %s", chan, cmd, cmd ? "rumble" : "stop");
        rumble::pro_motor(chan, cmd != 0);
    }
    ret(c, (uint32_t)kWpadErrNone);
}
HLE(padscore, WPADGetBatteryLevel) { ret(c, 4); }  // full
HLE(padscore, WPADCanSendStreamData) { ret(c, 0); }
HLE(padscore, WPADSendStreamData) { ret(c, (uint32_t)kWpadErrNoController); }
HLE(padscore, WPADControlSpeaker) { ret(c, (uint32_t)kWpadErrNoController); }

HLE(padscore, WPADProbe) {
    // (chan, uint32* type) -> WPAD error
    uint32_t chan = arg(c, 0), type = arg(c, 1);
    bool on = connected(chan);
    TRACE("[pad] WPADProbe(%u) -> %s", chan, on ? "pro" : "none");
    if (type) st32(type, on ? kDevURCC : kDevNone);
    ret(c, (uint32_t)(on ? kWpadErrNone : kWpadErrNoController));
}

HLE(padscore, KPADReadEx) {
    // (chan, KPADStatus* buf, count, int32* err) -> samples written
    uint32_t chan = arg(c, 0), st = arg(c, 1), count = arg(c, 2), err = arg(c, 3);
    TRACE("[pad] KPADReadEx(%u, %08X, %u) connected=%d", chan, st, count, connected(chan));
    if (!connected(chan) || !st || (int32_t)count <= 0) {
        if (err) st32(err, (uint32_t)(connected(chan) ? kKpadErrNone : kKpadErrNoController));
        ret(c, 0);
        return;
    }
    static uint32_t last = 0;
    static input::PadState last_p;
    const bool repeat = interp::repeat_input();
    input::PadState p = repeat ? last_p : crashrec::read(1);  // see interp.cpp; crash recovery records/replays it
    if (repeat && interp::fresh_sticks()) {  // true 60: sticks every pass, buttons on full passes
        input::PadState f = input::read();
        p.lx = f.lx; p.ly = f.ly; p.rx = f.rx; p.ry = f.ry;
    }
    last_p = p;
    mods::move_speed_input(p.buttons);
    mods::fast_forward_input(p.buttons);
    if (!repeat) motion::right_stick(p.rx, p.ry);  // the Pro Controller's stick decides gyro use too (motion.h)
    uint32_t hold = pro_buttons(p.buttons);
    // stick-as-button bits (WPAD_PRO_STICK_*_EMULATION, wut), with hysteresis: TTT2 moves the fighter
    // with these, not with the stick values
    auto stick_dirs = [&](float x, float y, uint32_t up, uint32_t down, uint32_t left, uint32_t right) {
        auto dir = [&](float v, bool held, uint32_t bit) { if (v >= 0.5f || (held && v >= 0.25f)) hold |= bit; };
        dir(-x, last & left, left); if (!(hold & left)) dir(x, last & right, right);
        dir(-y, last & down, down); if (!(hold & down)) dir(y, last & up, up);
    };
    stick_dirs(p.lx, p.ly, 0x00200000, 0x00100000, 0x00040000, 0x00080000);
    stick_dirs(p.rx, p.ry, 0x02000000, 0x01000000, 0x00400000, 0x00800000);
    memset(mem::ptr(st), 0, 0xF0);
    st8(st + 0x5C, kDevURCC);     // devType
    st8(st + 0x5D, 0);            // wpadErr
    st8(st + 0x5F, kFormatURCC);  // data_format
    st32(st + 0x60, hold);                // ex_status.uc.hold
    st32(st + 0x64, hold & ~last);        // trig
    st32(st + 0x68, last & ~hold);        // release
    last = hold;
    stf32(st + 0x6C, p.lx); stf32(st + 0x70, p.ly);  // lstick
    stf32(st + 0x74, p.rx); stf32(st + 0x78, p.ry);  // rstick
    st32(st + 0x7C, 1);                   // charge
    st32(st + 0x80, 1);                   // cable
    // Optional read-by-read diagnostics, including the data actually written to guest memory.
    static FILE* trace = getenv("WWHD_KPAD_TRACE") ? fopen(getenv("WWHD_KPAD_TRACE"), "w") : nullptr;
    if (trace) {
        fprintf(trace, "%llu %d %08X %08X %08X %.6f %.6f %.6f %.6f\n",
                (unsigned long long)interp::logic_steps(), (int)repeat, hold,
                ld32(st + 0x64), ld32(st + 0x68), ldf32(st + 0x6C), ldf32(st + 0x70),
                ldf32(st + 0x74), ldf32(st + 0x78));
        fflush(trace);
    }
    if (err) st32(err, kKpadErrNone);
    ret(c, 1);
}

// ---- TTT2: the rest of the KPAD/WPAD setup calls it makes
HLE(padscore, KPADInit) {}
HLE(padscore, KPADShutdown) {}
HLE(padscore, KPADEnableDPD) {}
HLE(padscore, KPADDisableDPD) {}
HLE(padscore, KPADSetConnectCallback) { ret(c, 0); }  // previous callback: none
HLE(padscore, WPADRegisterAllocator) {}
HLE(padscore, WPADSetDataFormat) { ret(c, (uint32_t)(connected(arg(c, 0)) ? kWpadErrNone : kWpadErrNoController)); }
HLE(padscore, WPADClampStick) {}
HLE(padscore, WPADClampTrigger) {}
HLE(padscore, WPADControlDpd) { ret(c, (uint32_t)kWpadErrNone); }
HLE(padscore, WPADIsMotorEnabled) { ret(c, 1); }
// WPADRead(chan, WPADStatus*): the raw status in the channel's data format. For the Pro Controller
// (WPADStatusProController, wut; Cemu's WPADController::WPADRead): extension type at +0x28, error at
// +0x29, buttons at +0x2C, sticks as int16 [-2048, 2047] at +0x30/+0x34. TTT2 moves the fighter with it.
static void wpad_fill(uint32_t chan, uint32_t st) {
    if (!connected(chan)) {
        memset(mem::ptr(st), 0, 0x2A);
        st8(st + 0x29, (uint8_t)(int8_t)kWpadErrNoController);
        return;
    }
    input::PadState p = input::read();
    uint32_t hold = pro_buttons(p.buttons);
    auto bit = [&](float v, uint32_t b) { if (v >= 0.5f) hold |= b; };
    bit(p.ly, 0x00200000); bit(-p.ly, 0x00100000); bit(-p.lx, 0x00040000); bit(p.lx, 0x00080000);
    bit(p.ry, 0x02000000); bit(-p.ry, 0x01000000); bit(-p.rx, 0x00400000); bit(p.rx, 0x00800000);
    auto axis = [](float v) { return (uint16_t)(int16_t)std::max(-2048.0f, std::min(2047.0f, v * 2048.0f)); };
    memset(mem::ptr(st), 0, 0x40);
    st8(st + 0x28, kDevURCC);
    st8(st + 0x29, 0);
    st32(st + 0x2C, hold);
    st16(st + 0x30, axis(p.lx)); st16(st + 0x32, axis(p.ly));
    st16(st + 0x34, axis(p.rx)); st16(st + 0x36, axis(p.ry));
    st32(st + 0x38, 1);  // charging
    st32(st + 0x3C, 1);  // wired
}
HLE(padscore, WPADRead) { if (arg(c, 1)) wpad_fill(arg(c, 0), arg(c, 1)); }
// auto sampling: the game reads the latest status from its ring buffer (one slot is enough here)
static uint32_t g_sampling_buf[4], g_sampling_len[4];
HLE(padscore, WPADSetAutoSamplingBuf) {
    uint32_t chan = arg(c, 0);
    if (chan < 4) { g_sampling_buf[chan] = arg(c, 1); g_sampling_len[chan] = arg(c, 2); }
}
HLE(padscore, WPADGetLatestIndexInBuf) {
    uint32_t chan = arg(c, 0);
    if (chan < 4 && g_sampling_buf[chan] && g_sampling_len[chan]) wpad_fill(chan, g_sampling_buf[chan]);
    ret(c, 0);
}
