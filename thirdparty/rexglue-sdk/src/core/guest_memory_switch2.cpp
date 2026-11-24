/*
 * Guest memory on Switch 2 under kaiHEN: Backend::Heap.
 *
 * Same API as guest_memory_switch.cpp (guest_memory_switch.h), different
 * mechanism. Ported from LostOdysseyRecomp's
 * LostOdysseyRecomp/kernel/guest_address_space_switch.cpp (commit 1eadf13b,
 * "pure userland heap backend (zero svcMap* syscalls)", and heap_backend_fix.txt
 * in that repository).
 *
 * Why
 *
 * kaiHEN runs the NRO inside a donor game's process, under that title's NPDM
 * syscall mask. svcMapProcessCodeMemory (0x77), svcMapProcessMemory (0x74),
 * svcSetProcessMemoryPermission (0x73), svcMapTransferMemory (0x51),
 * svcMapMemory (0x04) and svcMapPhysicalMemory (0x2C) are outside it. A masked
 * SVC does not return an error: the kernel ends the process (LO, early_boot.log
 * of 2026-11-16, two boots). The Switch 1 design (code-memory shadow plus one
 * svcMapProcessMemory per view) therefore cannot run at all.
 *
 * The design
 *
 * - The whole 4 GiB guest window is one memalign() block from the newlib heap,
 *   which hbloader/kaiHEN's loader already backed with physical memory. No SVC.
 * - Every guest address 0x00000000..0xFFFFFFFF is an offset into that block, so
 *   the recompiled code's base + address keeps working unchanged.
 * - There are no hardware aliases. The views Xenia mirrors onto the same file
 *   offset (0x7F000000, 0xA0000000, 0xC0000000, 0xE0000000 over physical memory;
 *   0x90000000 over 0x80000000) are separate ranges of the block. Xenia's
 *   physical heaps hand each physical page to exactly one of A, C and E, so the
 *   guest itself only ever sees one copy; host code that reaches a physical
 *   address through Memory::TranslatePhysical is sent to that page's own view by
 *   the per-page home table in xmemory.cpp (REX_PLATFORM_SWITCH2).
 * - The raw physical view (0x100000000..0x11FFFFFFF in the Xenia layout) is not
 *   part of the block: xmemory.cpp points physical_membase_ at the A view.
 * - Commit zeroes a page the first time it is committed (memalign memory is
 *   uninitialized) and records it. Decommit keeps the contents, as the Switch 1
 *   and Linux builds do (their backing outlives a decommit too).
 * - Protection is logical only: recorded, answered by RexGmQueryProtect, never
 *   enforced. Without svcSetProcessMemoryPermission there is no way to make a
 *   heap page fault. Write watches (Xenia's access callbacks) therefore never
 *   fire; the same loss LO documented for its heap backend.
 */

#if defined(__SWITCH__) && defined(__SWITCH2__)

#include "guest_memory_switch.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>

namespace {

constexpr size_t kPageSize = 0x1000;
constexpr size_t kPageShift = 12;
/* The guest's 32-bit address space: everything the recompiled code can reach. */
constexpr size_t kWindowSize = 0x100000000ull;

constexpr size_t AlignDown(size_t v, size_t a) { return v & ~(a - 1); }
constexpr size_t AlignUp(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

struct View {
    uint8_t* base;
    size_t offset;  /* mapping (file) offset this view starts at */
    size_t length;
};

std::atomic<uint8_t*> g_base_rapido{nullptr};
std::atomic<size_t> g_size_rapido{0};

struct State {
    std::mutex mutex;
    uint8_t* base = nullptr;
    size_t size = 0;
    std::vector<View> views;
    /* One bit per 4 KiB page of the window: committed (and zeroed) at least once. */
    std::vector<uint64_t> committed_bits;
    size_t committed_pages = 0;
    /* Logical permissions of pages that are not REX_GM_WRITE, keyed by window offset. */
    std::map<size_t, RexGmAccess> protection;
    uint32_t last_result = 0;
};

State& S() {
    static State s;
    return s;
}

bool IsCommittedPage(const State& s, size_t page) {
    return (s.committed_bits[page >> 6] >> (page & 63)) & 1;
}

/* Zeroes and records the pages of [win_off, win_off + len) not committed before. */
void CommitWindowRange(State& s, size_t win_off, size_t len) {
    const size_t first = AlignDown(win_off, kPageSize) >> kPageShift;
    const size_t last = AlignUp(win_off + len, kPageSize) >> kPageShift;
    size_t run_start = SIZE_MAX;
    auto flush = [&](size_t end) {
        if (run_start != SIZE_MAX) {
            memset(s.base + (run_start << kPageShift), 0, (end - run_start) << kPageShift);
            run_start = SIZE_MAX;
        }
    };
    for (size_t page = first; page < last; ++page) {
        if (IsCommittedPage(s, page)) {
            flush(page);
            continue;
        }
        s.committed_bits[page >> 6] |= uint64_t(1) << (page & 63);
        ++s.committed_pages;
        if (run_start == SIZE_MAX) run_start = page;
    }
    flush(last);
}

const View* ViewFor(const State& s, uint64_t addr) {
    for (const View& v : s.views) {
        const uint64_t lo = reinterpret_cast<uint64_t>(v.base);
        if (addr >= lo && addr < lo + v.length) return &v;
    }
    return nullptr;
}

bool IsCommittedWindowAddress(const State& s, uint64_t addr) {
    const uint64_t b = reinterpret_cast<uint64_t>(s.base);
    if (addr < b || addr >= b + s.size) return false;
    return IsCommittedPage(s, size_t(addr - b) >> kPageShift);
}

RexGmAccess AccessOf(const State& s, size_t page_off) {
    auto it = s.protection.find(page_off);
    return it == s.protection.end() ? REX_GM_WRITE : it->second;
}

}  // namespace

extern "C" {

uint8_t* RexGmInit(size_t size) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.base) return s.base;

    const size_t want = AlignUp(std::min(size, kWindowSize), kPageSize);
    uint8_t* block = static_cast<uint8_t*>(memalign(kPageSize, want));
    if (!block) {
        const struct mallinfo mi = mallinfo();
        fprintf(stderr,
                "[guest memory] Backend::Heap: memalign(0x%zx) failed (arena %zu MiB, in use %zu MiB). "
                "Start re:Blue through kaiHEN's title takeover (hold R on a game).\n",
                want, size_t(mi.arena) >> 20, size_t(mi.uordblks) >> 20);
        s.last_result = REX_GM_ERR_NOMEM;
        return nullptr;
    }
    s.base = block;
    s.size = want;
    s.committed_bits.assign((want >> kPageShift) / 64 + 1, 0);
    s.committed_pages = 0;
    fprintf(stderr, "[guest memory] Backend::Heap: 4 GiB window at %p (0x%zx bytes, no svcMap* syscalls)\n",
            static_cast<void*>(block), want);
    g_size_rapido.store(want, std::memory_order_release);
    g_base_rapido.store(block, std::memory_order_release);
    return block;
}

void RexGmShutdown(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    g_base_rapido.store(nullptr, std::memory_order_release);
    g_size_rapido.store(0, std::memory_order_release);
    free(s.base);
    s.base = nullptr;
    s.size = 0;
    s.views.clear();
    s.committed_bits.clear();
    s.committed_pages = 0;
    s.protection.clear();
}

uint8_t* RexGmBase(void) { return g_base_rapido.load(std::memory_order_acquire); }

size_t RexGmSize(void) { return g_size_rapido.load(std::memory_order_acquire); }

bool RexGmWindowToOffset(uint64_t window_address, size_t* offset_out) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    const View* v = ViewFor(s, window_address);
    if (!v) return false;
    if (offset_out) {
        *offset_out = v->offset + size_t(window_address - reinterpret_cast<uint64_t>(v->base));
    }
    return true;
}

bool RexGmAddView(uint8_t* base, size_t offset, size_t length) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    if (base < s.base || size_t(base - s.base) + length > s.size) {
        /* Outside the 4 GiB block: xmemory.cpp leaves the raw physical view out on Switch 2. */
        return false;
    }
    s.views.push_back(View{base, offset, length});
    return true;
}

bool RexGmCommit(size_t offset, size_t length, RexGmAccess access) {
    (void)access; /* applied by RexGmProtect, as on Switch 1 */
    if (length == 0) return true;
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    /*
     * Every view over this mapping offset gets the range. They are separate
     * ranges here, and xmemory.cpp commits physical memory through the parent
     * heap (A view) and the owning heap (A, C or E) separately, so this only
     * zeroes ranges no live allocation uses yet.
     */
    const size_t end = offset + length;
    bool any = false;
    for (const View& v : s.views) {
        const size_t lo = std::max(offset, v.offset);
        const size_t hi = std::min(end, v.offset + v.length);
        if (lo >= hi) continue;
        any = true;
        CommitWindowRange(s, size_t(v.base - s.base) + (lo - v.offset), hi - lo);
    }
    return any;
}

bool RexGmDecommit(size_t offset, size_t length) {
    /* The contents stay: the block is never given back while the process runs. */
    (void)offset;
    (void)length;
    return S().base != nullptr;
}

bool RexGmFaultIn(uint64_t window_address) {
    /* Nothing is ever unmapped in the block: a fault inside it is a real error. */
    (void)window_address;
    return false;
}

bool RexGmProtect(uint8_t* address, size_t length, RexGmAccess access, RexGmAccess* out_old) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    const uint64_t addr = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr + length > wbase + s.size) return false;

    const size_t begin = AlignDown(size_t(addr - wbase), kPageSize);
    const size_t end = AlignUp(size_t(addr - wbase) + length, kPageSize);
    if (out_old) *out_old = AccessOf(s, begin);

    if (access == REX_GM_WRITE) {
        /* Common case (every AllocFixed commit): nothing recorded in the range. */
        auto it = s.protection.lower_bound(begin);
        while (it != s.protection.end() && it->first < end) it = s.protection.erase(it);
        return true;
    }
    for (size_t p = begin; p < end; p += kPageSize) s.protection[p] = access;
    return true;
}

bool RexGmQueryProtect(uint8_t* address, size_t* length, RexGmAccess* out_access) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.base) return false;
    const uint64_t addr = reinterpret_cast<uint64_t>(address);
    const uint64_t wbase = reinterpret_cast<uint64_t>(s.base);
    if (addr < wbase || addr >= wbase + s.size) return false;

    const size_t begin = AlignDown(size_t(addr - wbase), kPageSize);
    /* Uncommitted reads as no access (see guest_memory_switch.cpp: the MMIO retry loop). */
    if (!IsCommittedWindowAddress(s, wbase + begin)) {
        if (out_access) *out_access = REX_GM_NONE;
        if (length) *length = kPageSize;
        return true;
    }
    const RexGmAccess a = AccessOf(s, begin);
    if (out_access) *out_access = a;
    size_t maximo = (length && *length) ? *length : kPageSize;
    if (begin + maximo > s.size) maximo = s.size - begin;
    size_t p = begin;
    while (p < begin + maximo && AccessOf(s, p) == a && IsCommittedWindowAddress(s, wbase + p)) p += kPageSize;
    if (length) *length = p - begin;
    return true;
}

void* RexGmShadowFor(uint64_t window_address) {
    /* The block itself is always accessible: it is its own shadow. */
    const uint64_t b = reinterpret_cast<uint64_t>(RexGmBase());
    if (!b || window_address < b || window_address >= b + RexGmSize()) return nullptr;
    return reinterpret_cast<void*>(window_address);
}

size_t RexGmCommittedBytes(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.committed_pages << kPageShift;
}

size_t RexGmMappedBytes(void) {
    /* No mirrors and no mappings: the committed figure. */
    return RexGmCommittedBytes();
}

int RexGmModoProteccion(void) { return 0; }

uint32_t RexGmLastResult(void) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.last_result;
}

void RexGmDescribeKernelUse(char* out, size_t out_size) {
    u64 mem_total = 0, mem_used = 0;
    svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    const struct mallinfo mi = mallinfo();
    snprintf(out, out_size,
             "Backend::Heap (no mappings): committed %zu MiB of the 4 GiB window; newlib arena %zu MiB, "
             "in use %zu MiB; process memory %llu/%llu MiB",
             RexGmCommittedBytes() >> 20, size_t(mi.arena) >> 20, size_t(mi.uordblks) >> 20,
             (unsigned long long)(mem_used >> 20), (unsigned long long)(mem_total >> 20));
}

}  // extern "C"

#endif /* __SWITCH__ && __SWITCH2__ */
