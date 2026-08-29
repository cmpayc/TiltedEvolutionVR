#include <TiltedOnlinePCH.h>

#include <TiltedOnlineApp.h>

#include <DInputHook.hpp>
#include <dinput.h>
#include <WindowsHook.hpp>

#include <World.h>
#include <Games/TES.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Systems/RenderSystemD3D11.h>

#include <Services/OverlayService.h>
#include <Services/ImguiService.h>
#include <Services/DiscordService.h>

#include <ScriptExtender.h>
#include <NvidiaUtil.h>

using TiltedPhoques::Debug;

TiltedOnlineApp::TiltedOnlineApp()
{
    // Set console code page to UTF-8 so console known how to interpret string data
    SetConsoleOutputCP(CP_UTF8);

    auto logPath = TiltedPhoques::GetPath() / "logs";

    std::error_code ec;
    create_directory(logPath, ec);

    auto rotatingLogger = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(logPath / "tp_client.log", 1048576 * 5, 3);
    // rotatingLogger->set_level(spdlog::level::debug);
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("", spdlog::sinks_init_list{console, rotatingLogger});
    logger->set_pattern("%^[%Y-%m-%d %H:%M:%S.%e] [%l] [tid %t] %$ %v");
    spdlog::flush_every(std::chrono::seconds(1));
    set_default_logger(logger);
}

TiltedOnlineApp::~TiltedOnlineApp() = default;

void* TiltedOnlineApp::GetMainAddress() const
{
    POINTER_SKYRIMSE(void, winMain, 36544);

    return winMain.GetPtr();
}

bool TiltedOnlineApp::BeginMain()
{
    World::Create();
    World::Get().ctx().at<DiscordService>().Init();
    World::Get().ctx().emplace<RenderSystemD3D11>(World::Get().ctx().at<OverlayService>(), World::Get().ctx().at<ImguiService>());

#if TP_SKYRIMVR
    // SKSEVR 2.0.12 runs its full init inside StartSKSE rather than deferring it the way
    // SKSE64 does. Called from RunTiltedInit it would scan plugins before the game entry
    // point, ahead of the EngineFixes _initterm_e preload hook, and EngineFixes then aborts
    // startup with "plugin did not preload". BeginMain runs from inside game startup, so
    // the preloader has already had its turn.
    LoadScriptExtender();
#endif

    // TODO: Figure out a way to un-blacklist NvCamera64.dll (see DllBlocklist.cpp). Then this hack can be removed
    if (IsNvidiaOverlayLoaded())
        ApplyNvidiaFix();

    return true;
}

bool TiltedOnlineApp::EndMain()
{
    UninstallHooks();
    if (m_pDevice)
        m_pDevice->Release();

    return true;
}

namespace
{
/**
 * @brief Prints the load order once, as soon as the game has one.
 *
 * `standardId` is not a label, it is the byte that prefixes every form id from that plugin, so two players
 * whose plugins sit at different indices disagree about what any DLC form id means. SkyrimVR's exe orders the
 * official masters differently from SkyrimSE's, which broke ten hardcoded ids once already, silently
 * (PROGRESS.md session 6), and until now comparing two machines meant reading the plugin array out of a save
 * from each.
 *
 * Waits for the mod array rather than printing from startup, because it is empty until the game has loaded
 * its data files, and the answer is worthless before then.
 */
void LogLoadOrderOnce() noexcept
{
    static bool logged = false;

    if (logged)
        return;

    auto* const cpModManager = ModManager::Get();

    if (!cpModManager)
        return;

    size_t count = 0;

    for (auto* pMod : cpModManager->mods)
    {
        if (!pMod->IsLoaded())
            continue;

        if (!count++)
            spdlog::info("Load order, as this game reports it. The index is the top byte of every form id from that plugin:");

        spdlog::info("  {:02X}  {}{}", pMod->GetId(), pMod->filename, pMod->IsLite() ? "  (light)" : "");
    }

    if (count)
        logged = true;
}
} // namespace

void TiltedOnlineApp::Update()
{
    LogLoadOrderOnce();

    // Reverting a change that used to be here to disable bUseFaceGenPreprocessedHeads==true (which is 
    // the default) handling. Extensive testing over months by multiple parties showed that enabling 
    // the flag introduces no issues WITH PROPERLY GENERATED CHARACTERS (in-game character generation 
    // or showracemenu). The shortcut of  "coc riverwood" from the main menu skips proper character generation.
    // 
    // Plus, having it on  has some benefits like helping with neck seams. Comment to avoid revisiting.
    // 
    // There are still some issues to track down, like hair color and maybe face tint not syncing correctly,
    // but they are unrelated and unchanged by this flag.
    // 
 
    // Make sure the window stays active
    POINTER_SKYRIMSE(uint32_t, bAlwaysActive, 380768);

    *bAlwaysActive = 1;

    World::Get().Update();
}

bool TiltedOnlineApp::Attach()
{
    TiltedPhoques::Debug::OnAttach();

    // TiltedPhoques::Nop(0x1405D3FA1, 6);
    return true;
}

bool TiltedOnlineApp::Detach()
{
    TiltedPhoques::Debug::OnDetach();
    return true;
}

void TiltedOnlineApp::InstallHooks2()
{
    TiltedPhoques::Initializer::RunAll();

    TiltedPhoques::DInputHook::Install();
    TiltedPhoques::DInputHook::Get().SetToggleKeys({DIK_F2, DIK_RCONTROL});
}

void TiltedOnlineApp::UninstallHooks()
{
}

void TiltedOnlineApp::ApplyNvidiaFix() noexcept
{
    auto d3dFeatureLevelOut = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = CreateEarlyDxDevice(&m_pDevice, &d3dFeatureLevelOut);
    if (FAILED(hr))
        spdlog::error("D3D11CreateDevice failed. Detected an NVIDIA GPU, error code={0:x}", hr);

    if (d3dFeatureLevelOut < D3D_FEATURE_LEVEL_11_0)
        spdlog::warn("Unexpected D3D11 feature level detected (< 11.0), may cause issues");
}
