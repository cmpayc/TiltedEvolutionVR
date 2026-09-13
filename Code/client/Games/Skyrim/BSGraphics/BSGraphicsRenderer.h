#pragma once

#include <d3d11.h>

namespace BSGraphics
{
struct __declspec(align(8)) BSCriticalSection
{
    _RTL_CRITICAL_SECTION CriticalSection;
    unsigned int ulThreadOwner;
    unsigned int ulPrevOwner;
    unsigned int uiLockCount;
};

struct RenderTarget
{
    ID3D11Texture2D* pTexture;
    ID3D11Texture2D* pCopyTexture;
    ID3D11RenderTargetView* pRTView;
    ID3D11ShaderResourceView* pSRView;
    ID3D11ShaderResourceView* pCopySRView;
    ID3D11UnorderedAccessView* pUAView;
};

struct RendererWindow
{
    HWND hWnd;
    int iWindowX;
    int iWindowY;
    int uiWindowWidth;
    int uiWindowHeight;
    IDXGISwapChain* pSwapChain;
    BSGraphics::RenderTarget SwapChainRenderTarget;

    bool IsForeground();
};

RendererWindow* GetMainWindow();

struct DepthStencilTarget
{
    ID3D11Texture2D* pTexture;
    ID3D11DepthStencilView* pDSView[4];
    ID3D11DepthStencilView* pDSViewReadOnlyDepth[4];
    ID3D11DepthStencilView* pDSViewReadOnlyStencil[4];
    ID3D11DepthStencilView* pDSViewReadOnlyDepthStencil[4];
    ID3D11ShaderResourceView* pSRViewDepth;
    ID3D11ShaderResourceView* pSRViewStencil;
};

struct CubeMapRenderTarget
{
    ID3D11Texture2D* pTexture;
    ID3D11RenderTargetView* pRTView[6];
    ID3D11ShaderResourceView* pSRView;
};

struct RendererData
{
    uint32_t uiAdapter;            // 0x0000
    uint64_t DesiredRefreshRate;   // 0x0004
    uint64_t ActualRefreshRate;    // 0x000C
    uint32_t ScaleMode;            // 0x0014
    uint32_t ScanlineMode;         // 0x0018
    int32_t bFullScreen;           // 0x001C
    bool bAppFullScreen;           // 0x0020
    bool bBorderlessWindow;        // 0x0021
    bool bVSync;                   // 0x0022
    bool bInitialized;             // 0x0023
    bool bRequestWindowSizeChange; // 0x0024
    uint32_t uiNewWidth;           // 0x0028
    uint32_t uiNewHeight;          // 0x002C
    uint32_t uiPresentInterval;    // 0x0030
    ID3D11Device* pForwarder;      // 0x0038
    ID3D11DeviceContext* pContext; // 0x0040

    BSGraphics::RendererWindow RenderWindowA[32]; // V
    BSGraphics::RenderTarget pRenderTargetsA[114];
    BSGraphics::DepthStencilTarget pDepthStencilTargetsA[12];
    BSGraphics::CubeMapRenderTarget pCubeMapRenderTargetsA[1];
};

static_assert(offsetof(RendererData, pForwarder) == 0x38);
static_assert(offsetof(RendererData, RenderWindowA) == 0x48);
static_assert(offsetof(RendererData, pRenderTargetsA) == 0xA48);
static_assert(offsetof(RendererData, pDepthStencilTargetsA) == 0x1FA8);
static_assert(offsetof(RendererData, pCubeMapRenderTargetsA) == 0x26C8);

// Published by the renderer's init hook, so null until the renderer has been built.
const RendererData* GetRendererData();

struct Renderer
{
    bool bSkipNextPresent;
    void (*ResetRenderTargets)();
#if TP_SKYRIMVR
    // SkyrimVR 1.4.15 carries one more 8-byte member ahead of Data than 1.6.1170 does. Both
    // constructors publish &Data to the same global right after building it, which pins the
    // offset exactly:
    //   SE 0x140E42390: lea rbx, [rcx+0x10] ... mov [rip+0x2444656], rbx
    //   VR 0x140DBA7F0: lea rbx, [rcx+0x18] ... mov [rip+0x23C3F80], rbx
    // Which of the leading members grew is not established, and the client reads nothing but
    // Data, so the difference is carried as padding instead of being guessed at.
    uint64_t vrLeadingPad;
#endif
    BSGraphics::RendererData Data;
};

// Getting this wrong reads the window's width and height where the swap chain should be.
#if TP_SKYRIMVR
static_assert(offsetof(Renderer, Data) == 0x18);
#else
static_assert(offsetof(Renderer, Data) == 0x10);
#endif

struct RendererInitReturn
{
    HWND hwnd;
};

// former ViewportConfig
struct RendererInitOSData
{
    HWND hWnd;
    HINSTANCE hInstance;
    WNDPROC pWndProc;
    HICON hIcon;
    const char* pClassName;
    uint32_t uiAdapter;
    int bCreateSwapChainRenderTarget;
};

// former WindowConfig
struct ApplicationWindowProperties
{
    uint32_t uiWidth;  // V
    uint32_t uiHeight; // V
    int iX;
    int iY;
    uint32_t uiRefreshRate;
    bool bFullScreen;
    bool bBorderlessWindow;
    bool bVSync;
    uint32_t uiPresentInterval;
};

} // namespace BSGraphics
