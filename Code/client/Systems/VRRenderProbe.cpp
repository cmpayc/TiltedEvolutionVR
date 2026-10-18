#include <TiltedOnlinePCH.h>

#include <Systems/VRRenderProbe.h>

#if TP_SKYRIMVR

#include <BSGraphics/BSGraphicsRenderer.h>
#include <ModCompat/openvr.h>
#include <PlayerCharacter.h>

#include <d3d11.h>

namespace
{
// Frames to let pass before the first census. The renderer is up well before the main menu is drawn, and an
// empty swapchain says nothing worth reading.
constexpr uint32_t kStartupFrame = 300;

// And how long to wait once the player has 3D. A save finishes loading before the world is being drawn
// steadily, and the bound target during a fade is not the one a menu would live in.
constexpr uint32_t kInGameFrames = 120;

// D3D11 allows eight simultaneous render targets. The game uses far fewer, but reading them all costs nothing.
constexpr uint32_t kBoundSlots = 8;

const char* FormatName(const DXGI_FORMAT aFormat) noexcept
{
    switch (aFormat)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    default: return "?";
    }
}

// Which entry of the renderer's table this texture is, or -1 for a texture that is not one of them.
int FindTarget(const BSGraphics::RendererData& acData, const void* acpTexture) noexcept
{
    for (int i = 0; i < static_cast<int>(std::size(acData.pRenderTargetsA)); ++i)
    {
        if (static_cast<const void*>(acData.pRenderTargetsA[i].pTexture) == acpTexture)
            return i;
    }

    return -1;
}

/**
 * @brief What the game has bound at the frame end hook, which is where a fallback overlay would draw.
 *
 * The desktop render handler never binds a target of its own, so this is the whole reason the menu lands where
 * it does. Naming the texture against the renderer's table says whether that is the mirror window or an eye.
 */
void LogBound(ID3D11DeviceContext* apContext, const BSGraphics::RendererData& acData) noexcept
{
    ID3D11RenderTargetView* views[kBoundSlots]{};
    ID3D11DepthStencilView* pDepth = nullptr;

    apContext->OMGetRenderTargets(kBoundSlots, views, &pDepth);

    for (uint32_t slot = 0; slot < kBoundSlots; ++slot)
    {
        if (!views[slot])
            continue;

        ID3D11Resource* pResource = nullptr;
        views[slot]->GetResource(&pResource);

        ID3D11Texture2D* pTexture = nullptr;

        if (pResource)
            pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pTexture));

        if (pTexture)
        {
            D3D11_TEXTURE2D_DESC desc{};
            pTexture->GetDesc(&desc);

            spdlog::info("VR probe:   bound slot {} is {}x{} {} samples={}, render target {}, texture {}", slot, desc.Width, desc.Height,
                         FormatName(desc.Format), desc.SampleDesc.Count, FindTarget(acData, pTexture), static_cast<const void*>(pTexture));

            pTexture->Release();
        }

        if (pResource)
            pResource->Release();

        views[slot]->Release();
    }

    if (pDepth)
        pDepth->Release();

    uint32_t count = kBoundSlots;
    D3D11_VIEWPORT viewports[kBoundSlots]{};

    apContext->RSGetViewports(&count, viewports);

    for (uint32_t i = 0; i < count && i < kBoundSlots; ++i)
    {
        spdlog::info("VR probe:   viewport {} at {},{} size {}x{}", i, viewports[i].TopLeftX, viewports[i].TopLeftY, viewports[i].Width,
                     viewports[i].Height);
    }
}

void LogCensus(const char* acpWhen) noexcept
{
    const BSGraphics::RendererData* pData = BSGraphics::GetRendererData();

    if (!pData)
    {
        spdlog::error("VR probe: the renderer never published its data, so there is nothing to census");
        return;
    }

    spdlog::info("VR probe: render target census, {}", acpWhen);

    const BSGraphics::RendererWindow& cWindow = pData->RenderWindowA[0];

    spdlog::info("VR probe:   window 0 is {}x{}, swapchain {}, its target texture {}", cWindow.uiWindowWidth, cWindow.uiWindowHeight,
                 static_cast<const void*>(cWindow.pSwapChain), static_cast<const void*>(cWindow.SwapChainRenderTarget.pTexture));

    if (pData->pContext)
        LogBound(pData->pContext, *pData);

    for (size_t i = 0; i < std::size(pData->pRenderTargetsA); ++i)
    {
        const BSGraphics::RenderTarget& cTarget = pData->pRenderTargetsA[i];

        if (!cTarget.pTexture)
            continue;

        D3D11_TEXTURE2D_DESC desc{};
        cTarget.pTexture->GetDesc(&desc);

        spdlog::info("VR probe:   rt {:3} {:5}x{:<5} {} samples={} array={} bind=0x{:X} rtv={} srv={} uav={} copy={} texture {}", i, desc.Width,
                     desc.Height, FormatName(desc.Format), desc.SampleDesc.Count, desc.ArraySize, desc.BindFlags, cTarget.pRTView != nullptr,
                     cTarget.pSRView != nullptr, cTarget.pUAView != nullptr, cTarget.pCopyTexture != nullptr,
                     static_cast<const void*>(cTarget.pTexture));
    }
}
} // namespace

void VRRenderProbe::OnFrame() noexcept
{
    static uint32_t s_frame = 0;
    static uint32_t s_inGameFrame = 0;
    static bool s_startupDone = false;
    static bool s_inGameDone = false;

    ++s_frame;

    if (!s_startupDone && s_frame >= kStartupFrame)
    {
        LogCensus("at startup");
        s_startupDone = true;
    }

    if (s_inGameDone)
        return;

    auto* pPlayer = PlayerCharacter::Get();

    if (!pPlayer || !pPlayer->GetNiNode())
    {
        s_inGameFrame = 0;
        return;
    }

    if (++s_inGameFrame < kInGameFrames)
        return;

    LogCensus("with the player in the world");
    s_inGameDone = true;
}

#endif
