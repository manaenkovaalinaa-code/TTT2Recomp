// GX2 calls that Tekken Tag Tournament 2 makes and Wind Waker HD does not: the Init*Reg/Set*Reg
// pairs (state packed into a struct first, applied later), GX2R resource buffers and surfaces,
// and a few queries. Register packing follows the GX2 SDK as documented by Cemu.
#include "gx2.h"
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "gx2_surface_calc.h"
#include "runtime.h"
#include <mutex>

using namespace Latte;
using gx2::set_reg;
using gx2::emit;

extern "C" void imp_gx2_GX2BeginDisplayListEx(Cpu*);
extern "C" void imp_gx2_GX2EndDisplayList(Cpu*);
extern "C" void imp_gx2_GX2CopySurface(Cpu*);
uint32_t mem_default_alloc(uint32_t size, int32_t align);  // coreinit_mem.cpp

namespace {
template <typename T>
T* gp(uint32_t addr) { return (T*)mem::ptr(addr); }
float fa(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }  // i-th float argument
uint32_t stack_arg(Cpu* c, int i) { return ld32(c->r[1] + 8 + 4 * i); }  // i-th argument past r10

uint32_t alpha_test(uint32_t enable, uint32_t func) {
    LATTE_SX_ALPHA_TEST_CONTROL ctrl;
    ctrl.set_ALPHA_FUNC((LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC)func);
    ctrl.set_ALPHA_TEST_ENABLE(enable != 0);
    return ctrl.getRawValue();
}
uint32_t alpha_to_mask(uint32_t enable, uint32_t mode) {
    static const uint32_t offsets[] = {0xAA, 0x78, 0xB4, 0x1E, 0x00};
    return (enable & 1) | ((mode < 5 ? offsets[mode] : 0) << 8);
}
uint32_t blend(const uint32_t* a) {  // colorSrc, colorDst, colorComb, separateAlpha, alphaSrc, alphaDst, alphaComb
    LATTE_CB_BLENDN_CONTROL r;
    using BF = LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR;
    using CF = LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC;
    r.set_COLOR_SRCBLEND((BF)a[0]).set_COLOR_DSTBLEND((BF)a[1]).set_COLOR_COMB_FCN((CF)a[2]);
    r.set_SEPARATE_ALPHA_BLEND(a[3] != 0);
    r.set_ALPHA_SRCBLEND((BF)a[4]).set_ALPHA_DSTBLEND((BF)a[5]).set_ALPHA_COMB_FCN((CF)a[6]);
    return r.getRawValue();
}
uint32_t color_control(uint32_t rop, uint32_t blend_mask, uint32_t multiwrite, uint32_t enable) {
    LATTE_CB_COLOR_CONTROL r;
    r.set_MULTIWRITE_ENABLE(multiwrite != 0);
    r.set_SPECIAL_OP(enable == 0 ? LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE : LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL);
    r.set_BLEND_MASK(blend_mask);
    r.set_ROP((LATTE_CB_COLOR_CONTROL::E_LOGICOP)rop);
    return r.getRawValue();
}
uint32_t depth_stencil(const uint32_t* a) {  // the 13 arguments of GX2SetDepthStencilControl
    using SF = LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC;
    using SA = LATTE_DB_DEPTH_CONTROL::E_STENCILACTION;
    LATTE_DB_DEPTH_CONTROL r;
    r.set_Z_ENABLE(a[0] != 0).set_Z_WRITE_ENABLE(a[1] != 0).set_Z_FUNC((LATTE_DB_DEPTH_CONTROL::E_ZFUNC)a[2]);
    r.set_STENCIL_ENABLE(a[3] != 0).set_BACK_STENCIL_ENABLE(a[4] != 0);
    r.set_STENCIL_FUNC_F((SF)a[5]).set_STENCIL_ZPASS_F((SA)a[6]).set_STENCIL_ZFAIL_F((SA)a[7]).set_STENCIL_FAIL_F((SA)a[8]);
    r.set_STENCIL_FUNC_B((SF)a[9]).set_STENCIL_ZPASS_B((SA)a[10]).set_STENCIL_ZFAIL_B((SA)a[11]).set_STENCIL_FAIL_B((SA)a[12]);
    return r.getRawValue();
}
uint32_t polygon(const uint32_t* a) {  // the 9 arguments of GX2SetPolygonControl
    LATTE_PA_SU_SC_MODE_CNTL v;
    v.set_FRONT_FACE((LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE)a[0]);
    v.set_CULL_FRONT((a[1] & 1) != 0).set_CULL_BACK((a[2] & 1) != 0);
    v.set_POLYGON_MODE((LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE)a[3]);
    v.set_FRONT_POLY_MODE((LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE)a[4]).set_BACK_POLY_MODE((LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE)a[5]);
    v.set_OFFSET_FRONT_ENABLED((a[6] & 1) != 0).set_OFFSET_BACK_ENABLED((a[7] & 1) != 0).set_OFFSET_PARA_ENABLED((a[8] & 1) != 0);
    return v.getRawValue();
}
void scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t out[2]) {
    uint32_t tlx = std::min(x, 8192u), tly = std::min(y, 8192u), brx = std::min(x + w, 8192u), bry = std::min(y + h, 8192u);
    LATTE_PA_SC_GENERIC_SCISSOR_TL tl;
    tl.set_TL_X(tlx).set_TL_Y(tly).set_WINDOW_OFFSET_DISABLE(true);
    LATTE_PA_SC_GENERIC_SCISSOR_BR br;
    br.set_BR_X(brx).set_BR_Y(bry);
    out[0] = tl.getRawValue();
    out[1] = br.getRawValue();
}
}  // namespace

// ---------------------------------------------------------------- Init*Reg / Set*Reg
// Struct layouts: the same words the matching Set*Reg (gx2_state.cpp, or below) reads back.
HLE(gx2, GX2InitAlphaTestReg) {  // (reg, enable, func, float ref) -> {SX_ALPHA_TEST_CONTROL, SX_ALPHA_REF}
    st32(arg(c, 0), alpha_test(arg(c, 1), arg(c, 2)));
    st32(arg(c, 0) + 4, gx2::fbits(fa(c, 0)));
}
HLE(gx2, GX2InitAlphaToMaskReg) { st32(arg(c, 0), alpha_to_mask(arg(c, 1), arg(c, 2))); }
HLE(gx2, GX2SetAlphaToMaskReg) { set_reg(mmDB_ALPHA_TO_MASK, ld32(arg(c, 0))); }
HLE(gx2, GX2InitBlendControlReg) {  // (reg, target, 7 blend arguments; the last one on the stack) -> {target, CB_BLENDn}
    uint32_t a[7] = {arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), stack_arg(c, 0)};
    st32(arg(c, 0), arg(c, 1));
    st32(arg(c, 0) + 4, blend(a));
}
HLE(gx2, GX2InitColorControlReg) { st32(arg(c, 0), color_control(arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4))); }
HLE(gx2, GX2InitDepthStencilControlReg) {  // (reg, 13 arguments: 7 in registers, 6 on the stack) -> {DB_DEPTH_CONTROL}
    uint32_t a[13];
    for (int i = 0; i < 7; i++) a[i] = arg(c, 1 + i);
    for (int i = 7; i < 13; i++) a[i] = stack_arg(c, i - 7);
    st32(arg(c, 0), depth_stencil(a));
}
HLE(gx2, GX2InitStencilMaskReg) {  // (reg, maskF, writeMaskF, refF, maskB, writeMaskB, refB) -> {DB_STENCILREFMASK, _BF}
    LATTE_DB_STENCILREFMASK f;
    f.set_STENCILREF_F(arg(c, 3) & 0xFF).set_STENCILMASK_F(arg(c, 1) & 0xFF).set_STENCILWRITEMASK_F(arg(c, 2) & 0xFF);
    LATTE_DB_STENCILREFMASK_BF b;
    b.set_STENCILREF_B(arg(c, 6) & 0xFF).set_STENCILMASK_B(arg(c, 4) & 0xFF).set_STENCILWRITEMASK_B(arg(c, 5) & 0xFF);
    st32(arg(c, 0), f.getRawValue());
    st32(arg(c, 0) + 4, b.getRawValue());
}
HLE(gx2, GX2SetStencilMaskReg) {
    set_reg(REGADDR::DB_STENCILREFMASK, ld32(arg(c, 0)));
    set_reg(REGADDR::DB_STENCILREFMASK_BF, ld32(arg(c, 0) + 4));
}
HLE(gx2, GX2InitPolygonControlReg) {  // (reg, 9 arguments: 7 in registers, 2 on the stack) -> {PA_SU_SC_MODE_CNTL}
    uint32_t a[9];
    for (int i = 0; i < 7; i++) a[i] = arg(c, 1 + i);
    a[7] = stack_arg(c, 0);
    a[8] = stack_arg(c, 1);
    st32(arg(c, 0), polygon(a));
}
// (reg, float frontOffset, frontScale, backOffset, backScale, clamp) -> {FRONT_SCALE, FRONT_OFFSET, BACK_SCALE,
// BACK_OFFSET, CLAMP}; the scales are in 1/16 units
HLE(gx2, GX2InitPolygonOffsetReg) {
    uint32_t r = arg(c, 0);
    st32(r + 0, gx2::fbits(fa(c, 1) * 16.0f));
    st32(r + 4, gx2::fbits(fa(c, 0)));
    st32(r + 8, gx2::fbits(fa(c, 3) * 16.0f));
    st32(r + 12, gx2::fbits(fa(c, 2)));
    st32(r + 16, gx2::fbits(fa(c, 4)));
}
HLE(gx2, GX2InitScissorReg) {  // (reg, x, y, w, h) -> {TL, BR}
    uint32_t v[2];
    scissor(arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), v);
    st32(arg(c, 0), v[0]);
    st32(arg(c, 0) + 4, v[1]);
}
HLE(gx2, GX2SetScissorReg) {
    set_reg(REGADDR::PA_SC_GENERIC_SCISSOR_TL, ld32(arg(c, 0)));
    set_reg(REGADDR::PA_SC_GENERIC_SCISSOR_BR, ld32(arg(c, 0) + 4));
}
HLE(gx2, GX2InitViewportReg) {  // (reg, float x, y, w, h, near, far) -> the six PA_CL_VPORT values
    float x = fa(c, 0), y = fa(c, 1), w = fa(c, 2), h = fa(c, 3), n = fa(c, 4), f = fa(c, 5);
    uint32_t r = arg(c, 0);
    st32(r + 0, gx2::fbits(w * 0.5f));
    st32(r + 4, gx2::fbits(x + w * 0.5f));
    st32(r + 8, gx2::fbits(h * -0.5f));
    st32(r + 12, gx2::fbits(y + h * 0.5f));
    st32(r + 16, gx2::fbits((f - n) * 0.5f));
    st32(r + 20, gx2::fbits((n + f) * 0.5f));
}
HLE(gx2, GX2SetViewportReg) {
    uint32_t r = arg(c, 0);
    set_reg(REGADDR::PA_CL_VPORT_XSCALE, ld32(r + 0));
    set_reg(REGADDR::PA_CL_VPORT_XOFFSET, ld32(r + 4));
    set_reg(REGADDR::PA_CL_VPORT_YSCALE, ld32(r + 8));
    set_reg(REGADDR::PA_CL_VPORT_YOFFSET, ld32(r + 12));
    set_reg(REGADDR::PA_CL_VPORT_ZSCALE, ld32(r + 16));
    set_reg(REGADDR::PA_CL_VPORT_ZOFFSET, ld32(r + 20));
}

HLE(gx2, GX2SetPrimitiveRestartIndex) { set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, arg(c, 0)); }

// GX2SetDefaultState: the fixed-function defaults of GX2 (no blending, depth test LEQUAL with writes,
// no culling, all channels of target 0)
HLE(gx2, GX2SetDefaultState) {
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, alpha_test(0, 7));
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    set_reg(mmDB_ALPHA_TO_MASK, alpha_to_mask(0, 0));
    set_reg(REGADDR::CB_COLOR_CONTROL, color_control(0xCC, 0, 0, 1));
    const uint32_t b[7] = {1, 0, 0, 0, 1, 0, 0};  // ONE, ZERO, ADD
    for (int i = 0; i < 8; i++) set_reg(REGADDR::CB_BLEND0_CONTROL + i, blend(b));
    set_reg(REGADDR::CB_TARGET_MASK, 0xF);
    const uint32_t d[13] = {1, 1, 3, 0, 0, 7, 0, 0, 0, 7, 0, 0, 0};
    set_reg(REGADDR::DB_DEPTH_CONTROL, depth_stencil(d));
    const uint32_t p[9] = {0, 0, 0, 0, 2, 2, 0, 0, 0};
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, polygon(p));
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
}

// ---------------------------------------------------------------- queries
HLE(gx2, GX2GetSwapInterval) { extern uint32_t gx2_swap_interval(); ret(c, gx2_swap_interval()); }
HLE(gx2, GX2GetGPUTimeout) { ret(c, 10000); }
HLE(gx2, GX2GetLastSubmittedTimeStamp) { ret64(c, timebase::guest_now()); }
HLE(gx2, GX2WaitTimeStamp) { ret(c, 1); }
HLE(gx2, GX2PrintGPUStatus) {}
HLE(gx2, GX2Shutdown) {}
HLE(gx2, GX2SurfaceIsCompressed) {
    uint32_t f = arg(c, 0) & 0x3F;
    ret(c, f >= 0x31 && f <= 0x35);
}
HLE(gx2, GX2GetSurfaceFormatBits) { ret(c, Latte::GetFormatBits((Latte::E_GX2SURFFMT)arg(c, 0))); }
HLE(gx2, GX2GetSurfaceMipPitch) {
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    gx2calc::CalculateSurfaceInfo(gp<GX2Surface>(arg(c, 0)), arg(c, 1), &info);
    ret(c, info.pitch);
}

// copies the renderer can do as a surface copy: a resolve (the first sample) and a depth buffer
// read as a texture. GX2ColorBuffer and GX2DepthBuffer start with their GX2Surface.
static void copy_view_to_surface(Cpu* c) {  // (GX2ColorBuffer*/GX2DepthBuffer*, GX2Surface* dst, mip, slice)
    uint32_t buf = arg(c, 0);
    Cpu tmp = *c;
    tmp.r[3] = buf;                   // the buffer's GX2Surface
    tmp.r[4] = ld32(buf + 0x74);      // viewMip
    tmp.r[5] = ld32(buf + 0x78);      // viewFirstSlice
    tmp.r[6] = arg(c, 1);
    tmp.r[7] = arg(c, 2);
    tmp.r[8] = arg(c, 3);
    imp_gx2_GX2CopySurface(&tmp);
}
HLE(gx2, GX2ResolveAAColorBuffer) { copy_view_to_surface(c); }
HLE(gx2, GX2ConvertDepthBufferToTextureSurface) { copy_view_to_surface(c); }

// stream out (transform feedback), as Cemu's GX2_Streamout.cpp. GX2StreamOutBuffer: +0 size,
// +4 data, +8 vertex stride, +0xC GX2RBuffer (when data is null), +0x1C context. The buffer base
// register holds the whole guest address here (the renderer is ours; Latte keeps physical >> 8).
HLE(gx2, GX2SetStreamOutBuffer) {
    uint32_t index = arg(c, 0), buf = arg(c, 1);
    if (index >= 4 || !buf) return;
    uint32_t addr = ld32(buf + 4), size = ld32(buf + 0);
    if (!addr) { addr = ld32(buf + 0xC + 0xC); size = ld32(buf + 0xC + 4) * ld32(buf + 0xC + 8); }
    set_reg(mmVGT_STRMOUT_BUFFER_SIZE_0 + index * 4, size >> 2);
    set_reg(mmVGT_STRMOUT_BUFFER_BASE_0 + index * 4, addr);
    static int logged = 0;
    if (logged < 8) { ++logged; LOG("[gx2] stream out buffer %u: %08X size %X", index, addr, size); }
}
HLE(gx2, GX2SetStreamOutEnable) { set_reg(mmVGT_STRMOUT_EN, arg(c, 0) & 1); }
// reset (mode 1: offset 0) or restore the saved offset (mode 0); executed in command order
HLE(gx2, GX2SetStreamOutContext) {
    uint32_t index = arg(c, 0), buf = arg(c, 1), reset = arg(c, 2);
    if (index >= 4 || !buf) return;
    emit(gx2::OP_STREAMOUT_CONTEXT, {index, ld32(buf + 0x1C), reset ? 0u : 1u});
}
HLE(gx2, GX2SaveStreamOutContext) {
    uint32_t index = arg(c, 0), buf = arg(c, 1);
    if (index >= 4 || !buf) return;
    emit(gx2::OP_STREAMOUT_CONTEXT, {index, ld32(buf + 0x1C), 2u});
}

// ---------------------------------------------------------------- GX2R: resources with an allocator
// GX2RBuffer: +0 flags, +4 element size, +8 element count, +0xC buffer
namespace {
std::mutex g_gx2r_m;
uint32_t g_gx2r_alloc = 0, g_gx2r_free = 0;
constexpr uint32_t kGx2rLocked = 1u << 31;  // internal: the resource is locked (GX2R keeps flags in the top bits)
constexpr uint32_t kGx2rUserMemory = 1u << 29;

uint32_t gx2r_alloc(uint32_t flags, uint32_t size, uint32_t align) {
    uint32_t a = 0, f = 0;
    {
        std::lock_guard<std::mutex> lk(g_gx2r_m);
        a = g_gx2r_alloc;
        f = g_gx2r_free;
    }
    (void)f;
    if (a) {
        Cpu* c = threads::current();
        if (c) return guest_call(c, a, {flags, size, align});
    }
    return mem_default_alloc(size, (int32_t)align);
}
void gx2r_free(uint32_t flags, uint32_t ptr) {
    uint32_t f;
    {
        std::lock_guard<std::mutex> lk(g_gx2r_m);
        f = g_gx2r_free;
    }
    Cpu* c = threads::current();
    if (f && c && ptr) guest_call(c, f, {flags, ptr});
}
uint32_t buffer_size(uint32_t b) { return ld32(b + 4) * ld32(b + 8); }
// GX2RResourceFlags bind bits -> GX2InvalidateMode (ATTRIBUTE 0x1, TEXTURE 0x2, UNIFORM 0x4, SHADER 0x8,
// COLOR 0x10, DEPTH 0x20, CPU 0x40, STREAMOUT 0x80); the CPU wrote the data, so CPU is always part of it
uint32_t invalidate_mode(uint32_t flags) {
    uint32_t m = 0x40;
    if (flags & 0x1) m |= 0x02;               // texture
    if (flags & (0x2 | 0x8)) m |= 0x10;       // color / scan buffer
    if (flags & 0x4) m |= 0x20;               // depth buffer
    if (flags & (0x10 | 0x20)) m |= 0x01;     // vertex / index buffer
    if (flags & 0x40) m |= 0x04;              // uniform block
    if (flags & 0x80) m |= 0x08;              // shader program
    if (flags & 0x100) m |= 0x80;             // stream out
    return m;
}
void invalidate(uint32_t flags, uint32_t ptr, uint32_t size) {
    if (ptr && size) emit(gx2::OP_INVALIDATE, {invalidate_mode(flags), ptr, size});
}
}  // namespace

HLE(gx2, GX2RSetAllocator) {
    std::lock_guard<std::mutex> lk(g_gx2r_m);
    g_gx2r_alloc = arg(c, 0);
    g_gx2r_free = arg(c, 1);
}
HLE(gx2, GX2RGetBufferAlignment) { ret(c, 0x100); }
HLE(gx2, GX2RGetBufferAllocationSize) { ret(c, (buffer_size(arg(c, 0)) + 0xFF) & ~0xFFu); }
HLE(gx2, GX2RCreateBufferUserMemory) {  // (GX2RBuffer*, memory, size)
    uint32_t b = arg(c, 0);
    st32(b + 0xC, arg(c, 1));
    st32(b, (ld32(b) & ~kGx2rLocked) | kGx2rUserMemory);
    ret(c, 1);
}
HLE(gx2, GX2RDestroyBufferEx) {
    uint32_t b = arg(c, 0);
    if (!(ld32(b) & kGx2rUserMemory)) gx2r_free(ld32(b) | arg(c, 1), ld32(b + 0xC));
    st32(b + 0xC, 0);
}
HLE(gx2, GX2RLockBufferEx) {
    uint32_t b = arg(c, 0);
    st32(b, ld32(b) | kGx2rLocked);
    ret(c, ld32(b + 0xC));
}
HLE(gx2, GX2RUnlockBufferEx) {
    uint32_t b = arg(c, 0);
    st32(b, ld32(b) & ~kGx2rLocked);
    invalidate(ld32(b) | arg(c, 1), ld32(b + 0xC), buffer_size(b));
}
HLE(gx2, GX2RInvalidateBuffer) { invalidate(ld32(arg(c, 0)) | arg(c, 1), ld32(arg(c, 0) + 0xC), buffer_size(arg(c, 0))); }
HLE(gx2, GX2RInvalidateMemory) { invalidate(arg(c, 0), arg(c, 1), arg(c, 2)); }
HLE(gx2, GX2RSetAttributeBuffer) {  // (GX2RBuffer*, index, stride, offset)
    uint32_t b = arg(c, 0), ptr = ld32(b + 0xC) + arg(c, 3), size = buffer_size(b) - arg(c, 3);
    uint32_t w[7] = {ptr, size - 1, (arg(c, 2) & 0xFFFF) << 11, 0, 0, 0, 0xC0000000};
    gx2::set_regs(mmSQ_VTX_ATTRIBUTE_BLOCK_START + arg(c, 1) * 7, w, 7);
}
// (prim, GX2RBuffer* index buffer, index type, count, first index, base vertex, instances)
HLE(gx2, GX2RDrawIndexed) {
    uint32_t b = arg(c, 1), type = arg(c, 2);
    uint32_t isize = (type == 1 || type == 9) ? 4 : 2;  // GX2_INDEX_TYPE_U32(_LE)
    emit(gx2::OP_DRAW_INDEXED, {arg(c, 0), arg(c, 3), type, ld32(b + 0xC) + arg(c, 4) * isize, arg(c, 5), arg(c, 6)});
}
HLE(gx2, GX2RBeginDisplayListEx) {  // (GX2RBuffer*, profiling, flags): record into the buffer
    uint32_t b = arg(c, 0);
    Cpu tmp = *c;
    tmp.r[3] = ld32(b + 0xC);
    tmp.r[4] = buffer_size(b);
    tmp.r[5] = arg(c, 1);
    imp_gx2_GX2BeginDisplayListEx(&tmp);
}
HLE(gx2, GX2REndDisplayList) {
    Cpu tmp = *c;
    tmp.r[3] = ld32(arg(c, 0) + 0xC);
    imp_gx2_GX2EndDisplayList(&tmp);
    ret(c, tmp.r[3]);
}
HLE(gx2, GX2RDirectCallDisplayList) { emit(gx2::OP_CALL, {ld32(arg(c, 0) + 0xC), arg(c, 1)}); }

// GX2RCreateSurface(GX2Surface*, flags): size it, then allocate image and mips with the allocator
HLE(gx2, GX2RCreateSurface) {
    uint32_t s = arg(c, 0), flags = arg(c, 1);
    st32(s + 0x1C, flags);
    gx2calc::CalcSurfaceSizeAndAlignment(gp<GX2Surface>(s));
    uint32_t size = ld32(s + 0x20), mip = ld32(s + 0x28), align = ld32(s + 0x38);
    uint32_t mem = gx2r_alloc(flags, size + mip, align ? align : 0x100);
    st32(s + 0x24, mem);
    st32(s + 0x2C, mip ? mem + size : 0);
    ret(c, mem != 0);
}
HLE(gx2, GX2RCreateSurfaceUserMemory) {  // (GX2Surface*, image, mips, flags)
    uint32_t s = arg(c, 0);
    st32(s + 0x1C, arg(c, 3) | kGx2rUserMemory);
    gx2calc::CalcSurfaceSizeAndAlignment(gp<GX2Surface>(s));
    st32(s + 0x24, arg(c, 1));
    st32(s + 0x2C, arg(c, 2));
    ret(c, 1);
}
HLE(gx2, GX2RDestroySurfaceEx) {
    uint32_t s = arg(c, 0);
    if (!(ld32(s + 0x1C) & kGx2rUserMemory)) gx2r_free(ld32(s + 0x1C) | arg(c, 1), ld32(s + 0x24));
    st32(s + 0x24, 0);
    st32(s + 0x2C, 0);
}
HLE(gx2, GX2RLockSurfaceEx) {  // (GX2Surface*, level, flags): level 0 is the image, others the mip chain
    uint32_t s = arg(c, 0), level = arg(c, 1);
    ret(c, level == 0 ? ld32(s + 0x24) : ld32(s + 0x2C) + (level > 1 ? ld32(s + 0x40 + 4 * (level - 1)) : 0));
}
HLE(gx2, GX2RUnlockSurfaceEx) {
    uint32_t s = arg(c, 0);
    invalidate(ld32(s + 0x1C) | arg(c, 2), ld32(s + 0x24), ld32(s + 0x20));
    if (ld32(s + 0x2C)) invalidate(ld32(s + 0x1C) | arg(c, 2), ld32(s + 0x2C), ld32(s + 0x28));
}
HLE(gx2, GX2RInvalidateSurface) {
    uint32_t s = arg(c, 0);
    invalidate(ld32(s + 0x1C) | arg(c, 2), ld32(s + 0x24), ld32(s + 0x20));
    if (ld32(s + 0x2C)) invalidate(ld32(s + 0x1C) | arg(c, 2), ld32(s + 0x2C), ld32(s + 0x28));
}
