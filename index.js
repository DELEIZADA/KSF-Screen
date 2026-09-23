const {
    app,
    BrowserWindow,
    ipcMain,
    desktopCapturer,
    session
} = require("electron");

const { autoUpdater } = require("electron-updater");


/*
================================
VARIÁVEIS GERAIS
================================
*/

let selectedDisplaySourceId = null;

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

    // Remove o menu superior do Electron
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
PARA A INTERFACE
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

    /*
    Não baixa atualização automaticamente.

    Primeiro o aplicativo avisa que existe
    uma versão nova.

    O usuário escolhe quando baixar.
    */

    autoUpdater.autoDownload = false;

    /*
    Não instala automaticamente
    quando o aplicativo for fechado.

    A instalação será iniciada
    pelo botão "Atualizar e reiniciar".
    */

    autoUpdater.autoInstallOnAppQuit = false;


    /*
    Procurando atualização
    */

    autoUpdater.on(
        "checking-for-update",
        () => {

            sendUpdateStatus(
                "checking"
            );

        }
    );


    /*
    Existe atualização nova
    */

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


    /*
    Já está atualizado
    */

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


    /*
    Progresso do download
    */

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


    /*
    Download concluído
    */

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


    /*
    Erro no sistema de atualização
    */

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

        /*
        Durante npm start o aplicativo
        não está instalado/empacotado.

        Portanto o updater real só funciona
        na versão instalada.
        */

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


        /*
        true  = fecha o aplicativo
        true  = executa o instalador
        */

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
VERSÃO ATUAL DO KSF SCREEN
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
CAPTURA DE TELAS E JANELAS
================================
*/

ipcMain.handle(
    "get-screen-sources",
    async () => {

        const sources =
            await desktopCapturer.getSources({
                types: [
                    "screen",
                    "window"
                ],

                thumbnailSize: {
                    width: 320,
                    height: 180
                },

                fetchWindowIcons: false
            });


        return sources.map(
            source => ({
                id: source.id,

                name: source.name,

                thumbnail:
                    source.thumbnail.toDataURL()
            })
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

        if (
            typeof sourceId !== "string" ||
            sourceId.length === 0
        ) {

            selectedDisplaySourceId =
                null;

            return false;
        }


        selectedDisplaySourceId =
            sourceId;


        return true;
    }
);


/*
================================
DISPLAY MEDIA + ÁUDIO DO WINDOWS
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

                    /*
                    Só aceita pedidos que realmente
                    solicitaram vídeo.
                    */

                    if (
                        !request.videoRequested
                    ) {

                        callback(null);

                        return;
                    }


                    /*
                    Busca novamente as fontes atuais
                    para garantir que a fonte escolhida
                    ainda existe.
                    */

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


                    /*
                    No Windows:

                    loopback =
                    captura o áudio do sistema.

                    A reprodução local continua
                    funcionando normalmente.
                    */

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

        if (
            process.platform !==
            "darwin"
        ) {

            app.quit();

        }

    }
);