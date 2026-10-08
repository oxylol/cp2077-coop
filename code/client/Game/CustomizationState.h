#pragma once

#include "Game/CharacterCustomizationSystem.h"

// The local V's character customization state (what the character creator made: face, hair, body), which the client
// serializes and sends so the others see this V as it is.
//
// Upstream reads it from the customization system at +0x78, which PR 58 found null during normal play on patch 2.31
// (it serialized nothing, so remote players had no look). This tries, in order, and logs which one worked:
//   1. the +0x78 handle (patch 2.2);
//   2. any function of the customization system that takes no arguments and returns a customization state;
//   3. every handle in the first 0x400 bytes of the system that points at a customization state (the field may
//      simply have moved).
// The first time it runs it also logs the system's functions, so a test run shows what 2.31 offers.
Red::Handle<Red::game::ui::CharacterCustomizationState> FindLocalCustomizationState();
