#include <windows.h>

#include <atomic>
#include <cstdint>

#include "ksf_hook_ipc.h"

// ======================================================
// KSF SCREEN - DXGI BACKEND
// ======================================================
//
// Implementado em ksf_hook_dxgi.cpp.
//
// Mantemos apenas a interface aqui para que o ciclo
// principal da DLL possa iniciar e parar o backend sem
// misturar a implementacao DXGI com o IPC.
//
// ======================================================

extern "C" bool KsfStartDxgiHook();

extern "C" void KsfStopDxgiHook();

// ======================================================
// KSF SCREEN - GRAPHICS HOOK
// ======================================================
//
// DLL carregada dentro do processo que sera capturado.
//
// Responsabilidades deste arquivo:
//
// 1. Inicializar o IPC.
// 2. Abrir a memoria compartilhada criada pelo KSF.
// 3. Abrir os eventos de sincronizacao.
// 4. Informar que o hook esta vivo.
// 5. Manter heartbeat.
// 6. Receber comandos do processo principal.
// 7. Iniciar/parar o backend DXGI / D3D11.
// 8. Encerrar o hook de forma limpa.
//
// A interceptacao DXGI / D3D11 fica em:
// ksf_hook_dxgi.cpp
//
// ======================================================

namespace
{

// ======================================================
// ESTADO GLOBAL
// ======================================================

HMODULE g_module = nullptr;

DWORD g_processId = 0;

std::atomic<bool> g_running{
    false
};

std::atomic<bool> g_shutdownRequested{
    false
};

std::atomic<bool> g_dxgiStarted{
    false
};

HANDLE g_workerThread = nullptr;

// ======================================================
// IPC
// ======================================================

HANDLE g_sharedMapping = nullptr;

ksf_hook::SharedState*
    g_sharedState = nullptr;

HANDLE g_readyEvent = nullptr;

HANDLE g_frameEvent = nullptr;

HANDLE g_stopEvent = nullptr;

HANDLE g_textureMutex = nullptr;

// ======================================================
// NOMES DOS OBJETOS
// ======================================================

wchar_t g_sharedMemoryName[
    ksf_hook::NAME_CAPACITY
] = {};

wchar_t g_readyEventName[
    ksf_hook::NAME_CAPACITY
] = {};

wchar_t g_frameEventName[
    ksf_hook::NAME_CAPACITY
] = {};

wchar_t g_stopEventName[
    ksf_hook::NAME_CAPACITY
] = {};

wchar_t g_textureMutexName[
    ksf_hook::NAME_CAPACITY
] = {};

// ======================================================
// ERRO
// ======================================================

void SetHookError(
    DWORD error
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

    InterlockedExchange(
        &g_sharedState->hookState,
        static_cast<LONG>(
            ksf_hook::HookState::Error
        )
    );
}

// ======================================================
// LIMPAR ERRO
// ======================================================

void ClearHookError()
{
    if (!g_sharedState)
    {
        return;
    }

    InterlockedExchange(
        &g_sharedState->lastError,
        ERROR_SUCCESS
    );
}

// ======================================================
// ESTADO
// ======================================================

void SetHookState(
    ksf_hook::HookState state
)
{
    if (!g_sharedState)
    {
        return;
    }

    InterlockedExchange(
        &g_sharedState->hookState,
        static_cast<LONG>(
            state
        )
    );
}

// ======================================================
// API GRAFICA
// ======================================================

void SetGraphicsApi(
    ksf_hook::GraphicsApi api
)
{
    if (!g_sharedState)
    {
        return;
    }

    InterlockedExchange(
        &g_sharedState->graphicsApi,
        static_cast<LONG>(
            api
        )
    );
}

// ======================================================
// FECHAR HANDLE
// ======================================================

void CloseHandleSafe(
    HANDLE& handle
)
{
    if (!handle)
    {
        return;
    }

    CloseHandle(
        handle
    );

    handle = nullptr;
}

// ======================================================
// DESMAPEAR MEMORIA
// ======================================================

void CloseSharedMemory()
{
    if (g_sharedState)
    {
        UnmapViewOfFile(
            g_sharedState
        );

        g_sharedState = nullptr;
    }

    CloseHandleSafe(
        g_sharedMapping
    );
}

// ======================================================
// FECHAR IPC
// ======================================================

void CloseIpc()
{
    CloseHandleSafe(
        g_textureMutex
    );

    CloseHandleSafe(
        g_stopEvent
    );

    CloseHandleSafe(
        g_frameEvent
    );

    CloseHandleSafe(
        g_readyEvent
    );

    CloseSharedMemory();
}

// ======================================================
// GERAR NOMES IPC
// ======================================================

void BuildIpcNames()
{
    ksf_hook::BuildSharedMemoryName(
        g_sharedMemoryName,
        _countof(
            g_sharedMemoryName
        ),
        g_processId
    );

    ksf_hook::BuildReadyEventName(
        g_readyEventName,
        _countof(
            g_readyEventName
        ),
        g_processId
    );

    ksf_hook::BuildFrameEventName(
        g_frameEventName,
        _countof(
            g_frameEventName
        ),
        g_processId
    );

    ksf_hook::BuildStopEventName(
        g_stopEventName,
        _countof(
            g_stopEventName
        ),
        g_processId
    );

    ksf_hook::BuildTextureMutexName(
        g_textureMutexName,
        _countof(
            g_textureMutexName
        ),
        g_processId
    );
}

// ======================================================
// ABRIR MEMORIA COMPARTILHADA
// ======================================================

bool OpenSharedState()
{
    g_sharedMapping =
        OpenFileMappingW(
            FILE_MAP_READ |
            FILE_MAP_WRITE,
            FALSE,
            g_sharedMemoryName
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
        CloseHandleSafe(
            g_sharedMapping
        );

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
        CloseSharedMemory();

        SetLastError(
            ERROR_INVALID_DATA
        );

        return false;
    }

    if (
        g_sharedState->targetProcessId !=
        g_processId
    )
    {
        CloseSharedMemory();

        SetLastError(
            ERROR_INVALID_PARAMETER
        );

        return false;
    }

    return true;
}

// ======================================================
// ABRIR EVENTOS
// ======================================================

bool OpenIpcEvents()
{
    g_readyEvent =
        OpenEventW(
            EVENT_MODIFY_STATE |
            SYNCHRONIZE,
            FALSE,
            g_readyEventName
        );

    if (!g_readyEvent)
    {
        return false;
    }

    g_frameEvent =
        OpenEventW(
            EVENT_MODIFY_STATE |
            SYNCHRONIZE,
            FALSE,
            g_frameEventName
        );

    if (!g_frameEvent)
    {
        return false;
    }

    g_stopEvent =
        OpenEventW(
            EVENT_MODIFY_STATE |
            SYNCHRONIZE,
            FALSE,
            g_stopEventName
        );

    if (!g_stopEvent)
    {
        return false;
    }

    g_textureMutex =
        OpenMutexW(
            SYNCHRONIZE,
            FALSE,
            g_textureMutexName
        );

    if (!g_textureMutex)
    {
        return false;
    }

    return true;
}

// ======================================================
// ABRIR IPC
// ======================================================

bool OpenIpc()
{
    BuildIpcNames();

    if (!OpenSharedState())
    {
        return false;
    }

    if (!OpenIpcEvents())
    {
        const DWORD error =
            GetLastError();

        SetHookError(
            error
        );

        CloseIpc();

        SetLastError(
            error
        );

        return false;
    }

    return true;
}

// ======================================================
// HEARTBEAT
// ======================================================

void UpdateHeartbeat()
{
    if (!g_sharedState)
    {
        return;
    }

    InterlockedIncrement64(
        &g_sharedState->heartbeat
    );
}

// ======================================================
// LER COMANDO
// ======================================================

ksf_hook::HookCommand ReadCommand()
{
    if (!g_sharedState)
    {
        return
            ksf_hook::HookCommand::None;
    }

    const LONG command =
        InterlockedCompareExchange(
            &g_sharedState->command,
            0,
            0
        );

    return static_cast<
        ksf_hook::HookCommand
    >(
        command
    );
}

// ======================================================
// LIMPAR COMANDO
// ======================================================

void ClearCommand()
{
    if (!g_sharedState)
    {
        return;
    }

    InterlockedExchange(
        &g_sharedState->command,
        static_cast<LONG>(
            ksf_hook::HookCommand::None
        )
    );
}

// ======================================================
// INICIAR BACKEND DXGI
// ======================================================

bool StartDxgiBackend()
{
    if (g_dxgiStarted.load())
    {
        return true;
    }

    ClearHookError();

    SetHookState(
        ksf_hook::HookState::
            WaitingForGraphics
    );

    const bool started =
        KsfStartDxgiHook();

    if (!started)
    {
        DWORD error =
            GetLastError();

        if (error == ERROR_SUCCESS)
        {
            error =
                ERROR_GEN_FAILURE;
        }

        SetHookError(
            error
        );

        g_dxgiStarted.store(
            false
        );

        return false;
    }

    g_dxgiStarted.store(
        true
    );

    return true;
}

// ======================================================
// PARAR BACKEND DXGI
// ======================================================

void StopDxgiBackend()
{
    if (!g_dxgiStarted.load())
    {
        return;
    }

    KsfStopDxgiHook();

    g_dxgiStarted.store(
        false
    );

    SetGraphicsApi(
        ksf_hook::GraphicsApi::
            Unknown
    );
}

// ======================================================
// PROCESSAR COMANDOS
// ======================================================

void ProcessCommand()
{
    const auto command =
        ReadCommand();

    switch (command)
    {
        case
            ksf_hook::HookCommand::
                StartCapture:
        {
            ClearCommand();

            StartDxgiBackend();

            break;
        }

        case
            ksf_hook::HookCommand::
                StopCapture:
        {
            ClearCommand();

            StopDxgiBackend();

            ClearHookError();

            SetGraphicsApi(
                ksf_hook::GraphicsApi::
                    Unknown
            );

            SetHookState(
                ksf_hook::HookState::
                    Ready
            );

            break;
        }

        case
            ksf_hook::HookCommand::
                Shutdown:
        {
            ClearCommand();

            StopDxgiBackend();

            g_shutdownRequested.store(
                true
            );

            break;
        }

        case
            ksf_hook::HookCommand::
                None:

        default:
        {
            break;
        }
    }
}

// ======================================================
// VERIFICAR EVENTO DE STOP
// ======================================================

bool StopEventRequested()
{
    if (!g_stopEvent)
    {
        return false;
    }

    const DWORD result =
        WaitForSingleObject(
            g_stopEvent,
            0
        );

    return
        result == WAIT_OBJECT_0;
}

// ======================================================
// LOOP PRINCIPAL
// ======================================================

DWORD WINAPI HookWorkerThread(
    LPVOID
)
{
    g_running.store(
        true
    );

    g_shutdownRequested.store(
        false
    );

    g_dxgiStarted.store(
        false
    );

    g_processId =
        GetCurrentProcessId();

    if (!OpenIpc())
    {
        g_running.store(
            false
        );

        return 1;
    }

    ClearHookError();

    SetGraphicsApi(
        ksf_hook::GraphicsApi::
            Unknown
    );

    SetHookState(
        ksf_hook::HookState::
            Ready
    );

    SetEvent(
        g_readyEvent
    );

    while (
        !g_shutdownRequested.load()
    )
    {
        UpdateHeartbeat();

        if (StopEventRequested())
        {
            g_shutdownRequested.store(
                true
            );

            break;
        }

        ProcessCommand();

        Sleep(
            100
        );
    }

    SetHookState(
        ksf_hook::HookState::
            Stopping
    );

    StopDxgiBackend();

    SetGraphicsApi(
        ksf_hook::GraphicsApi::
            Unknown
    );

    SetHookState(
        ksf_hook::HookState::
            Stopped
    );

    CloseIpc();

    g_running.store(
        false
    );

    return 0;
}

// ======================================================
// INICIAR WORKER
// ======================================================

bool StartWorker()
{
    if (g_workerThread)
    {
        return true;
    }

    DWORD threadId = 0;

    g_workerThread =
        CreateThread(
            nullptr,
            0,
            HookWorkerThread,
            nullptr,
            0,
            &threadId
        );

    return
        g_workerThread != nullptr;
}

// ======================================================
// FECHAR HANDLE DA THREAD
// ======================================================

void CloseWorkerHandle()
{
    if (!g_workerThread)
    {
        return;
    }

    CloseHandle(
        g_workerThread
    );

    g_workerThread = nullptr;
}

} // namespace

// ======================================================
// DLL MAIN
// ======================================================

BOOL APIENTRY DllMain(
    HMODULE moduleHandle,
    DWORD reason,
    LPVOID
)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
        {
            g_module =
                moduleHandle;

            DisableThreadLibraryCalls(
                moduleHandle
            );

            /*
            DllMain deve permanecer minimo.

            A inicializacao real acontece na worker
            separada para evitar executar o trabalho
            pesado sob o loader lock.
            */

            if (!StartWorker())
            {
                return FALSE;
            }

            break;
        }

        case DLL_PROCESS_DETACH:
        {
            /*
            Nao executamos KsfStopDxgiHook aqui.

            DllMain roda sob o loader lock. A limpeza
            normal do DXGI acontece pela worker antes
            do unload da DLL ou pelo comando Shutdown.
            */

            g_shutdownRequested.store(
                true
            );

            CloseWorkerHandle();

            g_module =
                nullptr;

            break;
        }

        default:
        {
            break;
        }
    }

    return TRUE;
}