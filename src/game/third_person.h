#pragma once

namespace mecmp::game::third_person {
void install();
bool available();
void tick(); // server game thread
}
