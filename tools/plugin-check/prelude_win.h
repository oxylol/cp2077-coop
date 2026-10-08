// Declarations of MSVC-only intrinsics so mingw can parse the SDK for this check. Never linked.
#pragma once
#include <windows.h>
extern "C" char InterlockedExchange8(char volatile* Target, char Value);
extern "C" char _InterlockedExchangeAdd8(char volatile* Addend, char Value);
