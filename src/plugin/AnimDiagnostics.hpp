#pragma once

#include <filesystem>
#include <string>

#include <RED4ext/RED4ext.hpp>
#include <RedLib.hpp>

namespace coop::plugin
{
// Writes what the animation graphs of an entity's animated components accept: per component, the graph resource
// (path hash, so two bodies can be compared), its variables (bool/int/float/vector inputs with default, min, max)
// and its AnimFeature slots (name and class). Round H showed V's captured inputs don't animate the lookalike;
// this says whether its graph has those inputs at all, and which inputs carry speed and direction.
// Returns a one-line summary.
std::string DumpAnimGraphs(Red::IScriptable* aEntity, const std::string& aLabel, const std::filesystem::path& aFile);
} // namespace coop::plugin
