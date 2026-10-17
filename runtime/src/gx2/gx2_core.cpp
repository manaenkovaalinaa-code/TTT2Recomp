#include "../game_clock.h"
#include "../write_watch.h"
#include "vsync_pacing.h"
#include "../mods/fast_forward.h"
#include "../platform/host.h"
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <condition_variable>
#include <deque>
// GX2 core: command execution, display lists, context states, draws, clears,
// copies and presentation.
#include <algorithm>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gx2.h"
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "register_blocks.h"
#include "gx2_texture_regs.h"
#ifdef WWHD_HAS_VULKAN
#include "shader_key_dirty.h"
#include "gfx/vulkan/api.h"
#endif
#include "runtime.h"
#include "savestate.h"
#include "guest_addr.h"
#include "../aspect.h"
#include "gfx/renderer.h"
#include "platform/perf_hint.h"
#include "platform/thread_sampler.h"
#include "render_prof.h"

using namespace Latte;

namespace gx2 {

// ---------------------------------------------------------------- register file and context states
static uint32 g_regs[kNumRegs];
static uint32* g_shadow = nullptr;  // register copy of the active GX2ContextState
static std::unordered_map<uint32, std::vector<uint32>> g_contexts;
static std::recursive_mutex g_exec_mutex;
static RegisterBlocks<kNumRegs> g_register_blocks;

uint32* regs() { return g_regs; }

// Register writes that can change how shaders are translated bump g_shader_state_gen; the
// renderer reuses its last shader lookup while it is unchanged. Uniforms, uniform/vertex buffer
// addresses and rewrites of an identical value don't count.
extern "C" { uint64_t g_shader_state_gen = 1; }

static bool shader_irrelevant(uint32 reg) {
#ifdef WWHD_HAS_VULKAN
    // Vulkan resolves texture addresses freshly in bind_stage. These words do
    // not participate in shader translation; keep Metal's broader dirty gate.
    static const bool addressMemo = [] {
        const char* e = getenv("WWHD_VK_SHADER_ADDRESS_MEMO");
        return render::vulkan() && e && !strcmp(e, "1");
    }();
    if (addressMemo)
        for (uint32 base : {uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS),
                            uint32(REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS)})
            if (reg >= base && reg < base + 7 * 18) {
                uint32 word = (reg - base) % 7;
                if (word == 2 || word == 3) return true;
            }
#endif
    if (reg >= mmSQ_ALU_CONSTANT0_0 && reg < mmSQ_ALU_CONSTANT0_0 + 0x1000) return true;
    for (uint32 base : {(uint32)mmSQ_VTX_UNIFORM_BLOCK_START, (uint32)mmSQ_PS_UNIFORM_BLOCK_START, (uint32)mmSQ_GS_UNIFORM_BLOCK_START})
        if (reg >= base && reg < base + 7 * 16) return true;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16) {
        uint32 w = (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7;
        return w != 2;  // word 2 holds the stride
    }
    return false;
}
static ShaderKeyDirtyStats shaderKeyDirtyStats;
ShaderKeyDirtyStats shader_key_dirty_stats() { return shaderKeyDirtyStats; }

#ifdef WWHD_HAS_VULKAN
static void apply_small_regs(uint32 first, const uint32* v, uint32 n) {
    static const bool keyDirty = [] {
        const char* e = getenv("WWHD_VK_SHADER_KEY_DIRTY");
        return e && !strcmp(e, "1");
    }();
    static const bool collectStats = getenv("WWHD_VK_STATS") != nullptr;
    const bool classify = rprof::enabled();
    bool changed = false, baselineBump = false, actualBump = false;
    uint64_t maskedWords = 0;
    for (uint32 i = 0; i < n; ++i) {
        const uint32 reg = first + i, value = v[i], old = g_regs[reg];
        if (old != value) {
            changed = true;
            if (classify) {
                if (rprof::fast_class_reg(reg)) rprof::g_reg_dirty |= 1;
                else rprof::note_other_reg(reg);
            }
            if ((collectStats || !actualBump) && !shader_irrelevant(reg)) {
                baselineBump = true;
                uint32 mask;
                if (keyDirty && vulkan_shader_key_mask(reg, mask) && !((old ^ value) & mask)) {
                    if (collectStats) ++maskedWords;
                } else actualBump = true;
            }
            g_regs[reg] = value;
        }
        // Shadow can differ even when registers already match: draw mutates
        // primitive type directly, and context setup initializes only shadow.
        if (g_shadow) g_shadow[reg] = value;
    }
    if (actualBump) ++g_shader_state_gen;
    if (changed && collectStats) {
        ++shaderKeyDirtyStats.changedBatches;
        shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
        shaderKeyDirtyStats.actualBumps += actualBump;
        shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
        shaderKeyDirtyStats.maskedWords += maskedWords;
    }
}
#endif

static void apply_regs(uint32 first, const uint32* v, uint32 n) {
    if (first >= kNumRegs || n > kNumRegs - first) return;
    g_register_blocks.touch(first, n);
#ifdef WWHD_HAS_VULKAN
    // Vulkan renderer only (the Metal renderer keeps the original bulk path)
    static const bool fusedSmall = [] {
        const char* e = getenv("WWHD_VK_FUSE_SMALL_REGS");
        // Enabled by default; explicit zero retains the original bulk path.
        return render::vulkan() && (!e || strcmp(e, "0") != 0);
    }();
    if (fusedSmall && n <= 16) {
        apply_small_regs(first, v, n);
        return;
    }
#endif
    if (memcmp(&g_regs[first], v, n * 4) != 0) {
        if (rprof::enabled())  // draw classifier (render_prof.h)
            for (uint32 i = 0; i < n; i++)
                if (g_regs[first + i] != v[i]) {
                    if (rprof::fast_class_reg(first + i)) rprof::g_reg_dirty |= 1;
                    else rprof::note_other_reg(first + i);
                }
#ifdef WWHD_HAS_VULKAN
        static const bool keyDirty = [] {
            const char* value = getenv("WWHD_VK_SHADER_KEY_DIRTY");
            return render::vulkan() && value && !strcmp(value, "1");
        }();
        static const bool collectStats = render::vulkan() && getenv("WWHD_VK_STATS") != nullptr;
        if(keyDirty || collectStats) {
            bool baselineBump = false, actualBump = false;
            uint64_t maskedWords = 0;
            for(uint32 i = 0; i < n; ++i) {
                uint32 reg = first + i;
                if(g_regs[reg] == v[i] || shader_irrelevant(reg)) continue;
                baselineBump = true;
                uint32 mask;
                if(keyDirty && vulkan_shader_key_mask(reg, mask) && !((g_regs[reg] ^ v[i]) & mask)) {
                    if(collectStats) ++maskedWords;
                } else actualBump = true;
                if(actualBump && !collectStats) break;
            }
            if(actualBump) ++g_shader_state_gen;
            if(collectStats) {
                ++shaderKeyDirtyStats.changedBatches;
                shaderKeyDirtyStats.baselineWouldBumps += baselineBump;
                shaderKeyDirtyStats.actualBumps += actualBump;
                shaderKeyDirtyStats.avoidedBumps += baselineBump && !actualBump;
                shaderKeyDirtyStats.maskedWords += maskedWords;
            }
        } else
#endif
        for (uint32 i = 0; i < n; i++)
            if (g_regs[first + i] != v[i] && !shader_irrelevant(first + i)) { g_shader_state_gen++; break; }
        memcpy(&g_regs[first], v, n * 4);
    }
    if (g_shadow) memcpy(&g_shadow[first], v, n * 4);
}

// ---------------------------------------------------------------- display list recording
struct Recording {
    uint32 start = 0, pos = 0, end = 0;
};
static thread_local Recording t_rec;

static void execute_one(Op op, const uint32* p, uint32 n);

// WWHD_UBO_CHECK=1 (diagnostic): does the game write a uniform block after the draw that uses it was
// issued (legal on the console: the GPU reads it when the command buffer runs, after GX2Flush)?
// Blocks set outside display list recording are hashed when set and again at the next flush or swap.
struct UboIssue { uint32 addr, size; uint64_t hash; };
static std::mutex g_ubo_mutex;
static std::vector<UboIssue> g_ubo_issued;
static uint64_t ubo_hash(uint32 addr, uint32 size) {
    const uint8_t* p = (const uint8_t*)mem::ptr(addr);
    uint64_t h = 1469598103934665603ull;
    for (uint32 i = 0; i < size; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static bool ubo_check_on() { static const bool on = getenv("WWHD_UBO_CHECK") != nullptr; return on; }
void ubo_check_issue(uint32 addr, uint32 size) {
    if (!ubo_check_on() || !addr || t_rec.start) return;
    size = std::min<uint32>(size, 0x10000);
    std::lock_guard<std::mutex> lk(g_ubo_mutex);
    if (g_ubo_issued.size() < 200000) g_ubo_issued.push_back({addr, size, ubo_hash(addr, size)});
}
// WWHD_DL_CHECK=1 (diagnostic): does the game change a display list after recording it (patching
// words it expects to be PM4 packets)? Each recorded list is copied at GX2EndDisplayList and compared
// whenever it is called.
static std::mutex g_dl_mutex;
static std::unordered_map<uint32, std::vector<uint32>> g_dl_copies;
static bool dl_check_on() { static const bool on = getenv("WWHD_DL_CHECK") != nullptr; return on; }
static void dl_check_recorded(uint32 start, uint32 bytes) {
    if (!dl_check_on() || !start || !bytes) return;
    const uint32* w = (const uint32*)mem::ptr(start);
    std::lock_guard<std::mutex> lk(g_dl_mutex);
    g_dl_copies[start].assign(w, w + bytes / 4);
}
static void dl_check_call(uint32 addr, uint32 bytes) {
    if (!dl_check_on()) return;
    static uint64_t calls = 0, changed = 0, logged = 0, unknown = 0;
    const uint32* w = (const uint32*)mem::ptr(addr);
    std::lock_guard<std::mutex> lk(g_dl_mutex);
    ++calls;
    auto it = g_dl_copies.find(addr);
    if (it == g_dl_copies.end()) {
        if (unknown++ < 20) LOG("[dl-check] call of %08X (+%X): not recorded by GX2(R)BeginDisplayList; first words %08X %08X %08X %08X",
                                addr, bytes, w[0], bytes > 4 ? w[1] : 0, bytes > 8 ? w[2] : 0, bytes > 12 ? w[3] : 0);
    } else {
        auto& v = it->second;
        const uint32 n = std::min<uint32>(uint32(v.size()), bytes / 4);
        for (uint32 i = 0; i < n; i++)
            if (v[i] != w[i]) {
                ++changed;
                if (logged < 80) {
                    ++logged;
                    LOG("[dl-check] display list %08X (+%X) changed after recording: word %u (op %08X at its start) %08X -> %08X",
                        addr, bytes, i, v[0], v[i], w[i]);
                }
                v.assign(w, w + bytes / 4);
                break;
            }
    }
    if (calls % 200000 == 0) LOG("[dl-check] %llu calls, %llu changed lists, %llu unrecorded", (unsigned long long)calls,
                                 (unsigned long long)changed, (unsigned long long)unknown);
}

static void ubo_check_point(const char* where) {
    if (!ubo_check_on()) return;
    static uint64_t issued = 0, changed = 0, points = 0, logged = 0;
    std::vector<UboIssue> v;
    { std::lock_guard<std::mutex> lk(g_ubo_mutex); v.swap(g_ubo_issued); }
    for (auto& u : v) {
        ++issued;
        if (ubo_hash(u.addr, u.size) != u.hash) {
            ++changed;
            if (logged < 60) { ++logged; LOG("[ubo-check] block %08X (+%X) changed after its draw was issued, before %s", u.addr, u.size, where); }
        }
    }
    if (++points % 600 == 0) LOG("[ubo-check] %llu blocks issued, %llu changed between issue and flush", (unsigned long long)issued, (unsigned long long)changed);
}


// ---------------------------------------------------------------- render thread
// Like the real GPU, command execution runs asynchronously to the game: GX2 calls append to a
// queue that a render thread turns into Metal work. WWHD_NO_RENDER_THREAD=1 executes inline.
static const bool g_render_thread = getenv("WWHD_NO_RENDER_THREAD") == nullptr;
static std::mutex g_q_mutex;
static std::condition_variable g_q_cv, g_q_done_cv;
static std::vector<uint32> g_q_pending, g_q_work;
static bool g_q_waiting = false;
static uint64_t g_fence_issued = 0, g_fence_done = 0;
// TTT2: the GPU reads guest memory when a command buffer runs, after GX2Flush (or a DrawDone, a
// swap, a fence), not when the command is issued; TTT2 issues texture copies and fills their
// source afterwards. So for it the render thread only runs commands up to the last such point
// (g_q_release: words of g_q_pending that may run). Experimental, off unless WWHD_GX2_HOLD=1: TTT2
// frees some copy sources right after GX2CopySurface, so copies stay synchronous.
static size_t g_q_release = 0;
static bool hold_until_flush() {
    static const bool on = getenv("WWHD_GX2_HOLD") != nullptr;
    return on;
}
static bool release_point(uint32 op) {
    return op == OP_FLUSH || op == OP_DRAW_DONE || op == OP_SWAP || op == OP_FENCE || op == OP_SETUP_CONTEXT;
}

static void render_thread_main() {
    host::set_thread_name("GX2 render");
    host::boost_thread_priority();
    perf_hint::add_current_thread();
    thread_sampler::add_current_thread("render");
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            if (hold_until_flush()) {
                if (!g_q_release) {
                    const uint64_t idle = rprof::enabled() ? rprof::now_ns() : 0;
                    g_q_cv.wait(lk, [] { return g_q_release != 0; });
                    if (idle) rprof::add_idle(rprof::now_ns() - idle);
                }
                g_q_waiting = false;
                g_q_work.assign(g_q_pending.begin(), g_q_pending.begin() + g_q_release);
                g_q_pending.erase(g_q_pending.begin(), g_q_pending.begin() + g_q_release);
                g_q_release = 0;
            } else {
            if (g_q_pending.empty()) {
                const uint64_t idle = rprof::enabled() ? rprof::now_ns() : 0;
                g_q_cv.wait(lk, [] { return !g_q_pending.empty(); });
                if (idle) rprof::add_idle(rprof::now_ns() - idle);
            }
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
            }
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            render::with_autorelease_pool([] { execute(g_q_work.data(), (uint32)g_q_work.size()); });
        }
        g_q_work.clear();
    }
}

static void enqueue(Op op, const uint32* payload, uint32 n) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread([] { exception_report::boundary("GX2 render thread", render_thread_main); }).detach(); });
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_q_pending.push_back(op | (n << 8));
    g_q_pending.insert(g_q_pending.end(), payload, payload + n);
    if (hold_until_flush()) {
        if (!release_point(op)) return;
        g_q_release = g_q_pending.size();
    }
    if (g_q_waiting) g_q_cv.notify_one();
}

// block the game thread until the render thread has executed everything queued so far
// debug: WWHD_SYNC_STATS=1 logs, every 5 s, how often each caller waited for the render thread to
// catch up (render_sync) and for how long
enum SyncSite { kSyncShutdown, kSyncFlip, kSyncDrawDone, kSyncVsyncUncapped, kSyncVsyncFlip, kSyncSaveState, kSyncCopySurface, kSyncSites };
static void sync_stat(int site, std::chrono::steady_clock::duration waited) {
    static const bool on = getenv("WWHD_SYNC_STATS") != nullptr;
    if (!on) return;
    static std::mutex mu;
    static uint64_t count[kSyncSites], ns[kSyncSites];
    static auto t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu);
    count[site]++;
    ns[site] += std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
    const auto now = std::chrono::steady_clock::now();
    if (now - t0 < std::chrono::seconds(5)) return;
    const double secs = std::chrono::duration<double>(now - t0).count();
    static const char* names[kSyncSites] = {"shutdown", "flip", "DrawDone", "vsync-uncapped", "vsync-flip", "savestate", "CopySurface"};
    char buf[900];
    int k = snprintf(buf, sizeof buf, "[gx2] render_sync per second:");
    for (int i = 0; i < kSyncSites; i++)
        if (count[i]) k += snprintf(buf + k, sizeof buf - k, " %s %.1f x %.2f ms", names[i], count[i] / secs, ns[i] / 1e6 / count[i]);
    LOG("%s", buf);
    memset(count, 0, sizeof count); memset(ns, 0, sizeof ns);
    t0 = now;
}
static void render_sync(int site = kSyncShutdown) {
    if (!g_render_thread) return;
    const auto started = std::chrono::steady_clock::now();
    struct Done {
        int site;
        std::chrono::steady_clock::time_point t;
        ~Done() {
            const auto waited = std::chrono::steady_clock::now() - t;
            sync_stat(site, waited);
            if (rprof::enabled())
                rprof::add_sync(site == kSyncDrawDone ? rprof::kSyncDrawDone : site == kSyncCopySurface ? rprof::kSyncCopySurface
                                : site == kSyncFlip || site == kSyncVsyncFlip ? rprof::kSyncFlip : rprof::kSyncOther,
                                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
        }
    } done{site, started};
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        id = ++g_fence_issued;
    }
    uint32 w = (uint32)id;
    enqueue(OP_FENCE, &w, 1);
    std::unique_lock<std::mutex> lk(g_q_mutex);
    g_q_done_cv.wait(lk, [&] { return g_fence_done >= id; });
}

void emit(Op op, const uint32* payload, uint32 n) {
    if (t_rec.start) {
        uint32 bytes = 4 * (n + 1);
        if (t_rec.pos + bytes > t_rec.end) {
            LOG("[gx2] display list overflow at %08X", t_rec.start);
            return;
        }
        uint32* w = (uint32*)mem::ptr(t_rec.pos);
        w[0] = op | (n << 8);
        memcpy(w + 1, payload, n * 4);
        t_rec.pos += bytes;
        return;
    }
    if (g_render_thread) {
        enqueue(op, payload, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload, n); });
}

// host-only commands never go into display lists
static void emit_host(Op op, std::initializer_list<uint32> payload) {
    if (g_render_thread) {
        enqueue(op, payload.begin(), (uint32)payload.size());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([&] { execute_one(op, payload.begin(), (uint32)payload.size()); });
}

#ifdef WWHD_HAS_VULKAN
void checkpoint_vulkan_caches() {
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] { gfxvk::save_renderer_caches(); });
}
void shutdown_vulkan_caches() {
    render_sync();
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    host::with_autorelease_pool([] { gfxvk::shutdown_renderer_caches(); });
}
#endif

void set_regs(uint32 first, const uint32* values, uint32 count) {
    if (!count) return;
    static thread_local std::vector<uint32> buf;
    buf.resize(count + 1);
    buf[0] = first;
    memcpy(&buf[1], values, count * 4);
    emit(OP_SET_REGS, buf.data(), count + 1);
}
void set_reg(uint32 reg, uint32 value) { emit(OP_SET_REGS, {reg, value}); }

void execute(const uint32* words, uint32 count) {
    uint32 i = 0;
    while (i < count) {
        uint32 hdr = words[i];
        Op op = (Op)(hdr & 0xFF);
        uint32 n = hdr >> 8;
        if (op >= OP_COUNT || i + 1 + n > count) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        execute_one(op, &words[i + 1], n);
        i += 1 + n;
    }
}

static void set_context(uint32 ctx) {
    if (!ctx) {
        g_shadow = nullptr;
        return;
    }
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) {
        g_shadow = nullptr;
        return;
    }
    g_shadow = it->second.data();
    // Draw writes primitive type without updating the context shadow.
    g_register_blocks.touch(uint32(REGADDR::VGT_PRIMITIVE_TYPE), 1);
    if (g_register_blocks.restore(g_regs, g_shadow, [](uint32 reg, uint32 old, uint32 value) {
        if (shader_irrelevant(reg)) return false;
#ifdef WWHD_HAS_VULKAN
        static const bool keyDirty = [] {
            const char* e = getenv("WWHD_VK_SHADER_KEY_DIRTY");
            return render::vulkan() && e && !strcmp(e, "1");
        }();
        uint32 mask;
        if (keyDirty && vulkan_shader_key_mask(reg, mask)) return ((old ^ value) & mask) != 0;
#endif
        return true;
    })) ++g_shader_state_gen;
    rprof::g_reg_dirty |= 2;  // draw classifier: a context load counts as a full state change
}

constexpr uint32 kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
// struct copies carried in a command, placed back in guest memory for the renderer (commands run
// one at a time, so a couple of fixed slots suffice)
static uint32 unpack_struct(const uint32* words, uint32 count, int slot) {
    static uint32 scratch = 0;
    if (!scratch) scratch = mem::host_alloc(2 * 0x100, 0x40);
    uint32 addr = scratch + slot * 0x100;
    memcpy(mem::ptr(addr), words, count * 4);
    return addr;
}

// Vulkan: GX2DrawDone queues the work instead of waiting for an idle device (the default on every
// platform since 2026-10-07; WWHD_VK_LAZY_DRAW_DONE=0 restores the full wait). The Metal renderer
// reads large vertex buffers straight from guest memory, so it keeps the real GPU wait.
static bool lazy_draw_done() {
    static const bool on = [] {
        const char* e = getenv("WWHD_VK_LAZY_DRAW_DONE");
        return render::vulkan() && (!e || atoi(e) != 0);
    }();
    return on;
}

static void execute_op(Op op, const uint32* p, uint32 n);
// every op is counted and (sampled) timed for the render-thread profiler (render_prof.h); display-list
// calls are not timed themselves: their ops are
// TTT2 port diagnostics: WWHD_OPLOG=first-last logs the commands the renderer runs in that range of
// swaps (register writes are summarized), to see what a frame draws
static void op_log(Op op, const uint32* p, uint32 n) {
    static uint64_t lo = ~0ull, hi = 0, swaps = 0;
    static uint32 regs_in_row = 0;
    static const bool on = [] {
        const char* e = []{ static const char* v = std::getenv("WWHD_OPLOG"); return v; }();
        if (!e) return false;
        lo = strtoull(e, nullptr, 10);
        const char* d = strchr(e, '-');
        hi = d ? strtoull(d + 1, nullptr, 10) : lo;
        return true;
    }();
    if (!on) return;
    if (op == OP_SWAP) swaps++;
    if (swaps < lo || swaps > hi) return;
    if (op == OP_SET_REGS || op == OP_SET_PROJ_REGS) { regs_in_row++; return; }
    static const char* names[] = {"NOP", "SET_REGS", "DRAW", "DRAW_INDEXED", "CLEAR_COLOR", "CLEAR_DEPTH", "CLEAR_BUFFERS",
                                  "COPY_SURFACE", "COPY_TO_SCAN", "CALL", "SET_CONTEXT", "INVALIDATE", "EXPAND_COLOR",
                                  "EXPAND_DEPTH", "FLUSH", "DRAW_DONE", "SWAP", "SETUP_CONTEXT", "FENCE", "SET_PROJ_REGS",
                                  "LAYOUT_ROOT", "PEEK_Z", "LAYOUT_CONTENT", "STREAMOUT_CONTEXT"};
    char buf[400];
    int k = snprintf(buf, sizeof buf, "[op %llu] %s (+%u regs)", (unsigned long long)swaps, op < OP_COUNT ? names[op] : "?", regs_in_row);
    regs_in_row = 0;
    for (uint32 i = 0; i < n && i < 12 && k < (int)sizeof buf - 12; i++) k += snprintf(buf + k, sizeof buf - k, " %08X", p[i]);
    if (op == OP_COPY_SURFACE && n >= 2 * kSurfaceWords + 4) {  // big-endian struct copies
        auto be = [](uint32 v) { return __builtin_bswap32(v); };
        const uint32* d = p + kSurfaceWords + 2;
        k += snprintf(buf + k, sizeof buf - k, " | src %08X %ux%u fmt %X tm %u pitch %u -> dst %08X %ux%u fmt %X tm %u pitch %u",
                      be(p[9]), be(p[1]), be(p[2]), be(p[5]), be(p[12]), be(p[15]), be(d[9]), be(d[1]), be(d[2]), be(d[5]), be(d[12]), be(d[15]));
    }
    if (op == OP_CLEAR_COLOR && n >= 4)
        k += snprintf(buf + k, sizeof buf - k, " | rgba %.3f %.3f %.3f %.3f", bitsf(p[n - 4]), bitsf(p[n - 3]), bitsf(p[n - 2]), bitsf(p[n - 1]));
    if (op == OP_DRAW || op == OP_DRAW_INDEXED) {
        const uint32* r = g_regs;
        k += snprintf(buf + k, sizeof buf - k, " | cb0 %08X fmt %08X tex0 %08X ps %08X vs %08X", r[mmCB_COLOR0_BASE], r[mmCB_COLOR0_INFO],
                      r[(uint32)REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + 2] << 8, r[mmSQ_PGM_START_PS] << 8, r[mmSQ_PGM_START_VS] << 8);
        const uint32 t0 = (uint32)REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
        k += snprintf(buf + k, sizeof buf - k, " texw %08X %08X %08X %08X %08X %08X %08X blend %08X cbctl %08X mask %08X",
                      r[t0], r[t0 + 1], r[t0 + 2], r[t0 + 3], r[t0 + 4], r[t0 + 5], r[t0 + 6],
                      r[(uint32)REGADDR::CB_BLEND0_CONTROL], r[(uint32)REGADDR::CB_COLOR_CONTROL], r[(uint32)REGADDR::CB_TARGET_MASK]);
        const uint32 cps = mmSQ_ALU_CONSTANT0_0;
        k += snprintf(buf + k, sizeof buf - k, " psc0 %.3f %.3f %.3f %.3f", bitsf(r[cps]), bitsf(r[cps + 1]), bitsf(r[cps + 2]), bitsf(r[cps + 3]));
        // first texels of texture 0 (guest memory)
        uint32 ta = r[t0 + 2] << 8;
        if (ta) { const uint8_t* tp = mem::ptr(ta); k += snprintf(buf + k, sizeof buf - k, " texels %02X%02X%02X%02X %02X%02X%02X%02X", tp[0], tp[1], tp[2], tp[3], tp[0x800], tp[0x801], tp[0x802], tp[0x803]); }
    }
    LOG("%s", buf);
}
static void execute_one(Op op, const uint32* p, uint32 n) {
    op_log(op, p, n);
    // TTT2 port diagnostics: WWHD_SKIP_PS=<pixel shader address> drops the draws that use it
    static const uint32 skip_ps = getenv("WWHD_SKIP_PS") ? (uint32)strtoul(getenv("WWHD_SKIP_PS"), nullptr, 16) : 0;
    if (skip_ps && (op == OP_DRAW || op == OP_DRAW_INDEXED) && ((g_regs[mmSQ_PGM_START_PS] << 8) == skip_ps || skip_ps == 1)) return;
    rprof::Op kind;
    switch (op) {
    case OP_CALL: execute_op(op, p, n); return;
    case OP_SET_REGS: case OP_SET_PROJ_REGS: kind = rprof::kOpRegs; break;
    case OP_DRAW: case OP_DRAW_INDEXED: kind = rprof::kOpDraw; rprof::classify_draw(); break;
    case OP_CLEAR_COLOR: case OP_CLEAR_DEPTH: case OP_CLEAR_BUFFERS: kind = rprof::kOpClear; break;
    case OP_COPY_SURFACE: kind = rprof::kOpCopy; break;
    case OP_COPY_TO_SCAN: kind = rprof::kOpScan; break;
    case OP_INVALIDATE: kind = rprof::kOpInvalidate; break;
    case OP_PEEK_Z: kind = rprof::kOpCopy; break;
    case OP_FLUSH: kind = rprof::kOpFlush; break;
    case OP_DRAW_DONE: kind = rprof::kOpDrawDone; break;
    case OP_SWAP: kind = rprof::kOpSwap; break;
    default: kind = rprof::kOpOther; break;
    }
    const uint64_t started = rprof::op_begin(kind);
    execute_op(op, p, n);
    rprof::op_end(kind, started);
    if (op == OP_SWAP) rprof::frame_end(n >= 2 && p[1]);
}

// Test only: WWHD_TEST_RENDER_LOAD_MS=x / WWHD_TEST_GAME_LOAD_MS=x busy-wait x ms per frame on the
// render thread (before each swap is executed) / on the game thread (at each GX2SwapScanBuffers):
// the frame time of a slower computer, for measuring frame pacing (docs/performance.md).
static void test_frame_load(const char* name) {
    const char* e = getenv(name);
    const double ms = e ? atof(e) : 0;
    if (ms <= 0) return;
    const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(int64_t(ms * 1e6));
    while (std::chrono::steady_clock::now() < until) {}
}

static void execute_op(Op op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: break;
    case OP_SET_REGS: apply_regs(p[0], p + 1, n - 1); break;
    case OP_DRAW: render::draw(g_regs, p[0], p[1], 0, 0, p[2], p[3]); break;
    case OP_DRAW_INDEXED: render::draw(g_regs, p[0], p[1], p[2], p[3], p[4], p[5]); break;
    case OP_CLEAR_COLOR: {
        const uint32* q = p + kColorBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, unpack_struct(p, kColorBufferWords, 0), rgba);
        break;
    }
    case OP_CLEAR_DEPTH: {
        const uint32* q = p + kDepthBufferWords;
        render::clear_depth_stencil(g_regs, unpack_struct(p, kDepthBufferWords, 0), bitsf(q[0]), q[1], q[2]);
        break;
    }
    case OP_CLEAR_BUFFERS: {
        uint32 cb = unpack_struct(p, kColorBufferWords, 0), db = unpack_struct(p + kColorBufferWords, kDepthBufferWords, 1);
        const uint32* q = p + kColorBufferWords + kDepthBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        render::clear_color(g_regs, cb, rgba);
        render::clear_depth_stencil(g_regs, db, bitsf(q[4]), q[5], q[6]);
        break;
    }
    case OP_COPY_SURFACE: {
        // debug: WWHD_GX2_DELAY_COPY=ms stalls the render thread before each surface copy (a slow
        // or busy render thread; reproduced the agl boot crash every time before GX2CopySurface waited)
        static const int delay = getenv("WWHD_GX2_DELAY_COPY") ? atoi(getenv("WWHD_GX2_DELAY_COPY")) : 0;
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        uint32 src = unpack_struct(p, kSurfaceWords, 0);
        const uint32* q = p + kSurfaceWords;
        uint32 dst = unpack_struct(q + 2, kSurfaceWords, 1);
        const uint32* r = q + 2 + kSurfaceWords;
        render::copy_surface(src, q[0], q[1], dst, r[0], r[1]);
        break;
    }
    case OP_COPY_TO_SCAN: render::copy_to_scan(unpack_struct(p, kColorBufferWords, 0), p[kColorBufferWords]); break;
    case OP_CALL: dl_check_call(p[0], p[1]); execute((const uint32*)mem::ptr(p[0]), p[1] / 4); break;
    case OP_SET_CONTEXT: set_context(p[0]); break;
    case OP_INVALIDATE: render::invalidate(p[0], p[1], p[2]); break;
    case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: break;  // MSAA/HiZ decompression: nothing to do on the host
    case OP_PEEK_Z: render::peek_z(p, n); break;
    case OP_FLUSH: render::guest_flush(); break;  // Vulkan: asynchronous submission
    case OP_DRAW_DONE:
        // The Vulkan renderer writes GPU results back to guest memory only for linear surfaces (guest data is copied
        // into fenced upload slices when work is recorded), so GX2DrawDone needs this op executed
        // (render_sync in the HLE), not an idle GPU. Lazy DrawDone (lazy_draw_done(), the default)
        // queues the work instead of waiting for the whole device every frame. A payload word of 1
        // (save states) always waits for the idle GPU.
        if (lazy_draw_done() && !(n && p[0])) render::guest_flush();
        else render::wait_idle();
        // except what the CPU reads back: linear render targets (the Picto Box picture, issue #53)
        render::write_back();
        break;
    case OP_SWAP: {
        static const bool load = getenv("WWHD_TEST_RENDER_LOAD_MS") != nullptr;
        if (load) test_frame_load("WWHD_TEST_RENDER_LOAD_MS");
        if (n) render::set_frame_aspect(gx2::bitsf(p[0]));  // aspect ratio from the next frame on (aspect.cpp)
        if (n >= 3 && p[2]) render::request_capture();
        render::swap();
        // WWHD_SCHED_STATS: the render thread's own CPU time per guest frame
        if (static const bool stats = getenv("WWHD_SCHED_STATS") != nullptr; stats) {
            static uint64_t frames = 0, cpu0 = 0;
            static auto t0 = std::chrono::steady_clock::now();
            if (++frames % 300 == 0) {
                const uint64_t cpu = rprof::thread_cpu_ns();
                const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (cpu0) LOG("[sched] render thread: %.2f ms CPU per frame (%.0f%% busy)", (cpu - cpu0) / 1e6 / 300, (cpu - cpu0) / 1e4 / wall);
                cpu0 = cpu;
                t0 = std::chrono::steady_clock::now();
            }
        }
        break;
    }
    case OP_SET_PROJ_REGS: {
        uint32 v[16];
        memcpy(v, p + 1, sizeof v);
        float kx, ky;
        if (n == 17 && render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky))
            for (int i = 0; i < 4; i++) {  // rows x and y: the 16:9 layout space centred in the wider picture
                v[i] = fbits(bitsf(v[i]) / kx);
                v[4 + i] = fbits(bitsf(v[4 + i]) / ky);
            }
        apply_regs(p[0], v, std::min<uint32>(n - 1, 16));
        break;
    }
    case OP_LAYOUT_CONTENT:
        aspect::set_content_clip(n && p[0]);
        break;
    case OP_STREAMOUT_CONTEXT: {
        // the write offset of a stream out buffer lives in the register file; draws advance it
        if (n < 3 || p[0] >= 4) break;
        uint32& offset = g_regs[mmVGT_STRMOUT_BUFFER_OFFSET_0 + p[0] * 4];
        if (p[2] == 0) offset = 0;
        else if (p[2] == 1) offset = p[1] ? ld32(p[1]) : 0;
        else if (p[1]) st32(p[1], offset);
        break;
    }
    case OP_LAYOUT_ROOT: {
        float kx, ky;
        aspect::layout_root_target(p[0], render::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky));
        break;
    }
    case OP_SETUP_CONTEXT:
        g_contexts[p[0]].assign(kNumRegs, 0);
        g_shadow = g_contexts[p[0]].data();
        break;
    case OP_FENCE: {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_fence_done = std::max<uint64_t>(g_fence_done, p[0]);
        g_q_done_cv.notify_all();
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- default state
static void set_default_state() {
    // GX2SetShaderModeEx(UNIFORM_REGISTER, ...)
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
    set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_PA_CL_VTE_CNTL vte{};
    vte.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
    vte.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true).set_VTX_W0_FMT(true);
    set_reg(REGADDR::PA_CL_VTE_CNTL, vte.getRawValue());
    set_reg(REGADDR::DB_DEPTH_CONTROL, (1 << 1) | (1 << 2) | (1 << 4));  // z test + write, LESS
    LATTE_SX_ALPHA_TEST_CONTROL at;
    at.set_ALPHA_FUNC(LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC::LESS).set_ALPHA_TEST_ENABLE(false);
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, at.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    LATTE_PA_SU_SC_MODE_CNTL pm{};
    pm.set_FRONT_FACE(LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW);
    pm.set_FRONT_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES).set_BACK_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES);
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, pm.getRawValue());
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_reg(REGADDR::CB_TARGET_MASK, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, 0);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(8).set_HEIGHT(8).getRawValue());
    LATTE_CB_COLOR_CONTROL cc;
    cc.set_SPECIAL_OP(LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL).set_ROP(LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY);
    set_reg(REGADDR::CB_COLOR_CONTROL, cc.getRawValue());
    LATTE_PA_CL_CLIP_CNTL clip{};
    clip.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, clip.getRawValue());
    set_reg(mmDB_DEPTH_CLEAR, fbits(1.0f));
}

}  // namespace gx2

using namespace gx2;

// ---------------------------------------------------------------- init / timing
// Display timing, modelled on the hardware: vsync ticks at 60 Hz on its own clock, and a requested
// flip executes on the first vsync that is at least `swap interval` vsyncs after the previous flip.
// Games pace themselves by waiting for vsync until their flips have executed.
// 120/240 fps frame interpolation needs 4/8 flips per logic step: the virtual vsync then ticks 2/4
// times per 59.94 Hz vsync (interp::vsync_rate()). The clock counts quarter vsyncs ("ticks"); a
// vsync at the current rate is a granule of 4/rate ticks, and every vsync count below is in ticks,
// rounded to granules, so at 30/60 fps (granule 4) the timing is the 59.94 Hz one exactly.
static uint64_t g_swap_count = 0, g_flip_count = 0;
namespace gx2 { uint64_t flips_presented() { return __atomic_load_n(&g_flip_count, __ATOMIC_RELAXED); } }  // live fps in the title
static uint32 g_swap_interval = 1;  // as set by the game (frame interpolation halves it)
namespace interp { uint32_t effective_swap_interval(uint32_t game); uint64_t logic_steps(); int vsync_rate(); }
static std::mutex g_flip_mutex;
static const auto g_vsync_epoch = std::chrono::steady_clock::now();
static constexpr std::chrono::nanoseconds kVsyncPeriod(16683333);  // 59.94 Hz
static constexpr uint64_t kTicksPerVsync = 4;
// a flip also waits for the GPU to finish that frame, as on hardware: the game reuses a frame's
// buffers once its flip has executed
struct PendingFlip { uint64_t swap; std::chrono::steady_clock::time_point at; };  // (at: when it was swapped)
static std::deque<PendingFlip> g_pending_flips;
static uint64_t g_last_flip_vsync = 0;  // tick (a granule boundary)
static uint64_t g_last_flip_time = 0;  // timebase
static int64_t g_count_offset = 0;     // guest-visible swap/flip counts minus ours (set by a loaded save state)

// The clock is advanced (g_vsync_shift) when a late flip moves the grid (vsync_pacing.h).
static constexpr vsync_pacing::Clock kVsyncClock{kVsyncPeriod.count(), kTicksPerVsync};
static std::atomic<int64_t> g_vsync_shift{0};  // ns; written with g_flip_mutex held
static int64_t vsync_clock_now() {  // ns since the epoch on the vsync's clock (the game's clock when it runs faster)
    if (!game_clock::shifted()) return (std::chrono::steady_clock::now() - g_vsync_epoch).count();
    const auto epoch = std::chrono::duration_cast<std::chrono::nanoseconds>(g_vsync_epoch.time_since_epoch()).count();
    return game_clock::sample().guest - epoch;
}
static uint64_t vsync_index() {  // in ticks
    return kVsyncClock.index(vsync_clock_now(), g_vsync_shift.load(std::memory_order_relaxed));
}
static uint64_t vsync_granule() {  // ticks per vsync at the current rate: 4 (30/60 fps), 2 (120), 1 (240)
    const int rate = interp::vsync_rate();
    return rate >= 4 ? 1 : rate >= 2 ? 2 : kTicksPerVsync;
}
static std::chrono::steady_clock::time_point tick_time(uint64_t tick) {
    const auto at = std::chrono::nanoseconds(kVsyncClock.time(tick, g_vsync_shift.load(std::memory_order_relaxed)));
    if (!game_clock::shifted())
        return g_vsync_epoch + at;
    const auto sample = game_clock::sample();
    const auto target = g_vsync_epoch + at;
    const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(target.time_since_epoch()).count() - sample.guest;
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(sample.host + std::max<int64_t>(0, (left + sample.rate - 1) / sample.rate)));
}
// the first tick at which this flip may execute: `swap interval` vsyncs after the previous flip.
// (On hardware also the vsync after its swap, which is the same for a swap made before that tick; a
// later swap, a frame that missed its vsync, flips as soon as it is ready: vsync_pacing.h.)
static uint64_t flip_due(const PendingFlip&) {
    const uint64_t g = vsync_granule();
    return g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval) * g;
}
// Uncapped (gx2.h): debug, to see how fast the renderer can go; GPU completion ordering is kept
static std::atomic<int> g_uncapped{-1};  // -1: not read from the environment yet
bool gx2::uncapped() {
    int v = g_uncapped.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* any = getenv("WWHD_UNCAPPED");
        const char* vk = getenv("WWHD_VK_UNCAPPED");
        v = (any && !strcmp(any, "1")) || (vk && !strcmp(vk, "1") && render::vulkan()) ? 1 : 0;
        int expected = -1;
        g_uncapped.compare_exchange_strong(expected, v);
        v = g_uncapped.load(std::memory_order_relaxed);
    }
    return v == 1;
}
void gx2::set_uncapped(bool on) {
    if (uncapped() == on) return;
    g_uncapped = on ? 1 : 0;
    LOG("[gx2] uncapped %s", on ? "on (debug: no frame limit, no vsync; the game runs faster than real time)" : "off");
}
static bool uncapped_benchmark() { return gx2::uncapped(); }

// debug: WWHD_LOG_FLIP_PACING=1 logs, every 300 flips, the intervals between flips (the frame times
// the game sees) and what the flips waited for
static void log_flip_pacing(const PendingFlip& f) {
    static const bool on = getenv("WWHD_LOG_FLIP_PACING") != nullptr;
    if (!on) return;
    using clk = std::chrono::steady_clock;
    static clk::time_point last{};
    static std::vector<float> intervals;
    static double waitSum = 0;
    const auto now = clk::now();
    if (last != clk::time_point{}) intervals.push_back(std::chrono::duration<float, std::milli>(now - last).count());
    last = now;
    waitSum += std::chrono::duration<double, std::milli>(now - f.at).count();
    if (intervals.size() < 300) return;
    std::vector<float> v = intervals;
    std::sort(v.begin(), v.end());
    double sum = 0;
    int over25 = 0, over34 = 0;
    for (float x : v) { sum += x; over25 += x > 25; over34 += x > 34; }
    LOG("[gx2 flips] %.1f flips/s, interval p50 %.2f p95 %.2f p99 %.2f max %.2f ms, >25 ms %d, >34 ms %d of %zu; swap to flip %.2f ms",
        1000.0 * v.size() / sum, v[v.size() / 2], v[v.size() * 95 / 100], v[v.size() * 99 / 100], v.back(), over25, over34, v.size(),
        waitSum / v.size());
    intervals.clear();
    waitSum = 0;
}
static void update_flips() {  // g_flip_mutex held
    uint64_t now = vsync_index();
    while (!g_pending_flips.empty()) {
        const uint64_t due = flip_due(g_pending_flips.front());
        if ((!uncapped_benchmark() && due > now) || render::frames_completed() < g_pending_flips.front().swap) break;
        uint64_t at = now / vsync_granule() * vsync_granule();
        if (!uncapped_benchmark()) {  // on its vsync, or late: then the grid may move (vsync_pacing.h)
            int64_t shift = g_vsync_shift.load(std::memory_order_relaxed);
            at = vsync_pacing::flip_executed(kVsyncClock, shift, due, vsync_clock_now(), vsync_granule());
            g_vsync_shift.store(shift, std::memory_order_relaxed);
            now = std::max(now, at);
        }
        log_flip_pacing(g_pending_flips.front());
        g_pending_flips.pop_front();
        g_last_flip_vsync = at;
        g_last_flip_time = timebase::simulation_now();
        g_flip_count++;
    }
}
// A flip whose vsync has come but whose frame is not ready yet (the game swapped late, or the
// renderer has not finished it): GX2WaitForVsync waits for that frame in short steps instead of
// sleeping to the next vsync, and returns when the flip has executed (vsync_pacing.h).
static bool late_flip_pending() {  // g_flip_mutex held
    if (g_pending_flips.empty()) return false;
    const auto& front = g_pending_flips.front();
    return flip_due(front) <= vsync_index() && render::frames_completed() < front.swap;
}
static bool wait_for_late_flip(std::chrono::steady_clock::time_point deadline) {
    uint64_t flips;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        flips = g_flip_count;
        update_flips();
        if (g_flip_count != flips) return true;
        if (!late_flip_pending()) return false;
    }
    constexpr auto kStep = std::chrono::microseconds(250);
    for (auto now = std::chrono::steady_clock::now(); now < deadline; now = std::chrono::steady_clock::now()) {
        threads::park_sleep_until(std::min(deadline, now + kStep));
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        if (g_flip_count != flips) return true;
        if (!late_flip_pending()) return false;
    }
    return false;
}
#ifdef WWHD_HAS_VULKAN
static void ready_flip_before_resume() {
    bool needsSync;
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        if(g_pending_flips.empty()) return;
        const auto& front = g_pending_flips.front();
        const uint64_t at = flip_due(front);
        if(at > vsync_index()) return;
        needsSync = render::frames_completed() < front.swap;
    }
    if(needsSync) render_sync(kSyncFlip); // Core already released; no flip lock held.
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
}
#endif

HLE(gx2, GX2Init) {
    set_default_state();
    LOG("[gx2] initialized (native GX2 -> %s)", render::api_name(render::active()));
    if(uncapped_benchmark())
        LOG("[gx2 benchmark] uncapped guest flips; GPU completion ordering retained; frame-based simulation accelerates while timebase/audio clocks remain real-time");
}

HLE(gx2, GX2SetupContextStateEx) {
    uint32 ctx = arg(c, 0);
    emit_host(OP_SETUP_CONTEXT, {ctx});
    set_default_state();
    // the context's "restore" display list lives inside the (0xA100 byte) context structure
    uint32 dl = ctx + 0x9800;
    uint32* w = (uint32*)mem::ptr(dl);
    w[0] = OP_SET_CONTEXT | (1u << 8);
    w[1] = ctx;
}
HLE(gx2, GX2SetContextState) { emit(OP_SET_CONTEXT, {arg(c, 0)}); }
HLE(gx2, GX2GetContextStateDisplayList) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) + 0x9800);
    if (arg(c, 2)) st32(arg(c, 2), 8);
}

// ---------------------------------------------------------------- display lists
HLE(gx2, GX2BeginDisplayListEx) {
    t_rec.start = t_rec.pos = arg(c, 0);
    t_rec.end = arg(c, 0) + arg(c, 1);
}
HLE(gx2, GX2EndDisplayList) {
    uint32 size = t_rec.pos - t_rec.start;
    dl_check_recorded(t_rec.start, size);
    t_rec = Recording{};
    ret(c, size);
}
HLE(gx2, GX2GetCurrentDisplayList) {
    if (arg(c, 0)) st32(arg(c, 0), t_rec.start);
    if (arg(c, 1)) st32(arg(c, 1), t_rec.start ? t_rec.end - t_rec.start : 0);
    ret(c, t_rec.start != 0);
}
HLE(gx2, GX2CallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }
HLE(gx2, GX2DirectCallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }

// ---------------------------------------------------------------- draws
HLE(gx2, GX2DrawEx) { emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
HLE(gx2, GX2DrawIndexedEx) { emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)}); }

// ---------------------------------------------------------------- clears and copies
static float farg(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }
// Struct parameters are copied into the command (as GX2 encodes them into the command buffer):
// games reuse one GX2ColorBuffer/GX2DepthBuffer and change its view between calls, also while
// recording display lists that run later.
static void put_struct(std::vector<uint32>& p, uint32 addr, uint32 words) {
    size_t at = p.size();
    p.resize(at + words);
    if (addr) memcpy(&p[at], mem::ptr(addr), words * 4);
}
HLE(gx2, GX2ClearColor) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    for (int i = 0; i < 4; i++) p.push_back(fbits(farg(c, i)));
    emit(OP_CLEAR_COLOR, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearDepthStencilEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), arg(c, 1) & 0xFF, arg(c, 2)});
    emit(OP_CLEAR_DEPTH, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearBuffersEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    put_struct(p, arg(c, 1), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)), fbits(farg(c, 4)),
                       arg(c, 2) & 0xFF, arg(c, 3)});
    emit(OP_CLEAR_BUFFERS, p.data(), (uint32)p.size());
}
HLE(gx2, GX2SetClearDepthStencil) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(arg(c, 0));
    db->clearDepth = farg(c, 0);
    db->clearStencil = arg(c, 1) & 0xFF;
}
static thread_local uint32_t wwatch_spy_after_copy = 0, wwatch_spy_size = 0;
// TTT2: copies out of a LINEAR_SPECIAL surface (its texture uploads) run at the next GX2DrawDone or swap,
// as the GPU would run them later: the game issues the copy, then a worker thread finishes writing
// the source (its CPU texture encoder), and the game waits for that before DrawDone.
// Experimental (WWHD_DEFER_LINEAR_COPIES=1): Cemu runs these copies synchronously, as by default here.
static std::mutex g_deferred_m;
static std::vector<std::vector<uint32>> g_deferred_copies;
static bool defer_linear_copies() {
    static const bool on = getenv("WWHD_DEFER_LINEAR_COPIES") != nullptr;  // experiment; Cemu copies at once
    return on;
}
static void run_deferred_copies() {
    std::vector<std::vector<uint32>> v;
    {
        std::lock_guard<std::mutex> lk(g_deferred_m);
        v.swap(g_deferred_copies);
    }
    if (v.empty()) return;
    for (auto& p : v) emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    BlockingScope b;
    render_sync(kSyncCopySurface);
}
HLE(gx2, GX2CopySurface) {
    // debug: WWHD_COPYDBG=1 logs each copy as issued (thread, caller, source and destination images)
    static const bool dbg = getenv("WWHD_COPYDBG") != nullptr;
    if (dbg)
        LOG("[copydbg] issue t=%.3f thread %08X lr %08X src %08X img %08X dst %08X img %08X size %X", timebase::now() / (double)timebase::kTicksPerSec,
            threads::current_thread(), c->lr, arg(c, 0), ld32(arg(c, 0) + 0x24), arg(c, 3), ld32(arg(c, 3) + 0x24), ld32(arg(c, 3) + 0x20));
    // TTT2 port diagnostics: WWHD_COPY_TRACE=WxH dumps every thread's recent guest functions
    // (WWHD_TRACE_FUNCS=1) when a copy of that size is issued
    if (const char* e = []{ static const char* v = std::getenv("WWHD_COPY_TRACE"); return v; }()) {
        unsigned w = 0, h = 0;
        if (sscanf(e, "%ux%u", &w, &h) == 2 && ld32(arg(c, 3) + 4) == w && ld32(arg(c, 3) + 8) == h) {
            static int n = 0;
            char name[64];
            snprintf(name, sizeof name, "copy_trace_%d.txt", n++);
            if (FILE* f = fopen(name, "w")) { extern void trace_dump(FILE*, unsigned); fprintf(f, "copy by thread %08X lr %08X\n", threads::current_thread(), c->lr); trace_dump(f, 400); fclose(f); }
        }
    }
    // TTT2 port diagnostics: WWHD_COPY_SPY=WxH logs who writes the copy's source after the copy
    if (const char* e = []{ static const char* v = std::getenv("WWHD_COPY_SPY"); return v; }()) {
        unsigned w = 0, h = 0;
        if (sscanf(e, "%ux%u", &w, &h) == 2 && ld32(arg(c, 3) + 4) == w && ld32(arg(c, 3) + 8) == h) {
            LOG("[spy] copy from %08X (+%X) by thread %08X lr %08X", ld32(arg(c, 0) + 0x24), ld32(arg(c, 0) + 0x20), threads::current_thread(), c->lr);
            wwatch_spy_after_copy = ld32(arg(c, 0) + 0x24) | 1;
            wwatch_spy_size = ld32(arg(c, 0) + 0x20);
        }
    }
    // WWHD_COPY_LOG=1: every copy's surfaces (address, size, format, tile mode, levels)
    if (static const bool copyLog = getenv("WWHD_COPY_LOG") != nullptr; copyLog) {
        static int n = 0;
        if (n < 600) {
            ++n;
            const uint32 a = arg(c, 0), b = arg(c, 3);
            LOG("[copy] %08X %ux%u fmt %X tm %u mips %u (lvl %u slice %u) -> %08X %ux%u fmt %X tm %u mips %u (lvl %u slice %u) rec %d lr %08X",
                ld32(a + 0x24), ld32(a + 4), ld32(a + 8), ld32(a + 0x14), ld32(a + 0x30), ld32(a + 0x10), arg(c, 1), arg(c, 2),
                ld32(b + 0x24), ld32(b + 4), ld32(b + 8), ld32(b + 0x14), ld32(b + 0x30), ld32(b + 0x10), arg(c, 4), arg(c, 5),
                t_rec.start != 0, c->lr);
        }
    }
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kSurfaceWords);
    p.insert(p.end(), {arg(c, 1), arg(c, 2)});
    put_struct(p, arg(c, 3), kSurfaceWords);
    p.insert(p.end(), {arg(c, 4), arg(c, 5)});
    if (!t_rec.start && defer_linear_copies() && ld32(arg(c, 0) + 0x30) == 16) {
        std::lock_guard<std::mutex> lk(g_deferred_m);
        g_deferred_copies.push_back(std::move(p));
        return;
    }
    // A copy from or to a LINEAR_SPECIAL surface is a CPU copy in RAM, done at once even while a
    // display list is being recorded (Cemu: GX2CopySurface -> LatteSurfaceCopy_CopyInRAM). TTT2
    // creates textures inside its display lists: it OSBlockMoves the texture into a temporary
    // buffer, copies it into the tiled image and frees the buffer right away; recorded, the copy
    // ran when the list was called, reading the reused buffer (garbled fonts, icons and stages).
    const bool linear_special = ld32(arg(c, 0) + 0x30) == 16 || ld32(arg(c, 3) + 0x30) == 16;
    if (dbg) LOG("[copydbg] recording %d linear special %d", t_rec.start != 0, linear_special);
    if (t_rec.start && linear_special) {
        if (g_render_thread) {
            enqueue(OP_COPY_SURFACE, p.data(), (uint32)p.size());
            BlockingScope b;
            render_sync(kSyncCopySurface);
        } else {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            host::with_autorelease_pool([&] { execute_one(OP_COPY_SURFACE, p.data(), (uint32)p.size()); });
        }
        if (wwatch_spy_after_copy) { wwatch::spy(wwatch_spy_after_copy & ~1u, wwatch_spy_size); wwatch_spy_after_copy = 0; }
        return;
    }
    emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    // The copy is complete when GX2CopySurface returns: the game uses the result (and frees the
    // surfaces) right away. agl's tile-mode conversion (027B5EEC) copies into a temporary surface,
    // OSBlockMoves it back and frees it at once; executed later on the render thread, the copy
    // wrote into the freed memory after the heap had reused it (boot crash: agl shader program
    // array 21EFE28C, program 0's +0x7c zeroed). Not for display lists (they run when called).
    static const bool async_copy = getenv("WWHD_ASYNC_COPY") != nullptr;
    // TTT2: a copy between tiled surfaces stays on the GPU (nothing in guest memory changes), so the
    // game need not wait for it; waiting cost its fights 13 ms a frame (one such copy per frame)
    static const bool ttt2 = strcmp(g_guest_build_name, "TTT2") == 0;
    const uint32 dst_tm = ld32(arg(c, 3) + 0x30);
    const uint32 src_tm = ld32(arg(c, 0) + 0x30);
    const bool gpu_only = ttt2 && dst_tm >= 2 && dst_tm != 16 && src_tm >= 2 && src_tm != 16;
    if (ttt2 && !t_rec.start && !gpu_only) {
        static int logged = 0;
        if (logged < 12) {
            ++logged;
            LOG("[gx2] waited copy: src %ux%u fmt %X tm %u -> dst %ux%u fmt %X tm %u, lr %08X", ld32(arg(c, 0) + 4), ld32(arg(c, 0) + 8),
                ld32(arg(c, 0) + 0x14), src_tm, ld32(arg(c, 3) + 4), ld32(arg(c, 3) + 8), ld32(arg(c, 3) + 0x14), dst_tm, c->lr);
        }
    }
    if (!t_rec.start && !async_copy && !gpu_only) {
        BlockingScope b;
        render_sync(kSyncCopySurface);
    }
    if (wwatch_spy_after_copy) { wwatch::spy(wwatch_spy_after_copy & ~1u, wwatch_spy_size); wwatch_spy_after_copy = 0; }
}
HLE(gx2, GX2CopyColorBufferToScanBuffer) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    p.push_back(arg(c, 1));
    emit(OP_COPY_TO_SCAN, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ExpandAAColorBuffer) { emit(OP_EXPAND_COLOR, {arg(c, 0)}); }
HLE(gx2, GX2ExpandDepthBuffer) { emit(OP_EXPAND_DEPTH, {arg(c, 0)}); }
HLE(gx2, GX2Invalidate) { emit(OP_INVALIDATE, {arg(c, 0), arg(c, 1), arg(c, 2)}); }

// ---------------------------------------------------------------- submission and presentation
HLE(gx2, GX2Flush) { ubo_check_point("GX2Flush"); emit_host(OP_FLUSH, {}); }
HLE(gx2, GX2DrawDone) {
    run_deferred_copies();
    BlockingScope b;
    emit_host(OP_DRAW_DONE, {});
    render_sync(kSyncDrawDone);
    ret(c, 1);
}
extern std::atomic<uint64_t> g_fs_reads, g_fs_read_bytes;  // hle/fs.cpp
HLE(gx2, GX2SwapScanBuffers) {
    ubo_check_point("GX2SwapScanBuffers");
    run_deferred_copies();
    // TTT2 diagnostics: WWHD_HITCH_LOG=ms logs game frames (swap to swap) longer than that
    static const double hitch_ms = getenv("WWHD_HITCH_LOG") ? atof(getenv("WWHD_HITCH_LOG")) : 0;
    if (hitch_ms > 0) {
        static auto last = std::chrono::steady_clock::now();
        static uint64_t swaps = 0, logged = 0;
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - last).count();
        const uint64_t reads = g_fs_reads.exchange(0), bytes = g_fs_read_bytes.exchange(0);
        if (++swaps > 300 && ms > hitch_ms && logged < 400) {
            ++logged;
            LOG("[hitch] game frame %llu: %.1f ms, file reads %llu (%.1f KiB)", (unsigned long long)swaps, ms,
                (unsigned long long)reads, bytes / 1024.0);
        }
        last = now;
    }
    // debug: WWHD_TRACE_SWAP=n logs the guest call chain of the first n swaps
    static int trace = getenv("WWHD_TRACE_SWAP") ? atoi(getenv("WWHD_TRACE_SWAP")) : 0;
    if (trace > 0) {
        trace--;
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[gx2] swap from lr=%08X", c->lr);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 8 && sp; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp) break;
            n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
            sp = prev;
        }
        LOG("%s", buf);
    }
    {
        static const bool load = getenv("WWHD_TEST_GAME_LOAD_MS") != nullptr;
        if (load) test_frame_load("WWHD_TEST_GAME_LOAD_MS");
    }
    float a = aspect::on_swap();  // aspect ratio of the next frame (game projections, render targets)
    uint32 ab;
    memcpy(&ab, &a, 4);
    // the frame drew no new logic step (an interpolation hold pass): for the render-thread profiler
    static uint64_t lastSteps = ~0ull;
    const uint64_t steps = interp::logic_steps();
    const bool hold = steps == lastSteps;
    lastSteps = steps;
    // Opt-in correctness capture: select the guest frame before it enters
    // the asynchronous render queue. Renderer-frame parity is not a game clock.
    static const char* captureCounter = getenv("WWHD_TEST_CAPTURE_LOAD_COUNTER");
    static const char* captureStep = getenv("WWHD_TEST_CAPTURE_LOAD_STEP");
    bool capture = false;
    if ((captureCounter || captureStep) && ss::last_load_frame()) {
        static uint64_t capturedLoad = 0;
        const auto loaded = ss::last_load_frame();
        const uint32_t counter = ld32(GD(0x101FF560));
        const uint64_t offset = captureStep ? steps - ss::last_load_step()
                                            : uint32_t(counter - ss::last_load_counter());
        const uint64_t target = strtoull(captureStep ? captureStep : captureCounter, nullptr, 10);
        if (capturedLoad != loaded && !hold && offset == target) {
            capturedLoad = loaded;
            capture = true;
            LOG("[test] capture at loaded %s +%llu (game counter %08X), full pass",
                captureStep ? "logic step" : "game counter",(unsigned long long)offset,counter);
        }
    }
    if (capture) emit_host(OP_SWAP, {ab, hold ? 1u : 0u, 1u});
    else emit_host(OP_SWAP, {ab, hold ? 1u : 0u});
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        g_swap_count++;
        g_pending_flips.push_back({g_swap_count, std::chrono::steady_clock::now()});
    }
    // debug: WWHD_LOG_SLOW_SWAP=ms logs swaps that came more than ms after the previous one
    static const double slow_ms = getenv("WWHD_LOG_SLOW_SWAP") ? atof(getenv("WWHD_LOG_SLOW_SWAP")) : 0;
    if (slow_ms > 0) {
        static auto prev = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - prev).count();
        prev = now;
        if (ms > slow_ms) LOG("[gx2] slow swap %llu: %.1f ms", (unsigned long long)g_swap_count, ms);
    }
    if (g_swap_count % 300 == 1) {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        last = now;
        LOG("[gx2] frame %llu, %.1f swaps/s, swap interval %u", (unsigned long long)g_swap_count, 300 / s, g_swap_interval);
        if ([]{ static const char* v = std::getenv("WWHD_SCHED_STATS"); return v; }()) threads::report_sched(300);
    }
}
HLE(gx2, GX2GetSwapStatus) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    if (arg(c, 0)) st32(arg(c, 0), (uint32)(g_swap_count + g_count_offset));
    if (arg(c, 1)) st32(arg(c, 1), (uint32)(g_flip_count + g_count_offset));
    if (arg(c, 2)) st64(arg(c, 2), timebase::to_guest(g_last_flip_time));
    if (arg(c, 3)) st64(arg(c, 3), timebase::guest_now());
}
HLE(gx2, GX2SetSwapInterval) { g_swap_interval = std::max<uint32>(arg(c, 0), 1); }
static std::atomic<bool> g_trace_busy{false};
namespace gx2 { void set_trace_busy(bool busy) { g_trace_busy = busy; } }
HLE(gx2, GX2WaitForVsync) {
    if (g_trace_busy.load(std::memory_order_relaxed)) {
        BlockingScope b;
        for (int i = 0; i < 6000 && g_trace_busy.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(uncapped_benchmark()) {
        BlockingScope b;
        // The queue fence follows earlier swaps, whose presentation path waits
        // for GPU completion. Never wait while holding the flip mutex.
        render_sync(kSyncVsyncUncapped);
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        return;
    }
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipWait = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_WAIT");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    if(readyFlipWait) {
        bool eligible = false, needsSync = false;
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            if(!g_pending_flips.empty()) {
                const auto& front = g_pending_flips.front();
                const uint64_t at = flip_due(front);
                eligible = at <= vsync_index();
                if(eligible) needsSync = render::frames_completed() < front.swap;
            }
        }
        if(eligible) {
            if(needsSync) {
                BlockingScope b;
                render_sync(kSyncVsyncFlip); // Queued swap completion; never hold flip mutex.
            }
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips(); // Retains minimum interval and FIFO GPU guards.
            return;
        }
    }
#endif
    static const bool preciseSleep = [] {
#ifdef WWHD_HAS_VULKAN
        // Vulkan renderer's pacing (docs/vulkan.md); the Metal renderer keeps plain sleeping
        if (!render::vulkan()) return false;
        const char* value = getenv("WWHD_VSYNC_PRECISE");
#ifdef __APPLE__
        // Avoid the measured macOS sleep overshoot; explicit zero opts out.
        return !value || atoi(value) != 0;
#else
        return value && atoi(value) != 0;
#endif
#else
        return false;
#endif
    }();
    const uint64_t granule = vsync_granule();
    const auto deadline = tick_time((vsync_index() / granule + 1) * granule);  // the next (virtual) vsync
    if (wait_for_late_flip(deadline)) return;
#ifdef WWHD_HAS_VULKAN
    static const bool readyFlipPark = [] {
        const char* value = getenv("WWHD_VK_READY_FLIP_PARK");
        return render::vulkan() && value && !strcmp(value, "1");
    }();
    threads::park_sleep_until(deadline, preciseSleep,
        readyFlipPark ? ready_flip_before_resume : nullptr);
#else
    threads::park_sleep_until(deadline, preciseSleep);
#endif
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    static uint64_t calls = 0;
    if (getenv("WWHD_LOG_VSYNC") && ++calls % 60 == 0)
        LOG("[gx2] vsync %llu: swaps %llu flips %llu pending %zu", (unsigned long long)vsync_index(),
            (unsigned long long)g_swap_count, (unsigned long long)g_flip_count, g_pending_flips.size());
}

// scan buffers: the game renders into its own color buffers and copies to "scan buffers";
// the renderer presents whatever was copied to the TV target.
// (buffer, size, mode, surfaceFormat, bufferingMode): an sRGB format means scan-out applies the encoding
HLE(gx2, GX2SetTVBuffer) {
    LOG("[gx2] TV buffer format %X", arg(c, 3));
    render::set_tv_format(arg(c, 3), true);
}
HLE(gx2, GX2SetDRCBuffer) { render::set_tv_format(arg(c, 3), false); }
HLE(gx2, GX2SetTVScale) {}
HLE(gx2, GX2SetDRCScale) {}
HLE(gx2, GX2SetTVEnable) {}
HLE(gx2, GX2SetDRCEnable) {}
HLE(gx2, GX2CalcTVSize) {
    // (mode, format, bufferingMode, uint32* size, bool* scaleNeeded)
    uint32 mode = arg(c, 0), buffers = std::max<uint32>(arg(c, 2), 1);
    uint32 w = mode >= 5 ? 1920 : mode <= 2 ? 854 : 1280, h = mode >= 5 ? 1080 : mode <= 2 ? 480 : 720;
    st32(arg(c, 3), w * h * 4 * buffers);
    st32(arg(c, 4), 0);
}
HLE(gx2, GX2CalcDRCSize) {
    st32(arg(c, 3), 854 * 480 * 4 * std::max<uint32>(arg(c, 2), 1));
    st32(arg(c, 4), 0);
}

// ---------------------------------------------------------------- misc queries
HLE(gx2, GX2TempGetGPUVersion) { ret(c, 2); }
HLE(gx2, GX2CalcGeometryShaderInputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcFetchShaderSizeEx) {
    uint32 n = arg(c, 0);
    uint32 cf = ((((n + 15) / 16) + 1) * 8 + 0xF) & ~0xFu;
    ret(c, std::max<uint32>(cf + n * 16, 16 + n * 16));
}
HLE(gx2, GX2GPUTimeToCPUTime) { ret64(c, arg64(c, 3)); }
HLE(gx2, GX2SampleTopGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }
HLE(gx2, GX2SampleBottomGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }

// ---------------------------------------------------------------- save states

// the game is frozen between frames: finish all queued GPU work and let pending flips execute, so no
// command reads guest memory while it is replaced and the swap/flip counts agree
void gx2_ss_drain() {
    emit_host(OP_DRAW_DONE, {1});  // full GPU wait, also with lazy DrawDone
    render_sync(kSyncSaveState);
    for (int i = 0; i < 300; i++) {
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            if (g_pending_flips.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG("[savestate] flips still pending after 300 ms");
}

void gx2_ss_save(ss::Writer& w) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    w.u32(kNumRegs);
    w.bytes(g_regs, sizeof g_regs);
    uint32 active = 0;
    std::vector<uint32> keys;
    for (auto& [k, v] : g_contexts) {
        keys.push_back(k);
        if (g_shadow == v.data()) active = k;
    }
    std::sort(keys.begin(), keys.end());
    w.u32((uint32)keys.size());
    for (uint32 k : keys) {
        w.u32(k);
        w.u32((uint32)g_contexts[k].size());
        w.bytes(g_contexts[k].data(), g_contexts[k].size() * 4);
    }
    w.u32(active);
    w.u32(g_swap_interval);
    std::lock_guard<std::mutex> fl(g_flip_mutex);
    w.u64(g_swap_count + g_count_offset);
}

bool gx2_ss_check(ss::Reader r, std::string& why) {
    if (r.u32() != kNumRegs) { why = "GX2 register file size differs"; return false; }
    return r.ok;
}

void gx2_ss_load(ss::Reader& r) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    r.u32();
    r.bytes(g_regs, sizeof g_regs);
    g_register_blocks = {};
    g_register_blocks.include(g_regs);
    g_contexts.clear();
    uint32 n = r.u32();
    for (uint32 i = 0; i < n && r.ok; i++) {
        uint32 k = r.u32(), words = r.u32();
        auto& v = g_contexts[k];
        v.resize(words);
        r.bytes(v.data(), (size_t)words * 4);
        if (words == kNumRegs) g_register_blocks.include(v.data());
    }
    uint32 active = r.u32();
    auto it = g_contexts.find(active);
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    g_shader_state_gen++;
    g_swap_interval = std::max<uint32>(r.u32(), 1);
    uint64_t guest_swaps = r.u64();
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        g_count_offset = (int64_t)guest_swaps - (int64_t)g_swap_count;
    }
    render::ss_reset();  // the renderer forgets surface contents and shader memos (Vulkan)
}
uint32_t gx2_swap_interval() { return g_swap_interval; }  // GX2GetSwapInterval (gx2_ttt2.cpp)
