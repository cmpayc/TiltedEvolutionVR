#include <TiltedOnlinePCH.h>

#include <Systems/RenderSystemD3D11.h>

#include <Services/DebugService.h>
#include <Services/OverlayService.h>
#include <Services/ImguiService.h>

#include <d3d11.h>

#if TP_SKYRIMVR
#include <Systems/VRRenderProbe.h>

namespace
{
// How long after the first frame the game window is kept in front. Long enough to outlast the console
// stealing focus while everything starts up, short enough that alt-tabbing away is never fought for long.
constexpr double kFocusWindowSeconds = 10.0;

/**
 * @brief Puts the game window in front of the launcher's console window.
 *
 * The launcher is a console application, so its log window is created before the game's and ends up in front
 * of it. On a monitor that is untidy; in a headset it means the keyboard and mouse are talking to a window the
 * player cannot see, and every launch begins by groping for the game window to click on.
 *
 * Focus is only ever taken from our own process, never from another application, so alt-tabbing to a browser
 * while the game loads is left alone. It keeps watching rather than acting once, because the console takes
 * focus back more than once during startup.
 */
void KeepGameWindowInFront(HWND aWindow) noexcept
{
    if (!aWindow)
        return;

    static const std::chrono::steady_clock::time_point s_first = std::chrono::steady_clock::now();

    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - s_first).count() > kFocusWindowSeconds)
        return;

    const HWND cForeground = GetForegroundWindow();

    if (cForeground == aWindow)
        return;

    DWORD owner = 0;

    if (cForeground)
        GetWindowThreadProcessId(cForeground, &owner);

    // Ours to take, or nobody's. Anything else belongs to the player.
    if (cForeground && owner != GetCurrentProcessId())
        return;

    BringWindowToTop(aWindow);
    SetForegroundWindow(aWindow);
}
} // namespace
#endif

RenderSystemD3D11::RenderSystemD3D11(OverlayService& aOverlay, ImguiService& aImguiService)
    : m_pSwapChain(nullptr)
    , m_pDevice(nullptr)
    , m_pDeviceContext(nullptr)
    , m_overlay(aOverlay)
    , m_imguiService(aImguiService)
{
    // Note: D3D11Hook is not utilized in the codebase

    // auto& d3d11 = TiltedPhoques::D3D11Hook::Get();
    // m_createConnection = d3d11.OnCreate.Connect(std::bind(&RenderSystemD3D11::OnDeviceCreation, this, std::placeholders::_2));
    // m_resetConnection = d3d11.OnLost.Connect(std::bind(&RenderSystemD3D11::OnReset, this, std::placeholders::_1));
    // m_renderConnection = d3d11.OnPresent.Connect(std::bind(&RenderSystemD3D11::OnRender, this, std::placeholders::_1));
}

HWND RenderSystemD3D11::GetWindow() const
{
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.OutputWindow = nullptr;

    if (m_pSwapChain)
        m_pSwapChain->GetDesc(&desc);

    return desc.OutputWindow;
}

void RenderSystemD3D11::OnDeviceCreation(IDXGISwapChain* apSwapChain, ID3D11Device* apDevice, ID3D11DeviceContext* apContext)
{
    m_pSwapChain = apSwapChain;
    m_pDevice = apDevice;
    m_pDeviceContext = apContext;

    m_imguiService.Create(this, GetWindow());
    m_overlay.Create(this);
    DebugService::ArrangeGameWindows(GetWindow());
}

void RenderSystemD3D11::OnRender()
{
#if TP_SKYRIMVR
    KeepGameWindowInFront(GetWindow());
    VRRenderProbe::OnFrame();
#endif

    m_imguiService.Render();
    m_overlay.Render();
}

void RenderSystemD3D11::OnReset(IDXGISwapChain* apSwapChain)
{
    m_pSwapChain = apSwapChain;

    m_overlay.Reset();
    m_imguiService.Reset();
}
