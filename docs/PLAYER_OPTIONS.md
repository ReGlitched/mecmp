# Third person and player collision

Open **You → Gameplay**. Settings are saved in `mecmp.ini`:

```ini
[third_person]
enabled=0
distance=3
horizontal_offset=0
vertical_offset=0

[collision]
enabled=0
```

The camera distance is 1.5–6 metres. **Left / right offset** and **Up / down
offset** each range from −2 to +2 metres and default to zero. Positive values
move right/up along the camera axes; negative values move left/down. These
offsets follow native camera rotation, including pitch and parkour roll.
Third person keeps native look controls, FOV, camera rotation and the regular
Faith skeleton. `game/third_person.cpp` changes
the native camera output and uses native third-person body selection. There is
no NPC rig replacement, pose remapping, duplicate player or echo recorder.

The camera checks three rays along the full requested camera offset from the
eye on the server simulation thread and retracts at walls. The ray spacing
uses the native camera right axis. Obstructions shorten distance and both
offsets together along that path. The native query excludes Faith's ClientCapsuleCollisionComponent
body ID and other native character physics bodies by ID instead
of guessing from surface normals. Solid scenery still counts close to the eye.
The client retains a recent obstruction distance through ordinary turns while
the next server probe catches up (at most 150 ms). It no longer snaps to 15 cm
whenever the camera direction changes. Teleports, stale probes and a new camera
suspend the override until the next result; the native view is used while waiting.
Retraction is immediate, recovery is limited to 8 m/s, and distances below 60 cm
use first-person body selection to keep the head out of the view. Game menus,
missing/dead characters and detached cameras suspend the override.

For collision, both runners must enable **Player collision**. `game/collision.cpp`
creates a real native Havok box / `fb::RigidBody` for each visible, nearby,
opted-in remote player through Catalyst's `PhysicsEntityFactory`. The native
physics manager registers it in the world and spatial query manager. Its
transform follows bounded, interpolated remote position and yaw on the client
update thread. Faith's movement queries and collision handling resolve contacts;
MECMP does not set the local player's position or velocity for collision response.

The box profile is hardcoded from Faith's model proportions in world metres:
**0.44 m width, 0.44 m depth, 1.70 m standing height and 1.00 m crouched height**,
at a fixed model scale of 1.0. Half-extents are `(0.22, height / 2, 0.22)` and
the transform is centred half the height above the received feet position.
The box has flat faces and follows yaw while remaining upright. Unlike the
old capsule, it needs no local-X-to-world-Y rotation or hemisphere subtraction.
Standing/crouching recreates the shape through native removal and creation;
other animations do not refit the shape. Position or yaw changes update its
registered body transform.

Collision does not query bones, evaluated model bounds, skinning/ANT state, or
render/culling bounds. It uses the existing position, yaw and crouch announcements;
there is no additional bounds packet. The box retains the zero-mass native body
kind and StaticLayer collision filter. Faith's own movement sweep still contributes
its normal radius at contact. The menu reports registered remote body count.
The local pose-copy tool, microphone echo and custom springboard perception
bridge have been removed, including their bindings and settings.

Hidden players, loopback ghosts, updates older than 300 ms, expired collision
announcements, players beyond 80 m, leaving multiplayer, disabling collision and
local teleports remove the bodies. Removal uses native spatial unregistration,
disable and deferred destruction. Body table identity, native type, retained
transform source and the full Havok user-data ID are checked before operations.
The engine owns teardown when its physics manager is replaced. Body transform
storage has process lifetime so deferred engine operations cannot use freed memory.
Shared character/physics assets and the Faith skeleton are never modified.

The native move tracker supplies crouch state 19, so keyboard and controller
remapping both work. Optional HELLO flags announce collision (0x10) and
crouch (0x20). The retired 0x40 bit is ignored and remains unused. Changes
are repeated three times at a bounded rate and
normal heartbeats renew them. STATE packets, bone protections and protocol 7
remain unchanged. Existing relays preserve these flags; older clients ignore
them and continue to appear without collisions.

Native points for the supported PC executable (image base 0x140000000):

| Purpose | Address / layout |
|---|---|
| FPSCamera transform output | vtable slot 0x141b18ec8 → 0x14327fb80 |
| Camera ray / exclusions | 0x1434e27f0; query IDs +0x10/+0x18, segment +0x60/+0x70 |
| Character physics body | ClassInfo 0x142831080; PhysicsWorld +8 → manager +0x130 body table |
| Faith's collision capsule | ClassInfo 0x1427bd350; body ID +0x40, active +0x44 (0x142d57660) |
| Character view mode | embedded interface +0x1d8, slot 0x141c3cc68 → 0x14390ed90 |
| Native move tracker | ClassInfo 0x142853420, state +0x150 |
| Body factory / native box constructor | 0x1435169e0; branch 0x143516562 → 0x143f2f140 |
| Box description | shape type 1; 16-byte-aligned half-extents vector at +0x20; body kind 2 at +0x14 |
| Physics creation info | 0x142d4d1a0; owner entity bus +0x28 |
| Body transform / source | 0x1434f8930 / 0x1434f6d90 |
| Native body enable / spatial registration | 0x1434e90f0 / 0x1434e9270 |
| Spatial removal / disable / deferred destruction | 0x1434f6610 / 0x1434f65f0 / 0x1434f5550 |

Camera hooks require both expected vtable slots. Collision validates the native
box constructor branch, shared factory call sites and rigid-body virtuals before enabling the feature.
Collision body lifecycle and transforms run from the native client replay updater;
camera world probes run on the server thread. The old local movement job hook,
swept overlap solver and movement-controller transform/velocity setters are removed.

Validation for this change: Release build only. No automated or gameplay tests were run.
