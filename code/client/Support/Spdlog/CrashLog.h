#pragma once

// Writes where the game crashed into the mod's log before the game's own crash handler takes over: the exception,
// the faulting address and the call stack, each as module+offset ("CyberpunkCoop.dll+0x1a2b3c"), then the mod's own
// frames as function and source line (from CyberpunkCoop.pdb next to the DLL).
namespace Support::CrashLog
{
void Install(const std::filesystem::path& acLogPath);
void Uninstall();
// From the main loop, every frame (the first call is taken to be on the main thread): the watchdog logs where the main
// thread is when this stops for a while.
void Heartbeat();
} // namespace Support::CrashLog
