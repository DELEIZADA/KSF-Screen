#include <napi.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ksf-hook/ksf_hook_ipc.h"


#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif


/*
========================================================
KSF SCREEN - CAPTURE ENGINE
========================================================

BACKENDS:

1. WGC
   Windows Graphics Capture + D3D11.
   Metodo principal.

2. HOOK
   DXGI / D3D11 dentro do processo alvo.
   Usado principalmente quando a janela e minimizada.

3. WIN32
   PrintWindow / GDI.
   Fallback para janelas minimizadas ou totalmente
   ocluidas quando o WGC deixa de entregar conteudo
   util para determinados aplicativos.

O JavaScript continua usando a mesma API:

- isVideoCaptureSupported
- listCapturableWindows
- startWindowCapture
- readVideoFrame
- stopWindowCapture
- getVideoCaptureStatus

========================================================
*/


// ======================================================
// TIPOS WINRT
// ======================================================

using WgcItem =
    winrt::Windows::Graphics::Capture::
        GraphicsCaptureItem;

using WgcFramePool =
    winrt::Windows::Graphics::Capture::
        Direct3D11CaptureFramePool;

using WgcSession =
    winrt::Windows::Graphics::Capture::
        GraphicsCaptureSession;

using WgcFrame =
    winrt::Windows::Graphics::Capture::
        Direct3D11CaptureFrame;

using WinrtD3DDevice =
    winrt::Windows::Graphics::DirectX::
        Direct3D11::IDirect3DDevice;

using WinrtD3DSurface =
    winrt::Windows::Graphics::DirectX::
        Direct3D11::IDirect3DSurface;

using PixelFormat =
    winrt::Windows::Graphics::DirectX::
        DirectXPixelFormat;


// ======================================================
// BACKEND
// ======================================================

enum class CaptureBackend
{
    None,
    WGC,
    Hook,
    Win32
};


static const char* BackendName(
    CaptureBackend backend
)
{
    switch (backend)
    {
        case CaptureBackend::WGC:
            return "WGC";

        case CaptureBackend::Hook:
            return "HOOK";

        case CaptureBackend::Win32:
            return "WIN32";

        default:
            return "NONE";
    }
}


// ======================================================
// ESTADO GLOBAL
// ======================================================

static std::mutex g_videoMutex;

static std::atomic<bool>
    g_stopRequested{ false };

static std::atomic<bool>
    g_monitorRunning{ false };

static bool g_videoActive = false;

static HWND g_videoHwnd = nullptr;

static int g_videoWidth = 0;

static int g_videoHeight = 0;

static uint64_t g_videoFrameNumber = 0;

static std::vector<uint8_t>
    g_latestFrame;

static std::string
    g_videoStage = "Nenhuma";

static HRESULT
    g_videoLastHRESULT = S_OK;

static CaptureBackend
    g_captureBackend =
        CaptureBackend::None;

static std::thread
    g_captureMonitorThread;


// ======================================================
// GRAPHICS HOOK - HOST IPC
// ======================================================

static HANDLE g_hookSharedMapping = nullptr;

static ksf_hook::SharedState*
    g_hookSharedState = nullptr;

static HANDLE g_hookReadyEvent = nullptr;

static HANDLE g_hookFrameEvent = nullptr;

static HANDLE g_hookStopEvent = nullptr;

static HANDLE g_hookTextureMutex = nullptr;

static DWORD g_hookTargetProcessId = 0;

static uint64_t g_hookLastFrameNumber = 0;

static bool g_hookHostActive = false;


// ======================================================
// DIRECT3D
// ======================================================

static winrt::com_ptr<ID3D11Device>
    g_d3dDevice;

static winrt::com_ptr<ID3D11DeviceContext>
    g_d3dContext;

static WinrtD3DDevice
    g_winrtDevice{ nullptr };


// ======================================================
// TEXTURA STAGING REUTILIZAVEL
// ======================================================

static winrt::com_ptr<ID3D11Texture2D>
    g_stagingTexture;

static UINT g_stagingWidth = 0;

static UINT g_stagingHeight = 0;

static DXGI_FORMAT
    g_stagingFormat =
        DXGI_FORMAT_UNKNOWN;


// ======================================================
// WINDOWS GRAPHICS CAPTURE
// ======================================================

static WgcItem
    g_captureItem{ nullptr };

static WgcFramePool
    g_framePool{ nullptr };

static WgcSession
    g_captureSession{ nullptr };

static winrt::event_token
    g_frameArrivedToken{};


// ======================================================
// DIAGNOSTICO
// ======================================================

static std::string HResultToHex(
    HRESULT hr
)
{
    char buffer[32] = {};

    sprintf_s(
        buffer,
        "0x%08lX",
        static_cast<unsigned long>(hr)
    );

    return std::string(buffer);
}


static void SetVideoStatus(
    const std::string& stage,
    HRESULT hr
)
{
    std::lock_guard<std::mutex>
        lock(g_videoMutex);

    g_videoStage = stage;

    g_videoLastHRESULT = hr;
}


// ======================================================
// HWND
// ======================================================

static HWND StringToHwnd(
    const std::string& value
)
{
    try
    {
        const unsigned long long number =
            std::stoull(value);

        return reinterpret_cast<HWND>(
            static_cast<uintptr_t>(
                number
            )
        );
    }
    catch (...)
    {
        return nullptr;
    }
}


// ======================================================
// UTF-16 -> UTF-8
// ======================================================

static std::string WideToUtf8(
    const std::wstring& value
)
{
    if (value.empty())
    {
        return std::string();
    }

    const int required =
        WideCharToMultiByte(
            CP_UTF8,
            0,
            value.c_str(),
            static_cast<int>(
                value.size()
            ),
            nullptr,
            0,
            nullptr,
            nullptr
        );

    if (required <= 0)
    {
        return std::string();
    }

    std::string result(
        static_cast<size_t>(
            required
        ),
        '\0'
    );

    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(
            value.size()
        ),
        result.data(),
        required,
        nullptr,
        nullptr
    );

    return result;
}


// ======================================================
// JANELA CLOAKED
// ======================================================

static bool IsWindowCloaked(
    HWND hwnd
)
{
    DWORD cloaked = 0;

    const HRESULT hr =
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_CLOAKED,
            &cloaked,
            sizeof(cloaked)
        );

    return
        SUCCEEDED(hr) &&
        cloaked != 0;
}


// ======================================================
// RECT DA JANELA
// ======================================================

static bool GetCaptureWindowRect(
    HWND hwnd,
    RECT& rect
)
{
    rect = {};

    RECT dwmRect{};

    const HRESULT hr =
        DwmGetWindowAttribute(
            hwnd,
            DWMWA_EXTENDED_FRAME_BOUNDS,
            &dwmRect,
            sizeof(dwmRect)
        );

    if (
        SUCCEEDED(hr) &&
        dwmRect.right >
            dwmRect.left &&
        dwmRect.bottom >
            dwmRect.top
    )
    {
        rect = dwmRect;

        return true;
    }

    if (!GetWindowRect(hwnd, &rect))
    {
        return false;
    }

    return
        rect.right >
            rect.left &&
        rect.bottom >
            rect.top;
}


// ======================================================
// VERIFICAR OCLUSAO
// ======================================================

static bool PointBelongsToWindow(
    HWND target,
    POINT point
)
{
    HWND found =
        WindowFromPoint(point);

    if (!found)
    {
        return false;
    }

    HWND root =
        GetAncestor(
            found,
            GA_ROOT
        );

    return root == target;
}


static bool IsWindowFullyOccluded(
    HWND hwnd
)
{
    if (
        !hwnd ||
        !IsWindow(hwnd)
    )
    {
        return false;
    }

    if (IsIconic(hwnd))
    {
        return true;
    }

    RECT rect{};

    if (!GetCaptureWindowRect(
        hwnd,
        rect
    ))
    {
        return false;
    }

    const int width =
        rect.right -
        rect.left;

    const int height =
        rect.bottom -
        rect.top;

    if (
        width <= 0 ||
        height <= 0
    )
    {
        return false;
    }

    const int insetX =
        (width > 40)
            ? 20
            : 1;

    const int insetY =
        (height > 40)
            ? 20
            : 1;

    const int left =
        rect.left +
        insetX;

    const int right =
        rect.right -
        insetX -
        1;

    const int top =
        rect.top +
        insetY;

    const int bottom =
        rect.bottom -
        insetY -
        1;

    const int centerX =
        rect.left +
        width / 2;

    const int centerY =
        rect.top +
        height / 2;

    POINT points[] =
    {
        { centerX, centerY },

        { left, top },
        { right, top },

        { left, bottom },
        { right, bottom },

        { centerX, top },
        { centerX, bottom },

        { left, centerY },
        { right, centerY }
    };

    for (
        const POINT& point :
        points
    )
    {
        if (
            PointBelongsToWindow(
                hwnd,
                point
            )
        )
        {
            return false;
        }
    }

    return true;
}


// ======================================================
// ENUMERAR JANELAS
// ======================================================

struct CapturableWindowInfo
{
    HWND hwnd = nullptr;

    DWORD processId = 0;

    std::string title;

    bool minimized = false;
};


static BOOL CALLBACK
EnumCapturableWindowsProc(
    HWND hwnd,
    LPARAM lParam
)
{
    if (
        !hwnd ||
        !IsWindow(hwnd) ||
        !IsWindowVisible(hwnd)
    )
    {
        return TRUE;
    }

    if (
        GetAncestor(
            hwnd,
            GA_ROOT
        ) != hwnd
    )
    {
        return TRUE;
    }

    if (IsWindowCloaked(hwnd))
    {
        return TRUE;
    }

    const LONG_PTR exStyle =
        GetWindowLongPtrW(
            hwnd,
            GWL_EXSTYLE
        );

    if (
        (exStyle &
            WS_EX_TOOLWINDOW) != 0 &&
        (exStyle &
            WS_EX_APPWINDOW) == 0
    )
    {
        return TRUE;
    }

    const int titleLength =
        GetWindowTextLengthW(
            hwnd
        );

    if (titleLength <= 0)
    {
        return TRUE;
    }

    std::wstring title(
        static_cast<size_t>(
            titleLength + 1
        ),
        L'\0'
    );

    const int copied =
        GetWindowTextW(
            hwnd,
            title.data(),
            titleLength + 1
        );

    if (copied <= 0)
    {
        return TRUE;
    }

    title.resize(
        static_cast<size_t>(
            copied
        )
    );

    DWORD processId = 0;

    GetWindowThreadProcessId(
        hwnd,
        &processId
    );

    if (processId == 0)
    {
        return TRUE;
    }

    auto* windows =
        reinterpret_cast<
            std::vector<
                CapturableWindowInfo
            >*
        >(lParam);

    if (!windows)
    {
        return FALSE;
    }

    CapturableWindowInfo info;

    info.hwnd =
        hwnd;

    info.processId =
        processId;

    info.title =
        WideToUtf8(title);

    info.minimized =
        IsIconic(hwnd) != FALSE;

    if (!info.title.empty())
    {
        windows->push_back(
            std::move(info)
        );
    }

    return TRUE;
}


static Napi::Value
ListCapturableWindows(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    std::vector<
        CapturableWindowInfo
    > windows;

    EnumWindows(
        EnumCapturableWindowsProc,
        reinterpret_cast<LPARAM>(
            &windows
        )
    );

    Napi::Array result =
        Napi::Array::New(
            env,
            windows.size()
        );

    for (
        size_t index = 0;
        index < windows.size();
        ++index
    )
    {
        const auto& window =
            windows[index];

        const unsigned long long
            hwndValue =
                static_cast<
                    unsigned long long
                >(
                    reinterpret_cast<
                        uintptr_t
                    >(
                        window.hwnd
                    )
                );

        Napi::Object item =
            Napi::Object::New(env);

        item.Set(
            "hwnd",
            std::to_string(
                hwndValue
            )
        );

        item.Set(
            "title",
            window.title
        );

        item.Set(
            "processId",
            static_cast<double>(
                window.processId
            )
        );

        item.Set(
            "minimized",
            window.minimized
        );

        result.Set(
            static_cast<uint32_t>(
                index
            ),
            item
        );
    }

    return result;
}


// ======================================================
// PUBLICAR FRAME
// ======================================================

static void PublishFrame(
    std::vector<uint8_t>&& frame,
    int width,
    int height,
    const char* stage
)
{
    if (
        frame.empty() ||
        width <= 0 ||
        height <= 0
    )
    {
        return;
    }

    std::lock_guard<std::mutex>
        lock(g_videoMutex);

    if (!g_videoActive)
    {
        return;
    }

    g_latestFrame.swap(
        frame
    );

    g_videoWidth =
        width;

    g_videoHeight =
        height;

    ++g_videoFrameNumber;

    g_videoStage =
        stage;

    g_videoLastHRESULT =
        S_OK;
}


// ======================================================
// CRIAR D3D11 DEVICE
// ======================================================

static HRESULT CreateD3DDevice()
{
    g_d3dDevice = nullptr;

    g_d3dContext = nullptr;

    g_winrtDevice = nullptr;

    g_stagingTexture = nullptr;

    g_stagingWidth = 0;

    g_stagingHeight = 0;

    g_stagingFormat =
        DXGI_FORMAT_UNKNOWN;

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL
        selectedFeatureLevel{};

    HRESULT hr =
        D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            ARRAYSIZE(
                featureLevels
            ),
            D3D11_SDK_VERSION,
            g_d3dDevice.put(),
            &selectedFeatureLevel,
            g_d3dContext.put()
        );

    if (FAILED(hr))
    {
        hr =
            D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels,
                ARRAYSIZE(
                    featureLevels
                ),
                D3D11_SDK_VERSION,
                g_d3dDevice.put(),
                &selectedFeatureLevel,
                g_d3dContext.put()
            );
    }

    if (FAILED(hr))
    {
        SetVideoStatus(
            "D3D11CreateDevice",
            hr
        );

        return hr;
    }

    winrt::com_ptr<IDXGIDevice>
        dxgiDevice;

    hr =
        g_d3dDevice->QueryInterface(
            IID_PPV_ARGS(
                dxgiDevice.put()
            )
        );

    if (FAILED(hr))
    {
        SetVideoStatus(
            "QueryInterface IDXGIDevice",
            hr
        );

        return hr;
    }

    winrt::com_ptr<IInspectable>
        inspectable;

    hr =
        CreateDirect3D11DeviceFromDXGIDevice(
            dxgiDevice.get(),
            inspectable.put()
        );

    if (FAILED(hr))
    {
        SetVideoStatus(
            "CreateDirect3D11DeviceFromDXGIDevice",
            hr
        );

        return hr;
    }

    g_winrtDevice =
        inspectable.as<
            WinrtD3DDevice
        >();

    return S_OK;
}


// ======================================================
// GRAPHICS CAPTURE ITEM PELO HWND
// ======================================================

static HRESULT CreateCaptureItemForWindow(
    HWND hwnd,
    WgcItem& item
)
{
    try
    {
        auto interop =
            winrt::get_activation_factory<
                WgcItem,
                IGraphicsCaptureItemInterop
            >();

        winrt::com_ptr<
            ABI::Windows::Graphics::
                Capture::
                IGraphicsCaptureItem
        > abiItem;

        HRESULT hr =
            interop->CreateForWindow(
                hwnd,
                __uuidof(
                    ABI::Windows::
                        Graphics::
                        Capture::
                        IGraphicsCaptureItem
                ),
                abiItem.put_void()
            );

        if (FAILED(hr))
        {
            SetVideoStatus(
                "CreateForWindow",
                hr
            );

            return hr;
        }

        item =
            abiItem.as<WgcItem>();

        return S_OK;
    }
    catch (
        const winrt::hresult_error&
            error
    )
    {
        SetVideoStatus(
            "CreateCaptureItemForWindow",
            error.code()
        );

        return error.code();
    }
}


// ======================================================
// PEGAR TEXTURA D3D11
// ======================================================

static HRESULT GetTextureFromSurface(
    const WinrtD3DSurface& surface,
    ID3D11Texture2D** texture
)
{
    if (!texture)
    {
        return E_POINTER;
    }

    *texture = nullptr;

    auto access =
        surface.as<
            ::Windows::Graphics::
                DirectX::
                Direct3D11::
                IDirect3DDxgiInterfaceAccess
        >();

    return access->GetInterface(
        __uuidof(
            ID3D11Texture2D
        ),
        reinterpret_cast<void**>(
            texture
        )
    );
}


// ======================================================
// WGC GPU -> CPU
// ======================================================

static void CopyWgcFrameToCpu(
    const WgcFrame& frame
)
{
    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        if (
            !g_videoActive ||
            g_captureBackend !=
                CaptureBackend::WGC
        )
        {
            return;
        }
    }

    if (
        !g_d3dDevice ||
        !g_d3dContext
    )
    {
        return;
    }

    WinrtD3DSurface surface =
        frame.Surface();

    winrt::com_ptr<
        ID3D11Texture2D
    > sourceTexture;

    HRESULT hr =
        GetTextureFromSurface(
            surface,
            sourceTexture.put()
        );

    if (FAILED(hr))
    {
        SetVideoStatus(
            "GetTextureFromSurface",
            hr
        );

        return;
    }

    D3D11_TEXTURE2D_DESC
        sourceDesc{};

    sourceTexture->GetDesc(
        &sourceDesc
    );

    if (
        sourceDesc.Width == 0 ||
        sourceDesc.Height == 0
    )
    {
        return;
    }

    const bool recreateStaging =
        !g_stagingTexture ||
        g_stagingWidth !=
            sourceDesc.Width ||
        g_stagingHeight !=
            sourceDesc.Height ||
        g_stagingFormat !=
            sourceDesc.Format;

    if (recreateStaging)
    {
        D3D11_TEXTURE2D_DESC
            stagingDesc =
                sourceDesc;

        stagingDesc.BindFlags = 0;

        stagingDesc.MiscFlags = 0;

        stagingDesc.Usage =
            D3D11_USAGE_STAGING;

        stagingDesc.CPUAccessFlags =
            D3D11_CPU_ACCESS_READ;

        stagingDesc.MipLevels = 1;

        stagingDesc.ArraySize = 1;

        g_stagingTexture = nullptr;

        hr =
            g_d3dDevice->
                CreateTexture2D(
                    &stagingDesc,
                    nullptr,
                    g_stagingTexture.put()
                );

        if (FAILED(hr))
        {
            SetVideoStatus(
                "CreateTexture2D staging",
                hr
            );

            g_stagingWidth = 0;

            g_stagingHeight = 0;

            g_stagingFormat =
                DXGI_FORMAT_UNKNOWN;

            return;
        }

        g_stagingWidth =
            sourceDesc.Width;

        g_stagingHeight =
            sourceDesc.Height;

        g_stagingFormat =
            sourceDesc.Format;
    }

    g_d3dContext->CopyResource(
        g_stagingTexture.get(),
        sourceTexture.get()
    );

    D3D11_MAPPED_SUBRESOURCE
        mapped{};

    hr =
        g_d3dContext->Map(
            g_stagingTexture.get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped
        );

    if (FAILED(hr))
    {
        SetVideoStatus(
            "Map staging texture",
            hr
        );

        return;
    }

    const int width =
        static_cast<int>(
            sourceDesc.Width
        );

    const int height =
        static_cast<int>(
            sourceDesc.Height
        );

    const size_t rowBytes =
        static_cast<size_t>(
            width
        ) * 4;

    std::vector<uint8_t>
        newFrame(
            rowBytes *
            static_cast<size_t>(
                height
            )
        );

    const uint8_t* source =
        static_cast<
            const uint8_t*
        >(
            mapped.pData
        );

    for (
        int y = 0;
        y < height;
        ++y
    )
    {
        std::memcpy(
            newFrame.data() +
                static_cast<size_t>(
                    y
                ) *
                rowBytes,

            source +
                static_cast<size_t>(
                    y
                ) *
                mapped.RowPitch,

            rowBytes
        );
    }

    g_d3dContext->Unmap(
        g_stagingTexture.get(),
        0
    );

    PublishFrame(
        std::move(newFrame),
        width,
        height,
        "CAPTURANDO - WGC"
    );
}


// ======================================================
// WGC FRAME ARRIVED
// ======================================================

static void OnFrameArrived(
    const WgcFramePool& sender,
    const winrt::Windows::
        Foundation::IInspectable&
)
{
    try
    {
        WgcFrame frame =
            sender.TryGetNextFrame();

        if (!frame)
        {
            return;
        }

        CopyWgcFrameToCpu(
            frame
        );
    }
    catch (
        const winrt::hresult_error&
            error
    )
    {
        SetVideoStatus(
            "FrameArrived",
            error.code()
        );
    }
    catch (...)
    {
        SetVideoStatus(
            "FrameArrived",
            E_FAIL
        );
    }
}


// ======================================================
// PARAR SOMENTE WGC
// ======================================================

static void StopWgcBackend()
{
    try
    {
        if (g_framePool)
        {
            if (
                g_frameArrivedToken.value !=
                0
            )
            {
                g_framePool.FrameArrived(
                    g_frameArrivedToken
                );

                g_frameArrivedToken = {};
            }
        }

        if (g_captureSession)
        {
            g_captureSession.Close();
        }

        if (g_framePool)
        {
            g_framePool.Close();
        }
    }
    catch (...)
    {
    }

    g_captureSession = nullptr;

    g_framePool = nullptr;

    g_captureItem = nullptr;

    g_stagingTexture = nullptr;

    g_stagingWidth = 0;

    g_stagingHeight = 0;

    g_stagingFormat =
        DXGI_FORMAT_UNKNOWN;

    g_winrtDevice = nullptr;

    g_d3dContext = nullptr;

    g_d3dDevice = nullptr;
}


// ======================================================
// INICIAR SOMENTE WGC
// ======================================================

static HRESULT StartWgcBackend(
    HWND hwnd
)
{
    try
    {
        winrt::init_apartment(
            winrt::apartment_type::
                multi_threaded
        );
    }
    catch (
        const winrt::hresult_error&
            error
    )
    {
        if (
            error.code() !=
            RPC_E_CHANGED_MODE
        )
        {
            return error.code();
        }
    }

    try
    {
        if (
            !WgcSession::
                IsSupported()
        )
        {
            return E_NOTIMPL;
        }
    }
    catch (
        const winrt::hresult_error&
            error
    )
    {
        return error.code();
    }

    HRESULT hr =
        CreateD3DDevice();

    if (FAILED(hr))
    {
        return hr;
    }

    WgcItem item{ nullptr };

    hr =
        CreateCaptureItemForWindow(
            hwnd,
            item
        );

    if (FAILED(hr))
    {
        StopWgcBackend();

        return hr;
    }

    try
    {
        auto size =
            item.Size();

        if (
            size.Width <= 0 ||
            size.Height <= 0
        )
        {
            StopWgcBackend();

            return E_FAIL;
        }

        WgcFramePool framePool =
            WgcFramePool::
                CreateFreeThreaded(
                    g_winrtDevice,

                    PixelFormat::
                        B8G8R8A8UIntNormalized,

                    2,

                    size
                );

        WgcSession session =
            framePool.
                CreateCaptureSession(
                    item
                );

        winrt::event_token token =
            framePool.FrameArrived(
                &OnFrameArrived
            );

        g_captureItem =
            item;

        g_framePool =
            framePool;

        g_captureSession =
            session;

        g_frameArrivedToken =
            token;

        session.StartCapture();

        return S_OK;
    }
    catch (
        const winrt::hresult_error&
            error
    )
    {
        const HRESULT hrError =
            error.code();

        StopWgcBackend();

        return hrError;
    }
    catch (...)
    {
        StopWgcBackend();

        return E_FAIL;
    }
}


// ======================================================
// GRAPHICS HOOK - UTILITARIOS
// ======================================================

static void CloseHookHandle(
    HANDLE& handle
)
{
    if (!handle)
    {
        return;
    }

    CloseHandle(handle);
    handle = nullptr;
}


static void CloseHookHostIpc()
{
    if (g_hookSharedState)
    {
        UnmapViewOfFile(
            g_hookSharedState
        );

        g_hookSharedState = nullptr;
    }

    CloseHookHandle(
        g_hookTextureMutex
    );

    CloseHookHandle(
        g_hookStopEvent
    );

    CloseHookHandle(
        g_hookFrameEvent
    );

    CloseHookHandle(
        g_hookReadyEvent
    );

    CloseHookHandle(
        g_hookSharedMapping
    );

    g_hookTargetProcessId = 0;
    g_hookLastFrameNumber = 0;
    g_hookHostActive = false;
}


static std::wstring GetKsfVideoModuleDirectory()
{
    HMODULE moduleHandle = nullptr;

    if (
        !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(
                &GetKsfVideoModuleDirectory
            ),
            &moduleHandle
        )
    )
    {
        return std::wstring();
    }

    std::vector<wchar_t>
        pathBuffer(32768, L'\0');

    const DWORD copied =
        GetModuleFileNameW(
            moduleHandle,
            pathBuffer.data(),
            static_cast<DWORD>(
                pathBuffer.size()
            )
        );

    if (
        copied == 0 ||
        copied >= pathBuffer.size()
    )
    {
        return std::wstring();
    }

    std::wstring fullPath(
        pathBuffer.data(),
        copied
    );

    const size_t slash =
        fullPath.find_last_of(
            L"\\/"
        );

    if (slash == std::wstring::npos)
    {
        return std::wstring();
    }

    return fullPath.substr(
        0,
        slash
    );
}


static bool FileExistsWide(
    const std::wstring& path
)
{
    const DWORD attributes =
        GetFileAttributesW(
            path.c_str()
        );

    return
        attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}


static bool LaunchHookInjector(
    DWORD targetProcessId
)
{
    const std::wstring directory =
        GetKsfVideoModuleDirectory();

    if (directory.empty())
    {
        SetVideoStatus(
            "HOOK - diretorio do modulo nao encontrado",
            HRESULT_FROM_WIN32(
                ERROR_PATH_NOT_FOUND
            )
        );

        return false;
    }

    const std::wstring injectorPath =
        directory +
        L"\\ksf_injector.exe";

    const std::wstring hookPath =
        directory +
        L"\\ksf_hook.dll";

    if (
        !FileExistsWide(injectorPath) ||
        !FileExistsWide(hookPath)
    )
    {
        SetVideoStatus(
            "HOOK - injector ou DLL nao encontrado",
            HRESULT_FROM_WIN32(
                ERROR_FILE_NOT_FOUND
            )
        );

        return false;
    }

    std::wstring commandLine =
        L"\"" + injectorPath +
        L"\" " +
        std::to_wstring(
            targetProcessId
        ) +
        L" \"" + hookPath +
        L"\"";

    std::vector<wchar_t> commandBuffer(
        commandLine.begin(),
        commandLine.end()
    );

    commandBuffer.push_back(
        L'\0'
    );

    STARTUPINFOW startupInfo{};
    startupInfo.cb =
        sizeof(startupInfo);

    PROCESS_INFORMATION
        processInfo{};

    const BOOL created =
        CreateProcessW(
            injectorPath.c_str(),
            commandBuffer.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            directory.c_str(),
            &startupInfo,
            &processInfo
        );

    if (!created)
    {
        SetVideoStatus(
            "HOOK - CreateProcess injector",
            HRESULT_FROM_WIN32(
                GetLastError()
            )
        );

        return false;
    }

    const DWORD waitResult =
        WaitForSingleObject(
            processInfo.hProcess,
            10000
        );

    DWORD exitCode =
        STILL_ACTIVE;

    if (waitResult == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(
            processInfo.hProcess,
            &exitCode
        );
    }

    CloseHandle(
        processInfo.hThread
    );

    CloseHandle(
        processInfo.hProcess
    );

    if (
        waitResult != WAIT_OBJECT_0 ||
        exitCode != 0
    )
    {
        SetVideoStatus(
            "HOOK - injector falhou",
            HRESULT_FROM_WIN32(
                waitResult == WAIT_TIMEOUT
                    ? WAIT_TIMEOUT
                    : ERROR_GEN_FAILURE
            )
        );

        return false;
    }

    return true;
}


static bool CreateHookHostIpc(
    DWORD targetProcessId
)
{
    CloseHookHostIpc();

    wchar_t sharedName[
        ksf_hook::NAME_CAPACITY
    ] = {};

    wchar_t readyName[
        ksf_hook::NAME_CAPACITY
    ] = {};

    wchar_t frameName[
        ksf_hook::NAME_CAPACITY
    ] = {};

    wchar_t stopName[
        ksf_hook::NAME_CAPACITY
    ] = {};

    wchar_t textureName[
        ksf_hook::NAME_CAPACITY
    ] = {};

    ksf_hook::BuildSharedMemoryName(
        sharedName,
        _countof(sharedName),
        targetProcessId
    );

    ksf_hook::BuildReadyEventName(
        readyName,
        _countof(readyName),
        targetProcessId
    );

    ksf_hook::BuildFrameEventName(
        frameName,
        _countof(frameName),
        targetProcessId
    );

    ksf_hook::BuildStopEventName(
        stopName,
        _countof(stopName),
        targetProcessId
    );

    ksf_hook::BuildTextureMutexName(
        textureName,
        _countof(textureName),
        targetProcessId
    );

    g_hookSharedMapping =
        CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(
                sizeof(
                    ksf_hook::SharedState
                )
            ),
            sharedName
        );

    if (!g_hookSharedMapping)
    {
        return false;
    }

    g_hookSharedState =
        static_cast<
            ksf_hook::SharedState*
        >(
            MapViewOfFile(
                g_hookSharedMapping,
                FILE_MAP_READ |
                FILE_MAP_WRITE,
                0,
                0,
                sizeof(
                    ksf_hook::SharedState
                )
            )
        );

    if (!g_hookSharedState)
    {
        CloseHookHostIpc();
        return false;
    }

    g_hookReadyEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            readyName
        );

    g_hookFrameEvent =
        CreateEventW(
            nullptr,
            FALSE,
            FALSE,
            frameName
        );

    g_hookStopEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            stopName
        );

    g_hookTextureMutex =
        CreateMutexW(
            nullptr,
            FALSE,
            textureName
        );

    if (
        !g_hookReadyEvent ||
        !g_hookFrameEvent ||
        !g_hookStopEvent ||
        !g_hookTextureMutex
    )
    {
        CloseHookHostIpc();
        return false;
    }

    ResetEvent(
        g_hookFrameEvent
    );

    ResetEvent(
        g_hookStopEvent
    );

    ksf_hook::InitializeSharedState(
        g_hookSharedState,
        GetCurrentProcessId(),
        targetProcessId
    );

    g_hookTargetProcessId =
        targetProcessId;

    g_hookLastFrameNumber = 0;

    return true;
}


static void StopHookBackend()
{
    if (g_hookSharedState)
    {
        InterlockedExchange(
            &g_hookSharedState->command,
            static_cast<LONG>(
                ksf_hook::HookCommand::
                    StopCapture
            )
        );

        Sleep(150);
    }

    CloseHookHostIpc();

    g_stagingTexture = nullptr;
    g_stagingWidth = 0;
    g_stagingHeight = 0;
    g_stagingFormat =
        DXGI_FORMAT_UNKNOWN;

    g_winrtDevice = nullptr;
    g_d3dContext = nullptr;
    g_d3dDevice = nullptr;
}


static HRESULT StartHookBackend(
    HWND hwnd
)
{
    if (
        !hwnd ||
        !IsWindow(hwnd)
    )
    {
        return E_INVALIDARG;
    }

    DWORD targetProcessId = 0;

    GetWindowThreadProcessId(
        hwnd,
        &targetProcessId
    );

    if (targetProcessId == 0)
    {
        return HRESULT_FROM_WIN32(
            ERROR_INVALID_PARAMETER
        );
    }

    StopWgcBackend();
    StopHookBackend();

    if (!CreateHookHostIpc(
        targetProcessId
    ))
    {
        const DWORD error =
            GetLastError();

        return HRESULT_FROM_WIN32(
            error == ERROR_SUCCESS
                ? ERROR_GEN_FAILURE
                : error
        );
    }

    if (!LaunchHookInjector(
        targetProcessId
    ))
    {
        CloseHookHostIpc();
        return E_FAIL;
    }

    const DWORD readyResult =
        WaitForSingleObject(
            g_hookReadyEvent,
            5000
        );

    if (readyResult != WAIT_OBJECT_0)
    {
        CloseHookHostIpc();

        return HRESULT_FROM_WIN32(
            readyResult == WAIT_TIMEOUT
                ? WAIT_TIMEOUT
                : ERROR_GEN_FAILURE
        );
    }

    HRESULT hr =
        CreateD3DDevice();

    if (FAILED(hr))
    {
        CloseHookHostIpc();
        return hr;
    }

    InterlockedExchange(
        &g_hookSharedState->command,
        static_cast<LONG>(
            ksf_hook::HookCommand::
                StartCapture
        )
    );

    g_hookHostActive = true;

    return S_OK;
}


static bool CopyHookTextureToCpu()
{
    if (
        !g_hookHostActive ||
        !g_hookSharedState ||
        !g_d3dDevice ||
        !g_d3dContext
    )
    {
        return false;
    }

    const uint64_t frameNumber =
        g_hookSharedState->
            frame.frameNumber;

    if (
        frameNumber == 0 ||
        frameNumber ==
            g_hookLastFrameNumber
    )
    {
        return false;
    }

    const uint64_t sharedHandleValue =
        g_hookSharedState->
            frame.sharedTextureHandle;

    const uint32_t sharedWidth =
        g_hookSharedState->
            frame.width;

    const uint32_t sharedHeight =
        g_hookSharedState->
            frame.height;

    const uint32_t sharedFormat =
        g_hookSharedState->
            frame.format;

    if (
        sharedHandleValue == 0 ||
        sharedWidth == 0 ||
        sharedHeight == 0
    )
    {
        return false;
    }

    HANDLE sharedHandle =
        reinterpret_cast<HANDLE>(
            static_cast<uintptr_t>(
                sharedHandleValue
            )
        );

    winrt::com_ptr<ID3D11Texture2D>
        sharedTexture;

    HRESULT hr =
        g_d3dDevice->OpenSharedResource(
            sharedHandle,
            __uuidof(ID3D11Texture2D),
            sharedTexture.put_void()
        );

    if (FAILED(hr))
    {
        SetVideoStatus(
            "HOOK - OpenSharedResource",
            hr
        );

        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};

    sharedTexture->GetDesc(
        &sourceDesc
    );

    if (
        sourceDesc.Width == 0 ||
        sourceDesc.Height == 0
    )
    {
        return false;
    }

    const bool recreateStaging =
        !g_stagingTexture ||
        g_stagingWidth !=
            sourceDesc.Width ||
        g_stagingHeight !=
            sourceDesc.Height ||
        g_stagingFormat !=
            sourceDesc.Format;

    if (recreateStaging)
    {
        D3D11_TEXTURE2D_DESC
            stagingDesc =
                sourceDesc;

        stagingDesc.BindFlags = 0;
        stagingDesc.MiscFlags = 0;
        stagingDesc.Usage =
            D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags =
            D3D11_CPU_ACCESS_READ;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;

        g_stagingTexture = nullptr;

        hr =
            g_d3dDevice->CreateTexture2D(
                &stagingDesc,
                nullptr,
                g_stagingTexture.put()
            );

        if (FAILED(hr))
        {
            SetVideoStatus(
                "HOOK - CreateTexture2D staging",
                hr
            );

            return false;
        }

        g_stagingWidth =
            sourceDesc.Width;

        g_stagingHeight =
            sourceDesc.Height;

        g_stagingFormat =
            sourceDesc.Format;
    }

    g_d3dContext->CopyResource(
        g_stagingTexture.get(),
        sharedTexture.get()
    );

    D3D11_MAPPED_SUBRESOURCE mapped{};

    hr =
        g_d3dContext->Map(
            g_stagingTexture.get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped
        );

    if (FAILED(hr))
    {
        SetVideoStatus(
            "HOOK - Map staging",
            hr
        );

        return false;
    }

    const int width =
        static_cast<int>(
            sourceDesc.Width
        );

    const int height =
        static_cast<int>(
            sourceDesc.Height
        );

    const size_t rowBytes =
        static_cast<size_t>(
            width
        ) * 4;

    std::vector<uint8_t> newFrame(
        rowBytes *
        static_cast<size_t>(
            height
        )
    );

    const uint8_t* source =
        static_cast<const uint8_t*>(
            mapped.pData
        );

    for (
        int y = 0;
        y < height;
        ++y
    )
    {
        uint8_t* destinationRow =
            newFrame.data() +
            static_cast<size_t>(y) *
            rowBytes;

        const uint8_t* sourceRow =
            source +
            static_cast<size_t>(y) *
            mapped.RowPitch;

        std::memcpy(
            destinationRow,
            sourceRow,
            rowBytes
        );

        if (
            sharedFormat ==
            static_cast<uint32_t>(
                ksf_hook::TextureFormat::
                    RGBA8
            )
        )
        {
            for (
                int x = 0;
                x < width;
                ++x
            )
            {
                uint8_t* pixel =
                    destinationRow +
                    static_cast<size_t>(x) *
                    4;

                const uint8_t red =
                    pixel[0];

                pixel[0] =
                    pixel[2];

                pixel[2] =
                    red;
            }
        }
    }

    g_d3dContext->Unmap(
        g_stagingTexture.get(),
        0
    );

    g_hookLastFrameNumber =
        frameNumber;

    PublishFrame(
        std::move(newFrame),
        width,
        height,
        "CAPTURANDO - HOOK"
    );

    return true;
}


static bool SwitchToHookBackend(
    HWND hwnd
)
{
    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        if (!g_videoActive)
        {
            return false;
        }

        g_videoStage =
            "TROCANDO PARA HOOK";
    }

    const HRESULT hr =
        StartHookBackend(
            hwnd
        );

    if (FAILED(hr))
    {
        SetVideoStatus(
            "HOOK NAO DISPONIVEL",
            hr
        );

        return false;
    }

    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        if (!g_videoActive)
        {
            StopHookBackend();
            return false;
        }

        g_captureBackend =
            CaptureBackend::Hook;

        g_videoStage =
            "CAPTURA INICIADA - HOOK";

        g_videoLastHRESULT =
            S_OK;
    }

    return true;
}


// ======================================================
// WIN32 / PRINTWINDOW
// ======================================================

static bool CaptureWin32Frame(
    HWND hwnd
)
{
    if (
        !hwnd ||
        !IsWindow(hwnd)
    )
    {
        return false;
    }

    RECT rect{};

    if (!GetCaptureWindowRect(
        hwnd,
        rect
    ))
    {
        return false;
    }

    const int width =
        rect.right -
        rect.left;

    const int height =
        rect.bottom -
        rect.top;

    if (
        width <= 1 ||
        height <= 1 ||
        width > 16384 ||
        height > 16384
    )
    {
        return false;
    }

    HDC screenDc =
        GetDC(nullptr);

    if (!screenDc)
    {
        return false;
    }

    HDC memoryDc =
        CreateCompatibleDC(
            screenDc
        );

    if (!memoryDc)
    {
        ReleaseDC(
            nullptr,
            screenDc
        );

        return false;
    }

    BITMAPINFO bitmapInfo{};

    bitmapInfo.bmiHeader.biSize =
        sizeof(
            BITMAPINFOHEADER
        );

    bitmapInfo.bmiHeader.biWidth =
        width;

    bitmapInfo.bmiHeader.biHeight =
        -height;

    bitmapInfo.bmiHeader.biPlanes =
        1;

    bitmapInfo.bmiHeader.biBitCount =
        32;

    bitmapInfo.bmiHeader.biCompression =
        BI_RGB;

    void* pixels = nullptr;

    HBITMAP bitmap =
        CreateDIBSection(
            screenDc,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &pixels,
            nullptr,
            0
        );

    if (
        !bitmap ||
        !pixels
    )
    {
        if (bitmap)
        {
            DeleteObject(bitmap);
        }

        DeleteDC(memoryDc);

        ReleaseDC(
            nullptr,
            screenDc
        );

        return false;
    }

    HGDIOBJ oldObject =
        SelectObject(
            memoryDc,
            bitmap
        );

    PatBlt(
        memoryDc,
        0,
        0,
        width,
        height,
        BLACKNESS
    );

    BOOL captured =
        PrintWindow(
            hwnd,
            memoryDc,
            PW_RENDERFULLCONTENT
        );

    if (!captured)
    {
        captured =
            PrintWindow(
                hwnd,
                memoryDc,
                0
            );
    }

    bool success = false;

    if (captured)
    {
        const size_t bytes =
            static_cast<size_t>(
                width
            ) *
            static_cast<size_t>(
                height
            ) *
            4;

        std::vector<uint8_t>
            frame(bytes);

        std::memcpy(
            frame.data(),
            pixels,
            bytes
        );

        PublishFrame(
            std::move(frame),
            width,
            height,
            "CAPTURANDO - WIN32"
        );

        success = true;
    }

    SelectObject(
        memoryDc,
        oldObject
    );

    DeleteObject(bitmap);

    DeleteDC(memoryDc);

    ReleaseDC(
        nullptr,
        screenDc
    );

    return success;
}


// ======================================================
// TROCAR PARA FALLBACK WIN32
// ======================================================

static void SwitchToWin32Backend()
{
    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        if (
            !g_videoActive ||
            g_captureBackend ==
                CaptureBackend::Win32
        )
        {
            return;
        }

        g_captureBackend =
            CaptureBackend::Win32;

        g_videoStage =
            "TROCANDO PARA WIN32";

        g_videoLastHRESULT =
            S_OK;
    }

    StopWgcBackend();
}


// ======================================================
// MONITOR DO CAPTURE ENGINE
// ======================================================

static void CaptureMonitorLoop()
{
    g_monitorRunning.store(
        true
    );

    int hiddenChecks = 0;

    auto nextWin32Frame =
        std::chrono::
            steady_clock::now();

    while (
        !g_stopRequested.load()
    )
    {
        HWND hwnd = nullptr;

        CaptureBackend backend =
            CaptureBackend::None;

        bool active = false;

        {
            std::lock_guard<std::mutex>
                lock(g_videoMutex);

            hwnd =
                g_videoHwnd;

            backend =
                g_captureBackend;

            active =
                g_videoActive;
        }

        if (
            !active ||
            !hwnd ||
            !IsWindow(hwnd)
        )
        {
            break;
        }

        if (
            backend ==
            CaptureBackend::WGC
        )
        {
            const bool minimized =
                IsIconic(hwnd) != FALSE;

            const bool occluded =
                IsWindowFullyOccluded(
                    hwnd
                );

            if (
                minimized ||
                occluded
            )
            {
                ++hiddenChecks;
            }
            else
            {
                hiddenChecks = 0;
            }

            if (hiddenChecks >= 3)
            {
                if (minimized)
                {
                    if (!SwitchToHookBackend(
                        hwnd
                    ))
                    {
                        SwitchToWin32Backend();

                        nextWin32Frame =
                            std::chrono::
                                steady_clock::now();
                    }
                }
                else
                {
                    /*
                    Para uma janela apenas coberta,
                    preservamos o fallback Win32 que ja
                    estava funcionando no KSF. O Hook e
                    priorizado quando a janela realmente
                    entra no estado minimizado.
                    */

                    SwitchToWin32Backend();

                    nextWin32Frame =
                        std::chrono::
                            steady_clock::now();
                }

                hiddenChecks = 0;
                continue;
            }

            std::this_thread::
                sleep_for(
                    std::chrono::
                        milliseconds(
                            250
                        )
                );

            continue;
        }

        if (
            backend ==
            CaptureBackend::Hook
        )
        {
            if (
                g_hookFrameEvent &&
                WaitForSingleObject(
                    g_hookFrameEvent,
                    8
                ) == WAIT_OBJECT_0
            )
            {
                CopyHookTextureToCpu();
            }
            else
            {
                /*
                Tambem consultamos o contador porque um
                evento pode ser coalescido quando o jogo
                publica frames mais rapido que o host.
                */

                CopyHookTextureToCpu();
            }

            continue;
        }

        if (
            backend ==
            CaptureBackend::Win32
        )
        {
            const auto now =
                std::chrono::
                    steady_clock::now();

            if (
                now >=
                nextWin32Frame
            )
            {
                CaptureWin32Frame(
                    hwnd
                );

                nextWin32Frame =
                    now +
                    std::chrono::
                        milliseconds(
                            33
                        );
            }

            std::this_thread::
                sleep_for(
                    std::chrono::
                        milliseconds(
                            2
                        )
                );

            continue;
        }

        std::this_thread::
            sleep_for(
                std::chrono::
                    milliseconds(
                        50
                    )
            );
    }

    g_monitorRunning.store(
        false
    );
}


// ======================================================
// PARAR CAPTURA COMPLETA
// ======================================================

static void StopVideoCaptureInternal()
{
    g_stopRequested.store(
        true
    );

    /*
    Primeiro marca a captura como inativa para impedir
    que callbacks atrasados publiquem novos frames.
    */

    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        g_videoActive =
            false;
    }

    if (
        g_captureMonitorThread.
            joinable()
    )
    {
        if (
            g_captureMonitorThread.
                get_id() !=
            std::this_thread::
                get_id()
        )
        {
            g_captureMonitorThread.
                join();
        }
    }

    StopWgcBackend();

    StopHookBackend();

    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        g_videoActive =
            false;

        g_videoHwnd =
            nullptr;

        g_videoWidth =
            0;

        g_videoHeight =
            0;

        g_videoFrameNumber =
            0;

        g_latestFrame.clear();

        g_captureBackend =
            CaptureBackend::None;

        g_videoStage =
            "CAPTURA PARADA";

        g_videoLastHRESULT =
            S_OK;
    }
}


// ======================================================
// INICIAR CAPTURE ENGINE
// ======================================================

static HRESULT StartVideoCaptureInternal(
    HWND hwnd
)
{
    StopVideoCaptureInternal();

    if (
        !hwnd ||
        !IsWindow(hwnd)
    )
    {
        SetVideoStatus(
            "HWND invalido",
            E_INVALIDARG
        );

        return E_INVALIDARG;
    }

    g_stopRequested.store(
        false
    );

    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        g_videoHwnd =
            hwnd;

        g_videoWidth =
            0;

        g_videoHeight =
            0;

        g_videoFrameNumber =
            0;

        g_latestFrame.clear();

        g_videoActive =
            true;

        g_captureBackend =
            CaptureBackend::None;

        g_videoStage =
            "INICIANDO CAPTURE ENGINE";

        g_videoLastHRESULT =
            S_OK;
    }

    /*
    Se a janela ja estiver minimizada no momento em que
    a transmissao comeca, nao tentamos inicializar WGC.
    Entramos diretamente no fallback.
    */

    if (IsIconic(hwnd))
    {
        if (!SwitchToHookBackend(
            hwnd
        ))
        {
            std::lock_guard<std::mutex>
                lock(g_videoMutex);

            g_captureBackend =
                CaptureBackend::Win32;

            g_videoStage =
                "HOOK FALHOU - USANDO WIN32";
        }

        g_captureMonitorThread =
            std::thread(
                CaptureMonitorLoop
            );

        return S_OK;
    }

    /*
    Caminho principal: WGC.
    */

    HRESULT hr =
        StartWgcBackend(
            hwnd
        );

    if (SUCCEEDED(hr))
    {
        {
            std::lock_guard<std::mutex>
                lock(g_videoMutex);

            g_captureBackend =
                CaptureBackend::WGC;

            g_videoStage =
                "CAPTURA INICIADA - WGC";

            g_videoLastHRESULT =
                S_OK;
        }

        g_captureMonitorThread =
            std::thread(
                CaptureMonitorLoop
            );

        return S_OK;
    }

    /*
    WGC nao iniciou. Antes do ultimo fallback Win32,
    tentamos o backend HOOK.
    */

    if (!SwitchToHookBackend(
        hwnd
    ))
    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        g_captureBackend =
            CaptureBackend::Win32;

        g_videoStage =
            "WGC/HOOK FALHARAM - USANDO WIN32";

        g_videoLastHRESULT =
            hr;
    }

    g_captureMonitorThread =
        std::thread(
            CaptureMonitorLoop
        );

    return S_OK;
}


// ======================================================
// JS - SUPORTE
// ======================================================

static Napi::Value
IsVideoCaptureSupported(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    /*
    O Capture Engine agora possui backend Win32.
    Portanto existe suporte mesmo quando WGC nao estiver
    disponivel.
    */

    return Napi::Boolean::New(
        env,
        true
    );
}


// ======================================================
// JS - START
// ======================================================

static Napi::Value
StartWindowCapture(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    if (
        info.Length() < 1 ||
        !info[0].IsString()
    )
    {
        Napi::TypeError::New(
            env,
            "HWND precisa ser uma string."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    const std::string hwndString =
        info[0]
            .As<Napi::String>()
            .Utf8Value();

    HWND hwnd =
        StringToHwnd(
            hwndString
        );

    if (
        !hwnd ||
        !IsWindow(hwnd)
    )
    {
        Napi::Error::New(
            env,
            "HWND invalido."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    const HRESULT hr =
        StartVideoCaptureInternal(
            hwnd
        );

    if (FAILED(hr))
    {
        std::string stage;

        {
            std::lock_guard<std::mutex>
                lock(g_videoMutex);

            stage =
                g_videoStage;
        }

        Napi::Error::New(
            env,

            "Captura de video falhou | Etapa: " +
            stage +
            " | HRESULT: " +
            HResultToHex(hr)

        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    Napi::Object result =
        Napi::Object::New(env);

    result.Set(
        "success",
        true
    );

    result.Set(
        "active",
        true
    );

    result.Set(
        "hwnd",
        hwndString
    );

    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        result.Set(
            "backend",
            BackendName(
                g_captureBackend
            )
        );
    }

    return result;
}


// ======================================================
// JS - LER FRAME
// ======================================================

static Napi::Value
ReadVideoFrame(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    std::vector<uint8_t>
        frame;

    int width = 0;

    int height = 0;

    uint64_t frameNumber = 0;

    bool active = false;

    std::string backend;

    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        frame =
            g_latestFrame;

        width =
            g_videoWidth;

        height =
            g_videoHeight;

        frameNumber =
            g_videoFrameNumber;

        active =
            g_videoActive;

        backend =
            BackendName(
                g_captureBackend
            );
    }

    Napi::Object result =
        Napi::Object::New(env);

    result.Set(
        "active",
        active
    );

    result.Set(
        "hasFrame",
        !frame.empty()
    );

    result.Set(
        "width",
        width
    );

    result.Set(
        "height",
        height
    );

    result.Set(
        "frameNumber",
        static_cast<double>(
            frameNumber
        )
    );

    result.Set(
        "backend",
        backend
    );

    if (frame.empty())
    {
        result.Set(
            "data",
            Napi::Buffer<
                uint8_t
            >::New(
                env,
                0
            )
        );
    }
    else
    {
        result.Set(
            "data",
            Napi::Buffer<
                uint8_t
            >::Copy(
                env,
                frame.data(),
                frame.size()
            )
        );
    }

    return result;
}


// ======================================================
// JS - STOP
// ======================================================

static Napi::Value
StopWindowCapture(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    StopVideoCaptureInternal();

    Napi::Object result =
        Napi::Object::New(env);

    result.Set(
        "success",
        true
    );

    result.Set(
        "active",
        false
    );

    return result;
}


// ======================================================
// JS - STATUS
// ======================================================

static Napi::Value
GetVideoCaptureStatus(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    bool active = false;

    HWND hwnd = nullptr;

    int width = 0;

    int height = 0;

    uint64_t frameNumber = 0;

    size_t frameBytes = 0;

    std::string stage;

    std::string backend;

    HRESULT hr = S_OK;

    bool minimized = false;

    bool occluded = false;

    {
        std::lock_guard<std::mutex>
            lock(g_videoMutex);

        active =
            g_videoActive;

        hwnd =
            g_videoHwnd;

        width =
            g_videoWidth;

        height =
            g_videoHeight;

        frameNumber =
            g_videoFrameNumber;

        frameBytes =
            g_latestFrame.size();

        stage =
            g_videoStage;

        backend =
            BackendName(
                g_captureBackend
            );

        hr =
            g_videoLastHRESULT;
    }

    if (
        hwnd &&
        IsWindow(hwnd)
    )
    {
        minimized =
            IsIconic(hwnd) != FALSE;

        occluded =
            IsWindowFullyOccluded(
                hwnd
            );
    }

    Napi::Object result =
        Napi::Object::New(env);

    result.Set(
        "active",
        active
    );

    result.Set(
        "width",
        width
    );

    result.Set(
        "height",
        height
    );

    result.Set(
        "frameNumber",
        static_cast<double>(
            frameNumber
        )
    );

    result.Set(
        "frameBytes",
        static_cast<double>(
            frameBytes
        )
    );

    result.Set(
        "stage",
        stage
    );

    result.Set(
        "backend",
        backend
    );

    result.Set(
        "minimized",
        minimized
    );

    result.Set(
        "occluded",
        occluded
    );

    result.Set(
        "hresult",
        HResultToHex(hr)
    );

    result.Set(
        "monitorRunning",
        g_monitorRunning.load()
    );

    if (hwnd)
    {
        const unsigned long long
            hwndValue =
                static_cast<
                    unsigned long long
                >(
                    reinterpret_cast<
                        uintptr_t
                    >(
                        hwnd
                    )
                );

        result.Set(
            "hwnd",
            std::to_string(
                hwndValue
            )
        );
    }
    else
    {
        result.Set(
            "hwnd",
            env.Null()
        );
    }

    return result;
}


// ======================================================
// KSF - COMPATIBILIDADE COM OCLUSAO DO CHROMIUM
// ======================================================

static Napi::Value EnableCaptureCompatibility(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    if (
        info.Length() < 1 ||
        !info[0].IsString()
    )
    {
        Napi::TypeError::New(
            env,
            "HWND precisa ser uma string."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    const std::string hwndString =
        info[0]
            .As<Napi::String>()
            .Utf8Value();

    HWND hwnd =
        StringToHwnd(
            hwndString
        );

    if (
        !hwnd ||
        !IsWindow(hwnd)
    )
    {
        Napi::Error::New(
            env,
            "HWND invalido para compatibilidade de captura."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    RECT windowRect{};

    if (!GetWindowRect(
        hwnd,
        &windowRect
    ))
    {
        Napi::Error::New(
            env,
            "Nao foi possivel obter o tamanho da janela KSF."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    const int width =
        windowRect.right -
        windowRect.left;

    const int height =
        windowRect.bottom -
        windowRect.top;

    if (
        width < 4 ||
        height < 4
    )
    {
        Napi::Error::New(
            env,
            "Janela KSF pequena demais para aplicar compatibilidade."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    /*
    A regiao cobre a janela inteira, exceto um unico pixel
    no canto inferior direito. Visualmente o KSF permanece
    opaco. A diferenca faz o Windows classificar a regiao
    como COMPLEXREGION em vez de SIMPLEREGION.
    */

    HRGN fullRegion =
        CreateRectRgn(
            0,
            0,
            width,
            height
        );

    HRGN onePixelHole =
        CreateRectRgn(
            width - 2,
            height - 2,
            width - 1,
            height - 1
        );

    if (
        !fullRegion ||
        !onePixelHole
    )
    {
        if (fullRegion)
        {
            DeleteObject(fullRegion);
        }

        if (onePixelHole)
        {
            DeleteObject(onePixelHole);
        }

        Napi::Error::New(
            env,
            "Nao foi possivel criar a regiao da janela KSF."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    const int combineResult =
        CombineRgn(
            fullRegion,
            fullRegion,
            onePixelHole,
            RGN_DIFF
        );

    DeleteObject(onePixelHole);

    if (
        combineResult == ERROR ||
        combineResult != COMPLEXREGION
    )
    {
        DeleteObject(fullRegion);

        Napi::Error::New(
            env,
            "A regiao KSF nao ficou complexa."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    /*
    Em caso de sucesso, o Windows passa a ser dono do HRGN.
    Portanto nao devemos chamar DeleteObject(fullRegion).
    */

    if (
        SetWindowRgn(
            hwnd,
            fullRegion,
            TRUE
        ) == 0
    )
    {
        DeleteObject(fullRegion);

        Napi::Error::New(
            env,
            "SetWindowRgn falhou ao ativar compatibilidade."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    /*
    A regiao personalizada pode fazer o Windows deixar
    de redesenhar imediatamente a moldura nao-cliente.
    Forcamos a recalculacao da moldura e o redesenho
    completo para preservar minimizar, maximizar e fechar.
    A COMPLEXREGION continua aplicada.
    */

    SetWindowPos(
        hwnd,
        nullptr,
        0,
        0,
        0,
        0,
        SWP_NOMOVE |
        SWP_NOSIZE |
        SWP_NOZORDER |
        SWP_NOACTIVATE |
        SWP_FRAMECHANGED
    );

    RedrawWindow(
        hwnd,
        nullptr,
        nullptr,
        RDW_INVALIDATE |
        RDW_FRAME |
        RDW_ALLCHILDREN |
        RDW_UPDATENOW
    );

    Napi::Object result =
        Napi::Object::New(env);

    result.Set(
        "success",
        true
    );

    result.Set(
        "active",
        true
    );

    result.Set(
        "regionType",
        "COMPLEXREGION"
    );

    result.Set(
        "width",
        width
    );

    result.Set(
        "height",
        height
    );

    return result;
}


static Napi::Value DisableCaptureCompatibility(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();

    if (
        info.Length() < 1 ||
        !info[0].IsString()
    )
    {
        Napi::TypeError::New(
            env,
            "HWND precisa ser uma string."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    const std::string hwndString =
        info[0]
            .As<Napi::String>()
            .Utf8Value();

    HWND hwnd =
        StringToHwnd(
            hwndString
        );

    if (
        !hwnd ||
        !IsWindow(hwnd)
    )
    {
        Napi::Error::New(
            env,
            "HWND invalido para remover compatibilidade."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    if (
        SetWindowRgn(
            hwnd,
            nullptr,
            TRUE
        ) == 0
    )
    {
        Napi::Error::New(
            env,
            "SetWindowRgn falhou ao remover compatibilidade."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    Napi::Object result =
        Napi::Object::New(env);

    result.Set(
        "success",
        true
    );

    result.Set(
        "active",
        false
    );

    return result;
}


// ======================================================
// EXPORTS
// ======================================================

static Napi::Object Init(
    Napi::Env env,
    Napi::Object exports
)
{
    exports.Set(
        "isVideoCaptureSupported",
        Napi::Function::New(
            env,
            IsVideoCaptureSupported
        )
    );

    exports.Set(
        "listCapturableWindows",
        Napi::Function::New(
            env,
            ListCapturableWindows
        )
    );

    exports.Set(
        "startWindowCapture",
        Napi::Function::New(
            env,
            StartWindowCapture
        )
    );

    exports.Set(
        "readVideoFrame",
        Napi::Function::New(
            env,
            ReadVideoFrame
        )
    );

    exports.Set(
        "stopWindowCapture",
        Napi::Function::New(
            env,
            StopWindowCapture
        )
    );

    exports.Set(
        "getVideoCaptureStatus",
        Napi::Function::New(
            env,
            GetVideoCaptureStatus
        )
    );

    exports.Set(
        "enableCaptureCompatibility",
        Napi::Function::New(
            env,
            EnableCaptureCompatibility
        )
    );

    exports.Set(
        "disableCaptureCompatibility",
        Napi::Function::New(
            env,
            DisableCaptureCompatibility
        )
    );

    return exports;
}


NODE_API_MODULE(
    ksf_video,
    Init
)