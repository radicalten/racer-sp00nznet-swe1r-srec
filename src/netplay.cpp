// Netplay: 2-player deterministic lockstep over UDP.
//
//   SWE1Racer.exe --host [port]          player 1, waits for a joiner (default port 7777)
//   SWE1Racer.exe --join address[:port]  player 2
//
// Both machines run the game in ultramodern's deterministic mode. At every virtual retrace each side
// samples its local controller once, sends it for retrace + delay, and waits for the other side's
// input for this retrace before the game sees the frame, so both games get identical inputs at
// identical points. Every 60 retraces both sides exchange a hash of game RAM to detect a desync.
//
// Before the game starts, the host sends its EEPROM save (unlocks change the game, so both sides
// need the same one). Netplay uses its own config folder (netplay/) so normal saves are untouched.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "ultramodern/input.hpp"

uint64_t det_hash_game_ram();                              // determinism.cpp
void det_start(void (*then)(uint64_t));                    // determinism.cpp
bool sample_local_input(uint16_t* buttons, float* x, float* y); // main.cpp

namespace {

constexpr uint32_t kMagic = 0x53573152; // "SW1R"
constexpr uint32_t kVersion = 1;
constexpr int kDefaultPort = 7777;
constexpr int kRedundancy = 16;     // each input packet repeats this many recent frames
constexpr int kHashEvery = 60;      // retraces between desync checks

enum PacketType : uint32_t { PKT_HELLO = 1, PKT_WELCOME = 2, PKT_ACK = 3, PKT_INPUT = 4, PKT_HASH = 5 };

struct Input { uint16_t buttons = 0; float x = 0, y = 0; };

#pragma pack(push, 1)
struct Header { uint32_t magic, type, version; };
struct WelcomePacket { Header h; uint32_t delay; uint32_t save_len; uint8_t save[4096]; };
struct InputPacket { Header h; uint64_t first; uint32_t count; Input inputs[kRedundancy]; };
struct HashPacket { Header h; uint64_t frame; uint64_t hash; };
#pragma pack(pop)

bool g_active = false;
bool g_is_host = false;
int g_delay = 2;
SOCKET g_sock = INVALID_SOCKET;
sockaddr_storage g_peer{};
int g_peer_len = 0;

std::map<uint64_t, Input> g_local, g_remote; // by retrace
std::map<uint64_t, uint64_t> g_local_hash, g_remote_hash;
Input g_frame_inputs[2];                     // what the game reads this retrace: [player 1, player 2]

void send_raw(const void* data, int len) {
    sendto(g_sock, (const char*)data, len, 0, (const sockaddr*)&g_peer, g_peer_len);
}

// Receives one packet (waiting up to `ms`). On the host, the first sender becomes the peer.
int recv_raw(void* buf, int cap, int ms) {
    fd_set fds; FD_ZERO(&fds); FD_SET(g_sock, &fds);
    timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return 0;
    sockaddr_storage from{}; int from_len = sizeof(from);
    int n = recvfrom(g_sock, (char*)buf, cap, 0, (sockaddr*)&from, &from_len);
    if (n < (int)sizeof(Header) || ((Header*)buf)->magic != kMagic) return 0;
    if (g_peer_len == 0) { g_peer = from; g_peer_len = from_len; }
    return n;
}

void handle_packet(const uint8_t* buf, int n) {
    const Header* h = (const Header*)buf;
    if (h->type == PKT_INPUT && n >= (int)offsetof(InputPacket, inputs)) {
        const InputPacket* p = (const InputPacket*)buf;
        for (uint32_t i = 0; i < p->count && i < kRedundancy; i++) g_remote.emplace(p->first + i, p->inputs[i]);
    } else if (h->type == PKT_HASH && n >= (int)sizeof(HashPacket)) {
        const HashPacket* p = (const HashPacket*)buf;
        g_remote_hash[p->frame] = p->hash;
    }
}

void send_inputs(uint64_t newest) {
    InputPacket p{ { kMagic, PKT_INPUT, kVersion } };
    p.first = newest >= kRedundancy - 1 ? newest - (kRedundancy - 1) : 0;
    p.count = (uint32_t)(newest - p.first + 1);
    for (uint32_t i = 0; i < p.count; i++) p.inputs[i] = g_local[p.first + i];
    send_raw(&p, (int)(offsetof(InputPacket, inputs) + p.count * sizeof(Input)));
}

void check_hashes() {
    for (auto it = g_local_hash.begin(); it != g_local_hash.end();) {
        auto r = g_remote_hash.find(it->first);
        if (r == g_remote_hash.end()) { ++it; continue; }
        if (r->second != it->second) {
            fprintf(stderr, "[netplay] DESYNC at retrace %llu (local %016llx, remote %016llx)\n",
                    (unsigned long long)it->first, (unsigned long long)it->second, (unsigned long long)r->second);
        }
        g_remote_hash.erase(r);
        it = g_local_hash.erase(it);
    }
}

// The lockstep step, run by the deterministic scheduler at every retrace (on a game thread).
void on_retrace(uint64_t r) {
    Input local;
    sample_local_input(&local.buttons, &local.x, &local.y);
    g_local[r + g_delay] = local;
    send_inputs(r + g_delay);

    if (r % kHashEvery == 0) {
        uint64_t h = det_hash_game_ram();
        g_local_hash[r] = h;
        HashPacket p{ { kMagic, PKT_HASH, kVersion }, r, h };
        send_raw(&p, sizeof(p));
    }

    // Wait for the peer's input for this retrace, resending ours meanwhile (the first `delay`
    // retraces have no input from anyone).
    auto start = std::chrono::steady_clock::now();
    auto last_note = start;
    while (r >= (uint64_t)g_delay && g_remote.find(r) == g_remote.end()) {
        uint8_t buf[sizeof(WelcomePacket)];
        int n = recv_raw(buf, sizeof(buf), 4);
        if (n > 0) { handle_packet(buf, n); continue; }
        send_inputs(r + g_delay);
        auto now = std::chrono::steady_clock::now();
        if (now - last_note > std::chrono::seconds(3)) {
            last_note = now;
            fprintf(stderr, "[netplay] waiting for the other player (retrace %llu)...\n", (unsigned long long)r);
        }
        if (now - start > std::chrono::seconds(30)) {
            fprintf(stderr, "[netplay] the other player stopped responding; quitting\n");
            std::exit(1);
        }
    }
    // Drain whatever else arrived.
    for (;;) { uint8_t buf[sizeof(WelcomePacket)]; int n = recv_raw(buf, sizeof(buf), 0); if (n <= 0) break; handle_packet(buf, n); }

    Input mine = r >= (uint64_t)g_delay ? g_local[r] : Input{};
    Input theirs = r >= (uint64_t)g_delay ? g_remote[r] : Input{};
    g_frame_inputs[0] = g_is_host ? mine : theirs;
    g_frame_inputs[1] = g_is_host ? theirs : mine;

    // Forget old frames.
    if (r > 120) { g_local.erase(g_local.begin(), g_local.lower_bound(r - 120)); g_remote.erase(g_remote.begin(), g_remote.lower_bound(r - 120)); }
    check_hashes();
}

bool open_socket(int bind_port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((u_short)bind_port); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[netplay] can't use UDP port %d\n", bind_port);
        return false;
    }
    return true;
}

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

// netplay/ gets the ROM (copied once) and the shared save.
void prepare_folder(const std::filesystem::path& base, const std::vector<uint8_t>& save) {
    std::filesystem::path dir = base / "netplay";
    std::filesystem::create_directories(dir / "saves");
    if (!std::filesystem::exists(dir / "swe1racer.z64")) {
        std::filesystem::copy_file(base / "swe1racer.z64", dir / "swe1racer.z64");
    }
    std::ofstream(dir / "saves" / "swe1racer.bin", std::ios::binary).write((const char*)save.data(), save.size());
}

} // namespace

bool netplay_active() { return g_active; }

// Parses --host / --join and runs the handshake. Returns the config folder to use (or base).
std::filesystem::path netplay_setup(int argc, char** argv, const std::filesystem::path& base, int delay) {
    std::string host_arg, join_arg;
    bool host = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--host") { host = true; if (i + 1 < argc && argv[i + 1][0] != '-') host_arg = argv[++i]; }
        else if (a == "--join" && i + 1 < argc) join_arg = argv[++i];
    }
    if (!host && join_arg.empty()) return base;
    g_delay = delay;

    if (host) {
        int port = host_arg.empty() ? kDefaultPort : std::stoi(host_arg);
        if (!open_socket(port)) std::exit(1);
        g_is_host = true;
        printf("[netplay] hosting on UDP port %d; waiting for player 2 (--join <this machine's address>)\n", port);
        std::vector<uint8_t> save = read_file(base / "saves" / "swe1racer.bin");
        save.resize(2048, 0);
        WelcomePacket w{ { kMagic, PKT_WELCOME, kVersion }, (uint32_t)g_delay, (uint32_t)save.size() };
        std::memcpy(w.save, save.data(), save.size());
        bool joined = false, acked = false;
        while (!acked) {
            uint8_t buf[sizeof(WelcomePacket)];
            int n = recv_raw(buf, sizeof(buf), 50);
            if (n <= 0) { if (joined) send_raw(&w, sizeof(w)); continue; }
            const Header* h = (const Header*)buf;
            if (h->type == PKT_HELLO) {
                if (h->version != kVersion) { fprintf(stderr, "[netplay] joiner has a different version\n"); continue; }
                if (!joined) printf("[netplay] player 2 connected; sending the save\n");
                joined = true;
                send_raw(&w, sizeof(w));
            } else if (h->type == PKT_ACK) {
                acked = true;
            } else if (h->type == PKT_INPUT) {
                acked = true; // the joiner already started (its ACKs were lost)
                handle_packet(buf, n);
            }
        }
        prepare_folder(base, save);
    } else {
        std::string addr = join_arg, port = std::to_string(kDefaultPort);
        if (auto c = addr.rfind(':'); c != std::string::npos) { port = addr.substr(c + 1); addr = addr.substr(0, c); }
        if (!open_socket(0)) std::exit(1);
        addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(addr.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            fprintf(stderr, "[netplay] can't resolve %s\n", addr.c_str());
            std::exit(1);
        }
        std::memcpy(&g_peer, res->ai_addr, res->ai_addrlen); g_peer_len = (int)res->ai_addrlen;
        freeaddrinfo(res);
        printf("[netplay] joining %s:%s as player 2...\n", addr.c_str(), port.c_str());
        Header hello{ kMagic, PKT_HELLO, kVersion };
        std::vector<uint8_t> save;
        auto start = std::chrono::steady_clock::now();
        while (save.empty()) {
            send_raw(&hello, sizeof(hello));
            uint8_t buf[sizeof(WelcomePacket)];
            int n = recv_raw(buf, sizeof(buf), 200);
            if (n >= (int)sizeof(WelcomePacket) - 4096 && ((Header*)buf)->type == PKT_WELCOME) {
                const WelcomePacket* w = (const WelcomePacket*)buf;
                if (w->h.version != kVersion) { fprintf(stderr, "[netplay] host has a different version\n"); std::exit(1); }
                g_delay = (int)w->delay;
                save.assign(w->save, w->save + std::min<uint32_t>(w->save_len, sizeof(w->save)));
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) {
                fprintf(stderr, "[netplay] no answer from %s:%s\n", addr.c_str(), port.c_str());
                std::exit(1);
            }
        }
        Header ack{ kMagic, PKT_ACK, kVersion };
        for (int i = 0; i < 5; i++) send_raw(&ack, sizeof(ack));
        prepare_folder(base, save);
        printf("[netplay] connected; input delay %d frames\n", g_delay);
    }

    g_active = true;
    det_start(on_retrace);
    return base / "netplay";
}

// The game's controller reads while netplay is active: ports 1 and 2 are the two players.
bool netplay_get_input(int port, uint16_t* buttons, float* x, float* y) {
    if (port < 0 || port > 1) return false;
    *buttons = g_frame_inputs[port].buttons; *x = g_frame_inputs[port].x; *y = g_frame_inputs[port].y;
    return true;
}

ultramodern::input::connected_device_info_t netplay_device_info(int port) {
    if (port == 0 || port == 1) return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}
