#pragma once

#include <cstdint>

namespace mecmp::game::collision {
void install();
bool available();
uint32_t bodyCount(); // registered native remote-player capsules
void tick(); // server game thread
void clientTick(); // native client replay updater; physics lifecycle and transforms
}
