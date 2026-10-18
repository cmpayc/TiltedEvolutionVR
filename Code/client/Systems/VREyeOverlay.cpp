#include <TiltedOnlinePCH.h>

#include <Systems/VREyeOverlay.h>

#if TP_SKYRIMVR

#include <Systems/VRMenuOverlay.h>

#include <BSGraphics/BSGraphicsRenderer.h>
#include <ModCompat/openvr.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl.h>

namespace
{
/**
 * @brief Where Submit sits in IVRCompositor's vtable.
 *
 * Sixth, after SetTrackingSpace, GetTrackingSpace, WaitGetPoses, GetLastPoses and
 * GetLastPoseForTrackedDeviceIndex. That prefix has not moved since IVRCompositor_015 and the bundled
 * header's _027 still agrees, so one number covers every runtime this can meet. The game asks for _022.
 */
constexpr size_t kSubmitSlot = 5;

/**
 * @brief How much of an eye's width the menu spans when there is no frustum to place it with.
 *
 * Centring is what a symmetric frustum wants, so this is right on a headset whose eyes look straight ahead and
 * merely off centre on one that does not. It only comes up if the projection cannot be read at all.
 */
constexpr float kFallbackWidthFraction = 0.9f;

// How big the pointer's dot is, as a fraction of the menu's width. About a centimetre and a half at the size
// the menu defaults to, which is small enough to point precisely with and large enough to find.
constexpr float kPointerFraction = 0.0075f;

// A quad as a triangle strip, positioned entirely by the viewport. No vertex buffer and no input layout: the
// four corners come from the vertex id, so the only thing deciding where it lands is where we say to draw.
constexpr char kShaderSource[] = R"(
struct VOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VOut vs_main(uint aId : SV_VertexID)
{
    const float2 cUv = float2(aId & 1, aId >> 1);

    VOut result;
    result.uv = cUv;
    result.pos = float4(cUv.x * 2.0 - 1.0, 1.0 - cUv.y * 2.0, 0.0, 1.0);

    return result;
}

Texture2D gPage : register(t0);
SamplerState gSampler : register(s0);

float4 ps_main(VOut aIn) : SV_Target
{
    return gPage.Sample(gSampler, aIn.uv);
}

// A round dot standing in for the laser the compositor would have drawn, softened at the rim so that a marker
// only a few pixels across does not look like a staircase.
float4 ps_pointer(VOut aIn) : SV_Target
{
    const float cEdge = length(aIn.uv - 0.5) * 2.0;

    return float4(1.0, 1.0, 1.0, saturate((1.0 - cEdge) * 6.0) * 0.85);
}
)";

using TD3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                     ID3DBlob**);

using TSubmit = vr::EVRCompositorError(__fastcall*)(void*, vr::EVREye, const vr::Texture_t*, const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

TSubmit s_pRealSubmit = nullptr;

/**
 * @brief The system interface the game itself was handed, not one we asked for.
 *
 * This is the difference between working everywhere and working on SteamVR. Asking for a version of our own
 * choosing depends on the runtime serving it; borrowing the game's depends only on the game running at all.
 * Only the first five slots are ever called, and that part of the layout has not moved in years.
 */
vr::IVRSystem* s_pVRSystem = nullptr;

Microsoft::WRL::ComPtr<ID3D11DeviceContext> s_pDeferred;
Microsoft::WRL::ComPtr<ID3D11VertexShader> s_pVertexShader;
Microsoft::WRL::ComPtr<ID3D11PixelShader> s_pPixelShader;
Microsoft::WRL::ComPtr<ID3D11PixelShader> s_pPointerShader;
Microsoft::WRL::ComPtr<ID3D11BlendState> s_pBlend;
Microsoft::WRL::ComPtr<ID3D11SamplerState> s_pSampler;
Microsoft::WRL::ComPtr<ID3D11RasterizerState> s_pRasterizer;
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> s_pDepthStencil;

// The view over whatever the game last submitted, with the size it was made against. The pointer alone is not
// enough to trust: a texture freed and replaced at the same address would leave a view of the wrong size, and
// the size is what every rectangle below is measured from.
Microsoft::WRL::ComPtr<ID3D11RenderTargetView> s_pEyeView;
const void* s_pEyeTexture = nullptr;
uint32_t s_eyeWidth = 0;
uint32_t s_eyeHeight = 0;

bool s_broken = false;

bool CompileShaders(ID3D11Device* apDevice) noexcept
{
    // Resolved rather than linked, so that a missing compiler costs the menu rather than the launch. Only
    // half a saving in truth: the imgui backend links d3dcompiler with a pragma of its own, so the exe imports
    // it either way. Kept because this file then depends on nothing but what it asks for itself.
    const HMODULE cCompiler = LoadLibraryA("d3dcompiler_47.dll");

    if (!cCompiler)
    {
        spdlog::error("VR eyes: no d3dcompiler_47.dll, so the menu cannot be drawn into the frame");
        return false;
    }

    const auto pCompile = reinterpret_cast<TD3DCompile>(GetProcAddress(cCompiler, "D3DCompile"));

    if (!pCompile)
    {
        spdlog::error("VR eyes: d3dcompiler_47.dll has no D3DCompile");
        return false;
    }

    auto build = [&](const char* acpEntry, const char* acpProfile, Microsoft::WRL::ComPtr<ID3DBlob>& aBlob)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> pErrors;
        const HRESULT cResult = pCompile(kShaderSource, sizeof(kShaderSource) - 1, "VREyeOverlay", nullptr, nullptr, acpEntry, acpProfile,
                                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, aBlob.ReleaseAndGetAddressOf(), pErrors.ReleaseAndGetAddressOf());

        if (FAILED(cResult))
        {
            spdlog::error("VR eyes: {} would not compile (0x{:08X}): {}", acpEntry, static_cast<uint32_t>(cResult),
                          pErrors ? static_cast<const char*>(pErrors->GetBufferPointer()) : "no message");
            return false;
        }

        return true;
    };

    Microsoft::WRL::ComPtr<ID3DBlob> pVertex;
    Microsoft::WRL::ComPtr<ID3DBlob> pPixel;
    Microsoft::WRL::ComPtr<ID3DBlob> pPointer;

    if (!build("vs_main", "vs_5_0", pVertex) || !build("ps_main", "ps_5_0", pPixel) || !build("ps_pointer", "ps_5_0", pPointer))
        return false;

    if (FAILED(apDevice->CreateVertexShader(pVertex->GetBufferPointer(), pVertex->GetBufferSize(), nullptr, s_pVertexShader.ReleaseAndGetAddressOf())) ||
        FAILED(apDevice->CreatePixelShader(pPixel->GetBufferPointer(), pPixel->GetBufferSize(), nullptr, s_pPixelShader.ReleaseAndGetAddressOf())) ||
        FAILED(apDevice->CreatePixelShader(pPointer->GetBufferPointer(), pPointer->GetBufferSize(), nullptr, s_pPointerShader.ReleaseAndGetAddressOf())))
    {
        spdlog::error("VR eyes: the compiled shaders would not load");
        return false;
    }

    return true;
}

bool EnsureResources(ID3D11Device* apDevice) noexcept
{
    if (s_pDeferred)
        return true;

    if (s_broken)
        return false;

    // Set before anything below can fail, so a failure is reported once rather than every frame forever.
    s_broken = true;

    if (!CompileShaders(apDevice))
        return false;

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = TRUE;

    // Nothing here should be occluded and no depth view is bound, so depth is off rather than merely ignored.
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.StencilEnable = FALSE;

    if (FAILED(apDevice->CreateBlendState(&blend, s_pBlend.ReleaseAndGetAddressOf())) ||
        FAILED(apDevice->CreateSamplerState(&sampler, s_pSampler.ReleaseAndGetAddressOf())) ||
        FAILED(apDevice->CreateRasterizerState(&rasterizer, s_pRasterizer.ReleaseAndGetAddressOf())) ||
        FAILED(apDevice->CreateDepthStencilState(&depth, s_pDepthStencil.ReleaseAndGetAddressOf())))
    {
        spdlog::error("VR eyes: the pipeline states would not create");
        return false;
    }

    // Recorded on a deferred context and executed with the immediate context's state put back afterwards. The
    // alternative is saving and restoring a dozen pieces of the game's state by hand, where getting one of
    // them wrong shows up as corruption somewhere else entirely.
    if (FAILED(apDevice->CreateDeferredContext(0, s_pDeferred.ReleaseAndGetAddressOf())))
    {
        spdlog::error("VR eyes: no deferred context on the game's device");
        return false;
    }

    s_broken = false;
    spdlog::info("VR eyes: ready to draw the menu into the submitted frame");

    return true;
}

bool EnsureEyeView(ID3D11Device* apDevice, ID3D11Texture2D* apTexture, const D3D11_TEXTURE2D_DESC& acDesc) noexcept
{
    if (s_pEyeView && s_pEyeTexture == apTexture && s_eyeWidth == acDesc.Width && s_eyeHeight == acDesc.Height)
        return true;

    if (FAILED(apDevice->CreateRenderTargetView(apTexture, nullptr, s_pEyeView.ReleaseAndGetAddressOf())))
    {
        spdlog::error("VR eyes: the submitted {}x{} texture takes no render target view, so the menu cannot go into it", acDesc.Width, acDesc.Height);
        s_pEyeTexture = nullptr;

        return false;
    }

    s_pEyeTexture = apTexture;
    s_eyeWidth = acDesc.Width;
    s_eyeHeight = acDesc.Height;

    spdlog::info("VR eyes: drawing into the {}x{} texture the game submits", acDesc.Width, acDesc.Height);

    return true;
}

/**
 * @brief How one eye turns a direction into a place in its half of the frame, and where that eye sits.
 *
 * Read once per eye and kept, since neither changes while the game runs.
 *
 * Taken from the game's own projection matrix rather than from the tangents GetProjectionRaw hands out. The
 * tangents are ambiguous: this headset reports top -1.4281 and bottom 0.9657, where the names and the signs
 * disagree, and nothing in the pair says which end is up. Choosing wrong puts the menu nineteen degrees off,
 * which is what happened. A projection matrix cannot be read two ways, because it is what the renderer
 * multiplies by, so the asymmetry comes out of it with no convention assumed.
 */
struct EyeProjection
{
    bool valid = false;

    // A direction's tangent becomes a normalised device coordinate as scale * tangent - offset.
    float scaleX = 0.f;
    float offsetX = 0.f;
    float scaleY = 0.f;
    float offsetY = 0.f;

    // Where this eye is relative to the head, in metres.
    float eyeX = 0.f;
    float eyeY = 0.f;
    float eyeZ = 0.f;
};

EyeProjection s_projection[2];

const EyeProjection& GetProjection(const vr::EVREye aEye) noexcept
{
    EyeProjection& projection = s_projection[aEye == vr::Eye_Left ? 0 : 1];

    if (projection.valid || !s_pVRSystem)
        return projection;

    // The near and far planes land in the third row, which nothing here reads.
    const vr::HmdMatrix44_t cMatrix = s_pVRSystem->GetProjectionMatrix(aEye, 0.1f, 100.f);

    const float cScaleX = cMatrix.m[0][0];
    const float cOffsetX = cMatrix.m[0][2];
    const float cScaleY = cMatrix.m[1][1];
    const float cOffsetY = cMatrix.m[1][2];

    // A guard on the numbers and, with them, on the assumption that this is the slot the header says it is.
    // Any sensible field of view gives a scale near one, and no headset is asymmetric by half a screen.
    const bool cPlausible =
        cScaleX > 0.2f && cScaleX < 5.f && cScaleY > 0.2f && cScaleY < 5.f && std::fabs(cOffsetX) < 1.f && std::fabs(cOffsetY) < 1.f;

    if (!cPlausible)
    {
        static bool reported = false;

        if (!reported)
        {
            spdlog::error("VR eyes: the projection reads as scale {} {} offset {} {}, which is not one, so the menu gets centred instead", cScaleX,
                          cScaleY, cOffsetX, cOffsetY);
            reported = true;
        }

        return projection;
    }

    const vr::HmdMatrix34_t cToHead = s_pVRSystem->GetEyeToHeadTransform(aEye);

    projection.scaleX = cScaleX;
    projection.offsetX = cOffsetX;
    projection.scaleY = cScaleY;
    projection.offsetY = cOffsetY;

    // Translation only. A headset whose displays are canted would also have a rotation here, and a quad placed
    // by a viewport cannot express one, so there is nothing useful to do with it.
    projection.eyeX = cToHead.m[0][3];
    projection.eyeY = cToHead.m[1][3];
    projection.eyeZ = cToHead.m[2][3];
    projection.valid = true;

    return projection;
}

/**
 * @brief Where a rectangle a given distance in front of the head lands in one eye's part of the frame.
 *
 * Everything drawn on this path is a rectangle facing the player at a fixed distance, so this is all the
 * geometry there is. Doing it the way the game projects the world is what makes the two eyes agree and fuse
 * into one image; centring something in each eye instead puts the two thirty degrees apart, because the middle
 * of a viewport is nowhere near the middle of an asymmetric frustum.
 */
D3D11_VIEWPORT Project(const EyeProjection& acProjection, const float acCentreX, const float acCentreY, const float acWidth, const float acHeight,
                       const float acDistance, const float acEyeX, const float acEyeY, const float acEyeWidth, const float acEyeHeight) noexcept
{
    const float cDepth = acDistance + acProjection.eyeZ;

    // Tangents of the rectangle's edges as this eye sees them. The eye's own offset moves the eye rather than
    // the rectangle, which is why it comes off here and why the two eyes end up with different answers.
    const float cTanMinX = (acCentreX - acWidth * 0.5f - acProjection.eyeX) / cDepth;
    const float cTanMaxX = (acCentreX + acWidth * 0.5f - acProjection.eyeX) / cDepth;
    const float cTanMinY = (acCentreY - acHeight * 0.5f - acProjection.eyeY) / cDepth;
    const float cTanMaxY = (acCentreY + acHeight * 0.5f - acProjection.eyeY) / cDepth;

    // Into device coordinates, then into the eye's rectangle. Device y is up while the frame's rows run down,
    // so the top edge is the one with the larger y.
    const float cLeftEdge = 0.5f * (acProjection.scaleX * cTanMinX - acProjection.offsetX + 1.f);
    const float cRightEdge = 0.5f * (acProjection.scaleX * cTanMaxX - acProjection.offsetX + 1.f);
    const float cTopEdge = 0.5f * (1.f - (acProjection.scaleY * cTanMaxY - acProjection.offsetY));
    const float cBottomEdge = 0.5f * (1.f - (acProjection.scaleY * cTanMinY - acProjection.offsetY));

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = acEyeX + cLeftEdge * acEyeWidth;
    viewport.TopLeftY = acEyeY + cTopEdge * acEyeHeight;
    viewport.Width = (cRightEdge - cLeftEdge) * acEyeWidth;
    viewport.Height = (cBottomEdge - cTopEdge) * acEyeHeight;
    viewport.MinDepth = 0.f;
    viewport.MaxDepth = 1.f;

    return viewport;
}

/**
 * @brief Reports the frustum the game renders each eye with, once, the first time there is a frame.
 *
 * The quad's placement is currently two tuned numbers, which is fine on one headset and wrong on the next: a
 * field of view differs enormously between them. These are the values that turn a size in metres into a
 * rectangle in the frame, and they are logged before being relied on because the vertical pair's sign
 * convention is not something to guess at. The recommended size coming back as one eye of the census also
 * confirms these slots are where the header says they are.
 */
void LogProjectionOnce() noexcept
{
    static bool s_logged = false;

    if (s_logged || !s_pVRSystem)
        return;

    s_logged = true;

    uint32_t width = 0;
    uint32_t height = 0;

    s_pVRSystem->GetRecommendedRenderTargetSize(&width, &height);

    spdlog::info("VR eyes: the runtime recommends {}x{} per eye", width, height);

    for (uint32_t eye = 0; eye < 2; ++eye)
    {
        const vr::EVREye cEye = eye == 0 ? vr::Eye_Left : vr::Eye_Right;
        const char* cpName = eye == 0 ? "left" : "right";

        float left = 0.f;
        float right = 0.f;
        float top = 0.f;
        float bottom = 0.f;

        s_pVRSystem->GetProjectionRaw(cEye, &left, &right, &top, &bottom);

        const vr::HmdMatrix34_t cToHead = s_pVRSystem->GetEyeToHeadTransform(cEye);
        const EyeProjection& cProjection = GetProjection(cEye);

        spdlog::info("VR eyes: {} eye tangents are left {:.4f} right {:.4f} top {:.4f} bottom {:.4f}", cpName, left, right, top, bottom);
        spdlog::info("VR eyes:   and it sits at {:.4f} {:.4f} {:.4f} metres from the head", cToHead.m[0][3], cToHead.m[1][3], cToHead.m[2][3]);

        // The tangents above are only logged now for comparison. This is what the placement actually uses, and
        // the sign of the second offset is the answer to which way the frustum leans, which the tangents would
        // not say. Straight ahead lands at this fraction across and down the eye's rectangle.
        spdlog::info("VR eyes:   projects with scale {:.4f} {:.4f} offset {:.4f} {:.4f}, putting the axis at {:.3f} across and {:.3f} down", cProjection.scaleX,
                     cProjection.scaleY, cProjection.offsetX, cProjection.offsetY, 0.5f * (1.f - cProjection.offsetX),
                     0.5f * (1.f + cProjection.offsetY));
    }
}

/**
 * @brief Draws the page into one eye's part of the frame the game is about to hand over.
 *
 * The eye's region comes from the bounds the game passes rather than from an assumption about halves, so a
 * build laying its eyes out differently needs nothing changed here.
 */
void DrawEye(const vr::EVREye aEye, const vr::Texture_t* acpTexture, const vr::VRTextureBounds_t* acpBounds) noexcept
{
    if (!acpTexture || acpTexture->eType != vr::TextureType_DirectX || !acpTexture->handle)
        return;

    const VRMenuOverlay::Page cPage = VRMenuOverlay::GetPage();

    if (!cPage.draw || !cPage.pView || !cPage.width || !cPage.height)
        return;

    const BSGraphics::RendererData* pData = BSGraphics::GetRendererData();

    if (!pData || !pData->pForwarder || !pData->pContext)
        return;

    auto* pTexture = static_cast<ID3D11Texture2D*>(acpTexture->handle);

    D3D11_TEXTURE2D_DESC desc{};
    pTexture->GetDesc(&desc);

    if (!EnsureResources(pData->pForwarder) || !EnsureEyeView(pData->pForwarder, pTexture, desc))
        return;

    LogProjectionOnce();

    const float cMinU = acpBounds ? acpBounds->uMin : 0.f;
    const float cMinV = acpBounds ? acpBounds->vMin : 0.f;
    const float cSpanU = (acpBounds ? acpBounds->uMax : 1.f) - cMinU;
    const float cSpanV = (acpBounds ? acpBounds->vMax : 1.f) - cMinV;

    const float cEyeX = cMinU * desc.Width;
    const float cEyeY = cMinV * desc.Height;
    const float cEyeWidth = cSpanU * desc.Width;
    const float cEyeHeight = cSpanV * desc.Height;

    const EyeProjection& cProjection = GetProjection(aEye);
    const float cPageHeightInMetres = cPage.widthInMetres * static_cast<float>(cPage.height) / static_cast<float>(cPage.width);

    D3D11_VIEWPORT viewport{};
    viewport.MinDepth = 0.f;
    viewport.MaxDepth = 1.f;

    if (cProjection.valid)
    {
        /**
         * The quad is a rectangle in front of the head, and each eye is told where that rectangle falls in its
         * own view. Projecting it the way the game projects the world is what makes the two images agree and
         * fuse into one; centring it in each eye instead puts them thirty degrees apart, because the middle of
         * a viewport is nowhere near the middle of an asymmetric frustum.
         *
         * The size, distance and offsets are the ones the SteamVR overlay uses, so the menu does not move when
         * the path does.
         */
        viewport = Project(cProjection, cPage.rightShiftInMetres, cPage.upShiftInMetres, cPage.widthInMetres, cPageHeightInMetres, cPage.distanceInMetres,
                           cEyeX, cEyeY, cEyeWidth, cEyeHeight);
    }
    else
    {
        viewport.Width = cEyeWidth * kFallbackWidthFraction;
        viewport.Height = viewport.Width * static_cast<float>(cPage.height) / static_cast<float>(cPage.width);
        viewport.TopLeftX = cEyeX + (cEyeWidth - viewport.Width) * 0.5f;
        viewport.TopLeftY = cEyeY + (cEyeHeight - viewport.Height) * 0.5f;
    }

    /**
     * The quad is wider than an eye can see: 3.2m at 1.2m is about 106 degrees against this frustum's 94, so
     * the viewport above runs past the edge of the eye's half of the frame and would spill into the other
     * eye's. Each eye therefore sees the middle of the menu and loses the far edge, which is exactly what the
     * compositor does with an overlay too big for the display, and what holding a newspaper to your face does.
     */
    const D3D11_RECT cScissor{static_cast<LONG>(cEyeX), static_cast<LONG>(cEyeY), static_cast<LONG>(cEyeX + cEyeWidth),
                              static_cast<LONG>(cEyeY + cEyeHeight)};

    ID3D11DeviceContext* pRecord = s_pDeferred.Get();
    ID3D11RenderTargetView* pEyeView = s_pEyeView.Get();
    ID3D11ShaderResourceView* pPageView = cPage.pView;

    pRecord->OMSetRenderTargets(1, &pEyeView, nullptr);
    pRecord->RSSetViewports(1, &viewport);
    pRecord->RSSetScissorRects(1, &cScissor);
    pRecord->RSSetState(s_pRasterizer.Get());
    pRecord->OMSetBlendState(s_pBlend.Get(), nullptr, 0xFFFFFFFF);
    pRecord->OMSetDepthStencilState(s_pDepthStencil.Get(), 0);
    pRecord->IASetInputLayout(nullptr);
    pRecord->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    pRecord->VSSetShader(s_pVertexShader.Get(), nullptr, 0);
    pRecord->PSSetShader(s_pPixelShader.Get(), nullptr, 0);
    pRecord->PSSetShaderResources(0, 1, &pPageView);
    pRecord->PSSetSamplers(0, 1, s_pSampler.GetAddressOf());
    pRecord->Draw(4, 0);

    /**
     * And a dot where the controller is pointing, which the SteamVR path gets as the compositor's laser. Being
     * on the same plane as the page and projected the same way, it fuses with it and sits on the page rather
     * than floating in front of or behind it.
     *
     * Skipped entirely when the frustum could not be read, since a marker placed by guesswork on a page placed
     * by guesswork would not agree with either the page or the hand.
     */
    if (cPage.cursorOnPage && cProjection.valid)
    {
        const float cPointerX = cPage.rightShiftInMetres + (cPage.cursorAcross - 0.5f) * cPage.widthInMetres;
        const float cPointerY = cPage.upShiftInMetres + (0.5f - cPage.cursorDown) * cPageHeightInMetres;
        const float cPointerSize = cPage.widthInMetres * kPointerFraction;

        const D3D11_VIEWPORT cPointerViewport = Project(cProjection, cPointerX, cPointerY, cPointerSize, cPointerSize, cPage.distanceInMetres, cEyeX,
                                                        cEyeY, cEyeWidth, cEyeHeight);

        pRecord->RSSetViewports(1, &cPointerViewport);
        pRecord->PSSetShader(s_pPointerShader.Get(), nullptr, 0);
        pRecord->Draw(4, 0);
    }

    Microsoft::WRL::ComPtr<ID3D11CommandList> pCommands;

    if (SUCCEEDED(pRecord->FinishCommandList(FALSE, pCommands.GetAddressOf())) && pCommands)
        pData->pContext->ExecuteCommandList(pCommands.Get(), TRUE);
}

vr::EVRCompositorError __fastcall HookSubmit(void* apThis, const vr::EVREye aEye, const vr::Texture_t* acpTexture,
                                             const vr::VRTextureBounds_t* acpBounds, const vr::EVRSubmitFlags aFlags)
{
    DrawEye(aEye, acpTexture, acpBounds);

    return s_pRealSubmit(apThis, aEye, acpTexture, acpBounds, aFlags);
}

/**
 * @brief Patches Submit on the compositor the game was handed.
 *
 * The slot is patched where it lives rather than on a copy of the vtable, because a copy has to be as long as
 * the original and only the runtime knows how long that is. The single write needs no such guess, and the only
 * other caller of this vtable is the game, which is exactly who we mean to intercept.
 */
void HookCompositor(void* apCompositor, const char* acpVersion) noexcept
{
    if (s_pRealSubmit || !apCompositor)
        return;

    void** ppVtable = *static_cast<void***>(apCompositor);
    DWORD previous = 0;

    if (!VirtualProtect(&ppVtable[kSubmitSlot], sizeof(void*), PAGE_READWRITE, &previous))
    {
        spdlog::error("VR eyes: {}'s vtable will not unprotect, so the menu can only come from IVROverlay", acpVersion);
        return;
    }

    s_pRealSubmit = reinterpret_cast<TSubmit>(ppVtable[kSubmitSlot]);
    ppVtable[kSubmitSlot] = reinterpret_cast<void*>(&HookSubmit);

    VirtualProtect(&ppVtable[kSubmitSlot], sizeof(void*), previous, &previous);

    spdlog::info("VR eyes: {} Submit hooked at slot {}", acpVersion, kSubmitSlot);
}

void*(VR_CALLTYPE* s_pRealGetGenericInterface)(const char*, vr::EVRInitError*) = nullptr;

void* VR_CALLTYPE HookGetGenericInterface(const char* apVersion, vr::EVRInitError* apError)
{
    void* pInterface = s_pRealGetGenericInterface(apVersion, apError);

    // Worth a line each. There are five in a session, and which versions an install serves is the whole
    // question behind having a second path at all.
    spdlog::info("VR eyes: the game asked for {} and got {} (error {})", apVersion ? apVersion : "(null)", pInterface,
                 apError ? static_cast<int>(*apError) : 0);

    if (pInterface && apVersion && std::strncmp(apVersion, "IVRCompositor_", 14) == 0)
        HookCompositor(pInterface, apVersion);

    if (pInterface && apVersion && !s_pVRSystem && std::strncmp(apVersion, "IVRSystem_", 10) == 0)
        s_pVRSystem = static_cast<vr::IVRSystem*>(pInterface);

    return pInterface;
}

// The game resolves this through the launcher's headers, which carry the game's import table by the time
// initializers run, so the ordinary IAT hook reaches it. It has to be in place before the game's VR startup,
// which is well after this and well before the first frame.
static TiltedPhoques::Initializer s_eyeOverlayHooks(
    []()
    {
        s_pRealGetGenericInterface =
            reinterpret_cast<decltype(s_pRealGetGenericInterface)>(TP_HOOK_IAT2("openvr_api.dll", "VR_GetGenericInterface", HookGetGenericInterface));

        if (!s_pRealGetGenericInterface)
            spdlog::error("VR eyes: openvr_api.dll!VR_GetGenericInterface is not imported, so the compositor cannot be reached");
    });
} // namespace

#endif
