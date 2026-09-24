const {
    app,
    BrowserWindow,
    ipcMain,
    desktopCapturer,
    session
} = require("electron");

const { autoUpdater } = require("electron-updater");
const path = require("path");


/*
================================
ÁUDIO NATIVO KSF SCREEN
================================
*/

let nativeAudio = null;

try {

    nativeAudio = require(
        path.join(
            __dirname,
            "native-audio",
            "build",
            "Release",
            "ksf_audio.node"
        )
    );

    console.log("KSF Audio nativo carregado.");

}
catch (error) {

    console.error(
        "KSF Audio nativo não pôde ser carregado:",
        error
    );

    nativeAudio = null;
}


/*
================================
VARIÁVEIS GERAIS
================================
*/

let selectedDisplaySourceId = null;
let selectedDisplaySourceType = null;
let selectedDisplayProcessId = 0;

let mainWindow = null;


/*
================================
JANELA PRINCIPAL
================================
*/

function createWindow() {

    mainWindow = new BrowserWindow({
        width: 1200,
        height: 780,
        minWidth: 900,
        minHeight: 600,

        backgroundColor: "#111318",

        webPreferences: {
            nodeIntegration: true,
            contextIsolation: false
        }
    });

    mainWindow.loadFile("index.html");

    mainWindow.setMenuBarVisibility(false);

    mainWindow.on(
        "closed",
        () => {

            mainWindow = null;

        }
    );
}


/*
================================
ENVIAR STATUS DO UPDATE
================================
*/

function sendUpdateStatus(
    status,
    data = {}
) {

    if (
        !mainWindow ||
        mainWindow.isDestroyed()
    ) {

        return;

    }

    mainWindow.webContents.send(
        "update-status",
        {
            status,
            ...data
        }
    );
}


/*
================================
SISTEMA DE ATUALIZAÇÃO
================================
*/

function configureAutoUpdater() {

    autoUpdater.autoDownload = false;
    autoUpdater.autoInstallOnAppQuit = false;


    autoUpdater.on(
        "checking-for-update",
        () => {

            sendUpdateStatus(
                "checking"
            );

        }
    );


    autoUpdater.on(
        "update-available",
        (info) => {

            sendUpdateStatus(
                "available",
                {
                    version:
                        info.version
                }
            );

        }
    );


    autoUpdater.on(
        "update-not-available",
        (info) => {

            sendUpdateStatus(
                "not-available",
                {
                    version:
                        info.version ||
                        app.getVersion()
                }
            );

        }
    );


    autoUpdater.on(
        "download-progress",
        (progress) => {

            sendUpdateStatus(
                "downloading",
                {
                    percent:
                        Math.round(
                            progress.percent
                        ),

                    transferred:
                        progress.transferred,

                    total:
                        progress.total,

                    bytesPerSecond:
                        progress.bytesPerSecond
                }
            );

        }
    );


    autoUpdater.on(
        "update-downloaded",
        (info) => {

            sendUpdateStatus(
                "downloaded",
                {
                    version:
                        info.version
                }
            );

        }
    );


    autoUpdater.on(
        "error",
        (error) => {

            console.error(
                "Erro no atualizador:",
                error
            );

            sendUpdateStatus(
                "error",
                {
                    message:
                        error.message ||
                        "Erro desconhecido"
                }
            );

        }
    );
}


/*
================================
VERIFICAR ATUALIZAÇÕES
================================
*/

ipcMain.handle(
    "check-for-updates",
    async () => {

        if (!app.isPackaged) {

            return {
                success: false,
                development: true,
                version:
                    app.getVersion()
            };

        }


        try {

            await autoUpdater
                .checkForUpdates();

            return {
                success: true,
                version:
                    app.getVersion()
            };

        }
        catch (error) {

            console.error(
                "Erro ao verificar atualização:",
                error
            );

            return {
                success: false,
                error:
                    error.message
            };

        }

    }
);


/*
================================
BAIXAR ATUALIZAÇÃO
================================
*/

ipcMain.handle(
    "download-update",
    async () => {

        if (!app.isPackaged) {

            return {
                success: false,
                development: true
            };

        }


        try {

            await autoUpdater
                .downloadUpdate();

            return {
                success: true
            };

        }
        catch (error) {

            console.error(
                "Erro ao baixar atualização:",
                error
            );

            return {
                success: false,
                error:
                    error.message
            };

        }

    }
);


/*
================================
INSTALAR E REINICIAR
================================
*/

ipcMain.handle(
    "install-update",
    async () => {

        if (!app.isPackaged) {

            return {
                success: false,
                development: true
            };

        }

        autoUpdater.quitAndInstall(
            false,
            true
        );

        return {
            success: true
        };

    }
);


/*
================================
VERSÃO ATUAL
================================
*/

ipcMain.handle(
    "get-app-version",
    async () => {

        return app.getVersion();

    }
);


/*
================================
PARAR ÁUDIO NATIVO
================================
*/

function stopNativeAudioCapture() {

    if (!nativeAudio) {

        return;

    }


    try {

        const status =
            nativeAudio.getCaptureStatus();


        if (
            status &&
            status.active
        ) {

            const result =
                nativeAudio.stopProcessCapture();

            console.log(
                "KSF Process Audio parado:",
                result
            );

        }

    }
    catch (error) {

        console.error(
            "Erro ao parar KSF Process Audio:",
            error
        );

    }
}


/*
================================
PEGAR HWND DA SOURCE ID
================================
*/

function getWindowHandleFromSourceId(
    sourceId
) {

    if (
        typeof sourceId !== "string" ||
        !sourceId.startsWith("window:")
    ) {

        return null;

    }


    const parts =
        sourceId.split(":");


    if (parts.length < 3) {

        return null;

    }


    /*
    O addon nativo espera o HWND
    como STRING.

    Exemplo:

    window:132138:0

    HWND = "132138"
    */

    const hwnd =
        parts[1];


    if (
        typeof hwnd !== "string" ||
        hwnd.length === 0 ||
        !/^\d+$/.test(hwnd) ||
        hwnd === "0"
    ) {

        return null;

    }


    return hwnd;
}


/*
================================
PEGAR PID DE UMA JANELA
================================
*/

function getProcessIdFromSourceId(
    sourceId
) {

    if (!nativeAudio) {

        return 0;

    }


    const hwnd =
        getWindowHandleFromSourceId(
            sourceId
        );


    if (
        typeof hwnd !== "string"
    ) {

        return 0;

    }


    try {

        /*
        Nosso addon retorna:

        {
            pid: 9128,
            hwnd: "132138"
        }

        Portanto precisamos guardar
        somente result.pid.
        */

        const result =
            nativeAudio
                .getProcessIdFromHwnd(
                    hwnd
                );


        if (
            !result ||
            typeof result !== "object"
        ) {

            return 0;

        }


        const pid =
            Number(
                result.pid
            );


        if (
            !Number.isFinite(pid) ||
            pid <= 0
        ) {

            return 0;

        }


        return Math.floor(pid);

    }
    catch (error) {

        console.error(
            "Erro ao descobrir PID:",
            error
        );

        return 0;

    }
}


/*
================================
STATUS DO ÁUDIO NATIVO
================================
*/

ipcMain.handle(
    "get-native-audio-status",
    async () => {

        if (!nativeAudio) {

            return {
                available: false,
                active: false,

                sourceType:
                    selectedDisplaySourceType,

                selectedPid:
                    selectedDisplayProcessId
            };

        }


        try {

            const status =
                nativeAudio.getCaptureStatus();


            return {
                available: true,

                sourceType:
                    selectedDisplaySourceType,

                selectedPid:
                    selectedDisplayProcessId,

                ...status
            };

        }
        catch (error) {

            return {
                available: true,
                active: false,
                error:
                    error.message
            };

        }

    }
);


/*
================================
INICIAR ÁUDIO ISOLADO DA JANELA
================================
*/

ipcMain.handle(
    "start-native-window-audio",
    async () => {

        if (!nativeAudio) {

            return {
                success: false,
                reason:
                    "native-audio-unavailable"
            };

        }


        if (
            selectedDisplaySourceType !==
            "window"
        ) {

            return {
                success: false,
                reason:
                    "not-window"
            };

        }


        if (
            !Number.isFinite(
                selectedDisplayProcessId
            ) ||
            selectedDisplayProcessId <= 0
        ) {

            return {
                success: false,
                reason:
                    "invalid-pid"
            };

        }


        try {

            stopNativeAudioCapture();


            console.log(
                "Iniciando áudio isolado do PID:",
                selectedDisplayProcessId
            );


            const result =
                nativeAudio
                    .startProcessCapture(
                        selectedDisplayProcessId
                    );


            console.log(
                "KSF Process Audio iniciado:",
                result
            );


            return {
                ...result,

                sourceType:
                    "window",

                pid:
                    selectedDisplayProcessId
            };

        }
        catch (error) {

            console.error(
                "Erro ao iniciar áudio isolado:",
                error
            );


            return {
                success: false,
                reason:
                    "capture-error",
                error:
                    error.message
            };

        }

    }
);


/*
================================
LER PCM DO ÁUDIO NATIVO
================================
*/

ipcMain.handle(
    "read-native-audio",
    async (
        event,
        maxBytes
    ) => {

        if (!nativeAudio) {

            return Buffer.alloc(0);

        }


        try {

            const requestedBytes =
                Number(maxBytes);


            const safeMaxBytes =
                Number.isFinite(
                    requestedBytes
                ) &&
                requestedBytes > 0

                    ? Math.floor(
                        requestedBytes
                    )

                    : 17640;


            return nativeAudio
                .readAudioData(
                    safeMaxBytes
                );

        }
        catch (error) {

            console.error(
                "Erro ao ler PCM:",
                error
            );

            return Buffer.alloc(0);

        }

    }
);


/*
================================
PARAR ÁUDIO ISOLADO
================================
*/

ipcMain.handle(
    "stop-native-audio",
    async () => {

        if (!nativeAudio) {

            return {
                success: true,
                active: false
            };

        }


        try {

            const status =
                nativeAudio.getCaptureStatus();


            if (
                !status ||
                !status.active
            ) {

                return {
                    success: true,
                    active: false
                };

            }


            const result =
                nativeAudio
                    .stopProcessCapture();


            console.log(
                "KSF Process Audio parado:",
                result
            );


            return result;

        }
        catch (error) {

            console.error(
                "Erro ao parar áudio isolado:",
                error
            );


            return {
                success: false,
                error:
                    error.message
            };

        }

    }
);


/*
================================
LISTAR TELAS E JANELAS
================================
*/

ipcMain.handle(
    "get-screen-sources",
    async () => {

        const sources =
            await desktopCapturer
                .getSources({
                    types: [
                        "screen",
                        "window"
                    ],

                    thumbnailSize: {
                        width: 320,
                        height: 180
                    },

                    fetchWindowIcons:
                        false
                });


        return sources.map(
            source => {

                const sourceType =
                    source.id.startsWith(
                        "window:"
                    )
                        ? "window"
                        : "screen";


                let processId = 0;


                if (
                    sourceType ===
                    "window"
                ) {

                    processId =
                        getProcessIdFromSourceId(
                            source.id
                        );

                }


                return {

                    id:
                        source.id,

                    name:
                        source.name,

                    type:
                        sourceType,

                    processId:
                        processId,

                    thumbnail:
                        source.thumbnail
                            .toDataURL()

                };

            }
        );

    }
);


/*
================================
SELECIONAR FONTE
================================
*/

ipcMain.handle(
    "set-display-source",
    async (
        event,
        sourceId
    ) => {

        /*
        Limpa seleção
        */

        if (
            typeof sourceId !== "string" ||
            sourceId.length === 0
        ) {

            selectedDisplaySourceId =
                null;

            selectedDisplaySourceType =
                null;

            selectedDisplayProcessId =
                0;


            stopNativeAudioCapture();


            return false;
        }


        /*
        Guarda a source ID
        */

        selectedDisplaySourceId =
            sourceId;


        /*
        JANELA
        */

        if (
            sourceId.startsWith(
                "window:"
            )
        ) {

            selectedDisplaySourceType =
                "window";


            selectedDisplayProcessId =
                getProcessIdFromSourceId(
                    sourceId
                );

        }

        /*
        TELA INTEIRA
        */

        else {

            selectedDisplaySourceType =
                "screen";

            selectedDisplayProcessId =
                0;

        }


        console.log(
            "Fonte preparada:",
            {
                id:
                    selectedDisplaySourceId,

                type:
                    selectedDisplaySourceType,

                pid:
                    selectedDisplayProcessId
            }
        );


        return {
            success: true,

            id:
                selectedDisplaySourceId,

            type:
                selectedDisplaySourceType,

            pid:
                selectedDisplayProcessId
        };

    }
);


/*
================================
DISPLAY MEDIA
================================
*/

function configureDisplayMedia() {

    session.defaultSession
        .setDisplayMediaRequestHandler(

            async (
                request,
                callback
            ) => {

                try {

                    if (
                        !request.videoRequested
                    ) {

                        callback(null);

                        return;

                    }


                    const sources =
                        await desktopCapturer
                            .getSources({

                                types: [
                                    "screen",
                                    "window"
                                ],

                                thumbnailSize: {
                                    width: 0,
                                    height: 0
                                },

                                fetchWindowIcons:
                                    false

                            });


                    const selectedSource =
                        sources.find(
                            source =>
                                source.id ===
                                selectedDisplaySourceId
                        );


                    if (!selectedSource) {

                        console.error(
                            "Fonte selecionada não encontrada:",
                            selectedDisplaySourceId
                        );

                        callback(null);

                        return;

                    }


                    const streams = {

                        video:
                            selectedSource

                    };


                    /*
                    POR ENQUANTO:

                    Mantemos o loopback geral
                    para não quebrar a transmissão
                    atual.

                    Na próxima etapa no index.html:

                    TELA INTEIRA
                    -> usa este loopback

                    JANELA
                    -> remove este áudio do stream
                       e usa o PCM isolado.
                    */

                    if (
                        process.platform ===
                        "win32" &&
                        request.audioRequested
                    ) {

                        streams.audio =
                            "loopback";

                    }


                    callback(
                        streams
                    );

                }
                catch (error) {

                    console.error(
                        "Erro no Display Media:",
                        error
                    );

                    callback(null);

                }

            }
        );
}


/*
================================
ENCERRAMENTO SEGURO
================================
*/

app.on(
    "before-quit",
    () => {

        stopNativeAudioCapture();

    }
);


/*
================================
INICIALIZAÇÃO
================================
*/

app.whenReady().then(
    () => {

        configureDisplayMedia();

        configureAutoUpdater();

        createWindow();

    }
);


/*
================================
FECHAR APLICATIVO
================================
*/

app.on(
    "window-all-closed",
    () => {

        stopNativeAudioCapture();


        if (
            process.platform !==
            "darwin"
        ) {

            app.quit();

        }

    }
);