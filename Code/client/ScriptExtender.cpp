
#include <ScriptExtender.h>
#include <TiltedOnlinePCH.h>
#include <VersionDb.h>

namespace
{
#if TP_SKYRIMVR
// SKSEVR ships as sksevr_1_4_15.dll and exports the same entry point. Same length as skse64, so
// the name matching below is unaffected.
constexpr wchar_t kScriptExtenderName[] = L"sksevr";
#else
constexpr wchar_t kScriptExtenderName[] = L"skse64";
#endif

constexpr char kScriptExtenderEntrypoint[] = "StartSKSE";

constexpr size_t kScriptExtenderNameLength = sizeof(kScriptExtenderName) / sizeof(wchar_t) - 1;

// AE+ only
// Use this to raise the SKSE baseline
#if TP_SKYRIMVR
// SKSEVR encodes 2.0.12 in its version resource as 0.2.0.12, so it scores 20012 below and the
// AE baseline rejects it. 2.0.12 is the current and only SKSE for SkyrimVR 1.4.15, and VR never
// had a pre-anniversary split, so it is the baseline here.
constexpr int kSKSEMinBuild = 20012;
#else
constexpr int kSKSEMinBuild = 20100;
#endif

HMODULE g_SKSEModuleHandle{nullptr};

struct FileVersion
{
    static constexpr uint8_t scVersionSize = 4;
    DWORD versions[scVersionSize];
};

int GetFileVersion(const std::filesystem::path& acFilePath, FileVersion& aVersion)
{
    const auto filename = acFilePath.c_str();

    DWORD dwHandle = 0, sz = GetFileVersionInfoSizeW(filename, &dwHandle);
    if (0 == sz)
    {
        return 1;
    }
    std::string buf(sz, '\0');
    if (!GetFileVersionInfoW(filename, dwHandle, sz, &buf[0]))
    {
        return 2;
    }
    VS_FIXEDFILEINFO* pvi;
    sz = sizeof(VS_FIXEDFILEINFO);
    if (!VerQueryValueA(&buf[0], "\\", reinterpret_cast<LPVOID*>(&pvi), reinterpret_cast<unsigned int*>(&sz)))
    {
        return 3;
    }

    aVersion.versions[0] = pvi->dwProductVersionMS >> 16;
    aVersion.versions[1] = pvi->dwFileVersionMS & 0xFFFF;
    aVersion.versions[2] = pvi->dwFileVersionLS >> 16;
    aVersion.versions[3] = pvi->dwFileVersionLS & 0xFFFF;

    return 0;
}

std::string GetSKSEStyleExeVersion()
{
    // make sure newer than anniversary!
    auto exeBuild = VersionDb::Get().GetLoadedVersionString();
    std::replace(exeBuild.begin(), exeBuild.end(), '.', '_');

    // chop off empty patch numbers for instance "1.6.323.0 becomes "1_6_323"
    auto patchPos = exeBuild.find_last_of("_0");
    if (patchPos != std::string::npos)
    {
        exeBuild.erase(exeBuild.begin() + (patchPos - 1), exeBuild.end());
    }

    return exeBuild;
}
} // namespace

bool IsScriptExtenderLoaded()
{
    return g_SKSEModuleHandle;
}

void LoadScriptExender()
{
    const auto exeVerson{GetSKSEStyleExeVersion()};

    // Get the path of the game, where the Script Extender dll resides
    const auto gameDir = std::filesystem::current_path();

    std::list<std::filesystem::path> dllMatches;
    for (const auto& dirEntry : std::filesystem::directory_iterator(gameDir))
    {
        const auto& path = dirEntry.path();
        if (path.extension() != L".dll")
            continue;

        auto fileName = path.filename().wstring();
        if (fileName.length() < kScriptExtenderNameLength)
            continue;

        if (fileName.substr(0, kScriptExtenderNameLength) == kScriptExtenderName)
        {
            dllMatches.push_back(path);
        }
    }

    // and before you ask, no, they dont expose it via file version info
    std::filesystem::path* needle = nullptr;
    for (auto& match : dllMatches)
    {
        auto fname = match.filename().string();
        auto ptr = &fname[kScriptExtenderNameLength + 1];
        // make extra sure!
        if (std::strncmp(ptr, exeVerson.c_str(), exeVerson.length()) == 0)
        {
            needle = &match;
            break;
        }
    }

    if (!needle)
        return;

    FileVersion fileVersion;
    if (GetFileVersion(*needle, fileVersion) != 0)
    {
        spdlog::error("Unable to verify Script Extender version");
        return;
    }

    auto skseVersion = fmt::format("v{}.{}.{}.{}", fileVersion.versions[0], fileVersion.versions[1], fileVersion.versions[2], fileVersion.versions[3]);

    // nice try.
    int SkseVCum = fileVersion.versions[0] * 1000000 + fileVersion.versions[1] * 10000 + fileVersion.versions[2] * 100 + fileVersion.versions[3];
    if (SkseVCum < kSKSEMinBuild)
    {
        spdlog::error("Pre anniversary Script Extender is unsupported");
        return;
    }

    if (g_SKSEModuleHandle = LoadLibraryW(needle->c_str()))
    {
#if TP_SKYRIMVR
        // SKSEVR 2.0.12 has an empty export directory, StartSKSE included. Like the whole SKSE
        // 2.0.x line it is injected with LoadLibrary and does its work from DllMain, so the load
        // above is the entire handshake. StartSKSE only exists in the 2.1+ builds AE needs.
        spdlog::info(
            "SKSEVR {} is active... be aware that messages that start without a colored [timestamp] prefix are "
            "logs from the "
            "Script Extender and its loaded mods.",
            skseVersion);
#else
        if (auto* pStartSKSE = reinterpret_cast<void (*)()>(GetProcAddress(g_SKSEModuleHandle, kScriptExtenderEntrypoint)))
        {
            spdlog::info(
                "Starting SKSE {}... be aware that messages that start without a colored [timestamp] prefix are "
                "logs from the "
                "Script Extender and its loaded mods.",
                skseVersion);
            pStartSKSE();
            spdlog::info("SKSE is active");
        }
        else
            spdlog::warn("SKSE dll doesn't expose StartSKSE(), it may be outdated.");
#endif
    }
    else
    {
        spdlog::error("Failed to load {}! Check your privileges or re-download the Script Extender files.", needle->string());
    }
}
