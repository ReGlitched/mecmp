#include "game/native_voice.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "game/level.h"
#include "game/player_audio.h"
#include "game/voice_limiter.h"

namespace mecmp::game::native_voice {
namespace {
constexpr uintptr_t kDacSlot = 0x1420fa368, kDacProcess = 0x142dfad40;
// SndPlayer0 outputs the sampler PCM. 0x1420fddc8 belongs to Sub0.
constexpr uintptr_t kSourceSlot = 0x1420fd9f8, kSourceProcess = 0x142e17410;
constexpr uintptr_t kSourceClass = 0x1420fdaa0, kSourceDispatch = 0x1420fd9e8;
constexpr uintptr_t kInstanceVtable = 0x141acfad0, kSystemVtable = 0x141abd460;
constexpr size_t kPeers = 15, kBlocks = 320 / net::kVoiceFrameMs, kFrames = 256;
constexpr size_t kVoiceRefStride = 0x28;
constexpr uint32_t kMaxGraphVoices = 64;
using ProcessFn = uint64_t (*)(uintptr_t, uintptr_t, bool);
ProcessFn g_originalDac = nullptr, g_originalSource = nullptr;
uintptr_t g_instanceVtable = 0, g_managerAddress = 0;
std::atomic<bool> g_installed{false}, g_templateReady{false};
std::atomic<DWORD> g_outputAt{0};
std::atomic<uint32_t> g_sourceCount{0};
std::atomic<uint64_t> g_created{0}, g_blocksRendered{0};
struct Block { uint32_t sequence = 0; bool valid = false; std::array<float, net::kVoiceSamples> pcm{}; };
struct Speaker {
    uint32_t peer = 0;
    uint32_t stream = 0, expected = 0;
    DWORD received = 0, start = 0;
    size_t cursor = 0;
    Mix mix;
    float gain = 0, phase = 0, a = 0, b = 0, limiter = 1.f;
    bool primed = false;
    std::array<Block, kBlocks> blocks;
};
std::mutex g_mutex, g_clientMutex;
std::array<Speaker, kPeers> g_speakers;

// EBX arrays store their count immediately before the first element.
template<class T, size_t N> struct Array {
    uint32_t reserved = 0, count = 0;
    std::array<T, N> values{};
    T* data() { return values.data(); }
};
struct Connection { uintptr_t context = 0; uint64_t routing = 0; };
struct Graph {
    alignas(16) uint8_t asset[0x90]{}, graph[0x90]{}, sampler[0x100]{}, output[0x110]{};
    Array<uintptr_t, 32> nodes;
    Array<uintptr_t, 1> outputs;
    Array<Connection, 256> connections;
};
// Retain the small graph descriptions for DLL lifetime: native audio commands
// are deferred and may still hold them after the game thread releases a sound.
std::vector<std::unique_ptr<Graph>> g_graphs;
Graph* g_graph = nullptr;
uintptr_t g_world = 0, g_templateAsset = 0;
DWORD g_tickAt = 0, g_retryAt = 0;
struct Source {
    std::atomic<uintptr_t> instance{0}, asset{0};
    std::atomic<uint32_t> peer{0};
    DWORD retryAt = 0;
};
std::array<Source, kPeers> g_sources;
template<class T> void put(uint8_t* data, size_t offset, T value) { std::memcpy(data + offset, &value, sizeof(value)); }
template<class T> T at(const uint8_t* data, size_t offset) { T value; std::memcpy(&value, data + offset, sizeof(value)); return value; }

void clear(Speaker& s) {
    s.peer = s.stream = 0; s.cursor = 0;
    s.phase = s.a = s.b = s.gain = 0; s.limiter = 1.f; s.primed = false;
    for (auto& block : s.blocks) block.valid = false;
}
float pop(Speaker& s) {
    auto& block = s.blocks[s.expected % kBlocks];
    const float value = block.valid && block.sequence == s.expected ? block.pcm[s.cursor] : 0.f;
    if (++s.cursor == net::kVoiceSamples) {
        if (block.sequence == s.expected) block.valid = false;
        s.cursor = 0; ++s.expected;
    }
    return value;
}
bool finite(const Mix& m) {
    if (!m.peer || !std::isfinite(m.gain) || m.gain < 0 || m.gain > 2) return false;
    for (float p : m.position) if (!std::isfinite(p) || std::fabs(p) > 100000.f) return false;
    return true;
}

// The pipe template sends to "Reverb & EarlyReflections". That master bus
// forks the unpanned signal into both the room IR and a separate reflection
// branch. Use its same-format "Reverb" input so only the room IR is returned.
uintptr_t roomReverbBus() {
    const auto system = mem::readOr<uintptr_t>(rva(0x1423dd580));
    if (!system || mem::readOr<uintptr_t>(system) != rva(kSystemVtable)) return 0;
    const auto audio = mem::readOr<uintptr_t>(system + 0x50);
    if (!audio || !isKindOf(reinterpret_cast<void*>(audio), 0x1427d0e30)) return 0;
    const auto master = mem::readOr<uintptr_t>(audio + 0x18);
    if (!master || !isKindOf(reinterpret_cast<void*>(master), 0x1427d1070)) return 0;
    const auto buses = mem::readOr<uintptr_t>(master + 0x20);
    const auto count = buses ? mem::readOr<uint32_t>(buses - 4) : 0;
    if (!count || count > 128) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const auto bus = mem::readOr<uintptr_t>(buses + i * sizeof(uintptr_t));
        if (!bus || !isKindOf(reinterpret_cast<void*>(bus), 0x1427c85e0) ||
            mem::readOr<uint8_t>(bus + 0x18) != 2 || !mem::readOr<uint8_t>(bus + 0x19)) continue;
        std::string name;
        if (mem::readCString(mem::readOr<uintptr_t>(bus + 0x10), name, 64) && name == "Reverb") return bus;
    }
    return 0;
}

Graph* copyGraph(uintptr_t asset) {
    if (!asset || !isKindOf(reinterpret_cast<void*>(asset), 0x1427d1130)) return nullptr;
    auto copy = std::make_unique<Graph>();
    auto& g = *copy;
    if (!mem::read(asset, g.asset, sizeof(g.asset))) return nullptr;
    const auto graph = at<uintptr_t>(g.asset, 0x38), outputs = at<uintptr_t>(g.asset, 0x48);
    if (!graph || !outputs || mem::readOr<uint32_t>(outputs - 4) != 1 ||
        !mem::read(graph, g.graph, sizeof(g.graph))) return nullptr;
    const auto output = mem::readOr<uintptr_t>(outputs);
    if (!isKindOf(reinterpret_cast<void*>(output), 0x1427c7020) ||
        !mem::read(output, g.output, sizeof(g.output))) return nullptr;
    const auto originalReverbBus = at<uintptr_t>(g.output, 0x100);
    const auto reverbBus = roomReverbBus();
    if (!originalReverbBus || !reverbBus || originalReverbBus == at<uintptr_t>(g.output, 0x48) ||
        mem::readOr<uint8_t>(originalReverbBus + 0x18) != 2) return nullptr;
    // Sound +0x1b0 has one VoiceRef per compiled SoundGraphInfo voice.
    // Refuse an unsupported layout before starting an audible template.
    const auto voices = at<uintptr_t>(g.graph, 0x38);
    const auto voiceCount = voices ? mem::readOr<uint32_t>(voices - 4) : 0;
    if (!voiceCount || voiceCount > kMaxGraphVoices) return nullptr;
    const auto nodes = at<uintptr_t>(g.graph, 0x10);
    const auto count = nodes ? mem::readOr<uint32_t>(nodes - 4) : 0;
    if (!count || count > g.nodes.values.size() || !mem::read(nodes, g.nodes.data(), count * 8)) return nullptr;
    g.nodes.count = count;
    uintptr_t sampler = 0;
    for (auto node : g.nodes.values) {
        if (node && isKindOf(reinterpret_cast<void*>(node), 0x1427c89a0)) {
            if (sampler) return nullptr; // only the simple one-sampler template
            sampler = node;
        }
    }
    if (!sampler || !mem::read(sampler, g.sampler, sizeof(g.sampler))) return nullptr;
    const auto wave = at<uintptr_t>(g.sampler, 0xd0);
    if (!wave || mem::readOr<uint8_t>(wave + 0x71) != 1) return nullptr;
    for (auto& node : g.nodes.values) {
        if (node == output) node = reinterpret_cast<uintptr_t>(g.output);
        else if (node == sampler) node = reinterpret_cast<uintptr_t>(g.sampler);
    }
    const auto connections = at<uintptr_t>(g.graph, 0x48);
    const auto n = connections ? mem::readOr<uint32_t>(connections - 4) : 0;
    if (n > g.connections.values.size() || (n && !mem::read(connections, g.connections.data(), n * sizeof(Connection)))) return nullptr;
    g.connections.count = n;
    bool reverbRouted = false;
    for (uint32_t i = 0; i < n; ++i) {
        auto& c = g.connections.values[i];
        if (c.context == output) c.context = reinterpret_cast<uintptr_t>(g.output);
        else if (c.context == sampler) c.context = reinterpret_cast<uintptr_t>(g.sampler);
        else if (c.context == originalReverbBus) {
            // Send0 binds to the compiled connection's bus context. Changing
            // ConeOutput::ReverbSend alone leaves the old bus audible.
            c.context = reverbBus;
            reverbRouted = true;
        }
    }
    if (!reverbRouted) return nullptr;
    put(g.output, 0x100, reverbBus);
    put(g.graph, 0x10, reinterpret_cast<uintptr_t>(g.nodes.data()));
    put(g.graph, 0x48, reinterpret_cast<uintptr_t>(g.connections.data()));
    put(g.asset, 0x38, reinterpret_cast<uintptr_t>(g.graph));
    g.outputs.count = 1; g.outputs.values[0] = reinterpret_cast<uintptr_t>(g.output);
    put(g.asset, 0x48, reinterpret_cast<uintptr_t>(g.outputs.data()));
    // Keep the native source scheduled. Its PCM is replaced in our callback;
    // do not mute the downstream sampler gain (that mutes the speech too).
    g.asset[0x88] = g.asset[0x89] = g.asset[0x8a] = 1;
    put(g.asset, 0x5c, 0.f); // disable Doppler for network position updates
    put(g.sampler, 0x74, 1.f); // unity amplitude
    put(g.sampler, 0x78, uint16_t{0}); g.sampler[0x7a] = 0; // unconnected constant
    g.sampler[0xf8] = 1;
    if (at<uintptr_t>(g.sampler, 0xe0) == output) put(g.sampler, 0xe0, reinterpret_cast<uintptr_t>(g.output));
    // Faith's first-person graphs use Listener. A world voice must use Sound.
    put(g.output, 0x54, uint32_t{0});
    // ConeOutput's near-listener bleed is separate from TransformSource. The
    // native panner radius is (max(distance, min) - min) / (max - min), and
    // radii >= 1 are fully positional. Use [-1, 0] so even zero distance has
    // radius 1, with a nonzero denominator. Never collapse this range to [0, 0].
    put(g.output, 0xc8, -1.f); // BleedMinDistance
    put(g.output, 0xcc, 0.f); // BleedMaxDistance: no listener-centered blend
    put(g.output, 0xc4, 0.f); // PanSize: point source
    // Constant, disconnected CenterLevel / LfeLevel ports: no extra sends.
    put(g.output, 0xd0, uint64_t{0});
    put(g.output, 0xd8, uint64_t{0});
    put(g.output, 0x44, 0.f); // dB
    // Native HF damping fields are factors, not dB gains.
    put(g.output, 0x3c, 1.f); put(g.output, 0x40, 1.f);
    // ConeOutput ReverbGain is +0xf8; +0xf0 belongs to its attenuation curve.
    put(g.output, 0xf8, -6.f); // room IR send gain, dB; early-reflection branch excluded
    g.output[0x71] = 0; // no HDR ducking of conversational speech
    g_graphs.push_back(std::move(copy));
    MLOG("voice: native spatial graph ready (sound transform, obstruction, reverb-only send)");
    return g_graphs.back().get();
}

// Read only our known instances. Sound +0x1b0 is a 40-byte VoiceRef array,
// sized by SoundGraphInfo::Voices. VoiceRef +8 holds the Kernel pointer
// itself, exactly as module +0x20 does. Sound +0x1b8 is a separate table.
int sourceIndex(uintptr_t module) {
    __try {
        const auto kernel = *reinterpret_cast<const uintptr_t*>(module + 0x20);
        if (!kernel) return -1;
        for (size_t i = 0; i < kPeers; ++i) {
            const auto instance = g_sources[i].instance.load();
            const auto asset = g_sources[i].asset.load();
            if (!instance || !asset || *reinterpret_cast<const uintptr_t*>(instance) != g_instanceVtable) continue;
            const auto description = *reinterpret_cast<const uintptr_t*>(instance + 0x28);
            const auto channel = *reinterpret_cast<const uintptr_t*>(instance + 0x30);
            if (!description || !channel || *reinterpret_cast<const uintptr_t*>(description + 0x90) != asset) continue;
            const auto sound = *reinterpret_cast<const uintptr_t*>(channel + 0x20);
            if (!sound) continue;
            const auto graph = *reinterpret_cast<const uintptr_t*>(asset + 0x38);
            // Sound owns an internal description distinct from instance +0x28.
            // Its graph is the stable identity shared with our private asset.
            if (!graph || *reinterpret_cast<const uintptr_t*>(sound + 0x188) != graph) continue;
            const auto voiceInfo = graph ? *reinterpret_cast<const uintptr_t*>(graph + 0x38) : 0;
            const auto count = voiceInfo ? *reinterpret_cast<const uint32_t*>(voiceInfo - 4) : 0;
            const auto voices = *reinterpret_cast<const uintptr_t*>(sound + 0x1b0);
            if (!voices || !count || count > kMaxGraphVoices) continue;
            for (uint32_t voice = 0; voice < count; ++voice)
                if (*reinterpret_cast<const uintptr_t*>(voices + voice * kVoiceRefStride + 8) == kernel)
                    return static_cast<int>(i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}
struct Output { float* pcm = nullptr; uint32_t frames = 0; float rate = 0; };
bool sourceOutput(uintptr_t mixer, Output& out) {
    __try {
        // SndPlayer0 owns buffer swaps, readiness and block size. Only inspect
        // the output it actually produced; changing those fields breaks its
        // decoder/FIFO contract and can corrupt downstream native modules.
        out.frames = *reinterpret_cast<const uint32_t*>(mixer + 0xc4b8);
        if (!out.frames) return true;
        const auto channels = *reinterpret_cast<const uint8_t*>(mixer + 0xc4c4);
        out.rate = *reinterpret_cast<const float*>(mixer + 0xc4bc);
        const auto buffer = *reinterpret_cast<const uintptr_t*>(mixer + 0xc490);
        const uint32_t stride = buffer ? *reinterpret_cast<const uint16_t*>(buffer + 0x12) : 0;
        out.pcm = buffer ? *reinterpret_cast<float* const*>(buffer + 8) : nullptr;
        return out.pcm && channels == 1 && out.frames <= kFrames && stride >= out.frames && stride <= 4096 &&
            std::isfinite(out.rate) && out.rate >= 8000.f && out.rate <= 192000.f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void render(size_t index, const Output& out, DWORD now) {
    std::fill_n(out.pcm, out.frames, 0.f);
    std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    auto& s = g_speakers[index];
    if (!s.peer || s.peer != g_sources[index].peer.load()) return;
    if (now - s.received > 300) { clear(s); return; }
    if (static_cast<int32_t>(now - s.start) < 0) return;
    if (!s.primed) { s.a = pop(s); s.b = pop(s); s.primed = true; }
    const float rate = out.rate, step = static_cast<float>(net::kVoiceRate) / rate;
    const float smooth = 1.f - std::exp(-1.f / (rate * .005f));
    for (size_t i = 0; i < out.frames; ++i) {
        s.gain += (s.mix.gain - s.gain) * smooth;
        out.pcm[i] = (s.a + (s.b - s.a) * s.phase) * s.gain;
        s.phase += step;
        while (s.phase >= 1.f) { s.phase -= 1.f; s.a = s.b; s.b = pop(s); }
    }
    voice_limiter::apply(out.pcm, out.frames, rate, s.limiter);
}
uint64_t sourceProcess(uintptr_t module, uintptr_t mixer, bool last) {
    const int index = sourceIndex(module);
    const auto result = g_originalSource(module, mixer, last);
    if (index < 0 || result != 1) return result;
    Output out;
    // An owned block with an invalid layout must not leak the template sample.
    if (!sourceOutput(mixer, out)) return 0;
    if (out.frames) {
        render(static_cast<size_t>(index), out, GetTickCount());
        ++g_blocksRendered;
    }
    return result;
}
void outputHeartbeat(uintptr_t dac) {
    __try {
        const auto manager = *reinterpret_cast<const uintptr_t*>(g_managerAddress);
        const auto core = manager ? *reinterpret_cast<const uintptr_t*>(manager + 0x2268) : 0;
        if (!core || *reinterpret_cast<const uintptr_t*>(core + 0xa8) != dac) return;
        const float rate = *reinterpret_cast<const float*>(dac + 0xa8);
        if (std::isfinite(rate) && rate >= 8000 && rate <= 192000) {
            g_outputAt = GetTickCount();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
uint64_t dacProcess(uintptr_t dac, uintptr_t mixer, bool last) {
    const auto result = g_originalDac(dac, mixer, last);
    outputHeartbeat(dac); // device clock only: never add speech at the DAC
    return result;
}
void release(Source& source) {
    const auto instance = source.instance.load();
    const auto description = instance ? mem::readOr<uintptr_t>(instance + 0x28) : 0;
    if (instance && description && mem::readOr<uintptr_t>(instance) == g_instanceVtable &&
        mem::readOr<uintptr_t>(description + 0x90) == source.asset.load()) {
        if (const auto stop = gameVirtual(reinterpret_cast<void*>(instance), 0x40))
            reinterpret_cast<void (*)(uintptr_t, int)>(stop)(instance, 1);
        if (const auto dispose = gameVirtual(reinterpret_cast<void*>(instance), 0x48))
            reinterpret_cast<void (*)(uintptr_t)>(dispose)(instance);
    }
    source.instance = 0; source.asset = 0; source.peer = 0;
}
} // namespace

bool install() {
    if (g_installed) return true;
    if (mem::readOr<uintptr_t>(rva(kDacSlot)) != rva(kDacProcess) ||
        mem::readOr<uintptr_t>(rva(kSourceClass) + 0x18) != rva(kSourceDispatch) ||
        mem::readOr<uintptr_t>(rva(kSourceSlot)) != rva(kSourceProcess)) {
        MLOG("voice: native source processing descriptors differ; playback unavailable");
        return false;
    }
    g_originalDac = reinterpret_cast<ProcessFn>(rva(kDacProcess));
    g_originalSource = reinterpret_cast<ProcessFn>(rva(kSourceProcess));
    g_instanceVtable = rva(kInstanceVtable); g_managerAddress = rva(0x1423dd5b0);
    if (!mem::patchPointer(rva(kSourceSlot), reinterpret_cast<uintptr_t>(&sourceProcess), nullptr)) return false;
    if (!mem::patchPointer(rva(kDacSlot), reinterpret_cast<uintptr_t>(&dacProcess), nullptr)) {
        mem::patchPointer(rva(kSourceSlot), rva(kSourceProcess), nullptr); return false;
    }
    g_installed = true;
    MLOG("voice: native SndPlayer0 PCM replacement hook installed (native block sizes)");
    return true;
}
bool available() {
    const auto at = g_outputAt.load();
    return g_installed && g_templateReady && at && GetTickCount() - at < 1000;
}
PlaybackStatus playbackStatus() { return {g_sourceCount.load(), g_created.load(), g_blocksRendered.load()}; }
void reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& s : g_speakers) clear(s);
}
void scene(const Mix* mixes, size_t count) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& s : g_speakers) {
        if (!s.peer) continue;
        const Mix* found = nullptr;
        for (size_t i = 0; i < count; ++i) if (mixes[i].peer == s.peer) { found = &mixes[i]; break; }
        if (!found || !finite(*found) || found->gain == 0) clear(s);
        else s.mix = *found;
    }
}
bool submit(const Mix& m, const net::VoiceFrame& frame) {
    if (!finite(m) || m.gain == 0 || !frame.stream) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    const DWORD now = GetTickCount();
    Speaker* speaker = nullptr;
    for (auto& s : g_speakers) if (s.peer == m.peer) { speaker = &s; break; }
    if (!speaker) for (auto& s : g_speakers) if (!s.peer || now - s.received > 300) { speaker = &s; clear(s); break; }
    if (!speaker) return false;
    auto& s = *speaker;
    const bool idle = !s.peer || now - s.received > 300;
    if (!idle && frame.stream != s.stream && static_cast<int32_t>(frame.stream - s.stream) < 0) return false;
    const int32_t delta = static_cast<int32_t>(frame.sequence - s.expected);
    if (idle || frame.stream != s.stream || delta >= static_cast<int32_t>(kBlocks)) {
        clear(s); s.peer = m.peer; s.stream = frame.stream; s.expected = frame.sequence; s.start = now + 60;
    } else if (delta < 0) return false;
    auto& block = s.blocks[frame.sequence % kBlocks];
    if (block.valid && block.sequence == frame.sequence) return false;
    for (size_t i = 0; i < net::kVoiceSamples; ++i) block.pcm[i] = net::decodeVoiceSample(frame.samples[i]) / 32768.f;
    block.sequence = frame.sequence; block.valid = true;
    s.received = now; s.mix = m;
    return true;
}
void clientTick() {
    if (!g_installed) return;
    std::lock_guard<std::mutex> clientLock(g_clientMutex);
    const DWORD now = GetTickCount();
    if (now == g_tickAt) return;
    g_tickAt = now;
    const auto world = level::serverWorld();
    if (world != g_world) {
        for (auto& source : g_sources) release(source);
        g_world = world; g_graph = nullptr; g_templateAsset = 0; g_templateReady = false; g_retryAt = 0;
    }
    const auto templateAsset = world && !level::inFrontEnd() ? player_audio::spatialVoiceTemplate() : 0;
    if (g_graph && templateAsset != g_templateAsset) {
        for (auto& source : g_sources) release(source);
        g_graph = nullptr; g_templateReady = false; g_retryAt = 0;
    }
    if (world && !level::inFrontEnd() && !g_graph && (!g_retryAt || now - g_retryAt >= 1000)) {
        g_retryAt = now;
        g_graph = copyGraph(templateAsset); g_templateAsset = templateAsset;
        g_templateReady = g_graph != nullptr;
    }
    struct Wanted { uint32_t peer = 0; Mix mix; };
    std::array<Wanted, kPeers> wanted{};
    if (world && !level::inFrontEnd() && g_graph && config::get()->voiceEnabled) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (size_t i = 0; i < kPeers; ++i) {
            const auto& s = g_speakers[i];
            if (s.peer && now - s.received <= 300) wanted[i] = {s.peer, s.mix};
        }
    }
    const auto system = mem::readOr<uintptr_t>(rva(0x1423dd580));
    for (size_t i = 0; i < kPeers; ++i) {
        auto& source = g_sources[i];
        if (source.peer.load() != wanted[i].peer) release(source);
        if (!wanted[i].peer) continue;
        auto instance = source.instance.load();
        if (instance) {
            const auto description = mem::readOr<uintptr_t>(instance + 0x28);
            if (mem::readOr<uintptr_t>(instance) != g_instanceVtable || !description ||
                mem::readOr<uintptr_t>(description + 0x90) != source.asset.load()) {
                release(source); instance = 0;
            }
        }
        if (!instance && (!source.retryAt || now - source.retryAt >= 1000)) {
            source.retryAt = now;
            if (!system || mem::readOr<uintptr_t>(system) != rva(kSystemVtable)) continue;
            const auto create = gameVirtual(reinterpret_cast<void*>(system), 0x40);
            if (!create) continue;
            instance = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(create)(system, reinterpret_cast<uintptr_t>(g_graph->asset));
            if (!instance || mem::readOr<uintptr_t>(instance) != g_instanceVtable) continue;
            source.asset = reinterpret_cast<uintptr_t>(g_graph->asset);
            source.peer = wanted[i].peer; source.instance = instance;
            ++g_created;
        }
        if (!instance || mem::readOr<uintptr_t>(instance) != g_instanceVtable) continue;
        alignas(16) auto transform = transformFromYaw(wanted[i].mix.position, 0);
        reinterpret_cast<void (*)(uintptr_t, const LinearTransform*)>(rva(0x14301dc30))(instance, &transform);
        alignas(16) const float zero[4]{};
        reinterpret_cast<void (*)(uintptr_t, const float*)>(rva(0x14301dd30))(instance, zero);
        // Start is deferred; publish identity and position before the first audio block.
        if ((mem::readOr<uint8_t>(instance + 0x40) & 0x10) == 0)
            reinterpret_cast<void (*)(uintptr_t, int)>(rva(0x14301d760))(instance, 0);
    }
    uint32_t count = 0;
    for (const auto& source : g_sources) if (source.instance.load()) ++count;
    g_sourceCount = count;
}
} // namespace mecmp::game::native_voice
