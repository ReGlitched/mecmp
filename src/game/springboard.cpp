#include "game/springboard.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "game/collision.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/hooks.h"
#include "game/level.h"
#include "game/teleport.h"
#include "net/net.h"

namespace mecmp::game::springboard {
namespace {
constexpr uintptr_t kProcessorSlot = 0x141aabc50, kEvaluate = 0x142f29960;
constexpr uintptr_t kInstanceFromActor = 0x142c2cb70;
constexpr uintptr_t kAntDrivenType = 0x142854b90;
constexpr DWORD kPeerAge = 300, kPhysicalAge = 1300;
using EvaluateFn = void (*)(void* asset, void* context);
using InstanceFn = uintptr_t (*)(void* actor);
EvaluateFn g_evaluate = nullptr;
std::atomic<bool> g_available{false};
std::atomic<uintptr_t> g_client{0}, g_instance{0};
std::atomic<DWORD> g_at{0};

// Native ANT value-reader interfaces used by SpringboardProcessorAsset. Only the five obstacle
// inputs are substituted, in a stack copy of the asset. The shared asset and all its native
// variable references stay untouched. The native processor consumes these readers synchronously
// and copies observation values into its own per-actor history (142ee8d80).
struct BoolValue { const uintptr_t* vtable; bool value; };
struct IntValue { const uintptr_t* vtable; int value; };
struct VectorValue { const uintptr_t* vtable; float value[4]; };
bool readBool(const BoolValue* self, void*) { return self->value; }
int readInt(const IntValue* self, void*) { return self->value; }
float* readVector(const VectorValue* self, float* out, void*) {
    std::memcpy(out, self->value, sizeof(self->value));
    return out;
}
const uintptr_t kBoolVtable[] = {reinterpret_cast<uintptr_t>(&readBool)};
const uintptr_t kIntVtable[] = {reinterpret_cast<uintptr_t>(&readInt)};
const uintptr_t kVectorVtable[] = {reinterpret_cast<uintptr_t>(&readVector)};
struct IntReference { uint8_t prefix[0x10] = {}; IntValue reader{kIntVtable, 6}; };
struct VectorReference { uint8_t prefix[0x20] = {}; VectorValue reader{kVectorVtable, {0, 0, 0, 0}}; };
static_assert(offsetof(IntReference, reader) == 0x10 && offsetof(VectorReference, reader) == 0x20, "ANT reader offsets");
using ProcessorAsset = std::array<uintptr_t, 17>; // native SpringboardProcessorAsset, 0x88 bytes

struct Candidate { float position[4] = {}, normal[4] = {}; float distance2 = 0; };
bool finite3(const float* p) {
    for (int i = 0; i < 3; ++i) if (!std::isfinite(p[i]) || std::fabs(p[i]) > 100000.f) return false;
    return true;
}
uintptr_t instanceOf(void* client) {
    void* driven = client ? getComponent(client, kAntDrivenType) : nullptr;
    const uintptr_t binding = driven ? mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(driven) + 0x58) : 0;
    const uintptr_t controller = binding ? mem::readOr<uintptr_t>(binding + 0xe8) : 0;
    return controller ? mem::readOr<uintptr_t>(controller + 0x28) : 0;
}

bool chooseCandidate(void* client, Candidate& result) {
    LinearTransform body;
    if (!readEntityTransform(client, body) || !finite3(body.trans)) return false;
    const float forwardLength = std::sqrt(body.forward[0] * body.forward[0] + body.forward[2] * body.forward[2]);
    if (!std::isfinite(forwardLength) || forwardLength < .1f) return false;
    bool found = false;
    const auto peers = net::remotes(kPeerAge);
    const auto shown = ghosts::shownPeers();
    for (const auto& peer : peers) {
        if (peer.loopback || !peer.collision || !peer.crouched || !peer.springboard || peer.physicalAgeMs > kPhysicalAge)
            continue;
        bool visible = false;
        for (uint32_t id : shown) visible = visible || id == peer.id;
        if (!visible) continue;
        net::PlayerState state;
        uint32_t age = 0;
        if (!net::remoteState(peer.id, state, age) || age > kPeerAge || !(state.flags & net::kStateInLevel) ||
            !finite3(state.pos)) continue;
        const float dx = state.pos[0] - body.trans[0], dz = state.pos[2] - body.trans[2];
        const float distance2 = dx * dx + dz * dz;
        if (distance2 < .35f * .35f || distance2 > 1.8f * 1.8f ||
            body.trans[1] < state.pos[1] - .4f || body.trans[1] > state.pos[1] + 1.2f ||
            (found && distance2 >= result.distance2)) continue;
        const float distance = std::sqrt(distance2);
        // A runner behind us or directly underneath is not an obstacle in front of our jump.
        if ((dx * body.forward[0] + dz * body.forward[2]) / (distance * forwardLength) < .55f) continue;
        Candidate candidate;
        candidate.normal[0] = -dx / distance;
        candidate.normal[2] = -dz / distance;
        if (!collision::springboardSurface(peer.id, candidate.normal, candidate.position)) continue;
        candidate.distance2 = distance2;
        result = candidate;
        found = true;
    }
    return found;
}

bool nativeObstacleCloser(const ProcessorAsset& asset, void* variables, void* client, float distance2) {
    // Keep a real, closer springboard obstacle when the world already supplied one.
    const auto stand = reinterpret_cast<bool (*)(void*, void*)>(gameVirtual(reinterpret_cast<void*>(asset[6]), 0));
    const auto vault = reinterpret_cast<bool (*)(void*, void*)>(gameVirtual(reinterpret_cast<void*>(asset[7]), 0));
    const auto type = reinterpret_cast<int (*)(void*, void*)>(gameVirtual(reinterpret_cast<void*>(asset[8] + 0x10), 0));
    const auto position = reinterpret_cast<float* (*)(void*, float*, void*)>(
        gameVirtual(reinterpret_cast<void*>(asset[4] + 0x20), 0));
    if (!stand || !vault || !type || !position || !stand(reinterpret_cast<void*>(asset[6]), variables) ||
        !vault(reinterpret_cast<void*>(asset[7]), variables)) return false;
    const int kind = type(reinterpret_cast<void*>(asset[8] + 0x10), variables);
    if (kind != 6 && kind != 10) return false; // the native processor's two eligible obstacle categories
    alignas(16) float p[4] = {};
    const float* value = position(reinterpret_cast<void*>(asset[4] + 0x20), p, variables);
    LinearTransform body;
    if (!value || !finite3(value) || !readEntityTransform(client, body)) return false;
    const float dx = value[0] - body.trans[0], dz = value[2] - body.trans[2];
    return dx * dx + dz * dz < distance2;
}

void evaluate(void* asset, void* context) {
    const auto cfg = config::get();
    const uintptr_t client = g_client.load(), instance = g_instance.load();
    if (!g_available || !cfg->playerCollision || !cfg->playerSpringboard || !collision::available() ||
        !client || !instance || GetTickCount() - g_at.load() > 150 || !context || teleport::busy() ||
        !net::running() || (!net::hosting() && !net::connectedToHost()) || hooks::gameMenuOpen()) {
        g_evaluate(asset, context);
        return;
    }
    const uintptr_t actor = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(context) + 0x28);
    if (!actor || reinterpret_cast<InstanceFn>(rva(kInstanceFromActor))(reinterpret_cast<void*>(actor)) != instance ||
        reinterpret_cast<uintptr_t>(reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))()) != client ||
        instanceOf(reinterpret_cast<void*>(client)) != instance) {
        g_evaluate(asset, context);
        return;
    }
    Candidate candidate;
    ProcessorAsset copy;
    const uintptr_t variables = mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(context) + 0x30);
    if (!variables || !chooseCandidate(reinterpret_cast<void*>(client), candidate) ||
        !mem::read(reinterpret_cast<uintptr_t>(asset), copy.data(), sizeof(copy)) ||
        nativeObstacleCloser(copy, reinterpret_cast<void*>(variables), reinterpret_cast<void*>(client), candidate.distance2)) {
        g_evaluate(asset, context);
        return;
    }
    VectorReference position, normal;
    std::memcpy(position.reader.value, candidate.position, sizeof(candidate.position));
    std::memcpy(normal.reader.value, candidate.normal, sizeof(candidate.normal));
    BoolValue stand{kBoolVtable, true}, vault{kBoolVtable, true};
    IntReference type;
    copy[4] = reinterpret_cast<uintptr_t>(&position); // ObstaclePosition, vector interface at +0x20
    copy[5] = reinterpret_cast<uintptr_t>(&normal);   // ObstacleNormal
    copy[6] = reinterpret_cast<uintptr_t>(&stand);   // ObstacleCanStand
    copy[7] = reinterpret_cast<uintptr_t>(&vault);   // ObstacleCanVault
    copy[8] = reinterpret_cast<uintptr_t>(&type);    // ObstacleType, integer interface at +0x10
    // Feed only perception into the actual native processor. MoveEnumTracker, TimestampNow,
    // JumpStartPosition and every native output reference are unchanged. The processor's normal
    // history checks decide SpringboardAllowed; the normal SpringboardDriver performs the move.
    g_evaluate(copy.data(), context);
}
} // namespace

bool available() { return g_available.load(); }
void install() {
    uintptr_t current = 0;
    const uint8_t expected[] = {0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x1d};
    uint8_t actual[sizeof(expected)] = {};
    if (mem::read(rva(kProcessorSlot), current) && current == rva(kEvaluate) &&
        mem::read(rva(kInstanceFromActor), actual, sizeof(actual)) && !std::memcmp(expected, actual, sizeof(actual))) {
        g_evaluate = reinterpret_cast<EvaluateFn>(current);
        g_available = mem::patchPointer(rva(kProcessorSlot), reinterpret_cast<uintptr_t>(&evaluate), nullptr);
    }
    MLOG("springboard: native processor %s", available() ? "available" : "unavailable");
}
void tick() {
    const auto cfg = config::get();
    void* client = available() && cfg->playerCollision && cfg->playerSpringboard && level::serverWorld() &&
        !level::inFrontEnd() ? reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))() : nullptr;
    g_client = reinterpret_cast<uintptr_t>(client);
    g_instance = instanceOf(client);
    g_at = GetTickCount();
}
} // namespace mecmp::game::springboard
