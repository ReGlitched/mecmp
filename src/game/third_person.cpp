#include "game/third_person.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "core/config.h"
#include "core/log.h"
#include "game/engine.h"
#include "game/hooks.h"
#include "game/level.h"
#include "game/raycast.h"

namespace mecmp::game::third_person {
namespace {
// Native FPSCamera output and the character's embedded view-mode interface.
constexpr uintptr_t kCameraSlot = 0x141b18ec8, kCameraGetTransform = 0x14327fb80;
constexpr uintptr_t kViewModeSlot = 0x141c3cc68, kGetViewMode = 0x14390ed90;
constexpr size_t kCharacterInterface = 0x1d8;
constexpr uintptr_t kMoveTrackerType = 0x142853420;
constexpr uintptr_t kCapsuleType = 0x1427bd350;
struct V3 {
    float x = 0, y = 0, z = 0;
    V3 operator+(V3 b) const { return {x + b.x, y + b.y, z + b.z}; }
    V3 operator-(V3 b) const { return {x - b.x, y - b.y, z - b.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
};
V3 vec(const float* p) { return {p[0], p[1], p[2]}; }
void store(float* p, V3 v) { p[0] = v.x; p[1] = v.y; p[2] = v.z; }
float length2(V3 v) { return v.x * v.x + v.y * v.y + v.z * v.z; }
float horizontal2(V3 v) { return v.x * v.x + v.z * v.z; }
bool finite(V3 v, float limit = 100000.f) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
        std::fabs(v.x) <= limit && std::fabs(v.y) <= limit && std::fabs(v.z) <= limit;
}
bool validTransform(const LinearTransform& t) {
    if (!finite(vec(t.trans))) return false;
    for (const float* row : {t.right, t.up, t.forward}) {
        const float n = length2(vec(row));
        if (!std::isfinite(n) || n < .95f || n > 1.05f) return false;
    }
    return true;
}

using CameraFn = void (*)(void*, void*, void*);
using ViewFn = int (*)(void*);
CameraFn g_camera = nullptr;
ViewFn g_view = nullptr;
std::atomic<bool> g_cameraReady{false};
std::atomic<uintptr_t> g_client{0};
std::atomic<DWORD> g_clientAt{0}, g_thirdActiveAt{0};

// Camera output is sampled on the client, but Havok queries belong to the server tick.
struct CameraProbe {
    uintptr_t client = 0, camera = 0;
    DWORD at = 0;
    V3 eye, direction;
    float distance = 0;
};
std::mutex g_probeM;
CameraProbe g_probe, g_clearanceProbe;
float g_clearance = 0;
// Camera thread only: recover smoothly after an obstruction, retract immediately.
uintptr_t g_renderClient = 0, g_renderCamera = 0;
DWORD g_renderAt = 0;
float g_renderDistance = 0;

uintptr_t cameraManager() {
    const uintptr_t managers = mem::readOr<uintptr_t>(rva(kClientCameraViewManagers));
    const uintptr_t list = managers ? mem::readOr<uintptr_t>(managers + CamMgrs_List) : 0;
    return list ? mem::readOr<uintptr_t>(list) : 0;
}

int viewMode(void* characterInterface) {
    const uintptr_t client = g_client.load();
    if (client && reinterpret_cast<uintptr_t>(characterInterface) == client + kCharacterInterface &&
        g_thirdActiveAt.load() && GetTickCount() - g_thirdActiveAt.load() < 150)
        return 1; // native third-person mesh selection
    return g_view(characterInterface);
}

void cameraTransform(void* camera, void* info, void* output) {
    g_camera(camera, info, output);
    const auto cfg = config::get();
    if (!cfg->thirdPerson || !output || !g_cameraReady || hooks::gameMenuOpen()) {
        g_thirdActiveAt = 0;
        return;
    }
    const uintptr_t client = g_client.load(), manager = cameraManager();
    const DWORD now = GetTickCount();
    if (!client || now - g_clientAt.load() > 150 || !manager ||
        mem::readOr<uintptr_t>(manager + CamMgr_ActiveCamera) != reinterpret_cast<uintptr_t>(camera)) {
        g_thirdActiveAt = 0;
        return;
    }
    auto* transforms = static_cast<LinearTransform*>(output);
    if (!validTransform(transforms[0]) || !validTransform(transforms[1])) { g_thirdActiveAt = 0; return; }
    LinearTransform body;
    if (!readEntityTransform(reinterpret_cast<void*>(client), body) ||
        length2(vec(transforms[0].trans) - vec(body.trans)) > 16.f) {
        g_thirdActiveAt = 0; // detached / cinematic camera
        return;
    }
    // Keep native look input, FOV, camera shake and parkour camera rotation.
    // Frostbite looks down -forward, so +forward moves the camera behind Faith.
    const V3 direction = vec(transforms[0].forward);
    const V3 eye = vec(transforms[0].trans);
    float distance = std::clamp(cfg->thirdPersonDistance, 1.5f, 6.f);
    {
        std::lock_guard<std::mutex> lock(g_probeM);
        const auto& p = g_clearanceProbe;
        // A turn changes the native forward vector every frame. Do not collapse the
        // camera into Faith's head while the next server probe catches up. Keep the
        // most recent obstruction distance for this camera; teleports and stale
        // results still suspend the override until a new probe arrives.
        const bool fresh = p.client == client && p.camera == reinterpret_cast<uintptr_t>(camera) &&
            now - p.at < 150 && length2(p.eye - eye) < 9.f;
        g_probe = {client, reinterpret_cast<uintptr_t>(camera), now, eye, direction, cfg->thirdPersonDistance};
        if (!fresh) { g_thirdActiveAt = 0; return; }
        distance = std::min(distance, g_clearance);
    }
    if (g_renderClient == client && g_renderCamera == reinterpret_cast<uintptr_t>(camera) && now - g_renderAt < 150)
        distance = std::min(distance, g_renderDistance + 8.f * static_cast<float>(now - g_renderAt) / 1000.f);
    g_renderClient = client; g_renderCamera = reinterpret_cast<uintptr_t>(camera);
    g_renderAt = now; g_renderDistance = distance;
    // The two transforms are used for the camera and camera mesh. Translate both by the same amount.
    const V3 offset = direction * distance;
    store(transforms[0].trans, vec(transforms[0].trans) + offset);
    store(transforms[1].trans, vec(transforms[1].trans) + offset);
    // Very tight spaces use the native first-person body selection so the head
    // does not fill the view when a real wall pushes the camera against Faith.
    g_thirdActiveAt = distance >= .6f ? now : 0;
}

} // namespace

bool available() { return g_cameraReady.load(); }

void install() {
    uintptr_t camera = 0, view = 0;
    if (mem::read(rva(kCameraSlot), camera) && camera == rva(kCameraGetTransform) &&
        mem::read(rva(kViewModeSlot), view) && view == rva(kGetViewMode)) {
        g_camera = reinterpret_cast<CameraFn>(camera);
        g_view = reinterpret_cast<ViewFn>(view);
        if (mem::patchPointer(rva(kViewModeSlot), reinterpret_cast<uintptr_t>(&viewMode), nullptr)) {
            if (mem::patchPointer(rva(kCameraSlot), reinterpret_cast<uintptr_t>(&cameraTransform), nullptr))
                g_cameraReady = true;
            else mem::patchPointer(rva(kViewModeSlot), view, nullptr);
        }
    }
    MLOG("third person: camera %s", available() ? "available" : "unavailable");
}

void tick() {
    const auto cfg = config::get();
    const bool inLevel = level::serverWorld() && !level::inFrontEnd();
    void* client = inLevel && cfg->thirdPerson
        ? reinterpret_cast<void* (*)()>(rva(Fn_LocalClientCharacter))() : nullptr;
    int state = -1;
    void* tracker = client ? getComponent(client, kMoveTrackerType) : nullptr;
    const bool alive = tracker && mem::read(reinterpret_cast<uintptr_t>(tracker) + 0x150, state) && state >= 0 && state < 256;
    g_client = alive ? reinterpret_cast<uintptr_t>(client) : 0;
    g_clientAt = GetTickCount();
    if (!alive || !cfg->thirdPerson || !available()) {
        g_thirdActiveAt = 0;
        std::lock_guard<std::mutex> lock(g_probeM);
        g_probe = {}; g_clearanceProbe = {};
        return;
    }
    CameraProbe p;
    {
        std::lock_guard<std::mutex> lock(g_probeM);
        p = g_probe;
    }
    if (p.client != reinterpret_cast<uintptr_t>(client) || GetTickCount() - p.at >= 150) return;
    // Catalyst uses ClientCapsuleCollisionComponent for Faith, whose registered
    // physics body can be a plain RigidBody. Exclude its full ID explicitly as
    // the component's native enable/disable handlers do (+0x40 ID, +0x44 active).
    const auto capsule = reinterpret_cast<uintptr_t>(getComponent(client, kCapsuleType));
    const uint32_t ownBody = capsule && mem::readOr<uint8_t>(capsule + 0x44)
        ? mem::readOr<uint32_t>(capsule + 0x40, ~0u) : ~0u;
    float clearance = std::clamp(p.distance, 1.5f, 6.f);
    // Three parallel probes give the near plane space next to walls. Native
    // physics body IDs exclude characters without skipping nearby world geometry.
    V3 right = {p.direction.z, 0, -p.direction.x};
    const float rightLength = std::sqrt(horizontal2(right));
    if (rightLength > .01f) right = right * (.16f / rightLength);
    for (V3 side : {V3{}, right, right * -1.f}) {
        const V3 from = p.eye + side;
        const V3 to = p.eye + p.direction * clearance + side;
        float a[3], b[3]; store(a, from); store(b, to);
        raycast::Hit hit;
        if (raycast::castCamera(a, b, hit, ownBody))
            clearance = std::max(.15f, std::min(clearance, hit.distance - .2f));
    }
    {
        std::lock_guard<std::mutex> lock(g_probeM);
        g_clearanceProbe = p;
        g_clearance = clearance;
    }
}

} // namespace mecmp::game::third_person
