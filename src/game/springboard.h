#pragma once

namespace mecmp::game::springboard {
// Adds crouched runners to the native SpringboardProcessor's obstacle perception.
// Catalyst's own history checks, jump input, driver, animation and movement remain in charge.
void install();
bool available();
void tick(); // server tick: identify the local ANT instance
}
