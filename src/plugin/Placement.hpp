#pragma once

#include <cstdint>
#include <string>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

namespace coop::plugin
{
// Ways to put a body at a position every frame (direct drive, docs/01-architecture.md §4). Round F showed that the
// teleportation facility doesn't move a spawned NPC, with or without its AI, so the others are tried too. Every
// call goes through RTTI, so a function this game or Codeware version doesn't have is an error message, not a
// script compile failure.
enum class PlaceMethod : int32_t
{
    Auto = 0,       // the bridge picks: Transform, then AITeleport, then AI walking
    Teleport = 1,   // GameInstance.GetTeleportationFacility().Teleport (moves vehicles and V, not NPCs; round F)
    Transform = 2,  // Codeware's Entity.SetWorldTransform: sets the entity's root transform directly
    AITeleport = 3, // an AITeleportCommand through the NPC's own AI (the AI must be on)
    Count = 4,
};

const char* PlaceMethodName(int32_t aMethod);
// "auto", "teleport", "transform", "aiteleport" (case-insensitive); unknown text gives Auto.
int32_t ParsePlaceMethod(const std::string& aText);
// Whether the method needs the body's AI switched on.
bool PlaceMethodNeedsAI(int32_t aMethod);

// Puts aEntity at aPosition facing aYaw (degrees, game convention). False with aError set if the method isn't
// available or the call failed. Only calls the engine; whether the body really moved is for the caller to check.
bool PlaceEntity(const Red::Handle<Red::IScriptable>& aEntity, const Red::Vector4& aPosition, float aYaw,
                 int32_t aMethod, std::string& aError);

// Switches every component of aEntity whose class is aClassName (or derives from it) on or off. Returns how many,
// or -1 with aError set.
int SetComponentsEnabled(Red::IScriptable* aEntity, const std::string& aClassName, bool aEnabled, std::string& aError);

// Drops remembered AI teleport commands of bodies that are gone.
void ForgetPlacementState();
} // namespace coop::plugin
