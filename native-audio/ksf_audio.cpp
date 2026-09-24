#include <napi.h>

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <wrl.h>

#include <string>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <thread>
#include <atomic>
#include <deque>
#include <vector>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;


/*
========================================================
ESTADO GLOBAL
========================================================
*/

static std::mutex g_mutex;

static ComPtr<IAudioClient> g_audioClient;
static ComPtr<IAudioCaptureClient> g_captureClient;

static HANDLE g_audioEvent = nullptr;

static DWORD g_targetPid = 0;
static bool g_captureActive = false;

static std::string g_lastStage = "Nenhuma";
static HRESULT g_lastHRESULT = S_OK;


/*
========================================================
THREAD / CONTADORES
========================================================
*/

static std::thread g_captureThread;
static std::atomic<bool> g_stopCaptureThread(false);

static std::atomic<uint64_t> g_totalPackets(0);
static std::atomic<uint64_t> g_totalFrames(0);
static std::atomic<uint64_t> g_totalBytes(0);
static std::atomic<uint64_t> g_silentFrames(0);


/*
========================================================
FORMATO DE AUDIO

PCM 16-bit
44100 Hz
Stereo
========================================================
*/

static constexpr UINT32 KSF_SAMPLE_RATE = 44100;
static constexpr UINT32 KSF_CHANNELS = 2;
static constexpr UINT32 KSF_BITS_PER_SAMPLE = 16;

static constexpr UINT32 KSF_BYTES_PER_FRAME =
    KSF_CHANNELS *
    (KSF_BITS_PER_SAMPLE / 8);


/*
========================================================
FILA PCM

Mantemos no maximo aproximadamente 2 segundos.

44100 frames/s
4 bytes/frame
= 176400 bytes/s

2 segundos ~= 352800 bytes
========================================================
*/

static std::mutex g_pcmMutex;

static std::deque<std::vector<uint8_t>> g_pcmQueue;

static size_t g_pcmQueuedBytes = 0;

static constexpr size_t KSF_MAX_PCM_QUEUE_BYTES =
    static_cast<size_t>(
        KSF_SAMPLE_RATE *
        KSF_BYTES_PER_FRAME *
        2
    );


/*
========================================================
DIAGNOSTICO
========================================================
*/

static std::string HResultToHex(HRESULT hr)
{
    char buffer[32] = {};

    sprintf_s(
        buffer,
        "0x%08lX",
        static_cast<unsigned long>(hr)
    );

    return std::string(buffer);
}


static void SetLastErrorInfo(
    const std::string& stage,
    HRESULT hr
)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_lastStage = stage;
    g_lastHRESULT = hr;
}


/*
========================================================
LIMPAR FILA PCM
========================================================
*/

static void ClearPcmQueue()
{
    std::lock_guard<std::mutex> lock(g_pcmMutex);

    g_pcmQueue.clear();
    g_pcmQueuedBytes = 0;
}


/*
========================================================
ADICIONAR PCM NA FILA
========================================================
*/

static void PushPcmData(
    const BYTE* data,
    size_t byteCount,
    bool silent
)
{
    if (byteCount == 0)
    {
        return;
    }

    std::vector<uint8_t> block(byteCount);

    if (silent || data == nullptr)
    {
        std::fill(
            block.begin(),
            block.end(),
            0
        );
    }
    else
    {
        std::memcpy(
            block.data(),
            data,
            byteCount
        );
    }


    std::lock_guard<std::mutex> lock(g_pcmMutex);


    /*
    Se um unico bloco for maior que o limite,
    ficamos somente com a parte mais recente.
    */

    if (block.size() > KSF_MAX_PCM_QUEUE_BYTES)
    {
        size_t start =
            block.size() -
            KSF_MAX_PCM_QUEUE_BYTES;

        std::vector<uint8_t> trimmed(
            block.begin() + start,
            block.end()
        );

        block.swap(trimmed);
    }


    /*
    Remove os blocos mais antigos se necessario.
    */

    while (
        !g_pcmQueue.empty() &&
        g_pcmQueuedBytes + block.size() >
            KSF_MAX_PCM_QUEUE_BYTES
    )
    {
        g_pcmQueuedBytes -=
            g_pcmQueue.front().size();

        g_pcmQueue.pop_front();
    }


    g_pcmQueuedBytes +=
        block.size();

    g_pcmQueue.push_back(
        std::move(block)
    );
}


/*
========================================================
HWND -> PID
========================================================
*/

static HWND StringToHwnd(
    const std::string& value
)
{
    try
    {
        unsigned long long number =
            std::stoull(value);

        return reinterpret_cast<HWND>(
            static_cast<uintptr_t>(number)
        );
    }
    catch (...)
    {
        return nullptr;
    }
}


Napi::Value GetProcessIdFromHwnd(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env = info.Env();

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

    std::string hwndString =
        info[0]
            .As<Napi::String>()
            .Utf8Value();

    HWND hwnd =
        StringToHwnd(hwndString);

    if (
        hwnd == nullptr ||
        !IsWindow(hwnd)
    )
    {
        Napi::Error::New(
            env,
            "HWND invalido."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    DWORD processId = 0;

    GetWindowThreadProcessId(
        hwnd,
        &processId
    );

    if (processId == 0)
    {
        Napi::Error::New(
            env,
            "Nao foi possivel descobrir o PID."
        ).ThrowAsJavaScriptException();

        return env.Null();
    }

    Napi::Object result =
        Napi::Object::New(env);

    result.Set(
        "pid",
        Napi::Number::New(
            env,
            processId
        )
    );

    result.Set(
        "hwnd",
        Napi::String::New(
            env,
            hwndString
        )
    );

    return result;
}


/*
========================================================
THREAD DE CAPTURA
========================================================
*/

static void CaptureThreadFunction()
{
    HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED
        );

    bool shouldUninitialize =
        SUCCEEDED(comResult);


    while (!g_stopCaptureThread.load())
    {
        HANDLE eventHandle = nullptr;

        {
            std::lock_guard<std::mutex> lock(g_mutex);

            eventHandle =
                g_audioEvent;
        }


        if (eventHandle == nullptr)
        {
            break;
        }


        DWORD waitResult =
            WaitForSingleObject(
                eventHandle,
                500
            );


        if (g_stopCaptureThread.load())
        {
            break;
        }


        if (waitResult == WAIT_TIMEOUT)
        {
            continue;
        }


        if (waitResult != WAIT_OBJECT_0)
        {
            SetLastErrorInfo(
                "WaitForSingleObject(audioEvent)",
                HRESULT_FROM_WIN32(
                    GetLastError()
                )
            );

            break;
        }


        ComPtr<IAudioCaptureClient>
            captureClient;


        {
            std::lock_guard<std::mutex> lock(g_mutex);

            captureClient =
                g_captureClient;
        }


        if (!captureClient)
        {
            break;
        }


        while (!g_stopCaptureThread.load())
        {
            UINT32 packetLength = 0;


            HRESULT hr =
                captureClient->GetNextPacketSize(
                    &packetLength
                );


            if (FAILED(hr))
            {
                SetLastErrorInfo(
                    "IAudioCaptureClient::GetNextPacketSize",
                    hr
                );

                g_stopCaptureThread.store(true);

                break;
            }


            if (packetLength == 0)
            {
                break;
            }


            BYTE* audioData = nullptr;

            UINT32 numFramesAvailable = 0;

            DWORD flags = 0;

            UINT64 devicePosition = 0;
            UINT64 qpcPosition = 0;


            hr =
                captureClient->GetBuffer(
                    &audioData,
                    &numFramesAvailable,
                    &flags,
                    &devicePosition,
                    &qpcPosition
                );


            if (FAILED(hr))
            {
                SetLastErrorInfo(
                    "IAudioCaptureClient::GetBuffer",
                    hr
                );

                g_stopCaptureThread.store(true);

                break;
            }


            uint64_t byteCount =
                static_cast<uint64_t>(
                    numFramesAvailable
                ) *
                KSF_BYTES_PER_FRAME;


            bool silent =
                (
                    flags &
                    AUDCLNT_BUFFERFLAGS_SILENT
                ) != 0;


            /*
            IMPORTANTE:

            Copiamos os dados ANTES de ReleaseBuffer(),
            pois o ponteiro audioData deixa de ser nosso
            depois que o buffer WASAPI for liberado.
            */

            PushPcmData(
                audioData,
                static_cast<size_t>(
                    byteCount
                ),
                silent
            );


            g_totalPackets.fetch_add(
                1
            );


            g_totalFrames.fetch_add(
                static_cast<uint64_t>(
                    numFramesAvailable
                )
            );


            g_totalBytes.fetch_add(
                byteCount
            );


            if (silent)
            {
                g_silentFrames.fetch_add(
                    static_cast<uint64_t>(
                        numFramesAvailable
                    )
                );
            }


            hr =
                captureClient->ReleaseBuffer(
                    numFramesAvailable
                );


            if (FAILED(hr))
            {
                SetLastErrorInfo(
                    "IAudioCaptureClient::ReleaseBuffer",
                    hr
                );

                g_stopCaptureThread.store(true);

                break;
            }
        }
    }


    if (shouldUninitialize)
    {
        CoUninitialize();
    }
}


/*
========================================================
PARAR CAPTURA
========================================================
*/

static void StopCaptureInternal()
{
    g_stopCaptureThread.store(true);


    HANDLE eventHandle = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        eventHandle =
            g_audioEvent;
    }


    if (eventHandle != nullptr)
    {
        SetEvent(
            eventHandle
        );
    }


    if (
        g_captureThread.joinable() &&
        g_captureThread.get_id() !=
            std::this_thread::get_id()
    )
    {
        g_captureThread.join();
    }


    std::lock_guard<std::mutex> lock(g_mutex);


    if (g_audioClient)
    {
        g_audioClient->Stop();
    }


    g_captureClient.Reset();
    g_audioClient.Reset();


    if (g_audioEvent != nullptr)
    {
        CloseHandle(
            g_audioEvent
        );

        g_audioEvent = nullptr;
    }


    g_targetPid = 0;
    g_captureActive = false;
}


/*
========================================================
HANDLER DE ATIVACAO
========================================================
*/

class AudioActivationHandler :
    public IActivateAudioInterfaceCompletionHandler
{
public:

    AudioActivationHandler()
        :
        m_refCount(1),
        m_completed(false),
        m_result(E_FAIL)
    {
    }


    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return InterlockedIncrement(
            &m_refCount
        );
    }


    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG result =
            InterlockedDecrement(
                &m_refCount
            );

        if (result == 0)
        {
            delete this;
        }

        return result;
    }


    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object
    ) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }


        *object = nullptr;


        if (
            riid == __uuidof(IUnknown) ||
            riid == __uuidof(IAgileObject) ||
            riid ==
                __uuidof(
                    IActivateAudioInterfaceCompletionHandler
                )
        )
        {
            *object =
                static_cast<
                    IActivateAudioInterfaceCompletionHandler*
                >(this);

            AddRef();

            return S_OK;
        }


        return E_NOINTERFACE;
    }


    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation
    ) override
    {
        HRESULT activationResult =
            E_FAIL;


        ComPtr<IUnknown>
            activatedInterface;


        HRESULT hr =
            operation->GetActivateResult(
                &activationResult,
                &activatedInterface
            );


        if (FAILED(hr))
        {
            SetLastErrorInfo(
                "GetActivateResult",
                hr
            );

            Finish(hr);

            return S_OK;
        }


        if (FAILED(activationResult))
        {
            SetLastErrorInfo(
                "ActivateAudioInterfaceAsync / activationResult",
                activationResult
            );

            Finish(
                activationResult
            );

            return S_OK;
        }


        ComPtr<IAudioClient>
            audioClient;


        hr =
            activatedInterface.As(
                &audioClient
            );


        if (FAILED(hr))
        {
            SetLastErrorInfo(
                "QueryInterface IAudioClient",
                hr
            );

            Finish(hr);

            return S_OK;
        }


        /*
        ====================================================
        PCM 44100 / STEREO / 16-BIT
        ====================================================
        */

        WAVEFORMATEX captureFormat = {};


        captureFormat.wFormatTag =
            WAVE_FORMAT_PCM;


        captureFormat.nChannels =
            KSF_CHANNELS;


        captureFormat.nSamplesPerSec =
            KSF_SAMPLE_RATE;


        captureFormat.wBitsPerSample =
            KSF_BITS_PER_SAMPLE;


        captureFormat.nBlockAlign =
            static_cast<WORD>(
                KSF_BYTES_PER_FRAME
            );


        captureFormat.nAvgBytesPerSec =
            captureFormat.nSamplesPerSec *
            captureFormat.nBlockAlign;


        captureFormat.cbSize = 0;


        DWORD streamFlags =
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;


        hr =
            audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                streamFlags,
                0,
                0,
                &captureFormat,
                nullptr
            );


        if (FAILED(hr))
        {
            SetLastErrorInfo(
                "IAudioClient::Initialize",
                hr
            );

            Finish(hr);

            return S_OK;
        }


        HANDLE audioEvent =
            CreateEventW(
                nullptr,
                FALSE,
                FALSE,
                nullptr
            );


        if (audioEvent == nullptr)
        {
            hr =
                HRESULT_FROM_WIN32(
                    GetLastError()
                );


            SetLastErrorInfo(
                "CreateEventW",
                hr
            );


            Finish(hr);

            return S_OK;
        }


        hr =
            audioClient->SetEventHandle(
                audioEvent
            );


        if (FAILED(hr))
        {
            CloseHandle(
                audioEvent
            );


            SetLastErrorInfo(
                "IAudioClient::SetEventHandle",
                hr
            );


            Finish(hr);

            return S_OK;
        }


        ComPtr<IAudioCaptureClient>
            captureClient;


        hr =
            audioClient->GetService(
                IID_PPV_ARGS(
                    &captureClient
                )
            );


        if (FAILED(hr))
        {
            CloseHandle(
                audioEvent
            );


            SetLastErrorInfo(
                "IAudioClient::GetService(IAudioCaptureClient)",
                hr
            );


            Finish(hr);

            return S_OK;
        }


        hr =
            audioClient->Start();


        if (FAILED(hr))
        {
            CloseHandle(
                audioEvent
            );


            SetLastErrorInfo(
                "IAudioClient::Start",
                hr
            );


            Finish(hr);

            return S_OK;
        }


        {
            std::lock_guard<std::mutex>
                lock(g_mutex);


            g_audioClient =
                audioClient;


            g_captureClient =
                captureClient;


            g_audioEvent =
                audioEvent;


            g_captureActive =
                true;


            g_lastStage =
                "CAPTURA INICIADA";


            g_lastHRESULT =
                S_OK;
        }


        g_stopCaptureThread.store(
            false
        );


        g_captureThread =
            std::thread(
                CaptureThreadFunction
            );


        Finish(
            S_OK
        );


        return S_OK;
    }


    HRESULT Wait(
        DWORD timeoutMilliseconds
    )
    {
        std::unique_lock<std::mutex>
            lock(m_waitMutex);


        bool completed =
            m_condition.wait_for(
                lock,

                std::chrono::milliseconds(
                    timeoutMilliseconds
                ),

                [this]()
                {
                    return m_completed;
                }
            );


        if (!completed)
        {
            SetLastErrorInfo(
                "Timeout aguardando ActivateCompleted",
                HRESULT_FROM_WIN32(
                    WAIT_TIMEOUT
                )
            );


            return HRESULT_FROM_WIN32(
                WAIT_TIMEOUT
            );
        }


        return m_result;
    }


private:

    ~AudioActivationHandler()
    {
    }


    void Finish(
        HRESULT result
    )
    {
        {
            std::lock_guard<std::mutex>
                lock(m_waitMutex);


            m_result =
                result;


            m_completed =
                true;
        }


        m_condition.notify_all();
    }


private:

    volatile LONG m_refCount;

    std::mutex m_waitMutex;

    std::condition_variable
        m_condition;

    bool m_completed;

    HRESULT m_result;
};


/*
========================================================
INICIAR PROCESS LOOPBACK
========================================================
*/

static HRESULT StartProcessLoopback(
    DWORD processId
)
{
    StopCaptureInternal();


    ClearPcmQueue();


    g_totalPackets.store(0);
    g_totalFrames.store(0);
    g_totalBytes.store(0);
    g_silentFrames.store(0);


    {
        std::lock_guard<std::mutex>
            lock(g_mutex);


        g_lastStage =
            "Preparando AUDIOCLIENT_ACTIVATION_PARAMS";


        g_lastHRESULT =
            S_OK;
    }


    AUDIOCLIENT_ACTIVATION_PARAMS
        activationParams = {};


    activationParams.ActivationType =
        AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;


    activationParams
        .ProcessLoopbackParams
        .TargetProcessId =
            processId;


    activationParams
        .ProcessLoopbackParams
        .ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;


    PROPVARIANT activateParams = {};


    activateParams.vt =
        VT_BLOB;


    activateParams.blob.cbSize =
        sizeof(
            activationParams
        );


    activateParams.blob.pBlobData =
        reinterpret_cast<BYTE*>(
            &activationParams
        );


    AudioActivationHandler*
        rawHandler =
            new AudioActivationHandler();


    ComPtr<
        IActivateAudioInterfaceCompletionHandler
    > handler;


    handler.Attach(
        rawHandler
    );


    ComPtr<
        IActivateAudioInterfaceAsyncOperation
    > asyncOperation;


    SetLastErrorInfo(
        "Chamando ActivateAudioInterfaceAsync",
        S_OK
    );


    HRESULT hr =
        ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,

            __uuidof(
                IAudioClient
            ),

            &activateParams,

            handler.Get(),

            &asyncOperation
        );


    if (FAILED(hr))
    {
        SetLastErrorInfo(
            "ActivateAudioInterfaceAsync",
            hr
        );


        return hr;
    }


    SetLastErrorInfo(
        "Aguardando ActivateCompleted",
        S_OK
    );


    hr =
        rawHandler->Wait(
            10000
        );


    if (FAILED(hr))
    {
        return hr;
    }


    {
        std::lock_guard<std::mutex>
            lock(g_mutex);


        if (!g_captureActive)
        {
            g_lastStage =
                "ActivateCompleted terminou mas captura nao ficou ativa";


            g_lastHRESULT =
                E_FAIL;


            return E_FAIL;
        }


        g_targetPid =
            processId;
    }


    return S_OK;
}


/*
========================================================
JS: START
========================================================
*/

Napi::Value StartProcessCapture(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();


    if (
        info.Length() < 1 ||
        !info[0].IsNumber()
    )
    {
        Napi::TypeError::New(
            env,
            "PID precisa ser um numero."
        ).ThrowAsJavaScriptException();


        return env.Null();
    }


    DWORD processId =
        static_cast<DWORD>(
            info[0]
                .As<Napi::Number>()
                .Uint32Value()
        );


    if (processId == 0)
    {
        Napi::Error::New(
            env,
            "PID invalido."
        ).ThrowAsJavaScriptException();


        return env.Null();
    }


    HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED
        );


    if (
        FAILED(comResult) &&
        comResult != RPC_E_CHANGED_MODE
    )
    {
        std::string message =
            "CoInitializeEx falhou: " +
            HResultToHex(
                comResult
            );


        Napi::Error::New(
            env,
            message
        ).ThrowAsJavaScriptException();


        return env.Null();
    }


    HRESULT hr =
        StartProcessLoopback(
            processId
        );


    if (FAILED(hr))
    {
        std::string stage;
        HRESULT detailedHr;


        {
            std::lock_guard<std::mutex>
                lock(g_mutex);


            stage =
                g_lastStage;


            detailedHr =
                g_lastHRESULT;
        }


        std::string message =
            "Process Loopback falhou | Etapa: " +
            stage +
            " | HRESULT: " +
            HResultToHex(
                detailedHr
            );


        Napi::Error::New(
            env,
            message
        ).ThrowAsJavaScriptException();


        return env.Null();
    }


    Napi::Object result =
        Napi::Object::New(env);


    result.Set(
        "success",
        Napi::Boolean::New(
            env,
            true
        )
    );


    result.Set(
        "active",
        Napi::Boolean::New(
            env,
            true
        )
    );


    result.Set(
        "pid",
        Napi::Number::New(
            env,
            processId
        )
    );


    result.Set(
        "sampleRate",
        Napi::Number::New(
            env,
            KSF_SAMPLE_RATE
        )
    );


    result.Set(
        "channels",
        Napi::Number::New(
            env,
            KSF_CHANNELS
        )
    );


    result.Set(
        "bitsPerSample",
        Napi::Number::New(
            env,
            KSF_BITS_PER_SAMPLE
        )
    );


    result.Set(
        "stage",
        Napi::String::New(
            env,
            "CAPTURA INICIADA"
        )
    );


    return result;
}


/*
========================================================
JS: READ AUDIO DATA

Retorna um Buffer Node.js contendo PCM 16-bit LE.

Argumento opcional:
maxBytes

Exemplo:
readAudioData(17640)

17640 bytes ~= 100 ms em:
44100 Hz / stereo / 16-bit
========================================================
*/

Napi::Value ReadAudioData(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();


    size_t maxBytes =
        static_cast<size_t>(
            KSF_SAMPLE_RATE *
            KSF_BYTES_PER_FRAME /
            10
        );


    if (
        info.Length() >= 1 &&
        info[0].IsNumber()
    )
    {
        double requested =
            info[0]
                .As<Napi::Number>()
                .DoubleValue();


        if (requested > 0)
        {
            maxBytes =
                static_cast<size_t>(
                    requested
                );
        }
    }


    /*
    Mantemos alinhamento por frame.
    */

    maxBytes -=
        maxBytes %
        KSF_BYTES_PER_FRAME;


    if (maxBytes == 0)
    {
        maxBytes =
            KSF_BYTES_PER_FRAME;
    }


    std::vector<uint8_t> output;


    {
        std::lock_guard<std::mutex>
            lock(g_pcmMutex);


        size_t amountToRead =
            std::min(
                maxBytes,
                g_pcmQueuedBytes
            );


        amountToRead -=
            amountToRead %
            KSF_BYTES_PER_FRAME;


        output.reserve(
            amountToRead
        );


        size_t remaining =
            amountToRead;


        while (
            remaining > 0 &&
            !g_pcmQueue.empty()
        )
        {
            std::vector<uint8_t>& front =
                g_pcmQueue.front();


            size_t take =
                std::min(
                    remaining,
                    front.size()
                );


            /*
            Sempre manter alinhamento por frame.
            */

            take -=
                take %
                KSF_BYTES_PER_FRAME;


            if (take == 0)
            {
                break;
            }


            output.insert(
                output.end(),
                front.begin(),
                front.begin() + take
            );


            if (take == front.size())
            {
                g_pcmQueuedBytes -=
                    front.size();

                g_pcmQueue.pop_front();
            }
            else
            {
                front.erase(
                    front.begin(),
                    front.begin() + take
                );

                g_pcmQueuedBytes -=
                    take;
            }


            remaining -=
                take;
        }
    }


    if (output.empty())
    {
        return Napi::Buffer<uint8_t>::New(
            env,
            0
        );
    }


    return Napi::Buffer<uint8_t>::Copy(
        env,
        output.data(),
        output.size()
    );
}


/*
========================================================
JS: STOP
========================================================
*/

Napi::Value StopProcessCapture(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();


    StopCaptureInternal();


    Napi::Object result =
        Napi::Object::New(env);


    result.Set(
        "success",
        Napi::Boolean::New(
            env,
            true
        )
    );


    result.Set(
        "active",
        Napi::Boolean::New(
            env,
            false
        )
    );


    result.Set(
        "packets",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_totalPackets.load()
            )
        )
    );


    result.Set(
        "frames",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_totalFrames.load()
            )
        )
    );


    result.Set(
        "bytes",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_totalBytes.load()
            )
        )
    );


    result.Set(
        "silentFrames",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_silentFrames.load()
            )
        )
    );


    return result;
}


/*
========================================================
JS: STATUS
========================================================
*/

Napi::Value GetCaptureStatus(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();


    bool active;
    DWORD pid;
    std::string stage;
    HRESULT lastHr;


    {
        std::lock_guard<std::mutex>
            lock(g_mutex);


        active =
            g_captureActive;


        pid =
            g_targetPid;


        stage =
            g_lastStage;


        lastHr =
            g_lastHRESULT;
    }


    size_t queuedBytes = 0;


    {
        std::lock_guard<std::mutex>
            lock(g_pcmMutex);


        queuedBytes =
            g_pcmQueuedBytes;
    }


    Napi::Object result =
        Napi::Object::New(env);


    result.Set(
        "active",
        Napi::Boolean::New(
            env,
            active
        )
    );


    result.Set(
        "pid",
        Napi::Number::New(
            env,
            pid
        )
    );


    result.Set(
        "stage",
        Napi::String::New(
            env,
            stage
        )
    );


    result.Set(
        "hresult",
        Napi::String::New(
            env,
            HResultToHex(
                lastHr
            )
        )
    );


    result.Set(
        "packets",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_totalPackets.load()
            )
        )
    );


    result.Set(
        "frames",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_totalFrames.load()
            )
        )
    );


    result.Set(
        "bytes",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_totalBytes.load()
            )
        )
    );


    result.Set(
        "silentFrames",
        Napi::Number::New(
            env,
            static_cast<double>(
                g_silentFrames.load()
            )
        )
    );


    result.Set(
        "queuedBytes",
        Napi::Number::New(
            env,
            static_cast<double>(
                queuedBytes
            )
        )
    );


    result.Set(
        "sampleRate",
        Napi::Number::New(
            env,
            KSF_SAMPLE_RATE
        )
    );


    result.Set(
        "channels",
        Napi::Number::New(
            env,
            KSF_CHANNELS
        )
    );


    result.Set(
        "bitsPerSample",
        Napi::Number::New(
            env,
            KSF_BITS_PER_SAMPLE
        )
    );


    return result;
}


/*
========================================================
SUPORTE
========================================================
*/

Napi::Value IsProcessLoopbackSupported(
    const Napi::CallbackInfo& info
)
{
    Napi::Env env =
        info.Env();


    return Napi::Boolean::New(
        env,
        true
    );
}


/*
========================================================
EXPORTS
========================================================
*/

Napi::Object Init(
    Napi::Env env,
    Napi::Object exports
)
{
    exports.Set(
        "getProcessIdFromHwnd",

        Napi::Function::New(
            env,
            GetProcessIdFromHwnd
        )
    );


    exports.Set(
        "startProcessCapture",

        Napi::Function::New(
            env,
            StartProcessCapture
        )
    );


    exports.Set(
        "readAudioData",

        Napi::Function::New(
            env,
            ReadAudioData
        )
    );


    exports.Set(
        "stopProcessCapture",

        Napi::Function::New(
            env,
            StopProcessCapture
        )
    );


    exports.Set(
        "getCaptureStatus",

        Napi::Function::New(
            env,
            GetCaptureStatus
        )
    );


    exports.Set(
        "isProcessLoopbackSupported",

        Napi::Function::New(
            env,
            IsProcessLoopbackSupported
        )
    );


    return exports;
}


NODE_API_MODULE(
    ksf_audio,
    Init
)