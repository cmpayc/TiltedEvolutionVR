
#include <TiltedOnlineApp.h>
#include <TiltedOnlinePCH.h>
#include <ScriptExtender.h>
#include <VRAddressMap.h>

#include <Commctrl.h>
#include <Windows.h>

#include <base/dialogues/win/TaskDialog.h>

std::unique_ptr<TiltedOnlineApp> g_appInstance{nullptr};

extern HICON g_SharedWindowIcon;

static void ShowAddressLibraryError(const wchar_t* apGamePath)
{
#if TP_SKYRIMVR
    auto errorDetail = fmt::format(L"Game path: {}", apGamePath);
#else
    auto errorDetail = fmt::format(L"Looking for it here: {}\\Data\\SKSE\\Plugins", apGamePath);
#endif

#if TP_SKYRIMVR
    // VR carries its address table in the binary, so there is nothing for the user to install.
    // Reaching this means the game version string could not be read.
    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Failed to read the Skyrim VR version", L"Skyrim Together VR expects SkyrimVR.exe 1.4.15.0", errorDetail.c_str());
    dia.Show();
#else
    Base::TaskDialog dia(g_SharedWindowIcon, L"Error", L"Failed to load Skyrim Address Library", L"Make sure to use \"All in one\"", errorDetail.c_str());

    dia.AppendButton(0xBEED, L"Visit troubleshooting page on wiki.tiltedphoques.com");
    dia.AppendButton(0xBEEF, L"Visit Address Library modpage on nexusmods.com");
    const int result = dia.Show();
    if (result == 0xBEEF)
    {
        ShellExecuteW(nullptr, L"open", LR"(https://www.nexusmods.com/skyrimspecialedition/mods/32444?tab=files)", nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (result == 0xBEED)
    {
        ShellExecuteW(nullptr, L"open", LR"(https://wiki.tiltedphoques.com/tilted-online/guides/troubleshooting/address-library-error)", nullptr, nullptr, SW_SHOWNORMAL);
    }
#endif

    exit(4);
}

void RunTiltedInit(const std::filesystem::path& acGamePath, const String& aExeVersion)
{
    if (!VersionDb::Get().Load(acGamePath, aExeVersion))
    {
        ShowAddressLibraryError(acGamePath.c_str());
    }

    g_appInstance = std::make_unique<TiltedOnlineApp>();

#if TP_SKYRIMVR
    // Constructing the app is what creates the tp_client.log sink, so the address table summary
    // has to be written from here to end up in the log file rather than only on the console.
    VRAddresses::LogSummary();
#endif

    TiltedOnlineApp::InstallHooks2();
    TP_HOOK_COMMIT;

    LoadScriptExtender();
}

void RunTiltedApp()
{
    g_appInstance->BeginMain();
}
