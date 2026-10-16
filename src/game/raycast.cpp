#include "game/raycast.h"

#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/engine.h"

namespace mecmp::game::raycast {
namespace {
std::atomic<int> g_available{-1}; // -1 unchecked, 0 no, 1 yes

// What the caster reads (MOVAPS: 16-byte aligned). A radius above ~0 makes it a sphere sweep.
struct alignas(16) Query {
    float from[4] = {0, 0, 0, 0};
    float to[4] = {0, 0, 0, 0};
    float radius = 0.f;
    uint8_t flags[3] = {}; // set by the entity's query builder (0x143a22330); the caster does not read them
    uint8_t pad[9] = {};
};
static_assert(sizeof(Query) == 0x30 && offsetof(Query, radius) == 0x20, "Query layout");

// What the caster writes. A miss: position = to, fraction = 1, distance = |to - from|, hit = 0.
struct alignas(16) Result {
    float position[4] = {0, 0, 0, 0};
    float normal[4] = {0, 0, 0, 0};
    uint32_t id = 0;
    float fraction = 0.f;
    float distance = 0.f;
    uint8_t hit = 0;
    uint8_t pad2d[3] = {};
    uint16_t tail = 0; // always 0x0101
    uint8_t pad[0x0e] = {};
};
static_assert(sizeof(Result) == 0x40 && offsetof(Result, id) == 0x20 && offsetof(Result, hit) == 0x2c &&
              offsetof(Result, tail) == 0x30, "Result layout");

// Checked against the code: the caster's prologue (it keeps 0x1d0 bytes of locals).
bool checkFunction() {
    int a = g_available.load();
    if (a >= 0) return a == 1;
    static const uint8_t kExpect[] = {0x48, 0x89, 0xE0, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D,
                                      0xA8, 0x08, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xD0, 0x01, 0x00, 0x00};
    uint8_t got[sizeof(kExpect)] = {};
    const bool ok = mem::read(rva(Fn_RaycastQuery), got, sizeof(got)) && memcmp(got, kExpect, sizeof(got)) == 0;
    if (!ok) {
        char hex[sizeof(got) * 3 + 1] = {};
        for (size_t i = 0; i < sizeof(got); ++i) snprintf(hex + i * 3, 4, "%02X ", got[i]);
        MLOG("raycast: the ray cast function looks different in this build (%s); markers go where you stand", hex);
    }
    g_available = ok ? 1 : 0;
    return ok;
}

// The server world's physics, as the game's ray cast update finds it (0x143a1fd7a; the last three
// steps are the getter 0x1434ee940). Every object the cast then reads through is checked first, so
// a world that is going away gives a miss instead of a fault.
uintptr_t physics(uintptr_t world) {
    const uintptr_t manager = mem::readOr<uintptr_t>(world + World_PhysicsManager);
    const uintptr_t holder = manager ? mem::readOr<uintptr_t>(manager + PhysManager_Holder) : 0;
    const uintptr_t inner = holder ? mem::readOr<uintptr_t>(holder) : 0;
    const uintptr_t phys = inner ? mem::readOr<uintptr_t>(inner + PhysHolder_Physics) : 0;
    if (!phys || !inImage(mem::readOr<uintptr_t>(phys))) return 0;
    const uintptr_t havok = mem::readOr<uintptr_t>(phys + Phys_HavokWorld);
    const uintptr_t filterOwner = havok ? mem::readOr<uintptr_t>(havok + HavokWorld_FilterOwner) : 0;
    if (!filterOwner || !mem::readOr<uintptr_t>(filterOwner + FilterOwner_Filter)) return 0;
    return phys;
}

bool finite3(const float* v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::fabs(v[0]) < 100000.f &&
           std::fabs(v[1]) < 100000.f && std::fabs(v[2]) < 100000.f;
}

constexpr uintptr_t kClosestRay = 0x1434e27f0;
constexpr uintptr_t kCharacterBodyType = 0x142831080;
constexpr uintptr_t kCharacterPhysicsType = 0x142830800;

// Native ray-query layout, from the plain-ray branch of 0x143a24890 and its
// collector 0x143512f10. Exclusions are exact physics body IDs, not distances
// or surface normals. The collector reads this list and never modifies it.
struct alignas(16) CameraQuery {
    uint32_t flags = 1, pad4 = 0, filter = 9, padC = 0;
    const uint32_t* ignoredBegin = nullptr;
    const uint32_t* ignoredEnd = nullptr;
    const uint32_t* ignoredCapacity = nullptr;
    uint8_t reserved[0x38] = {};
    float from[4] = {};
    float to[4] = {};
    uint8_t tail[16] = {};
};
static_assert(offsetof(CameraQuery, from) == 0x60 && offsetof(CameraQuery, to) == 0x70 &&
              sizeof(CameraQuery) == 0x90, "Native camera ray query layout");
struct alignas(16) CameraResult {
    uint32_t id = ~0u, pad4 = 0;
    uint64_t subId = ~uint64_t{};
    uint32_t part = ~0u, pad14 = 0;
    float fraction = 1.f;
    uint32_t pad1C = 0;
    float normal[4] = {}, position[4] = {};
    uint8_t tail[16] = {};
};
static_assert(offsetof(CameraResult, fraction) == 0x18 && offsetof(CameraResult, normal) == 0x20 &&
              offsetof(CameraResult, position) == 0x30 && sizeof(CameraResult) == 0x50, "Native ray result layout");

bool cameraLayoutMatches() {
    static const bool ok = [] {
        const uint8_t expected[] = {0x49,0x89,0xe3,0x57,0x48,0x81,0xec,0xf0,0x02,0,0};
        uint8_t got[sizeof(expected)] = {};
        if (!mem::read(rva(kClosestRay), got, sizeof(got))) return false;
        // Both encodings are MOV r11,rsp; the unpacked executable uses either.
        const bool move = !std::memcmp(got, expected, 3) ||
            (got[0] == 0x4c && got[1] == 0x8b && got[2] == 0xdc);
        return move && !std::memcmp(got + 3, expected + 3, sizeof(got) - 3);
    }();
    return ok;
}

bool characterBody(uintptr_t phys, uint32_t id) {
    // PhysicsManager::getBody (0x1434ee750): low 16 bits index the body table.
    // The collector passes PhysicsWorld +8 (the manager), not the world wrapper.
    const auto manager = mem::readOr<uintptr_t>(phys + 8);
    const auto table = manager ? mem::readOr<uintptr_t>(manager + 0x130) : 0;
    const auto end = manager ? mem::readOr<uintptr_t>(manager + 0x138) : 0;
    const auto slot = table + static_cast<size_t>(id & 0xffffu) * 8;
    void* body = table && slot < end ? mem::readOr<void*>(slot) : nullptr;
    return body && (isKindOf(body, kCharacterBodyType) || isKindOf(body, kCharacterPhysicsType));
}
} // namespace

bool available() { return checkFunction(); } // checked once (on the first call), then remembered

bool cast(const float from[3], const float to[3], Hit& out) {
    if (!checkFunction() || !finite3(from) || !finite3(to)) return false;
    const uintptr_t world = mem::readOr<uintptr_t>(rva(kServerGameWorld));
    const uintptr_t phys = world ? physics(world) : 0;
    if (!phys) return false;
    Query q;
    Result r;
    memcpy(q.from, from, sizeof(float) * 3);
    memcpy(q.to, to, sizeof(float) * 3);
    using Fn = void (*)(const Query*, void* world, void* phys, Result*);
    reinterpret_cast<Fn>(rva(Fn_RaycastQuery))(&q, reinterpret_cast<void*>(world), reinterpret_cast<void*>(phys), &r);
    if (r.hit != 1 || !finite3(r.position)) return false;
    memcpy(out.pos, r.position, sizeof(out.pos));
    memcpy(out.normal, r.normal, sizeof(out.normal));
    out.distance = r.distance;
    return true;
}

bool castCamera(const float from[3], const float to[3], Hit& out, uint32_t ownBodyId) {
    if (!checkFunction() || !finite3(from) || !finite3(to)) return false;
    if (!cameraLayoutMatches()) return cast(from, to, out);
    const uintptr_t world = mem::readOr<uintptr_t>(rva(kServerGameWorld));
    const uintptr_t phys = world ? physics(world) : 0;
    if (!phys) return false;
    std::array<uint32_t, 16> ignored{};
    size_t count = 0;
    if (ownBodyId != ~0u) ignored[count++] = ownBodyId;
    CameraQuery q;
    std::memcpy(q.from, from, sizeof(float) * 3);
    std::memcpy(q.to, to, sizeof(float) * 3);
    q.ignoredBegin = ignored.data();
    q.ignoredCapacity = ignored.data() + ignored.size();
    const float dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    using Fn = bool (*)(void*, const CameraQuery*, CameraResult*);
    for (; count <= ignored.size(); ++count) {
        CameraResult r;
        q.ignoredEnd = ignored.data() + count;
        if (!reinterpret_cast<Fn>(rva(kClosestRay))(reinterpret_cast<void*>(phys), &q, &r)) return false;
        if (!finite3(r.position) || !finite3(r.normal) || !std::isfinite(r.fraction) ||
            r.fraction < 0.f || r.fraction > 1.f) return false;
        // At the bound, treat the last hit as an obstruction rather than allowing wall clipping.
        if (count == ignored.size() || !characterBody(phys, r.id)) {
            std::memcpy(out.pos, r.position, sizeof(out.pos));
            std::memcpy(out.normal, r.normal, sizeof(out.normal));
            out.distance = length * r.fraction;
            return true;
        }
        ignored[count] = r.id;
    }
    return false;
}

} // namespace mecmp::game::raycast
