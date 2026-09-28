#include <windows.h>

#include <iostream>
#include <string>
#include <vector>
#include <cwchar>

// ======================================================
// KSF SCREEN - GRAPHICS HOOK INJECTOR
// ======================================================
//
// Responsabilidade:
//
// 1. Receber o PID do processo alvo.
// 2. Receber o caminho da ksf_hook.dll.
// 3. Validar o processo e a DLL.
// 4. Abrir o processo alvo.
// 5. Copiar o caminho da DLL para o processo.
// 6. Executar LoadLibraryW dentro do processo.
// 7. Aguardar o carregamento.
// 8. Liberar os recursos utilizados.
//
// Esta versao utiliza somente APIs normais do Windows.
//
// Nao possui:
// - ocultacao
// - bypass
// - elevacao automatica
// - manipulacao de anti-cheat
//
// ======================================================

namespace
{

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
// VERIFICAR SE ARQUIVO EXISTE
// ======================================================

bool FileExists(
    const std::wstring& path
)
{
    const DWORD attributes =
        GetFileAttributesW(
            path.c_str()
        );

    if (
        attributes ==
        INVALID_FILE_ATTRIBUTES
    )
    {
        return false;
    }

    return
        (
            attributes &
            FILE_ATTRIBUTE_DIRECTORY
        ) == 0;
}

// ======================================================
// OBTER CAMINHO ABSOLUTO
// ======================================================

bool GetAbsolutePath(
    const std::wstring& input,
    std::wstring& output
)
{
    const DWORD required =
        GetFullPathNameW(
            input.c_str(),
            0,
            nullptr,
            nullptr
        );

    if (required == 0)
    {
        return false;
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(
            required
        ) + 1
    );

    const DWORD result =
        GetFullPathNameW(
            input.c_str(),
            static_cast<DWORD>(
                buffer.size()
            ),
            buffer.data(),
            nullptr
        );

    if (
        result == 0 ||
        result >= buffer.size()
    )
    {
        return false;
    }

    output.assign(
        buffer.data(),
        result
    );

    return true;
}

// ======================================================
// VALIDAR PID
// ======================================================

bool ParseProcessId(
    const wchar_t* text,
    DWORD& processId
)
{
    if (
        !text ||
        text[0] == L'\0'
    )
    {
        return false;
    }

    wchar_t* end = nullptr;

    const unsigned long value =
        std::wcstoul(
            text,
            &end,
            10
        );

    if (
        end == text ||
        !end ||
        *end != L'\0' ||
        value == 0
    )
    {
        return false;
    }

    processId =
        static_cast<DWORD>(
            value
        );

    return true;
}

// ======================================================
// VERIFICAR SE PROCESSO EXISTE
// ======================================================

bool IsProcessAlive(
    DWORD processId
)
{
    HANDLE process =
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId
        );

    if (!process)
    {
        return false;
    }

    DWORD exitCode = 0;

    const BOOL success =
        GetExitCodeProcess(
            process,
            &exitCode
        );

    CloseHandle(
        process
    );

    if (!success)
    {
        return false;
    }

    return
        exitCode == STILL_ACTIVE;
}

// ======================================================
// IMPRIMIR ERRO WINDOWS
// ======================================================

void PrintWindowsError(
    const wchar_t* operation,
    DWORD error
)
{
    std::wcerr
        << L"[KSF Injector] "
        << operation
        << L" falhou. Erro Windows: "
        << error
        << std::endl;
}

// ======================================================
// INJETAR DLL
// ======================================================

bool InjectDll(
    DWORD processId,
    const std::wstring& dllPath
)
{
    HANDLE process = nullptr;

    HANDLE remoteThread = nullptr;

    LPVOID remoteMemory = nullptr;

    bool success = false;

    // --------------------------------------------------
    // ABRIR PROCESSO
    // --------------------------------------------------

    process =
        OpenProcess(
            PROCESS_CREATE_THREAD |
            PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE |
            PROCESS_VM_READ,
            FALSE,
            processId
        );

    if (!process)
    {
        PrintWindowsError(
            L"OpenProcess",
            GetLastError()
        );

        return false;
    }

    // --------------------------------------------------
    // TAMANHO DO CAMINHO
    // --------------------------------------------------

    const SIZE_T pathBytes =
        (
            dllPath.size() + 1
        ) *
        sizeof(wchar_t);

    // --------------------------------------------------
    // ALOCAR MEMORIA NO PROCESSO
    // --------------------------------------------------

    remoteMemory =
        VirtualAllocEx(
            process,
            nullptr,
            pathBytes,
            MEM_COMMIT |
            MEM_RESERVE,
            PAGE_READWRITE
        );

    if (!remoteMemory)
    {
        PrintWindowsError(
            L"VirtualAllocEx",
            GetLastError()
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    // --------------------------------------------------
    // ESCREVER CAMINHO DA DLL
    // --------------------------------------------------

    SIZE_T bytesWritten = 0;

    if (
        !WriteProcessMemory(
            process,
            remoteMemory,
            dllPath.c_str(),
            pathBytes,
            &bytesWritten
        ) ||
        bytesWritten != pathBytes
    )
    {
        PrintWindowsError(
            L"WriteProcessMemory",
            GetLastError()
        );

        VirtualFreeEx(
            process,
            remoteMemory,
            0,
            MEM_RELEASE
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    // --------------------------------------------------
    // LOCALIZAR KERNEL32
    // --------------------------------------------------

    HMODULE kernel32 =
        GetModuleHandleW(
            L"kernel32.dll"
        );

    if (!kernel32)
    {
        PrintWindowsError(
            L"GetModuleHandleW(kernel32)",
            GetLastError()
        );

        VirtualFreeEx(
            process,
            remoteMemory,
            0,
            MEM_RELEASE
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    // --------------------------------------------------
    // LOCALIZAR LOADLIBRARYW
    // --------------------------------------------------

    FARPROC loadLibraryAddress =
        GetProcAddress(
            kernel32,
            "LoadLibraryW"
        );

    if (!loadLibraryAddress)
    {
        PrintWindowsError(
            L"GetProcAddress(LoadLibraryW)",
            GetLastError()
        );

        VirtualFreeEx(
            process,
            remoteMemory,
            0,
            MEM_RELEASE
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    // --------------------------------------------------
    // CRIAR THREAD REMOTA
    // --------------------------------------------------

    remoteThread =
        CreateRemoteThread(
            process,
            nullptr,
            0,
            reinterpret_cast<
                LPTHREAD_START_ROUTINE
            >(
                loadLibraryAddress
            ),
            remoteMemory,
            0,
            nullptr
        );

    if (!remoteThread)
    {
        PrintWindowsError(
            L"CreateRemoteThread",
            GetLastError()
        );

        VirtualFreeEx(
            process,
            remoteMemory,
            0,
            MEM_RELEASE
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    // --------------------------------------------------
    // AGUARDAR LOADLIBRARY
    // --------------------------------------------------

    const DWORD waitResult =
        WaitForSingleObject(
            remoteThread,
            10000
        );

    if (waitResult != WAIT_OBJECT_0)
    {
        if (waitResult == WAIT_TIMEOUT)
        {
            std::wcerr
                << L"[KSF Injector] "
                << L"Timeout aguardando LoadLibraryW."
                << std::endl;
        }
        else
        {
            PrintWindowsError(
                L"WaitForSingleObject",
                GetLastError()
            );
        }

        CloseHandleSafe(
            remoteThread
        );

        VirtualFreeEx(
            process,
            remoteMemory,
            0,
            MEM_RELEASE
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    // --------------------------------------------------
    // RESULTADO DA THREAD
    // --------------------------------------------------

    DWORD remoteResult = 0;

    if (
        !GetExitCodeThread(
            remoteThread,
            &remoteResult
        )
    )
    {
        PrintWindowsError(
            L"GetExitCodeThread",
            GetLastError()
        );

        CloseHandleSafe(
            remoteThread
        );

        VirtualFreeEx(
            process,
            remoteMemory,
            0,
            MEM_RELEASE
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    if (remoteResult == 0)
    {
        std::wcerr
            << L"[KSF Injector] "
            << L"LoadLibraryW retornou NULL."
            << std::endl;

        CloseHandleSafe(
            remoteThread
        );

        VirtualFreeEx(
            process,
            remoteMemory,
            0,
            MEM_RELEASE
        );

        CloseHandleSafe(
            process
        );

        return false;
    }

    success = true;

    // --------------------------------------------------
    // LIMPEZA
    // --------------------------------------------------

    CloseHandleSafe(
        remoteThread
    );

    VirtualFreeEx(
        process,
        remoteMemory,
        0,
        MEM_RELEASE
    );

    CloseHandleSafe(
        process
    );

    return success;
}

} // namespace

// ======================================================
// MAIN
// ======================================================

int wmain(
    int argc,
    wchar_t* argv[]
)
{
    std::wcout
        << L"KSF Screen Graphics Hook Injector"
        << std::endl;

    // --------------------------------------------------
    // ARGUMENTOS
    // --------------------------------------------------

    if (argc != 3)
    {
        std::wcerr
            << L"Uso:"
            << std::endl
            << L"  ksf_injector.exe <PID> <caminho-da-dll>"
            << std::endl;

        return 2;
    }

    // --------------------------------------------------
    // PID
    // --------------------------------------------------

    DWORD processId = 0;

    if (
        !ParseProcessId(
            argv[1],
            processId
        )
    )
    {
        std::wcerr
            << L"[KSF Injector] PID invalido."
            << std::endl;

        return 3;
    }

    // --------------------------------------------------
    // PROCESSO
    // --------------------------------------------------

    if (
        !IsProcessAlive(
            processId
        )
    )
    {
        std::wcerr
            << L"[KSF Injector] "
            << L"Processo nao encontrado ou encerrado."
            << std::endl;

        return 4;
    }

    // --------------------------------------------------
    // CAMINHO ABSOLUTO
    // --------------------------------------------------

    std::wstring dllPath;

    if (
        !GetAbsolutePath(
            argv[2],
            dllPath
        )
    )
    {
        PrintWindowsError(
            L"GetFullPathNameW",
            GetLastError()
        );

        return 5;
    }

    // --------------------------------------------------
    // DLL EXISTE
    // --------------------------------------------------

    if (!FileExists(dllPath))
    {
        std::wcerr
            << L"[KSF Injector] "
            << L"DLL nao encontrada:"
            << std::endl
            << dllPath
            << std::endl;

        return 6;
    }

    // --------------------------------------------------
    // INJETAR
    // --------------------------------------------------

    std::wcout
        << L"[KSF Injector] Processo alvo: "
        << processId
        << std::endl;

    std::wcout
        << L"[KSF Injector] DLL: "
        << dllPath
        << std::endl;

    if (
        !InjectDll(
            processId,
            dllPath
        )
    )
    {
        std::wcerr
            << L"[KSF Injector] "
            << L"Falha ao carregar a DLL."
            << std::endl;

        return 7;
    }

    // --------------------------------------------------
    // OK
    // --------------------------------------------------

    std::wcout
        << L"[KSF Injector] "
        << L"ksf_hook.dll carregada com sucesso."
        << std::endl;

    return 0;
}