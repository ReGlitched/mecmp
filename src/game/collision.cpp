#include "game/collision.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "game/ghosts.h"
#include "game/hooks.h"
#include "game/level.h"
#include "game/teleport.h"
#include "net/net.h"

namespace mecmp::game::collision {
namespace {
constexpr uintptr_t kCapsuleType = 0x1427bd350, kMoveTrackerType = 0x142853420;
constexpr uintptr_t kClientContext = 0x142401ca0, kFactoryType = 0x142831670;
constexpr uintptr_t kRigidBodyVtable = 0x141bac160;
constexpr uintptr_t kCreationInfo = 0x142d4d1a0, kFindFactory = 0x143506830, kCreateBody = 0x1435169e0;
constexpr uintptr_t kSetTransform = 0x1434f8930, kSetTransformSource = 0x1434f6d90;
constexpr uintptr_t kEnable = 0x1434e90f0, kAddSpatial = 0x1434e9270;
constexpr uintptr_t kRemoveSpatial = 0x1434f6610, kDisable = 0x1434f65f0, kDestroy = 0x1434f5550;
// Fixed Faith-sized box profile in world metres. Animation, bones and render
// bounds never affect this geometry. The box follows only position, yaw and crouch.
constexpr float kModelScale = 1.f;
constexpr float kHalfWidth = .22f * kModelScale, kHalfDepth = .22f * kModelScale;
constexpr float kStandingHeight = 1.70f * kModelScale, kCrouchHeight = 1.00f * kModelScale;
static_assert(kHalfWidth > 0 && kHalfDepth > 0 && kStandingHeight > 0 && kCrouchHeight > 0);
constexpr DWORD kPeerAge = 300, kPhysicalAge = 1300;
std::atomic<bool> g_ready{false};
std::atomic<uintptr_t> g_client{0};
std::atomic<DWORD> g_clientAt{0};
std::atomic<uint32_t> g_bodies{0};

// PhysicsEntityFactory reads shape type 1 synchronously to construct a native
// Havok box (143516562 -> 143f2f140). The shared body fields retain the same
// zero-mass body kind and StaticLayer used by native character collision.
uint32_t boxType(const void*) { return 1; }
const uintptr_t kBoxVtable[] = {reinterpret_cast<uintptr_t>(&boxType)};
struct alignas(16) BoxDescription {
    const uintptr_t* vtable = kBoxVtable;
    float mass = 0;
    uint32_t reservedC = 0, reserved10 = 0, kind = 2, quality = 0;
    uint32_t layer = 1;
    float halfExtents[4]{};
};
static_assert(sizeof(BoxDescription) == 0x30 && alignof(BoxDescription) == 16 &&
              offsetof(BoxDescription, kind) == 0x14 && offsetof(BoxDescription, halfExtents) == 0x20,
              "Native box description");
struct alignas(16) Body {
    uint32_t peer = 0, id = ~0u;
    uintptr_t object = 0, owner = 0, manager = 0;
    bool crouched = false, wanted = false;
    DWORD retryAt = 0;
    uint64_t transformPadding = 0;
    LinearTransform transform{};
};
// RigidBody +0xc8 retains its transform source. Storage must never relocate,
// including while native deferred destruction is pending.
std::array<Body, 15> g_slots;
uintptr_t g_context = 0, g_sharedManager = 0;
uint32_t g_generation = 0;
DWORD g_lastUpdate = 0;
std::mutex g_updateMutex;

bool finite3(const float* p) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > 100000.f) return false;
    return true;
}
bool callAt(uintptr_t caller, uintptr_t target) {
    uint8_t instruction[5]{};
    int32_t displacement = 0;
    if (!mem::read(rva(caller), instruction, sizeof(instruction)) || instruction[0] != 0xe8) return false;
    std::memcpy(&displacement, instruction + 1, 4);
    return rva(caller) + 5 + displacement == rva(target);
}
bool layoutMatches() {
    uintptr_t fn = 0;
    // Verify the box constructor branch in addition to the shared body lifecycle.
    const uint8_t shapeCheck[] = {0x83, 0xf8, 0x01}; // cmp eax, 1
    const uint8_t extentsLoad[] = {0x0f, 0x28, 0x4f, 0x20}; // movaps xmm1, [rdi+20h]
    uint8_t check[sizeof(shapeCheck)]{}, load[sizeof(extentsLoad)]{};
    return mem::read(rva(0x143516532), check, sizeof(check)) && !std::memcmp(check, shapeCheck, sizeof(check)) &&
        mem::read(rva(0x14351653a), load, sizeof(load)) && !std::memcmp(load, extentsLoad, sizeof(load)) &&
        callAt(0x143516562, 0x143f2f140) &&
        mem::read(rva(kRigidBodyVtable) + 0x178, fn) && fn == rva(0x1434f21e0) &&
        mem::read(rva(kRigidBodyVtable) + 0x250, fn) && fn == rva(0x1434f17d0) &&
        callAt(0x142d4f9e1, kCreationInfo) && callAt(0x142d4f9f8, kFindFactory) &&
        callAt(0x142d4fa18, kCreateBody) && callAt(0x142d4facb, kSetTransform) &&
        callAt(0x142d4fb6a, kEnable) && callAt(0x142d4fb82, kAddSpatial);
}
uintptr_t worldManager(uintptr_t shared, uint32_t index) {
    const auto list = mem::readOr<uintptr_t>(shared + 0xf0);
    // This is an indexed world table (1434ee7e0), not a vector. +0xf8 is a
    // different table, not its end. Validate the selected world's back-reference.
    const auto manager = list && index < 256 ? mem::readOr<uintptr_t>(list + static_cast<uintptr_t>(index) * 8) : 0;
    return manager && mem::readOr<uintptr_t>(manager + 8) == shared &&
        mem::readOr<uint8_t>(manager + 0xb84, 0xff) == index ? manager : 0;
}
uintptr_t bodyObject(uintptr_t manager, uint32_t id) {
    const auto table = mem::readOr<uintptr_t>(manager + 0x130);
    const auto end = mem::readOr<uintptr_t>(manager + 0x138);
    const auto entry = table + static_cast<uintptr_t>(id & 0xffffu) * 8;
    return table && entry < end ? mem::readOr<uintptr_t>(entry) : 0;
}
bool ours(const Body& b) {
    if (!b.object || b.id == ~0u || b.manager != worldManager(g_sharedManager, (b.id >> 16) & 0xff) ||
        bodyObject(b.manager, b.id) != b.object || mem::readOr<uintptr_t>(b.object) != rva(kRigidBodyVtable) ||
        mem::readOr<uintptr_t>(b.object + 0x30) != b.manager ||
        mem::readOr<uintptr_t>(b.object + 0xc8) != reinterpret_cast<uintptr_t>(&b.transform)) return false;
    // Validate the full ID in Havok user data as well as the table index/pointer.
    const auto world = mem::readOr<uintptr_t>(b.manager + 0xe0);
    const auto table = world ? mem::readOr<uintptr_t>(world + 0x20) : 0;
    const auto count = world ? (mem::readOr<uint32_t>(world + 0x2c) & 0x3fffffffu) : 0;
    const auto index = mem::readOr<uint32_t>(b.object + 0x58, ~0u);
    return table && index < count && mem::readOr<uint32_t>(table + static_cast<size_t>(index) * 0x90 + 0x88, ~0u) == b.id;
}
using BodyFn = void (*)(uintptr_t, const uint32_t*);
void remove(Body& b) {
    if (ours(b)) {
        reinterpret_cast<BodyFn>(rva(kRemoveSpatial))(b.manager, &b.id);
        reinterpret_cast<BodyFn>(rva(kDisable))(b.manager, &b.id);
        reinterpret_cast<BodyFn>(rva(kDestroy))(b.manager, &b.id);
    }
    b.peer = 0; b.id = ~0u; b.object = b.owner = b.manager = 0;
    b.wanted = false; b.retryAt = 0;
}
void move(Body& b) {
    reinterpret_cast<void (*)(uintptr_t, const uint32_t*, const LinearTransform*, bool)>(rva(kSetTransform))
        (b.manager, &b.id, &b.transform, true);
}
bool create(Body& b, uintptr_t owner, uintptr_t rigidData, uintptr_t manager) {
    const auto bus = mem::readOr<uintptr_t>(owner + 0x28);
    const auto factories = mem::readOr<uintptr_t>(g_context + 0x58);
    if (!bus || !factories) return false;
    const auto factory = reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t)>(rva(kFindFactory))
        (factories, rva(kFactoryType));
    if (!factory) return false;
    alignas(16) uint8_t info[0xe0]{};
    reinterpret_cast<void* (*)(void*, uintptr_t, uint32_t, uintptr_t, uintptr_t)>(rva(kCreationInfo))
        (info, bus, 0, manager, 0);
    BoxDescription description;
    description.halfExtents[0] = kHalfWidth;
    description.halfExtents[1] = (b.crouched ? kCrouchHeight : kStandingHeight) * .5f;
    description.halfExtents[2] = kHalfDepth;
    uint32_t id = ~0u;
    reinterpret_cast<uint32_t* (*)(uintptr_t, uint32_t*, void*, uintptr_t, const BoxDescription*)>(rva(kCreateBody))
        (factory, &id, info, rigidData, &description);
    const auto object = id != ~0u ? bodyObject(manager, id) : 0;
    if (!object) return false;
    b.id = id; b.object = object; b.owner = owner; b.manager = manager;
    reinterpret_cast<void (*)(uintptr_t, const uint32_t*, const LinearTransform*)>(rva(kSetTransformSource))
        (manager, &b.id, &b.transform);
    if (!ours(b)) {
        reinterpret_cast<BodyFn>(rva(kDestroy))(manager, &id);
        b.id = ~0u; b.object = b.owner = b.manager = 0;
        return false;
    }
    move(b);
    reinterpret_cast<BodyFn>(rva(kEnable))(manager, &b.id);
    reinterpret_cast<BodyFn>(rva(kAddSpatial))(manager, &b.id);
    VLOG("collision: native box %08x for peer %08x (%s)", id, b.peer, b.crouched ? "crouched" : "standing");
    return true;
}
void updateBody(Body& b, uint32_t peer, uintptr_t owner, bool crouched, const float* position, float yaw,
                uintptr_t rigidData, uintptr_t manager, DWORD now) {
    if (b.object && (b.owner != owner || b.crouched != crouched || b.manager != manager || !ours(b))) remove(b);
    b.peer = peer; b.crouched = crouched; b.wanted = true;
    const auto previous = b.transform;
    b.transform = transformFromYaw(position, yaw);
    // Box half-extents use ordinary XYZ axes; keep Y upright and feet anchored.
    b.transform.trans[1] += (crouched ? kCrouchHeight : kStandingHeight) * .5f;
    if (!b.object) {
        if (!b.retryAt || now - b.retryAt >= 1000) {
            b.retryAt = now;
            create(b, owner, rigidData, manager);
        }
    } else {
        float delta2 = 0;
        for (int i = 0; i < 3; ++i) {
            const float d = b.transform.trans[i] - previous.trans[i];
            const float r = b.transform.right[i] - previous.right[i];
            const float f = b.transform.forward[i] - previous.forward[i];
            delta2 += d * d + r * r + f * f;
        }
        if (delta2 > .0000001f) move(b);
    }
}
} // namespace

bool available() { return g_ready.load(); }
uint32_t bodyCount() { return g_bodies.load(); }
void install() {
    g_ready = layoutMatches();
    MLOG("collision: native fixed box factory %s", available() ? "available" : "unavailable");
}
void tick() {
    const auto cfg = config::get();
    const bool wanted = cfg->playerCollision;
    void* client = wanted && level::serverWorld() && !level::inFrontEnd()
        ? reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))() : nullptr;
    int state = -1;
    void* tracker = client ? getComponent(client, kMoveTrackerType) : nullptr;
    const bool alive = tracker && mem::read(reinterpret_cast<uintptr_t>(tracker) + 0x150, state) && state >= 0 && state < 256;
    g_client = alive ? reinterpret_cast<uintptr_t>(client) : 0;
    g_clientAt = GetTickCount();
    net::setPhysicalState(alive && cfg->playerCollision && available() && !teleport::busy(),
                          state == 19);
}
void clientTick() {
    if (!available()) return;
    std::lock_guard<std::mutex> lock(g_updateMutex);
    const DWORD now = GetTickCount();
    if (now == g_lastUpdate) return;
    g_lastUpdate = now;
    const auto context = mem::readOr<uintptr_t>(rva(kClientContext));
    const auto shared = context ? mem::readOr<uintptr_t>(context + 0x50) : 0;
    if (context != g_context || shared != g_sharedManager) {
        // The engine tears down a replaced world. Never call its old body table.
        // Transform storage stays alive even during deferred native destruction.
        for (auto& b : g_slots) { b.peer = 0; b.id = ~0u; b.object = b.owner = b.manager = 0; b.retryAt = 0; }
        g_context = context; g_sharedManager = shared;
    }
    for (auto& b : g_slots) b.wanted = false;
    const auto generation = net::sessionGeneration();
    if (generation != g_generation) {
        for (auto& b : g_slots) remove(b);
        g_generation = generation;
    }
    const auto cfg = config::get();
    const auto client = g_client.load();
    const bool enabled = shared && level::serverWorld() && !level::inFrontEnd() &&
        !teleport::busy() && !hooks::gameMenuOpen() && client && now - g_clientAt.load() <= 150 &&
        reinterpret_cast<uintptr_t>(reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))()) == client;
    const bool remoteEnabled = enabled && cfg->playerCollision && net::running() &&
        (net::hosting() || net::connectedToHost());
    uintptr_t rigidData = 0, manager = 0;
    LinearTransform local{};
    if (enabled && readEntityTransform(reinterpret_cast<void*>(client), local) && finite3(local.trans)) {
        const auto capsule = getComponent(reinterpret_cast<void*>(client), kCapsuleType);
        const auto data = capsule ? mem::readOr<uintptr_t>(reinterpret_cast<uintptr_t>(capsule) + 0x10) : 0;
        const auto bodies = data ? mem::readOr<uintptr_t>(data + 0x70) : 0;
        rigidData = bodies ? mem::readOr<uintptr_t>(bodies) : 0;
        const auto index = rigidData ? mem::readOr<uint8_t>(rigidData + 0x2a, 0xff) : 0xff;
        manager = index != 0xff ? worldManager(shared, index) : 0;
    }
    if (manager && rigidData && remoteEnabled) {
        for (const auto& peer : net::remotes(kPeerAge)) {
            if (peer.loopback || !peer.collision || peer.physicalAgeMs > kPhysicalAge) continue;
            const auto owner = ghosts::clientCharacter(peer.id);
            net::PlayerState state;
            uint32_t age = 0;
            if (!owner || !net::remoteState(peer.id, state, age) || age > kPeerAge ||
                !(state.flags & net::kStateInLevel) || !finite3(state.pos) || !std::isfinite(state.yaw)) continue;
            float distance2 = 0;
            for (int i = 0; i < 3; ++i) { const float d = state.pos[i] - local.trans[i]; distance2 += d * d; }
            if (distance2 > 80.f * 80.f) continue;
            Body* body = nullptr;
            for (auto& b : g_slots) if (b.peer == peer.id) { body = &b; break; }
            if (!body) for (auto& b : g_slots) if (!b.peer) { body = &b; break; }
            if (!body) continue;
            updateBody(*body, peer.id, owner, peer.crouched, state.pos, state.yaw, rigidData, manager, now);
        }
    }
    uint32_t count = 0;
    for (auto& b : g_slots) {
        if (!b.wanted) remove(b);
        else if (b.object) ++count;
    }
    g_bodies = count;
}
} // namespace mecmp::game::collision
