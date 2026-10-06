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
VÍDEO NATIVO KSF SCREEN
================================
*/

let nativeVideo = null;

try {

    nativeVideo = require(
        path.join(
            __dirname,
            "native-audio",
            "build",
            "Release",
            "ksf_video.node"
        )
    );

    console.log("KSF Video nativo carregado.");

    console.log(
        "KSF Video WGC suportado:",
        nativeVideo.isVideoCaptureSupported()
    );

}
catch (error) {

    console.error(
        "KSF Video nativo não pôde ser carregado:",
        error
    );

    nativeVideo = null;
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
let updateCheckMode = "manual";
let automaticUpdateCheckTimer = null;

/*
================================
PRESENÇA ONLINE KSF
================================
*/

let ksfPresenceHeartbeatTimer = null;

const KSF_PRESENCE_HEARTBEAT_MS =
    45 * 1000;


function stopKsfPresenceHeartbeat() {

    if (ksfPresenceHeartbeatTimer) {

        clearInterval(
            ksfPresenceHeartbeatTimer
        );

        ksfPresenceHeartbeatTimer =
            null;

    }
}


async function sendKsfPresenceHeartbeat() {

    if (
        !mainWindow ||
        mainWindow.isDestroyed() ||
        mainWindow.webContents.isDestroyed()
    ) {

        return;

    }


    try {

        await mainWindow.webContents
            .executeJavaScript(
                `
                (async () => {
                    const token =
                        localStorage.getItem(
                            "ksfAuthToken"
                        );

                    if (!token) {
                        return false;
                    }

                    try {
                        const response =
                            await fetch(
                                "https://ksf-screen.onrender.com/api/auth/me",
                                {
                                    method: "GET",
                                    headers: {
                                        "Authorization":
                                            "Bearer " + token
                                    }
                                }
                            );

                        return response.ok;
                    }
                    catch (error) {
                        return false;
                    }
                })();
                `,
                true
            );

    }
    catch (error) {

        console.error(
            "Erro no heartbeat de presença KSF:",
            error
        );

    }
}


function startKsfPresenceHeartbeat() {

    stopKsfPresenceHeartbeat();

    sendKsfPresenceHeartbeat();

    ksfPresenceHeartbeatTimer =
        setInterval(
            sendKsfPresenceHeartbeat,
            KSF_PRESENCE_HEARTBEAT_MS
        );
}


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

        frame: false,

        backgroundColor: "#111318",

        webPreferences: {
            nodeIntegration: true,
            contextIsolation: false
        }
    });


    /*
    ================================================
    KSF SCREEN - COMPATIBILIDADE DE CAPTURA

    O KSF continua 100% opaco.

    No Windows, o addon nativo aplica uma região
    COMPLEXREGION à própria janela do KSF. Isso evita
    depender do antigo teste de transparência e mantém
    o WGC como backend principal.
    ================================================
    */

    if (process.platform === "win32") {

        mainWindow.setOpacity(1.0);

    }


    let captureCompatibilityTimer = null;


    function getMainWindowHandleString() {

        if (
            !mainWindow ||
            mainWindow.isDestroyed()
        ) {

            return null;

        }


        try {

            const handleBuffer =
                mainWindow.getNativeWindowHandle();


            if (
                !Buffer.isBuffer(handleBuffer) ||
                handleBuffer.length < 4
            ) {

                return null;

            }


            let hwnd = 0n;


            for (
                let index = handleBuffer.length - 1;
                index >= 0;
                --index
            ) {

                hwnd =
                    (hwnd << 8n) |
                    BigInt(
                        handleBuffer[index]
                    );

            }


            if (hwnd <= 0n) {

                return null;

            }


            return hwnd.toString();

        }
        catch (error) {

            console.error(
                "Erro ao obter HWND do KSF Screen:",
                error
            );

            return null;

        }

    }


    function applyCaptureCompatibility() {

        if (
            process.platform !== "win32" ||
            !nativeVideo ||
            typeof nativeVideo
                .enableCaptureCompatibility !==
                "function" ||
            !mainWindow ||
            mainWindow.isDestroyed()
        ) {

            return;

        }


        const hwnd =
            getMainWindowHandleString();


        if (!hwnd) {

            return;

        }


        try {

            const result =
                nativeVideo
                    .enableCaptureCompatibility(
                        hwnd
                    );


            console.log(
                "KSF Capture Compatibility:",
                result
            );

        }
        catch (error) {

            console.error(
                "Erro ao aplicar KSF Capture Compatibility:",
                error
            );

        }

    }


    function scheduleCaptureCompatibility() {

        if (captureCompatibilityTimer) {

            clearTimeout(
                captureCompatibilityTimer
            );

        }


        captureCompatibilityTimer =
            setTimeout(
                () => {

                    captureCompatibilityTimer =
                        null;

                    applyCaptureCompatibility();

                },
                120
            );

    }


    mainWindow.loadFile("index.html");

    mainWindow.setMenuBarVisibility(false);


    mainWindow.webContents.once(
        "did-finish-load",
        () => {

            scheduleCaptureCompatibility();

            startKsfPresenceHeartbeat();

        }
    );


    mainWindow.on(
        "resize",
        () => {

            scheduleCaptureCompatibility();

        }
    );


    mainWindow.on(
        "maximize",
        () => {

            scheduleCaptureCompatibility();

        }
    );


    mainWindow.on(
        "unmaximize",
        () => {

            scheduleCaptureCompatibility();

        }
    );


    mainWindow.on(
        "restore",
        () => {

            scheduleCaptureCompatibility();

        }
    );


    mainWindow.on(
        "closed",
        () => {

            if (captureCompatibilityTimer) {

                clearTimeout(
                    captureCompatibilityTimer
                );

                captureCompatibilityTimer =
                    null;

            }


            mainWindow = null;

        }
    );
}


/*
================================
CONTROLES DA JANELA
================================
*/

ipcMain.handle(
    "window-minimize",
    async () => {

        if (
            mainWindow &&
            !mainWindow.isDestroyed()
        ) {

            mainWindow.minimize();

        }

        return true;

    }
);


ipcMain.handle(
    "window-toggle-maximize",
    async () => {

        if (
            !mainWindow ||
            mainWindow.isDestroyed()
        ) {

            return false;

        }


        if (mainWindow.isMaximized()) {

            mainWindow.unmaximize();

        }
        else {

            mainWindow.maximize();

        }


        return mainWindow.isMaximized();

    }
);


ipcMain.handle(
    "window-close",
    async () => {

        if (
            mainWindow &&
            !mainWindow.isDestroyed()
        ) {

            mainWindow.close();

        }

        return true;

    }
);


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
            automatic:
                updateCheckMode === "automatic",
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

            updateCheckMode = "manual";

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
VERIFICAÇÃO AUTOMÁTICA AO INICIAR
================================
*/

function checkForUpdatesAutomatically() {

    if (!app.isPackaged) {

        console.log(
            "Verificação automática ignorada em desenvolvimento."
        );

        return;

    }


    if (automaticUpdateCheckTimer) {

        clearTimeout(
            automaticUpdateCheckTimer
        );

    }


    automaticUpdateCheckTimer =
        setTimeout(
            async () => {

                automaticUpdateCheckTimer =
                    null;

                try {

                    updateCheckMode =
                        "automatic";

                    console.log(
                        "Verificando atualizações automaticamente..."
                    );

                    await autoUpdater
                        .checkForUpdates();

                }
                catch (error) {

                    console.error(
                        "Erro na verificação automática de atualização:",
                        error
                    );

                }

            },
            2500
        );
}


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
VÍDEO NATIVO - SUPORTE
================================
*/

ipcMain.handle(
    "is-native-video-supported",
    async () => {

        if (!nativeVideo) {

            return false;

        }


        try {

            return Boolean(
                nativeVideo
                    .isVideoCaptureSupported()
            );

        }
        catch (error) {

            console.error(
                "Erro ao consultar suporte do KSF Video:",
                error
            );

            return false;

        }

    }
);


/*
================================
VÍDEO NATIVO - INICIAR
================================
*/

ipcMain.handle(
    "start-native-window-video",
    async () => {

        if (!nativeVideo) {

            return {
                success: false,
                reason:
                    "native-video-unavailable"
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


        const hwnd =
            getWindowHandleFromSourceId(
                selectedDisplaySourceId
            );


        if (!hwnd) {

            return {
                success: false,
                reason:
                    "invalid-hwnd"
            };

        }


        try {

            console.log(
                "Iniciando KSF Video para HWND:",
                hwnd
            );


            const result =
                nativeVideo
                    .startWindowCapture(
                        hwnd
                    );


            console.log(
                "KSF Video iniciado:",
                result
            );


            return {
                ...result,

                sourceType:
                    "window",

                hwnd:
                    hwnd
            };

        }
        catch (error) {

            console.error(
                "Erro ao iniciar KSF Video:",
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
VÍDEO NATIVO - LER FRAME
================================
*/

ipcMain.handle(
    "read-native-window-video",
    async () => {

        if (!nativeVideo) {

            return {
                active: false,
                hasFrame: false,
                width: 0,
                height: 0,
                frameNumber: 0,
                data:
                    Buffer.alloc(0)
            };

        }


        try {

            return nativeVideo
                .readVideoFrame();

        }
        catch (error) {

            console.error(
                "Erro ao ler frame do KSF Video:",
                error
            );


            return {
                active: false,
                hasFrame: false,
                width: 0,
                height: 0,
                frameNumber: 0,
                data:
                    Buffer.alloc(0),
                error:
                    error.message
            };

        }

    }
);


/*
================================
VÍDEO NATIVO - PARAR
================================
*/

ipcMain.handle(
    "stop-native-window-video",
    async () => {

        if (!nativeVideo) {

            return {
                success: true,
                active: false
            };

        }


        try {

            const result =
                nativeVideo
                    .stopWindowCapture();


            console.log(
                "KSF Video parado:",
                result
            );


            return result;

        }
        catch (error) {

            console.error(
                "Erro ao parar KSF Video:",
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
VÍDEO NATIVO - STATUS
================================
*/

ipcMain.handle(
    "get-native-video-status",
    async () => {

        if (!nativeVideo) {

            return {
                available: false,
                active: false
            };

        }


        try {

            const status =
                nativeVideo
                    .getVideoCaptureStatus();


            return {
                available: true,
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


        const result =
            sources.map(
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

                        minimized:
                            false,

                        nativeOnly:
                            false,

                        thumbnail:
                            source.thumbnail
                                .toDataURL()

                    };

                }
            );


        /*
        ================================================
        COMPLEMENTO NATIVO DE JANELAS

        O desktopCapturer do Electron pode deixar de
        mostrar algumas janelas, principalmente quando
        estão minimizadas.

        O addon KSF Video enumera as janelas diretamente
        pelo Windows e acrescenta aqui as que estiverem
        faltando.

        Para manter compatibilidade com o restante do
        KSF Screen, a ID continua no formato:

        window:HWND:0
        ================================================
        */

        if (
            nativeVideo &&
            typeof nativeVideo
                .listCapturableWindows ===
                "function"
        ) {

            try {

                const nativeWindows =
                    nativeVideo
                        .listCapturableWindows();


                const knownWindowHandles =
                    new Set(
                        result
                            .filter(
                                item =>
                                    item.type ===
                                    "window"
                            )
                            .map(
                                item =>
                                    getWindowHandleFromSourceId(
                                        item.id
                                    )
                            )
                            .filter(Boolean)
                    );


                for (
                    const nativeWindow
                    of nativeWindows
                ) {

                    if (
                        !nativeWindow ||
                        typeof nativeWindow !==
                            "object"
                    ) {

                        continue;

                    }


                    const hwnd =
                        String(
                            nativeWindow.hwnd ||
                            ""
                        );


                    const title =
                        String(
                            nativeWindow.title ||
                            ""
                        ).trim();


                    if (
                        !hwnd ||
                        hwnd === "0" ||
                        !/^\d+$/.test(hwnd) ||
                        !title
                    ) {

                        continue;

                    }


                    if (
                        knownWindowHandles
                            .has(hwnd)
                    ) {

                        const existing =
                            result.find(
                                item =>
                                    item.type ===
                                        "window" &&
                                    getWindowHandleFromSourceId(
                                        item.id
                                    ) === hwnd
                            );


                        if (existing) {

                            existing.minimized =
                                Boolean(
                                    nativeWindow
                                        .minimized
                                );

                        }


                        continue;

                    }


                    const processId =
                        Number(
                            nativeWindow
                                .processId
                        );


                    result.push({

                        id:
                            `window:${hwnd}:0`,

                        name:
                            title,

                        type:
                            "window",

                        processId:
                            Number.isFinite(
                                processId
                            )
                                ? Math.floor(
                                    processId
                                )
                                : 0,

                        minimized:
                            Boolean(
                                nativeWindow
                                    .minimized
                            ),

                        nativeOnly:
                            true,

                        thumbnail:
                            null

                    });


                    knownWindowHandles
                        .add(hwnd);

                }

            }
            catch (error) {

                console.error(
                    "Erro ao listar janelas pelo KSF Video:",
                    error
                );

            }

        }


        return result;

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


        selectedDisplaySourceId =
            sourceId;


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

        if (automaticUpdateCheckTimer) {

            clearTimeout(
                automaticUpdateCheckTimer
            );

            automaticUpdateCheckTimer =
                null;

        }

        stopKsfPresenceHeartbeat();

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

        checkForUpdatesAutomatically();

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