#include <BranchInfo.h>
#include "CrashHandler.h"
#include <DbgHelp.h>
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <exception>
#include <intrin.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <strsafe.h>

#ifndef STATUS_FATAL_APP_EXIT
#define STATUS_FATAL_APP_EXIT 0x40000015L
#endif

using time_point = std::chrono::system_clock::time_point;

std::string SerializeTimePoint(const time_point& time, const std::string& format)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::gmtime(&tt); // GMT (UTC)
    // std::tm tm = *std::localtime(&tt); //Locale time-zone, usually UTC by default.
    std::stringstream ss;
    ss << std::put_time(&tm, format.c_str());
    return ss.str();
}

static void WriteMiniDump(PEXCEPTION_POINTERS pExceptionInfo)
{
#if (IS_MASTER)
    volatile static bool bMiniDump = false;
#else
    volatile static bool bMiniDump = true;
#endif
    if (!bMiniDump)
        return;

    HANDLE hDumpFile = NULL;
    try
    {
        MINIDUMP_EXCEPTION_INFORMATION M;
        char dumpPath[MAX_PATH];

        M.ThreadId = GetCurrentThreadId();
        M.ExceptionPointers = pExceptionInfo;
        M.ClientPointers = 0;

        std::ostringstream oss;
        oss << "crash_" << SerializeTimePoint(std::chrono::system_clock::now(), "UTC_%Y-%m-%d_%H-%M-%S")
            << ".dmp";

        GetModuleFileNameA(NULL, dumpPath, sizeof(dumpPath));
        std::filesystem::path modulePath(dumpPath);
        auto subPath = modulePath.parent_path();

        CrashHandler::RemovePreviousDump(subPath);

        subPath /= oss.str();

        hDumpFile = CreateFileA(subPath.string().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);

        // baseline settings from https://stackoverflow.com/a/63123214/5273909
        //
        // MiniDumpWithIndirectlyReferencedMemory captures a small window around every pointer-like
        // value on the stack. Without it a dump holds the module data segments and the stacks but no
        // heap, so a crash that hands the game a bad object can be traced to the call but not to the
        // object: Tools/vr_addresses/dumpmem.mjs reads a game object's vtable and form id straight
        // out of a dump, and that only works if the object was captured. It costs a few MB, against
        // the ~1 GB MiniDumpWithDataSegs already writes.
        auto dumpSettings = MiniDumpWithDataSegs | MiniDumpWithProcessThreadData | MiniDumpWithHandleData |
                            MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                            /*
                            //MiniDumpWithPrivateReadWriteMemory | // this one gens bad dump
                            MiniDumpWithUnloadedModules |
                            MiniDumpWithFullMemoryInfo |
                            MiniDumpWithTokenInformation |
                            MiniDumpWithPrivateWriteCopyMemory |
                            */
                            0;

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, (MINIDUMP_TYPE)dumpSettings,
                          (pExceptionInfo) ? &M : NULL, NULL, NULL);
    }
    catch (...) // Mini-dump is best effort only.
    {
    }

    if (!hDumpFile)
        spdlog::critical(__FUNCTION__ ": coredump may have failed.");
    else
    {
        CloseHandle(hDumpFile);
        spdlog::critical(__FUNCTION__ ": coredump created -> flush logs.");
    }
}

// An unhandled C++ exception, std::bad_alloc being the one that actually shows up, ends in
// std::terminate. The vectored handler below never sees it, because it only answers to access
// violations, so the process aborts with "Fatal program exit requested", no log line and no
// coredump. That is the one failure shape there is no way to diagnose. Log what was thrown and
// write a dump with a real exception stream, so the usual tooling can read the stack out of it.
static void TerminateHandler()
{
    static int alreadyTerminating = 0;

    const char* pWhat = "not a std::exception";
    try
    {
        if (auto current = std::current_exception())
            std::rethrow_exception(current);
    }
    catch (const std::exception& e)
    {
        pWhat = e.what();
    }
    catch (...)
    {
    }

    if (alreadyTerminating++ == 0)
    {
        spdlog::critical(__FUNCTION__ ": unhandled exception, terminating: {}", pWhat);

        CONTEXT context{};
        RtlCaptureContext(&context);

        EXCEPTION_RECORD record{};
        record.ExceptionCode = STATUS_FATAL_APP_EXIT;
        record.ExceptionAddress = _ReturnAddress();

        EXCEPTION_POINTERS pointers{&record, &context};
        WriteMiniDump(&pointers);

        spdlog::shutdown();
    }

    abort();
}

LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
    static int alreadyCrashed = 0;
    auto retval = EXCEPTION_CONTINUE_SEARCH;

    // Serialize
    static std::mutex singleThreaded;
    const std::lock_guard lock{singleThreaded};

    // Check for severe, not continuable and not software-originated exception.
    //
    // STATUS_FATAL_APP_EXIT is what abort() raises, which is where an unhandled C++ exception ends
    // up. The terminate handler above cannot cover that on its own, because the Microsoft CRT's
    // set_terminate installs per thread: a handler set on the main thread never runs for a worker,
    // and the game runs plenty of workers. A vectored handler is process wide, so answering to the
    // abort here catches those wherever they happen.
    const auto cExceptionCode = pExceptionInfo->ExceptionRecord->ExceptionCode;
    if ((cExceptionCode == EXCEPTION_ACCESS_VIOLATION || cExceptionCode == STATUS_FATAL_APP_EXIT) &&
        alreadyCrashed++ == 0)
    {
        spdlog::critical (__FUNCTION__ ": crash occurred!");

        spdlog::error(__FUNCTION__ ": exception code is {:x}, at address {}, flags {:x} ",
                      pExceptionInfo->ExceptionRecord->ExceptionCode,
                      pExceptionInfo->ExceptionRecord->ExceptionAddress,
                      pExceptionInfo->ExceptionRecord->ExceptionFlags);

        WriteMiniDump(pExceptionInfo);

        // Something in STR breaks top-level unhandled exception filters.
        // The Win API for them is pretty clunky (non-atomic, not chainable), 
        // but they can do some important things. If someone actually set one
        // they probably meant it; make sure it actually runs.
        // This will make more CrashLogger mods work with STR.

        // Get the current unhandled exception filter. If it has changed
        // from when STR started up, invoke it here.
        LPTOP_LEVEL_EXCEPTION_FILTER pCurrentUnhandledExceptionFilter = SetUnhandledExceptionFilter(CrashHandler::GetOriginalUnhandledExceptionFilter());
        SetUnhandledExceptionFilter(pCurrentUnhandledExceptionFilter);
        if (pCurrentUnhandledExceptionFilter != CrashHandler::GetOriginalUnhandledExceptionFilter())
        {
            spdlog::critical(__FUNCTION__ ": UnhandledExceptionFilter() workaround triggered.");

            singleThreaded.unlock();        // Might reenter, but is safe at this point.
            if ((*pCurrentUnhandledExceptionFilter)(pExceptionInfo) == EXCEPTION_CONTINUE_EXECUTION)
                retval = EXCEPTION_CONTINUE_EXECUTION;
            singleThreaded.lock();
        }

        spdlog::shutdown();
    }
    return retval;
}

LPTOP_LEVEL_EXCEPTION_FILTER CrashHandler::m_pUnhandled;
CrashHandler::CrashHandler()
{
    // Record the original (or as close as we can get) top-level unhandled exception handler.
    // We grab this so we can see if it is changed, presumably by a mod or even graphics drivers.
    // Something in STR breaks unhandled exception handling, so we'll fake it if necessary.
    // This is the only way to get the current setting, but the race is small.
    m_pUnhandled = SetUnhandledExceptionFilter(NULL);
    SetUnhandledExceptionFilter(m_pUnhandled);

    m_handler = AddVectoredExceptionHandler(1, &VectoredExceptionHandler);

    std::set_terminate(&TerminateHandler);
}

CrashHandler::~CrashHandler()
{
}

void CrashHandler::RemovePreviousDump(std::filesystem::path path)
{
    for (auto& entry : std::filesystem::directory_iterator(path))
    {
        if (entry.path().string().find("crash") != std::string::npos)
        {
            DeleteFileA(entry.path().string().c_str());
        }
    }
}
