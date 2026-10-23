// Deterministic mode (SWE1R_DETERMINISTIC=1, or netplay): N64ModernRuntime's ultramodern::det
// delivers retraces, timers and task completions at fixed points, so the same inputs give the same
// game. This file adds the game-side checks: SWE1R_HASHLOG (a hash of game RAM per retrace) and
// SWE1R_DUMPFRAME (a RAM dump at one retrace), to compare two runs or two machines.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

extern uint8_t* g_rdram_base_for_det; // set by main.cpp's on_game_init

namespace {
FILE* g_hashlog = [] { const char* p = std::getenv("SWE1R_HASHLOG"); return p ? std::fopen(p, "w") : nullptr; }();
const long long g_dump_frame = [] { const char* e = std::getenv("SWE1R_DUMPFRAME"); return e ? std::atoll(e) : -1LL; }();
void (*g_next_callback)(uint64_t) = nullptr;

// The runtime keeps a native pointer at OSThread+0x20 (its thread context), which differs per run.
// ponytail: the game's four OSThreads are fixed addresses; track osCreateThread if that changes.
constexpr uint32_t kThreadContextFields[] = { 0x000D7770, 0x000D7A20, 0x00149EF0, 0x0014B200 };
constexpr uint32_t kGameRamEnd = 0x00600000; // framebuffers (and RT64 write-backs) live above this

bool masked(uint32_t offset) {
    for (uint32_t f : kThreadContextFields) if (offset == f || offset == f + 4) return true;
    return false;
}
} // namespace

// FNV-1a over game RAM, skipping the per-run fields above.
uint64_t det_hash_game_ram() {
    const uint8_t* rdram = g_rdram_base_for_det;
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t i = 0; i < kGameRamEnd; i += 4) {
        if (masked(i)) continue;
        h ^= *reinterpret_cast<const uint32_t*>(rdram + i);
        h *= 1099511628211ULL;
    }
    return h;
}

bool det_enabled() { return ultramodern::det::enabled(); }
uint64_t det_frame() { return ultramodern::det::retrace_count(); }

static void on_retrace(uint64_t retrace) {
    const uint8_t* rdram = g_rdram_base_for_det;
    if (rdram && g_hashlog) {
        std::fprintf(g_hashlog, "%llu %016llx", (unsigned long long)retrace, (unsigned long long)det_hash_game_ram());
        // SWE1R_HASHBLOCKS=1: also one short hash per 64 KB block, to locate a divergence.
        static const bool blocks = std::getenv("SWE1R_HASHBLOCKS") != nullptr;
        if (blocks) {
            for (uint32_t b = 0; b < kGameRamEnd; b += 0x10000) {
                uint32_t h = 2166136261u;
                for (uint32_t i = b; i < b + 0x10000; i += 4) {
                    if (!masked(i)) { h ^= *reinterpret_cast<const uint32_t*>(rdram + i); h *= 16777619u; }
                }
                std::fprintf(g_hashlog, " %08x", h);
            }
        }
        std::fputc('\n', g_hashlog);
        if ((retrace % 60) == 0) std::fflush(g_hashlog);
    }
    if (rdram && (long long)retrace == g_dump_frame) {
        std::string path = "ramdump_" + std::to_string(retrace) + ".bin";
        if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(rdram, 1, 0x00800000, f); std::fclose(f); }
        std::fprintf(stderr, "[det] dumped RDRAM at retrace %llu to %s\n", (unsigned long long)retrace, path.c_str());
    }
    if (g_next_callback) g_next_callback(retrace);
}

// Turns on deterministic mode; `then` (netplay) runs after the checks at each retrace.
void det_start(void (*then)(uint64_t)) {
    g_next_callback = then;
    ultramodern::det::enable();
    ultramodern::det::set_frame_callback(on_retrace);
}

// The game's osGetCount (racer_osGetCount, redirected by tools/post_recomp_patches.py). The runtime
// returns virtual time in deterministic mode.
extern "C" void racer_get_count(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (int32_t)osGetCount(); // mfc0 sign-extends
}
