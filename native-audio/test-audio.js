const {
    app,
    desktopCapturer
} = require("electron");

const path = require("path");


/*
========================================================
CARREGAR ADDON NATIVO
========================================================
*/

const nativeAudio = require(
    path.join(
        __dirname,
        "build",
        "Release",
        "ksf_audio.node"
    )
);


/*
========================================================
CONFIGURACAO DO TESTE
========================================================
*/

const TEST_DURATION_MS = 15000;

const READ_INTERVAL_MS = 100;

/*
44100 Hz
2 canais
16-bit = 4 bytes por frame

100 ms:
44100 * 4 * 0.1 = 17640 bytes
*/

const MAX_BYTES_PER_READ = 17640;


/*
========================================================
ESPERA
========================================================
*/

function sleep(ms) {
    return new Promise((resolve) => {
        setTimeout(resolve, ms);
    });
}


/*
========================================================
PEGAR HWND DO SOURCE ID
========================================================

Exemplo:

window:132138:0

132138 = HWND
========================================================
*/

function getHwndFromSourceId(sourceId) {

    if (
        typeof sourceId !== "string" ||
        !sourceId.startsWith("window:")
    ) {
        return null;
    }


    const parts = sourceId.split(":");


    if (parts.length < 3) {
        return null;
    }


    return parts[1];
}


/*
========================================================
LISTAR JANELAS
========================================================
*/

async function getWindows() {

    const sources =
        await desktopCapturer.getSources({
            types: [
                "window"
            ],

            fetchWindowIcons: false,

            thumbnailSize: {
                width: 0,
                height: 0
            }
        });


    const windows = [];


    for (const source of sources) {

        const hwnd =
            getHwndFromSourceId(
                source.id
            );


        if (!hwnd) {
            continue;
        }


        try {

            const processInfo =
                nativeAudio.getProcessIdFromHwnd(
                    hwnd
                );


            windows.push({
                name: source.name,
                sourceId: source.id,
                hwnd: hwnd,
                pid: processInfo.pid
            });

        }
        catch (error) {

            console.log(
                "Nao foi possivel descobrir PID de:",
                source.name
            );

        }
    }


    return windows;
}


/*
========================================================
MOSTRAR JANELAS
========================================================
*/

function printWindows(windows) {

    console.log("");
    console.log("Janelas encontradas:");
    console.log("");


    for (const windowInfo of windows) {

        console.log(
            `- ${windowInfo.name}`
        );

        console.log(
            `  PID: ${windowInfo.pid}`
        );

        console.log(
            `  HWND: ${windowInfo.hwnd}`
        );

        console.log("");
    }
}


/*
========================================================
CALCULAR AMOSTRAS NAO-ZERO

Isto NAO e analise de audio completa.

Serve apenas para confirmar que o Buffer recebido
pelo JavaScript contem dados PCM que nao sao somente
zeros.
========================================================
*/

function analyzePcm(buffer) {

    if (
        !Buffer.isBuffer(buffer) ||
        buffer.length === 0
    ) {
        return {
            samples: 0,
            nonZeroSamples: 0,
            peak: 0
        };
    }


    const sampleCount =
        Math.floor(
            buffer.length / 2
        );


    let nonZeroSamples = 0;

    let peak = 0;


    for (
        let offset = 0;
        offset + 1 < buffer.length;
        offset += 2
    ) {

        const sample =
            buffer.readInt16LE(
                offset
            );


        const absolute =
            Math.abs(sample);


        if (sample !== 0) {
            nonZeroSamples++;
        }


        if (absolute > peak) {
            peak = absolute;
        }
    }


    return {
        samples: sampleCount,
        nonZeroSamples,
        peak
    };
}


/*
========================================================
TESTE PRINCIPAL
========================================================
*/

async function runTest() {

    console.log("");
    console.log(
        "======================================"
    );

    console.log(
        " KSF SCREEN - TESTE C++ -> JAVASCRIPT"
    );

    console.log(
        "======================================"
    );

    console.log("");


    console.log(
        "Process Loopback suportado:",
        nativeAudio.isProcessLoopbackSupported()
    );


    /*
    ====================================================
    LISTAR JANELAS
    ====================================================
    */

    const windows =
        await getWindows();


    printWindows(
        windows
    );


    /*
    ====================================================
    ENCONTRAR OPERA
    ====================================================
    */

    const opera =
        windows.find(
            (windowInfo) => {

                return windowInfo.name
                    .toLowerCase()
                    .includes(
                        "opera"
                    );

            }
        );


    if (!opera) {

        console.log(
            "ERRO: nenhuma janela do Opera foi encontrada."
        );

        console.log(
            "Abra o Opera e execute o teste novamente."
        );

        return;
    }


    console.log(
        "======================================"
    );

    console.log(
        " JANELA SELECIONADA"
    );

    console.log(
        "======================================"
    );

    console.log("");


    console.log(
        "Nome:",
        opera.name
    );

    console.log(
        "PID:",
        opera.pid
    );

    console.log(
        "HWND:",
        opera.hwnd
    );

    console.log("");


    /*
    ====================================================
    INICIAR CAPTURA
    ====================================================
    */

    console.log(
        "Iniciando Process Loopback..."
    );

    console.log("");


    let started = false;


    try {

        const result =
            nativeAudio.startProcessCapture(
                opera.pid
            );


        started = true;


        console.log(
            "RESULTADO:"
        );

        console.log(
            result
        );

        console.log("");


        console.log(
            "STATUS INICIAL:"
        );

        console.log(
            nativeAudio.getCaptureStatus()
        );

    }
    catch (error) {

        console.log(
            "FALHA AO INICIAR PROCESS LOOPBACK:"
        );

        console.error(
            error
        );

        return;
    }


    console.log("");

    console.log(
        "======================================"
    );

    console.log(
        " LENDO PCM POR 15 SEGUNDOS"
    );

    console.log(
        "======================================"
    );

    console.log("");


    console.log(
        "Deixe um video ou musica TOCANDO no Opera."
    );

    console.log("");


    /*
    ====================================================
    CONTADORES DO JAVASCRIPT
    ====================================================
    */

    let totalJsBytes = 0;

    let totalJsBuffers = 0;

    let totalJsSamples = 0;

    let totalNonZeroSamples = 0;

    let highestPeak = 0;


    const startTime =
        Date.now();


    let nextReport =
        startTime + 1000;


    /*
    ====================================================
    CONSUMIR FILA PCM
    ====================================================
    */

    while (
        Date.now() - startTime <
        TEST_DURATION_MS
    ) {

        const pcm =
            nativeAudio.readAudioData(
                MAX_BYTES_PER_READ
            );


        if (
            Buffer.isBuffer(pcm) &&
            pcm.length > 0
        ) {

            totalJsBytes +=
                pcm.length;


            totalJsBuffers++;


            const analysis =
                analyzePcm(
                    pcm
                );


            totalJsSamples +=
                analysis.samples;


            totalNonZeroSamples +=
                analysis.nonZeroSamples;


            if (
                analysis.peak >
                highestPeak
            ) {

                highestPeak =
                    analysis.peak;
            }
        }


        /*
        Relatorio aproximadamente uma vez por segundo.
        */

        if (
            Date.now() >= nextReport
        ) {

            const status =
                nativeAudio.getCaptureStatus();


            console.log(
                "--------------------------------------"
            );


            console.log(
                "PCM recebido pelo JavaScript:",
                totalJsBytes,
                "bytes"
            );


            console.log(
                "Buffers recebidos:",
                totalJsBuffers
            );


            console.log(
                "Amostras nao-zero:",
                totalNonZeroSamples
            );


            console.log(
                "Pico PCM:",
                highestPeak
            );


            console.log(
                "Fila C++:",
                status.queuedBytes,
                "bytes"
            );


            console.log(
                "Pacotes WASAPI:",
                status.packets
            );


            nextReport +=
                1000;
        }


        await sleep(
            READ_INTERVAL_MS
        );
    }


    /*
    ====================================================
    DRENAR O RESTANTE DA FILA
    ====================================================
    */

    while (true) {

        const pcm =
            nativeAudio.readAudioData(
                MAX_BYTES_PER_READ
            );


        if (
            !Buffer.isBuffer(pcm) ||
            pcm.length === 0
        ) {
            break;
        }


        totalJsBytes +=
            pcm.length;


        totalJsBuffers++;


        const analysis =
            analyzePcm(
                pcm
            );


        totalJsSamples +=
            analysis.samples;


        totalNonZeroSamples +=
            analysis.nonZeroSamples;


        if (
            analysis.peak >
            highestPeak
        ) {

            highestPeak =
                analysis.peak;
        }
    }


    /*
    ====================================================
    PARAR
    ====================================================
    */

    console.log("");

    console.log(
        "Parando Process Loopback..."
    );


    let stopResult = null;


    if (started) {

        stopResult =
            nativeAudio.stopProcessCapture();

    }


    console.log("");

    console.log(
        "======================================"
    );

    console.log(
        " RESULTADO FINAL"
    );

    console.log(
        "======================================"
    );

    console.log("");


    console.log(
        "Bytes PCM recebidos pelo JavaScript:",
        totalJsBytes
    );


    console.log(
        "Buffers PCM recebidos:",
        totalJsBuffers
    );


    console.log(
        "Amostras PCM analisadas:",
        totalJsSamples
    );


    console.log(
        "Amostras nao-zero:",
        totalNonZeroSamples
    );


    console.log(
        "Maior pico PCM:",
        highestPeak
    );


    console.log("");


    console.log(
        "Resultado do stop:"
    );

    console.log(
        stopResult
    );


    console.log("");


    console.log(
        "STATUS FINAL:"
    );

    console.log(
        nativeAudio.getCaptureStatus()
    );


    console.log("");


    /*
    ====================================================
    CONCLUSAO AUTOMATICA
    ====================================================
    */

    if (
        totalJsBytes > 0 &&
        totalNonZeroSamples > 0
    ) {

        console.log(
            "SUCESSO:"
        );

        console.log(
            "O PCM saiu do WASAPI, passou pelo C++ e chegou ao JavaScript."
        );

    }
    else if (
        totalJsBytes > 0
    ) {

        console.log(
            "ATENCAO:"
        );

        console.log(
            "O JavaScript recebeu PCM, mas as amostras estavam zeradas."
        );

    }
    else {

        console.log(
            "FALHA:"
        );

        console.log(
            "Nenhum PCM chegou ao JavaScript."
        );

    }
}


/*
========================================================
ELECTRON
========================================================
*/

app.whenReady().then(
    async () => {

        try {

            await runTest();

        }
        catch (error) {

            console.error("");
            console.error(
                "ERRO INESPERADO:"
            );

            console.error(
                error
            );

        }


        /*
        Pequena espera para terminar de imprimir tudo.
        */

        await sleep(
            1000
        );


        app.quit();
    }
);


app.on(
    "window-all-closed",
    () => {

        app.quit();

    }
);