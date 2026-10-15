// Cafe OS calls that Tekken Tag Tournament 2 imports and Wind Waker HD does not: system services,
// DMA engine, locked cache, and the online libraries (reported unavailable, as in system_stubs.cpp).
#include "../runtime.h"
#include <cstring>
#include <mutex>

static constexpr uint32_t kResultOk = 0;
static constexpr uint32_t kResultFail = 0xA0000000;

// ---- coreinit: system information
HLE(coreinit, OSGetMemBound) {
    // type 1: MEM1 (32 MiB), type 2: MEM2 (the application heap area)
    uint32_t type = arg(c, 0), pa = arg(c, 1), ps = arg(c, 2);
    LOG("[mem] OSGetMemBound(%u) lr %08X", type, c->lr);
    if (type == 1) { st32(pa, mem::kMem1); st32(ps, mem::kMem1Size); ret(c, 0); return; }
    if (type == 2) { st32(pa, mem::kMem2Start); st32(ps, mem::kMem2End - mem::kMem2Start); ret(c, 0); return; }
    ret(c, (uint32_t)-1);
}
HLE(coreinit, OSGetForegroundBucket) {
    if (arg(c, 0)) st32(arg(c, 0), mem::kFgBucket);
    if (arg(c, 1)) st32(arg(c, 1), mem::kFgBucketSize);
    ret(c, 1);
}
HLE(coreinit, OSSetScreenCapturePermission) { ret(c, 1); }
HLE(coreinit, OSEnforceInorderIO) {}
HLE(coreinit, OSDriver_Register) { ret(c, 0); }
HLE(coreinit, bspGetHardwareVersion) { if (arg(c, 0)) st32(arg(c, 0), 0x26100010); ret(c, 0); }  // retail CAT-R
HLE(coreinit, OSReadRegister16) { ret(c, 0); }
HLE(coreinit, OSWriteRegister16) {}
HLE(coreinit, __OSReadRegister32Ex) { ret(c, 0); }
HLE(coreinit, __OSWriteRegister32Ex) {}
HLE(coreinit, MCP_Open) { ret(c, 1); }
HLE(coreinit, MCP_Close) { ret(c, 0); }
HLE(coreinit, MCP_GetSysProdSettings) {
    // MCPSysProdSettings (0x46 bytes): +0x03 platform region, +0x07 game region; 4 = Europe (this dump)
    uint32_t out = arg(c, 1);
    memset(mem::ptr(out), 0, 0x46);
    st8(out + 0x03, 4);
    st8(out + 0x07, 4);
    ret(c, 0);
}
HLE(coreinit, UCWriteSysConfig) { ret(c, 0); }
HLE(coreinit, UCDeleteSysConfig) { ret(c, 0); }

// ---- coreinit: locked cache (16 KiB scratchpad per core) and its DMA
static std::mutex g_lc_m;
static uint32_t g_lc_base = 0, g_lc_used = 0;
static constexpr uint32_t kLcSize = 0x4000;
HLE(coreinit, LCAlloc) {
    std::lock_guard<std::mutex> lk(g_lc_m);
    if (!g_lc_base) g_lc_base = mem::runtime_alloc(kLcSize, 0x200);
    uint32_t size = (arg(c, 0) + 0x1FF) & ~0x1FFu;
    if (g_lc_used + size > kLcSize) { ret(c, 0); return; }
    uint32_t a = g_lc_base + g_lc_used;
    g_lc_used += size;
    ret(c, a);
}
HLE(coreinit, LCDealloc) {
    std::lock_guard<std::mutex> lk(g_lc_m);
    if (arg(c, 0) == g_lc_base) g_lc_used = 0;  // the game frees what it allocated as one block
}
HLE(coreinit, LCGetUnallocated) { std::lock_guard<std::mutex> lk(g_lc_m); ret(c, kLcSize - g_lc_used); }
HLE(coreinit, LCEnableDMA) { ret(c, 1); }
HLE(coreinit, LCStoreDMABlocks) {
    static int logged = 0;
    if (logged++ < 10) LOG("[lc] store %08X <- %08X blocks %u lr %08X", arg(c, 0), arg(c, 1), arg(c, 2), c->lr);
    uint32_t blocks = arg(c, 2) ? arg(c, 2) : 128;  // 0 means 128 blocks of 32 bytes
    memmove(mem::ptr(arg(c, 0)), mem::ptr(arg(c, 1)), blocks * 32);
}

// ---- dmae (DMA engine): sizes in 32-bit words; returns a timestamp for DMAEWaitDone
static void dmae_swap(uint8_t* p, uint32_t bytes, uint32_t endian) {
    if (endian == 1) for (uint32_t i = 0; i + 1 < bytes; i += 2) std::swap(p[i], p[i + 1]);
    else if (endian == 2) for (uint32_t i = 0; i + 3 < bytes; i += 4) { std::swap(p[i], p[i + 3]); std::swap(p[i + 1], p[i + 2]); }
}
HLE(dmae, DMAECopyMem) {
    static int logged = 0;
    if (logged++ < 20) LOG("[dmae] copy %08X <- %08X words %u endian %u lr %08X", arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), c->lr);
    uint32_t bytes = arg(c, 2) * 4;
    memmove(mem::ptr(arg(c, 0)), mem::ptr(arg(c, 1)), bytes);
    dmae_swap(mem::ptr(arg(c, 0)), bytes, arg(c, 3));
    ret64(c, timebase::guest_now());
}
HLE(dmae, DMAEFillMem) {
    uint32_t dst = arg(c, 0), v = arg(c, 1), n = arg(c, 2);
    for (uint32_t i = 0; i < n; i++) st32(dst + 4 * i, v);
    ret64(c, timebase::guest_now());
}
HLE(dmae, DMAEWaitDone) { ret(c, 1); }

// ---- proc_ui, vpad, nn_save
HLE(proc_ui, ProcUIClearCallbacks) {}
HLE(proc_ui, ProcUIRegisterBackgroundCallback) {}
HLE(proc_ui, ProcUISetMEM1Storage) { ret(c, 0); }
HLE(vpad, VPADInit) {}
HLE(vpad, VPADSetAccParam) {}
HLE(vpad, VPADSetBtnRepeat) {}
HLE(nn_save, SAVERollbackQuota) { ret(c, 0); }

// ---- online services: not available
HLE(nn_ac, ConnectAsync__Q2_2nn2acFv) { ret(c, kResultFail); }
HLE(nn_ac, GetAssignedAddress__Q2_2nn2acFPUl) { if (arg(c, 0)) st32(arg(c, 0), 0); ret(c, kResultFail); }
HLE(nn_ac, GetAssignedSubnet__Q2_2nn2acFPUl) { if (arg(c, 0)) st32(arg(c, 0), 0); ret(c, kResultFail); }
HLE(nn_ac, GetStatus__Q2_2nn2acFPQ3_2nn2ac6Status) { if (arg(c, 0)) st32(arg(c, 0), (uint32_t)-1); ret(c, kResultFail); }
HLE(nn_ac, IsApplicationConnected__Q2_2nn2acFPb) { if (arg(c, 0)) st8(arg(c, 0), 0); ret(c, kResultOk); }

HLE(nn_acp, ACPGetNetworkTime) { ret(c, (uint32_t)-1); }
HLE(nn_acp, ACPConvertNetworkTimeToOSCalendarTime) { ret(c, (uint32_t)-1); }

HLE(nn_act, GetNumOfAccounts__Q2_2nn3actFv) { ret(c, 1); }
HLE(nn_act, GetAccountId__Q2_2nn3actFPc) { mem::write_cstr(arg(c, 0), "Player", 17); ret(c, kResultOk); }
HLE(nn_act, GetCountry__Q2_2nn3actFPc) { mem::write_cstr(arg(c, 0), "GB", 3); ret(c, kResultOk); }
HLE(nn_act, GetPersistentId__Q2_2nn3actFv) { ret(c, 0x80000001); }
HLE(nn_act, GetPrincipalIdEx__Q2_2nn3actFPUiUc) { if (arg(c, 0)) st32(arg(c, 0), 0); ret(c, kResultOk); }
HLE(nn_act, IsNetworkAccount__Q2_2nn3actFv) { ret(c, 0); }
HLE(nn_act, HasNfsAccount__Q2_2nn3actFv) { ret(c, 0); }
HLE(nn_act, GetUtcOffset__Q2_2nn3actFv) { ret64(c, 0); }
HLE(nn_act, GetErrorCode__Q2_2nn3actFRCQ2_2nn6Result) { ret(c, 0); }
HLE(nn_act, AcquireNexServiceToken__Q2_2nn3actFP26ACTNexAuthenticationResultUi) { ret(c, kResultFail); }
HLE(nn_act, GetHostServerSettings__Q2_2nn3actFP11ACTNnasTypeP10ACTNfsTypePUcUc) { ret(c, kResultFail); }
HLE(nn_act, GetNfsPasswordEx__Q2_2nn3actFPcUc) { ret(c, kResultFail); }

HLE(nn_fp, Initialize__Q2_2nn2fpFv) { ret(c, kResultOk); }
HLE(nn_fp, Finalize__Q2_2nn2fpFv) { ret(c, kResultOk); }
HLE(nn_fp, IsInitialized__Q2_2nn2fpFv) { ret(c, 1); }
HLE(nn_fp, AddRecentPlayRecord__Q2_2nn2fpFPCQ3_2nn2fp16RecentPlayRecordUi) { ret(c, kResultOk); }
HLE(nn_fp, RegisterAccountAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv) { ret(c, kResultFail); }
HLE(nn_fp, ResultToErrorCode__Q2_2nn2fpFQ2_2nn6Result) { ret(c, 0); }

HLE(nn_ndm, Initialize__Q2_2nn3ndmFv) { ret(c, kResultOk); }
HLE(nn_ndm, Finalize__Q2_2nn3ndmFv) { ret(c, kResultOk); }
HLE(nn_ndm, EnableHomeButtonMenuAndBgProcess__Q2_2nn3ndmFi) { ret(c, kResultOk); }
HLE(nn_ndm, SuspendDaemons__Q2_2nn3ndmFUi) { ret(c, kResultOk); }
HLE(nn_ndm, ResumeDaemons__Q2_2nn3ndmFUi) { ret(c, kResultOk); }

// sockets: no network. Byte order helpers are identities (the guest is big-endian).
static uint32_t g_sock_errno_slot = 0;
HLE(nsysnet, socket) { ret(c, (uint32_t)-1); }
HLE(nsysnet, socketclose) { ret(c, 0); }
HLE(nsysnet, socketlasterr) { ret(c, 1); }
HLE(nsysnet, bind) { ret(c, (uint32_t)-1); }
HLE(nsysnet, connect) { ret(c, (uint32_t)-1); }
HLE(nsysnet, getsockname) { ret(c, (uint32_t)-1); }
HLE(nsysnet, recv) { ret(c, (uint32_t)-1); }
HLE(nsysnet, recvfrom) { ret(c, (uint32_t)-1); }
HLE(nsysnet, send) { ret(c, (uint32_t)-1); }
HLE(nsysnet, sendto) { ret(c, (uint32_t)-1); }
HLE(nsysnet, select) { ret(c, 0); }
HLE(nsysnet, setsockopt) { ret(c, (uint32_t)-1); }
HLE(nsysnet, shutdown) { ret(c, (uint32_t)-1); }
HLE(nsysnet, gethostbyname) { ret(c, 0); }
HLE(nsysnet, htonl) { ret(c, arg(c, 0)); }
HLE(nsysnet, ntohl) { ret(c, arg(c, 0)); }
HLE(nsysnet, htons) { ret(c, arg(c, 0) & 0xFFFF); }
HLE(nsysnet, ntohs) { ret(c, arg(c, 0) & 0xFFFF); }
HLE(nsysnet, inet_pton) { ret(c, 0); }
HLE(nsysnet, inet_ntoa) {
    if (!g_sock_errno_slot) { g_sock_errno_slot = mem::runtime_alloc(16); mem::write_cstr(g_sock_errno_slot, "0.0.0.0", 16); }
    ret(c, g_sock_errno_slot);
}
HLE(nsysnet, NSSLCreateContext) { ret(c, (uint32_t)-1); }
HLE(nsysnet, NSSLDestroyContext) { ret(c, 0); }
HLE(nsysnet, NSSLAddServerPKIGroups) { ret(c, (uint32_t)-1); }

// libcurl: handles exist, transfers fail at once (CURLE_COULDNT_CONNECT = 7)
static uint32_t curl_handle() { return mem::runtime_alloc(64); }
HLE(nlibcurl, curl_easy_init) { ret(c, curl_handle()); }
HLE(nlibcurl, curl_easy_cleanup) {}
HLE(nlibcurl, curl_easy_reset) {}
HLE(nlibcurl, curl_easy_setopt) { ret(c, 0); }
HLE(nlibcurl, curl_easy_pause) { ret(c, 0); }
HLE(nlibcurl, curl_easy_getinfo) { if (arg(c, 2)) st32(arg(c, 2), 0); ret(c, 0); }
HLE(nlibcurl, curl_easy_strerror) {
    static uint32_t s = 0;
    if (!s) { s = mem::runtime_alloc(32); mem::write_cstr(s, "network unavailable", 32); }
    ret(c, s);
}
HLE(nlibcurl, curl_multi_init) { ret(c, curl_handle()); }
HLE(nlibcurl, curl_multi_cleanup) { ret(c, 0); }
HLE(nlibcurl, curl_multi_add_handle) { ret(c, 0); }
HLE(nlibcurl, curl_multi_remove_handle) { ret(c, 0); }
HLE(nlibcurl, curl_multi_perform) { if (arg(c, 1)) st32(arg(c, 1), 0); ret(c, 0); }
HLE(nlibcurl, curl_multi_fdset) { if (arg(c, 4)) st32(arg(c, 4), (uint32_t)-1); ret(c, 0); }
HLE(nlibcurl, curl_multi_timeout) { if (arg(c, 1)) st32(arg(c, 1), (uint32_t)-1); ret(c, 0); }
HLE(nlibcurl, curl_multi_info_read) { if (arg(c, 1)) st32(arg(c, 1), 0); ret(c, 0); }
HLE(nlibcurl, curl_share_init) { ret(c, curl_handle()); }
HLE(nlibcurl, curl_share_cleanup) { ret(c, 0); }
HLE(nlibcurl, curl_share_setopt) { ret(c, 0); }
HLE(nlibcurl, curl_slist_append) { ret(c, arg(c, 0) ? arg(c, 0) : curl_handle()); }
HLE(nlibcurl, curl_slist_free_all) {}
