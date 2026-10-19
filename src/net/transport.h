#pragma once
// How session messages travel. The session (handshake, version check, 20 Hz states, profiles, timeouts) is the
// same for every transport; only addressing and delivery differ:
//   UDP   - IP:port, for the same PC (tldbot), LAN, or forwarded ports.
//   Steam - a Steam lobby found by a short join code, then Steam P2P messages (relayed through Steam when needed,
//           so no IPs or port forwarding).
// All calls come from the session's network thread.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace net {

struct Config;
using LogFn = std::function<void(const std::string&)>;

// Opaque peer address: packed IPv4:port for UDP, a SteamID64 for Steam.
using PeerId = uint64_t;

class Transport {
public:
    virtual ~Transport() = default;

    // Host: start accepting (bind / create the lobby). Client: start finding the host.
    virtual bool open(const Config& config, const LogFn& log) = 0;
    // Housekeeping each update (e.g. Steam lobby results).
    virtual void poll(double now_ms) { (void)now_ms; }
    // Client: the host, once known (immediately for UDP; after the code lookup for Steam).
    virtual bool host_peer(PeerId& out) = 0;
    virtual void send(PeerId to, const uint8_t* data, size_t size) = 0;
    // One pending message, non-blocking.
    virtual bool receive(PeerId& from, std::vector<uint8_t>& data) = 0;
    // Sleep until something may have arrived or the timeout passes.
    virtual void wait(int timeout_ms) = 0;
    virtual void close() = 0;

    virtual std::string describe(PeerId peer) const = 0;  // for logs: "127.0.0.1:27015", a Steam name...
    virtual std::string status() const = 0;               // e.g. "hosting, code K7QX2B" or "no game with code ..."
};

std::unique_ptr<Transport> make_udp_transport();
std::unique_ptr<Transport> make_steam_transport();  // needs the game's steam_api64.dll (not in tldbot)

// Replies to our Steam requests (lobby create/search/join). TLD runs Steam in manual-dispatch mode and frees
// "call completed" notices it doesn't recognise, losing our replies. The mod hooks TLD's dispatch and, when a
// notice for one of our calls goes past, collects the reply right there; the transport then takes it from here.
namespace steam_calls {
using FetchFn = bool (*)(int32_t pipe, uint64_t call, void* out, int size, int callback_id, bool* failed);
void expect(uint64_t call, int callback_id, int size);
bool take(uint64_t call, void* out, int size);  // true once the reply has been collected
void forget(uint64_t call);
void on_completed(int32_t pipe, uint64_t call, FetchFn fetch);  // from the dispatch hook (game thread)

// The mod's hook installer; the transport runs it as soon as Steam is up, before its first request.
using InstallFn = void (*)();
void set_dispatch_installer(InstallFn install);
void install_dispatch_hook();
}  // namespace steam_calls

}  // namespace net
