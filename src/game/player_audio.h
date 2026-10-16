#pragma once
#include <string>
#include <cstdint>
namespace mecmp::game::player_audio {
bool install();
// Called from the game's client replay updater, where native sound entities run.
void tick();
std::string status();
uintptr_t spatialVoiceTemplate(); // loaded Faith graph; borrowed, client update thread
}
