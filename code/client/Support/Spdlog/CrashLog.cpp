#include "CrashLog.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <cstdio>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace
{
// Everything here runs while the game is crashing, possibly with a broken heap or with a lock held by the crashed
// code: fixed buffers, no allocation, no logger, no locks. The text is appended to the log file directly.
wchar_t s_logPath[MAX_PATH * 2]{};
void* s_pHandler = nullptr;
std::atomic<int> s_reports{0};
// Set while this thread writes a report: what faults in there (reading what a register points at) is the report's own
// to handle (__except), not another crash.
thread_local bool t_reporting = false;

// The ones that end the game. Others (C++ exceptions, breakpoints, guard pages) are part of normal running.
bool IsFatal(DWORD aCode)
{
    switch (aCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
    case 0xC0000420: // STATUS_ASSERTION_FAILURE
        return true;
    default:
        return false;
    }
}

void Append(const char* acText, int aLength)
{
    const HANDLE file = CreateFileW(s_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, acText, static_cast<DWORD>(aLength), &written, nullptr);
    CloseHandle(file);
}

// "CyberpunkCoop.dll+0x1a2b3c", or the bare address outside any module.
int Describe(uintptr_t aAddress, char* apOut, size_t aSize)
{
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(aAddress), &module) &&
        module)
    {
        char path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
        const char* name = path;
        for (DWORD i = 0; i < length; ++i)
        {
            if (path[i] == '\\' || path[i] == '/')
                name = path + i + 1;
        }
        return std::snprintf(apOut, aSize, "%s+0x%llx", name,
                             static_cast<unsigned long long>(aAddress - reinterpret_cast<uintptr_t>(module)));
    }
    return std::snprintf(apOut, aSize, "0x%llx", static_cast<unsigned long long>(aAddress));
}

// What a register points at, when it's an object with a vtable: the vtable (module+offset) and, with MSVC's run-time
// type information, its class (".?AVFoo@@"). A bad call through an object names the object. 0 when it isn't one.
int DescribeObject(uintptr_t aPointer, char* apOut, size_t aSize)
{
    __try
    {
        if (aPointer < 0x10000)
            return 0;
        const auto vtable = *reinterpret_cast<const uintptr_t*>(aPointer);
        int written = std::snprintf(apOut, aSize, " [0x%llx]", static_cast<unsigned long long>(vtable));
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(vtable), &module) ||
            !module)
            return written;

        written += std::snprintf(apOut + written, aSize - written, " = ");
        written += Describe(vtable, apOut + written, aSize - written);

        // x64: the complete object locator just before the vtable, signature 1, its type descriptor's offset from the
        // image at +12 and its own at +20; the descriptor's name follows its two pointers.
        const auto* pLocator = *reinterpret_cast<const uint32_t* const*>(vtable - sizeof(void*));
        if (pLocator[0] == 1)
        {
            const auto image = reinterpret_cast<uintptr_t>(pLocator) - pLocator[5];
            const auto* pName = reinterpret_cast<const char*>(image + pLocator[3] + 2 * sizeof(void*));
            if (pName[0] == '.' && pName[1] == '?')
                written += std::snprintf(apOut + written, aSize - written, " %.160s", pName);
        }
        return written;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// The return addresses from the crashed function up, unwound from aContext (which is changed: a copy, kept off the
// stack, as a stack overflow leaves little of it). A frame that can't be read ends it.
int Walk(CONTEXT& aContext, DWORD64* apFrames, int aMax)
{
    int count = 0;
    __try
    {
        while (count < aMax && aContext.Rip != 0)
        {
            apFrames[count++] = aContext.Rip;

            DWORD64 imageBase = 0;
            const auto pFunction = RtlLookupFunctionEntry(aContext.Rip, &imageBase, nullptr);
            if (pFunction)
            {
                void* pHandlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, aContext.Rip, pFunction, &aContext, &pHandlerData,
                                 &establisherFrame, nullptr);
            }
            else
            {
                // A leaf function: the return address is on top of the stack.
                aContext.Rip = *reinterpret_cast<DWORD64*>(aContext.Rsp);
                aContext.Rsp += 8;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return count;
}

// The mod's own frames as function and source line, from CyberpunkCoop.pdb next to the DLL (in the release zip).
// After the raw report: if this fails, nothing is lost.
void AppendSourceLines(const DWORD64* apFrames, int aCount)
{
    const auto base = reinterpret_cast<DWORD64>(&__ImageBase);
    const auto* pHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(&__ImageBase) +
                                                                     __ImageBase.e_lfanew);
    const DWORD64 size = pHeaders->OptionalHeader.SizeOfImage;

    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), modulePath, MAX_PATH))
        return;
    wchar_t folder[MAX_PATH]{};
    std::copy(std::begin(modulePath), std::end(modulePath), folder);
    for (auto i = static_cast<int>(wcslen(folder)) - 1; i >= 0; --i)
    {
        if (folder[i] == L'\\' || folder[i] == L'/')
        {
            folder[i] = L'\0';
            break;
        }
    }

    // Not the process handle: the game's own crash handling may use DbgHelp with that one.
    const auto session = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x43434F50));
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    if (!SymInitializeW(session, folder, FALSE))
        return;

    static char s_text[8192];
    int length = 0;
    const auto add = [&](int aWritten)
    {
        if (aWritten > 0)
            length = (length + aWritten < static_cast<int>(sizeof(s_text))) ? length + aWritten
                                                                             : static_cast<int>(sizeof(s_text)) - 1;
    };

    if (SymLoadModuleExW(session, nullptr, modulePath, nullptr, base, static_cast<DWORD>(size), nullptr, 0))
    {
        add(std::snprintf(s_text + length, sizeof(s_text) - length, "[crash] In the mod:\r\n"));
        for (int i = 0; i < aCount; ++i)
        {
            if (apFrames[i] < base || apFrames[i] >= base + size)
                continue;

            // A return address points after the call: look up the call itself.
            const DWORD64 address = i == 0 ? apFrames[i] : apFrames[i] - 1;

            alignas(SYMBOL_INFO) static char s_symbolBuffer[sizeof(SYMBOL_INFO) + 512];
            auto* pSymbol = reinterpret_cast<SYMBOL_INFO*>(s_symbolBuffer);
            *pSymbol = {};
            pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            pSymbol->MaxNameLen = 511;
            DWORD64 symbolOffset = 0;
            const bool named = SymFromAddr(session, address, &symbolOffset, pSymbol);

            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineOffset = 0;
            const bool located = SymGetLineFromAddr64(session, address, &lineOffset, &line);

            add(std::snprintf(s_text + length, sizeof(s_text) - length, "[crash]   %2d %s", i,
                              named ? pSymbol->Name : "?"));
            if (located)
                add(std::snprintf(s_text + length, sizeof(s_text) - length, " (%s:%lu)", line.FileName,
                                  line.LineNumber));
            add(std::snprintf(s_text + length, sizeof(s_text) - length, "\r\n"));
        }
    }
    SymCleanup(session);

    Append(s_text, length);
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* apInfo)
{
    if (!apInfo || !apInfo->ExceptionRecord || !apInfo->ContextRecord || !IsFatal(apInfo->ExceptionRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;
    if (t_reporting)
        return EXCEPTION_CONTINUE_SEARCH;

    // The game may survive one it handles itself; a few are enough to find the real one.
    if (s_reports.fetch_add(1) >= 3)
        return EXCEPTION_CONTINUE_SEARCH;
    t_reporting = true;

    static char s_text[8192];
    int length = 0;
    const auto add = [&](int aWritten)
    {
        if (aWritten > 0)
            length = (length + aWritten < static_cast<int>(sizeof(s_text))) ? length + aWritten
                                                                             : static_cast<int>(sizeof(s_text)) - 1;
    };

    const auto* pRecord = apInfo->ExceptionRecord;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    add(std::snprintf(s_text + length, sizeof(s_text) - length,
                      "\r\n[%04u-%02u-%02u %02u:%02u:%02u.%03u] [crash] Exception 0x%08lX on thread %lu at ", time.wYear,
                      time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                      pRecord->ExceptionCode, GetCurrentThreadId()));
    add(Describe(reinterpret_cast<uintptr_t>(pRecord->ExceptionAddress), s_text + length, sizeof(s_text) - length));
    if (pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2)
    {
        const auto kind = pRecord->ExceptionInformation[0];
        add(std::snprintf(s_text + length, sizeof(s_text) - length, " (%s 0x%llx)",
                          kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                          static_cast<unsigned long long>(pRecord->ExceptionInformation[1])));
    }
    add(std::snprintf(s_text + length, sizeof(s_text) - length, "\r\n"));

    // The registers a bad call or read went through, with what they point at.
    if (pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        const auto& context = *apInfo->ContextRecord;
        const struct
        {
            const char* Name;
            DWORD64 Value;
        } registers[] = {{"rax", context.Rax}, {"rbx", context.Rbx}, {"rcx", context.Rcx}, {"rdx", context.Rdx},
                         {"rsi", context.Rsi}, {"rdi", context.Rdi}, {"r8", context.R8},   {"r9", context.R9}};
        for (const auto& reg : registers)
        {
            add(std::snprintf(s_text + length, sizeof(s_text) - length, "[crash] %s 0x%llx", reg.Name,
                              static_cast<unsigned long long>(reg.Value)));
            add(DescribeObject(static_cast<uintptr_t>(reg.Value), s_text + length, sizeof(s_text) - length));
            add(std::snprintf(s_text + length, sizeof(s_text) - length, "\r\n"));
        }
    }
    add(std::snprintf(s_text + length, sizeof(s_text) - length, "[crash] Call stack:\r\n"));

    static DWORD64 s_frames[48];
    static CONTEXT s_context;
    s_context = *apInfo->ContextRecord;
    const int count = Walk(s_context, s_frames, 48);
    for (int i = 0; i < count; ++i)
    {
        add(std::snprintf(s_text + length, sizeof(s_text) - length, "[crash]   %2d ", i));
        add(Describe(static_cast<uintptr_t>(s_frames[i]), s_text + length, sizeof(s_text) - length));
        add(std::snprintf(s_text + length, sizeof(s_text) - length, "\r\n"));
    }

    Append(s_text, length);
    // Not with the stack used up: symbols need more of it than is left.
    if (pRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        AppendSourceLines(s_frames, count);
    t_reporting = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

// The watchdog: the main loop beats every frame (Heartbeat); when it stops for this long, where the main thread is goes
// into the log, once per stop. A game that froze, or died without an exception, then still says where it was.
constexpr ULONGLONG kStallMs = 15'000;
std::atomic<ULONGLONG> s_lastBeat{0};
HANDLE s_mainThread = nullptr;

void ReportStall(ULONGLONG aStalledMs)
{
    static char s_text[4096];
    int length = 0;
    const auto add = [&](int aWritten)
    {
        if (aWritten > 0)
            length = (length + aWritten < static_cast<int>(sizeof(s_text))) ? length + aWritten
                                                                             : static_cast<int>(sizeof(s_text)) - 1;
    };

    SYSTEMTIME time{};
    GetLocalTime(&time);
    add(std::snprintf(s_text + length, sizeof(s_text) - length,
                      "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [watchdog] The game's main loop hasn't run for %llu s "
                      "(stuck, or a long load). Where it is:\r\n",
                      time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                      aStalledMs / 1000));

    // Only paused for its registers: walked after, so nothing here can wait on something the paused thread holds.
    static CONTEXT s_context;
    s_context = {};
    s_context.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(s_mainThread) == static_cast<DWORD>(-1))
        return;
    const bool read = GetThreadContext(s_mainThread, &s_context) != FALSE;
    ResumeThread(s_mainThread);
    if (!read)
        return;

    static DWORD64 s_frames[48];
    const int count = Walk(s_context, s_frames, 48);
    for (int i = 0; i < count; ++i)
    {
        add(std::snprintf(s_text + length, sizeof(s_text) - length, "[watchdog]   %2d ", i));
        add(Describe(static_cast<uintptr_t>(s_frames[i]), s_text + length, sizeof(s_text) - length));
        add(std::snprintf(s_text + length, sizeof(s_text) - length, "\r\n"));
    }
    Append(s_text, length);
}

DWORD WINAPI Watch(void*)
{
    ULONGLONG stalledSince = 0;
    for (;;)
    {
        Sleep(1000);
        const auto last = s_lastBeat.load();
        const auto now = GetTickCount64();
        if (now - last >= kStallMs)
        {
            if (!stalledSince)
            {
                stalledSince = last;
                ReportStall(now - last);
            }
        }
        else if (stalledSince)
        {
            char text[160];
            const int length = std::snprintf(text, sizeof(text), "[watchdog] Running again after %llu s.\r\n",
                                             (last - stalledSince) / 1000);
            if (length > 0)
                Append(text, length);
            stalledSince = 0;
        }
    }
}
} // namespace

void Support::CrashLog::Heartbeat()
{
    s_lastBeat = GetTickCount64();
    if (s_mainThread || !s_logPath[0])
        return;

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_mainThread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
    {
        s_mainThread = INVALID_HANDLE_VALUE; // not tried again
        return;
    }

    // Room for the crash report should the main thread run out of stack.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);

    // The thread keeps the mod loaded for as long as it runs (the game's whole life): never left running code that's
    // gone.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(&Watch)), &self);
    if (const HANDLE thread = CreateThread(nullptr, 64 * 1024, &Watch, nullptr, 0, nullptr))
        CloseHandle(thread);
}

void Support::CrashLog::Install(const std::filesystem::path& acLogPath)
{
    if (s_pHandler)
        return;

    const auto& path = acLogPath.native();
    if (path.size() >= std::size(s_logPath))
        return;
    std::copy(path.begin(), path.end(), s_logPath);
    s_logPath[path.size()] = L'\0';

    // Called first, before the game's own handlers, and passes every exception on.
    s_pHandler = AddVectoredExceptionHandler(1, &OnException);
}

void Support::CrashLog::Uninstall()
{
    if (s_pHandler)
    {
        RemoveVectoredExceptionHandler(s_pHandler);
        s_pHandler = nullptr;
    }
}
