#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cwchar>

// ======================================================
// KSF SCREEN - GRAPHICS HOOK IPC
// ======================================================
//
// Estruturas compartilhadas entre:
//
// 1. KSF Screen
// 2. KSF Injector
// 3. KSF Graphics Hook DLL
//
// Esta primeira versao prepara a comunicacao para
// captura DXGI / Direct3D 11 usando textura compartilhada.
//
// IMPORTANTE:
//
// - Nenhum ponteiro e compartilhado entre processos.
// - Handles graficos sao transportados como uint64_t.
// - O protocolo possui versao propria.
// - Um hook atende somente uma transmissao por processo.
//
// ======================================================

namespace ksf_hook
{

// ======================================================
// VERSAO DO PROTOCOLO
// ======================================================

constexpr std::uint32_t IPC_MAGIC =
    0x4B534648; // "KSFH"

constexpr std::uint32_t IPC_VERSION =
    1;

// ======================================================
// LIMITES
// ======================================================

constexpr std::size_t NAME_CAPACITY =
    128;

// ======================================================
// ESTADO DO HOOK
// ======================================================

enum class HookState : std::uint32_t
{
    None = 0,

    Starting = 1,

    WaitingForGraphics = 2,

    Ready = 3,

    Capturing = 4,

    Stopping = 5,

    Stopped = 6,

    Error = 7
};

// ======================================================
// API GRAFICA DETECTADA
// ======================================================

enum class GraphicsApi : std::uint32_t
{
    Unknown = 0,

    Direct3D11 = 1,

    Direct3D12 = 2,

    OpenGL = 3,

    Vulkan = 4
};

// ======================================================
// FORMATO DA TEXTURA
// ======================================================

enum class TextureFormat : std::uint32_t
{
    Unknown = 0,

    BGRA8 = 1,

    RGBA8 = 2
};

// ======================================================
// COMANDOS KSF -> HOOK
// ======================================================

enum class HookCommand : std::uint32_t
{
    None = 0,

    StartCapture = 1,

    StopCapture = 2,

    Shutdown = 3
};

// ======================================================
// INFORMACOES DO FRAME COMPARTILHADO
// ======================================================
//
// sharedTextureHandle:
//
// Handle de uma textura D3D compartilhavel.
//
// O processo do KSF abre essa textura usando o
// dispositivo D3D dele.
//
// O handle nao deve ser interpretado como ponteiro.
//
// ======================================================

struct SharedFrameInfo
{
    std::uint32_t width;

    std::uint32_t height;

    std::uint32_t format;

    std::uint32_t reserved;

    std::uint64_t sharedTextureHandle;

    std::uint64_t frameNumber;

    std::uint64_t timestampQpc;
};

// ======================================================
// BLOCO PRINCIPAL DE MEMORIA COMPARTILHADA
// ======================================================

struct SharedState
{
    // --------------------------------------------------
    // Identificacao
    // --------------------------------------------------

    std::uint32_t magic;

    std::uint32_t version;

    // --------------------------------------------------
    // Processos
    // --------------------------------------------------

    std::uint32_t hostProcessId;

    std::uint32_t targetProcessId;

    // --------------------------------------------------
    // Estado
    // --------------------------------------------------

    volatile LONG hookState;

    volatile LONG graphicsApi;

    volatile LONG command;

    volatile LONG lastError;

    // --------------------------------------------------
    // Frame atual
    // --------------------------------------------------

    SharedFrameInfo frame;

    // --------------------------------------------------
    // Contadores
    // --------------------------------------------------

    volatile LONG64 heartbeat;

    volatile LONG64 publishedFrames;

    volatile LONG64 droppedFrames;

    // --------------------------------------------------
    // Reservado para futuras versoes
    // --------------------------------------------------

    std::uint64_t reserved[16];
};

// ======================================================
// TAMANHO ESPERADO
// ======================================================
//
// Mantemos a estrutura POD e com tamanho previsivel
// para ser usada diretamente em memoria mapeada.
//
// ======================================================

static_assert(
    sizeof(SharedFrameInfo) == 40,
    "SharedFrameInfo possui tamanho inesperado."
);

// ======================================================
// NOMES DOS OBJETOS IPC
// ======================================================
//
// Cada transmissao usa o PID alvo no nome.
// Assim podemos ter mais de um aplicativo capturado.
//
// Exemplo:
//
// Local\KSFHook_1234_Shared
// Local\KSFHook_1234_Ready
//
// ======================================================

inline void BuildObjectName(
    wchar_t* destination,
    std::size_t destinationCount,
    DWORD processId,
    const wchar_t* suffix
)
{
    if (
        !destination ||
        destinationCount == 0 ||
        !suffix
    )
    {
        return;
    }

    std::swprintf(
        destination,
        destinationCount,
        L"Local\\KSFHook_%lu_%ls",
        static_cast<unsigned long>(
            processId
        ),
        suffix
    );
}

// ======================================================
// MEMORIA COMPARTILHADA
// ======================================================

inline void BuildSharedMemoryName(
    wchar_t* destination,
    std::size_t destinationCount,
    DWORD processId
)
{
    BuildObjectName(
        destination,
        destinationCount,
        processId,
        L"Shared"
    );
}

// ======================================================
// EVENTO READY
// ======================================================

inline void BuildReadyEventName(
    wchar_t* destination,
    std::size_t destinationCount,
    DWORD processId
)
{
    BuildObjectName(
        destination,
        destinationCount,
        processId,
        L"Ready"
    );
}

// ======================================================
// EVENTO FRAME
// ======================================================

inline void BuildFrameEventName(
    wchar_t* destination,
    std::size_t destinationCount,
    DWORD processId
)
{
    BuildObjectName(
        destination,
        destinationCount,
        processId,
        L"Frame"
    );
}

// ======================================================
// EVENTO STOP
// ======================================================

inline void BuildStopEventName(
    wchar_t* destination,
    std::size_t destinationCount,
    DWORD processId
)
{
    BuildObjectName(
        destination,
        destinationCount,
        processId,
        L"Stop"
    );
}

// ======================================================
// MUTEX DA TEXTURA
// ======================================================

inline void BuildTextureMutexName(
    wchar_t* destination,
    std::size_t destinationCount,
    DWORD processId
)
{
    BuildObjectName(
        destination,
        destinationCount,
        processId,
        L"Texture"
    );
}

// ======================================================
// VALIDAR MEMORIA COMPARTILHADA
// ======================================================

inline bool IsValidSharedState(
    const SharedState* state
)
{
    if (!state)
    {
        return false;
    }

    return
        state->magic == IPC_MAGIC &&
        state->version == IPC_VERSION;
}

// ======================================================
// INICIALIZAR MEMORIA COMPARTILHADA
// ======================================================

inline void InitializeSharedState(
    SharedState* state,
    DWORD hostProcessId,
    DWORD targetProcessId
)
{
    if (!state)
    {
        return;
    }

    ZeroMemory(
        state,
        sizeof(SharedState)
    );

    state->magic =
        IPC_MAGIC;

    state->version =
        IPC_VERSION;

    state->hostProcessId =
        hostProcessId;

    state->targetProcessId =
        targetProcessId;

    state->hookState =
        static_cast<LONG>(
            HookState::Starting
        );

    state->graphicsApi =
        static_cast<LONG>(
            GraphicsApi::Unknown
        );

    state->command =
        static_cast<LONG>(
            HookCommand::None
        );

    state->lastError =
        ERROR_SUCCESS;

    state->frame.width =
        0;

    state->frame.height =
        0;

    state->frame.format =
        static_cast<std::uint32_t>(
            TextureFormat::Unknown
        );

    state->frame.reserved =
        0;

    state->frame.sharedTextureHandle =
        0;

    state->frame.frameNumber =
        0;

    state->frame.timestampQpc =
        0;

    state->heartbeat =
        0;

    state->publishedFrames =
        0;

    state->droppedFrames =
        0;
}

} // namespace ksf_hook