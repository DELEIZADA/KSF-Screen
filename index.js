const {
    app,
    BrowserWindow,
    ipcMain,
    desktopCapturer
} = require("electron");

function createWindow() {

    const win = new BrowserWindow({
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

    win.loadFile("index.html");

    // Remove o menu superior do Electron
    win.setMenuBarVisibility(false);
}


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

                fetchWindowIcons: true
            });

        return sources.map(source => ({
            id: source.id,

            name: source.name,

            thumbnail:
                source.thumbnail.toDataURL(),

            appIcon:
                source.appIcon
                    ? source.appIcon.toDataURL()
                    : null
        }));
    }
);


/*
================================
INICIALIZAÇÃO
================================
*/

app.whenReady().then(() => {

    createWindow();

});


app.on(
    "window-all-closed",
    () => {

        if (
            process.platform !== "darwin"
        ) {
            app.quit();
        }

    }
);