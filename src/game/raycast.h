#pragma once
#include <cstdint>
// A ray cast into the game's physics world, with the call the game's own ray cast entities make
// (ServerRaycastEntity: 0x143a204b0 builds a query per entity and calls the caster below).
//
// Threads: cast on the server game thread only (where the game casts its own); available from any.

namespace mecmp::game::raycast {

struct Hit {
    float pos[3] = {0, 0, 0};
    float normal[3] = {0, 0, 0};
    float distance = 0.f; // from the start of the ray, metres
};

// False once the caster did not look like this build's (casts then always miss; logged).
bool available();
// True if something lies on the segment from..to; `out` is the nearest hit. False between levels.
bool cast(const float from[3], const float to[3], Hit& out);
// Camera obstruction: excludes native character physics bodies, including Faith's own capsule.
// Uses the native query's body-id exclusion list; solid scenery still counts at any distance.
bool castCamera(const float from[3], const float to[3], Hit& out, uint32_t ownBodyId = ~0u);

} // namespace mecmp::game::raycast
