#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "ksf_hook_ipc.h"
#include "MinHook.h"

// ======================================================
// KSF SCREEN - DXGI / D3D11 GRAPHICS HOOK
// ======================================================
//
// Backend grafico executado dentro do processo alvo.
//
// Fluxo:
//
// Aplicativo
//    ↓
// IDXGISwapChain::Present
//    ↓
// KSF Present Hook
//    ↓
// Backbuffer D3D11
//    ↓
// Textura D3D11 compartilhada
//    ↓
// KSF Screen
//
// O hook NAO codifica video.
// Ele apenas disponibiliza o frame renderizado.
//
// ======================================================

namespace
{

// ======================================================
// TIPOS
// ======================================================

using PresentFunction =
    HRESULT(
        STDMETHODCALLTYPE*
    )(
        IDXGISwapChain* swapChain,
        UINT syncInterval,
        UINT flags
    );

// ======================================================
// ESTADO
// ======================================================

std::atomic<bool> g_dxgiActive{
    false
};

std::atomic<bool> g_dxgiStopping{
    false
};

std::mutex g_captureMutex;

PresentFunction g_originalPresent =
    nullptr;

// ======================================================
// IPC
// ======================================================

HANDLE g_sharedMapping =
    nullptr;

ksf_hook::SharedState*
    g_sharedState =
        nullptr;

HANDLE g_frameEvent =
    nullptr;

// ======================================================
// D3D11
// ======================================================

ID3D11Device* g_device =
    nullptr;

ID3D11DeviceContext* g_context =
    nullptr;

ID3D11Texture2D* g_sharedTexture =
    nullptr;

HANDLE g_sharedTextureHandle =
    nullptr;

UINT g_textureWidth =
    0;

UINT g_textureHeight =
    0;

DXGI_FORMAT g_textureFormat =
    DXGI_FORMAT_UNKNOWN;

// ======================================================
// FRAME
// ======================================================

std::uint64_t g_frameNumber =
    0;

// ======================================================
// QPC
// ======================================================

std::uint64_t GetTimestampQpc()
{
    LARGE_INTEGER value{};

    QueryPerformanceCounter(
        &value
    );

    return static_cast<
        std::uint64_t
    >(
        value.QuadPart
    );
}

// ======================================================
// RELEASE COM
// ======================================================

template<typename T>
void SafeRelease(
    T*& object
)
{
    if (!object)
    {
        return;
    }

    object->Release();

    object =
        nullptr;
}

// ======================================================
// IPC - FECHAR
// ======================================================

void CloseDxgiIpc()
{
    if (g_sharedState)
    {
        UnmapViewOfFile(
            g_sharedState
        );

        g_sharedState =
            nullptr;
    }

    if (g_sharedMapping)
    {
        CloseHandle(
            g_sharedMapping
        );

        g_sharedMapping =
            nullptr;
    }

    if (g_frameEvent)
    {
        CloseHandle(
            g_frameEvent
        );

        g_frameEvent =
            nullptr;
    }
}

// ======================================================
// IPC - ABRIR
// ======================================================

bool OpenDxgiIpc()
{
    const DWORD processId =
        GetCurrentProcessId();

    wchar_t sharedName[
        ksf_hook::NAME_CAPACITY
    ] = {};

    wchar_t frameName[
        ksf_hook::NAME_CAPACITY
    ] = {};

    ksf_hook::BuildSharedMemoryName(
        sharedName,
        _countof(
            sharedName
        ),
        processId
    );

    ksf_hook::BuildFrameEventName(
        frameName,
        _countof(
            frameName
        ),
        processId
    );

    g_sharedMapping =
        OpenFileMappingW(
            FILE_MAP_READ |
            FILE_MAP_WRITE,
            FALSE,
            sharedName
        );

    if (!g_sharedMapping)
    {
        return false;
    }

    void* mapped =
        MapViewOfFile(
            g_sharedMapping,
            FILE_MAP_READ |
            FILE_MAP_WRITE,
            0,
            0,
            sizeof(
                ksf_hook::SharedState
            )
        );

    if (!mapped)
    {
        CloseDxgiIpc();

        return false;
    }

    g_sharedState =
        static_cast<
            ksf_hook::SharedState*
        >(
            mapped
        );

    if (
        !ksf_hook::IsValidSharedState(
            g_sharedState
        )
    )
    {
        CloseDxgiIpc();

        return false;
    }

    g_frameEvent =
        OpenEventW(
            EVENT_MODIFY_STATE |
            SYNCHRONIZE,
            FALSE,
            frameName
        );

    if (!g_frameEvent)
    {
        CloseDxgiIpc();

        return false;
    }

    return true;
}

// ======================================================
// PUBLICAR ERRO
// ======================================================

void PublishError(
    HRESULT error
)
{
    if (!g_sharedState)
    {
        return;
    }

    InterlockedExchange(
        &g_sharedState->lastError,
        static_cast<LONG>(
            error
        )
    );
}

// ======================================================
// LIMPAR TEXTURA
// ======================================================

void DestroySharedTexture()
{
    g_sharedTextureHandle =
        nullptr;

    SafeRelease(
        g_sharedTexture
    );

    g_textureWidth =
        0;

    g_textureHeight =
        0;

    g_textureFormat =
        DXGI_FORMAT_UNKNOWN;

    if (g_sharedState)
    {
        g_sharedState->
            frame.
            sharedTextureHandle =
                0;

        g_sharedState->
            frame.
            width =
                0;

        g_sharedState->
            frame.
            height =
                0;

        g_sharedState->
            frame.
            format =
                static_cast<
                    std::uint32_t
                >(
                    ksf_hook::
                        TextureFormat::
                        Unknown
                );
    }
}

// ======================================================
// LIMPAR D3D
// ======================================================

void DestroyD3D()
{
    DestroySharedTexture();

    SafeRelease(
        g_context
    );

    SafeRelease(
        g_device
    );
}

// ======================================================
// MAPEAR FORMATO
// ======================================================

ksf_hook::TextureFormat
MapTextureFormat(
    DXGI_FORMAT format
)
{
    switch (format)
    {
        case
            DXGI_FORMAT_B8G8R8A8_UNORM:

        case
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        {
            return
                ksf_hook::
                    TextureFormat::
                    BGRA8;
        }

        case
            DXGI_FORMAT_R8G8B8A8_UNORM:

        case
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        {
            return
                ksf_hook::
                    TextureFormat::
                    RGBA8;
        }

        default:
        {
            return
                ksf_hook::
                    TextureFormat::
                    Unknown;
        }
    }
}

// ======================================================
// CRIAR TEXTURA COMPARTILHADA
// ======================================================

bool CreateSharedTexture(
    ID3D11Texture2D* backBuffer
)
{
    if (
        !backBuffer ||
        !g_device
    )
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};

    backBuffer->GetDesc(
        &sourceDesc
    );

    if (
        sourceDesc.Width == 0 ||
        sourceDesc.Height == 0
    )
    {
        return false;
    }

    const auto mappedFormat =
        MapTextureFormat(
            sourceDesc.Format
        );

    if (
        mappedFormat ==
        ksf_hook::
            TextureFormat::
            Unknown
    )
    {
        return false;
    }

    const bool sameTexture =
        g_sharedTexture &&
        g_textureWidth ==
            sourceDesc.Width &&
        g_textureHeight ==
            sourceDesc.Height &&
        g_textureFormat ==
            sourceDesc.Format;

    if (sameTexture)
    {
        return true;
    }

    DestroySharedTexture();

    D3D11_TEXTURE2D_DESC desc{};

    desc.Width =
        sourceDesc.Width;

    desc.Height =
        sourceDesc.Height;

    desc.MipLevels =
        1;

    desc.ArraySize =
        1;

    desc.Format =
        sourceDesc.Format;

    desc.SampleDesc.Count =
        1;

    desc.SampleDesc.Quality =
        0;

    desc.Usage =
        D3D11_USAGE_DEFAULT;

    desc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE;

    desc.CPUAccessFlags =
        0;

    desc.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr =
        g_device->
            CreateTexture2D(
                &desc,
                nullptr,
                &g_sharedTexture
            );

    if (FAILED(hr))
    {
        PublishError(
            hr
        );

        DestroySharedTexture();

        return false;
    }

    IDXGIResource* dxgiResource =
        nullptr;

    hr =
        g_sharedTexture->
            QueryInterface(
                __uuidof(
                    IDXGIResource
                ),
                reinterpret_cast<
                    void**
                >(
                    &dxgiResource
                )
            );

    if (
        FAILED(hr) ||
        !dxgiResource
    )
    {
        PublishError(
            hr
        );

        DestroySharedTexture();

        return false;
    }

    HANDLE sharedHandle =
        nullptr;

    hr =
        dxgiResource->
            GetSharedHandle(
                &sharedHandle
            );

    dxgiResource->Release();

    if (
        FAILED(hr) ||
        !sharedHandle
    )
    {
        PublishError(
            hr
        );

        DestroySharedTexture();

        return false;
    }

    g_sharedTextureHandle =
        sharedHandle;

    g_textureWidth =
        sourceDesc.Width;

    g_textureHeight =
        sourceDesc.Height;

    g_textureFormat =
        sourceDesc.Format;

    if (g_sharedState)
    {
        g_sharedState->
            frame.
            width =
                sourceDesc.Width;

        g_sharedState->
            frame.
            height =
                sourceDesc.Height;

        g_sharedState->
            frame.
            format =
                static_cast<
                    std::uint32_t
                >(
                    mappedFormat
                );

        g_sharedState->
            frame.
            sharedTextureHandle =
                static_cast<
                    std::uint64_t
                >(
                    reinterpret_cast<
                        std::uintptr_t
                    >(
                        sharedHandle
                    )
                );
    }

    return true;
}

// ======================================================
// PREPARAR DEVICE
// ======================================================

bool EnsureDevice(
    IDXGISwapChain* swapChain
)
{
    if (!swapChain)
    {
        return false;
    }

    ID3D11Device* device =
        nullptr;

    HRESULT hr =
        swapChain->
            GetDevice(
                __uuidof(
                    ID3D11Device
                ),
                reinterpret_cast<
                    void**
                >(
                    &device
                )
            );

    if (
        FAILED(hr) ||
        !device
    )
    {
        return false;
    }

    if (
        g_device ==
        device
    )
    {
        device->Release();

        return true;
    }

    DestroyD3D();

    g_device =
        device;

    g_device->
        GetImmediateContext(
            &g_context
        );

    if (!g_context)
    {
        DestroyD3D();

        return false;
    }

    return true;
}

// ======================================================
// CAPTURAR FRAME
// ======================================================

void CaptureFrame(
    IDXGISwapChain* swapChain
)
{
    if (
        !g_dxgiActive.load() ||
        g_dxgiStopping.load() ||
        !swapChain
    )
    {
        return;
    }

    std::lock_guard<
        std::mutex
    > lock(
        g_captureMutex
    );

    if (
        !g_dxgiActive.load() ||
        g_dxgiStopping.load()
    )
    {
        return;
    }

    if (
        !EnsureDevice(
            swapChain
        )
    )
    {
        return;
    }

    ID3D11Texture2D* backBuffer =
        nullptr;

    HRESULT hr =
        swapChain->
            GetBuffer(
                0,
                __uuidof(
                    ID3D11Texture2D
                ),
                reinterpret_cast<
                    void**
                >(
                    &backBuffer
                )
            );

    if (
        FAILED(hr) ||
        !backBuffer
    )
    {
        return;
    }

    if (
        !CreateSharedTexture(
            backBuffer
        )
    )
    {
        backBuffer->Release();

        return;
    }

    g_context->
        CopyResource(
            g_sharedTexture,
            backBuffer
        );

    backBuffer->Release();

    g_context->Flush();

    ++g_frameNumber;

    if (g_sharedState)
    {
        g_sharedState->
            frame.
            frameNumber =
                g_frameNumber;

        g_sharedState->
            frame.
            timestampQpc =
                GetTimestampQpc();

        InterlockedIncrement64(
            &g_sharedState->
                publishedFrames
        );

        InterlockedExchange(
            &g_sharedState->
                graphicsApi,
            static_cast<LONG>(
                ksf_hook::
                    GraphicsApi::
                    Direct3D11
            )
        );

        InterlockedExchange(
            &g_sharedState->
                hookState,
            static_cast<LONG>(
                ksf_hook::
                    HookState::
                    Capturing
            )
        );
    }

    if (g_frameEvent)
    {
        SetEvent(
            g_frameEvent
        );
    }
}

// ======================================================
// PRESENT HOOK
// ======================================================

HRESULT STDMETHODCALLTYPE
HookPresent(
    IDXGISwapChain* swapChain,
    UINT syncInterval,
    UINT flags
)
{
    CaptureFrame(
        swapChain
    );

    if (!g_originalPresent)
    {
        return E_FAIL;
    }

    return g_originalPresent(
        swapChain,
        syncInterval,
        flags
    );
}

// ======================================================
// OBTER ENDERECO DO PRESENT
// ======================================================

void* FindPresentAddress()
{
    HWND window =
        CreateWindowExW(
            0,
            L"STATIC",
            L"KSF_DXGI_PROBE",
            WS_OVERLAPPED,
            0,
            0,
            2,
            2,
            nullptr,
            nullptr,
            GetModuleHandleW(
                nullptr
            ),
            nullptr
        );

    if (!window)
    {
        return nullptr;
    }

    DXGI_SWAP_CHAIN_DESC swapDesc{};

    swapDesc.BufferCount =
        2;

    swapDesc.BufferDesc.Width =
        2;

    swapDesc.BufferDesc.Height =
        2;

    swapDesc.BufferDesc.Format =
        DXGI_FORMAT_R8G8B8A8_UNORM;

    swapDesc.BufferUsage =
        DXGI_USAGE_RENDER_TARGET_OUTPUT;

    swapDesc.OutputWindow =
        window;

    swapDesc.SampleDesc.Count =
        1;

    swapDesc.Windowed =
        TRUE;

    swapDesc.SwapEffect =
        DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevels[] =
    {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    ID3D11Device* probeDevice =
        nullptr;

    ID3D11DeviceContext* probeContext =
        nullptr;

    IDXGISwapChain* probeSwapChain =
        nullptr;

    D3D_FEATURE_LEVEL selectedLevel{};

    HRESULT hr =
        D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            ARRAYSIZE(
                featureLevels
            ),
            D3D11_SDK_VERSION,
            &swapDesc,
            &probeSwapChain,
            &probeDevice,
            &selectedLevel,
            &probeContext
        );

    if (FAILED(hr))
    {
        hr =
            D3D11CreateDeviceAndSwapChain(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                0,
                featureLevels,
                ARRAYSIZE(
                    featureLevels
                ),
                D3D11_SDK_VERSION,
                &swapDesc,
                &probeSwapChain,
                &probeDevice,
                &selectedLevel,
                &probeContext
            );
    }

    void* presentAddress =
        nullptr;

    if (
        SUCCEEDED(hr) &&
        probeSwapChain
    )
    {
        void** vtable =
            *reinterpret_cast<
                void***
            >(
                probeSwapChain
            );

        /*
        IDXGISwapChain:

        0  QueryInterface
        1  AddRef
        2  Release
        3  SetPrivateData
        4  SetPrivateDataInterface
        5  GetPrivateData
        6  GetParent
        7  GetDevice
        8  Present
        */

        presentAddress =
            vtable[8];
    }

    SafeRelease(
        probeSwapChain
    );

    SafeRelease(
        probeContext
    );

    SafeRelease(
        probeDevice
    );

    DestroyWindow(
        window
    );

    return presentAddress;
}

} // namespace

// ======================================================
// INICIAR DXGI HOOK
// ======================================================

extern "C"
bool KsfStartDxgiHook()
{
    if (g_dxgiActive.load())
    {
        return true;
    }

    g_dxgiStopping.store(
        false
    );

    if (!OpenDxgiIpc())
    {
        return false;
    }

    const MH_STATUS initStatus =
        MH_Initialize();

    if (
        initStatus != MH_OK &&
        initStatus != MH_ERROR_ALREADY_INITIALIZED
    )
    {
        CloseDxgiIpc();

        return false;
    }

    void* presentAddress =
        FindPresentAddress();

    if (!presentAddress)
    {
        CloseDxgiIpc();

        return false;
    }

    const MH_STATUS createStatus =
        MH_CreateHook(
            presentAddress,
            reinterpret_cast<
                LPVOID
            >(
                &HookPresent
            ),
            reinterpret_cast<
                LPVOID*
            >(
                &g_originalPresent
            )
        );

    if (
        createStatus != MH_OK &&
        createStatus != MH_ERROR_ALREADY_CREATED
    )
    {
        CloseDxgiIpc();

        return false;
    }

    const MH_STATUS enableStatus =
        MH_EnableHook(
            presentAddress
        );

    if (
        enableStatus != MH_OK &&
        enableStatus != MH_ERROR_ENABLED
    )
    {
        MH_RemoveHook(
            presentAddress
        );

        CloseDxgiIpc();

        return false;
    }

    g_dxgiActive.store(
        true
    );

    if (g_sharedState)
    {
        InterlockedExchange(
            &g_sharedState->
                graphicsApi,
            static_cast<LONG>(
                ksf_hook::
                    GraphicsApi::
                    Direct3D11
            )
        );

        InterlockedExchange(
            &g_sharedState->
                hookState,
            static_cast<LONG>(
                ksf_hook::
                    HookState::
                    WaitingForGraphics
            )
        );
    }

    return true;
}

// ======================================================
// PARAR DXGI HOOK
// ======================================================

extern "C"
void KsfStopDxgiHook()
{
    if (
        !g_dxgiActive.load() &&
        !g_sharedState
    )
    {
        return;
    }

    g_dxgiStopping.store(
        true
    );

    g_dxgiActive.store(
        false
    );

    /*
    Como esta DLL possui apenas o hook DXGI do KSF,
    removemos os hooks instalados pelo MinHook nesta
    instancia antes de liberar os recursos graficos.
    */

    MH_DisableHook(
        MH_ALL_HOOKS
    );

    MH_RemoveHook(
        MH_ALL_HOOKS
    );

    {
        std::lock_guard<
            std::mutex
        > lock(
            g_captureMutex
        );

        DestroyD3D();
    }

    if (g_sharedState)
    {
        InterlockedExchange(
            &g_sharedState->
                graphicsApi,
            static_cast<LONG>(
                ksf_hook::
                    GraphicsApi::
                    Unknown
            )
        );

        InterlockedExchange(
            &g_sharedState->
                hookState,
            static_cast<LONG>(
                ksf_hook::
                    HookState::
                    Ready
            )
        );
    }

    CloseDxgiIpc();

    g_originalPresent =
        nullptr;

    g_frameNumber =
        0;

    g_dxgiStopping.store(
        false
    );
}