#pragma once

struct AppearanceComponent
{
    Vector<uint64_t> equipment; // TweakDBIDs (SpawnCharacterRequest)
    Vector<uint8_t> ccstate;
};