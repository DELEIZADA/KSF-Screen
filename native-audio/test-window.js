const {
    app,
    BrowserWindow,
    desktopCapturer
} = require("electron");

const path = require("path");

const nativeAudio = require(
    path.join(
        __dirname,
        "build",
        "Release",
        "ksf_audio.node"
    )
);


async function testarJanelas() {

    console.log("");
    console.log("======================================");
    console.log(" KSF SCREEN - TESTE DE JANELAS");
    console.log("======================================");
    console.log("");


    console.log(
        "Process Loopback suportado:",
        nativeAudio.isProcessLoopbackSupported()
    );

    console.log("");


    const sources =
        await desktopCapturer.getSources({
            types: [
                "window",
                "screen"
            ],

            thumbnailSize: {
                width: 0,
                height: 0
            },

            fetchWindowIcons: false
        });


    console.log(
        `Fontes encontradas: ${sources.length}`
    );

    console.log("");


    for (const source of sources) {

        console.log("--------------------------------------");

        console.log(
            "Nome:",
            source.name
        );

        console.log(
            "ID:",
            source.id
        );


        if (
            typeof source.id === "string" &&
            source.id.startsWith("window:")
        ) {

            /*
            ========================================
            FORMATO ESPERADO

            window:ID_DA_JANELA:ID_DA_TELA

            Neste teste ainda NÃO assumimos
            definitivamente que o valor seja HWND.

            Vamos testar com o Windows e deixar
            nosso módulo validar usando IsWindow().
            ========================================
            */


            const partes =
                source.id.split(":");


            if (partes.length >= 3) {

                const windowId =
                    partes[1];


                console.log(
                    "Possível HWND:",
                    windowId
                );


                try {

                    const result =
                        nativeAudio
                            .getProcessIdFromHwnd(
                                windowId
                            );


                    console.log(
                        "HWND válido: SIM"
                    );

                    console.log(
                        "PID:",
                        result.pid
                    );

                }
                catch (error) {

                    console.log(
                        "HWND válido: NÃO"
                    );

                    console.log(
                        "Motivo:",
                        error.message
                    );

                }

            }

        }


        console.log("--------------------------------------");
        console.log("");

    }


    console.log("");
    console.log("======================================");
    console.log(" TESTE FINALIZADO");
    console.log("======================================");
    console.log("");


    setTimeout(
        () => {
            app.quit();
        },
        3000
    );
}


app.whenReady().then(
    async () => {

        /*
        Criamos uma janela invisível apenas para
        manter um contexto normal do Electron
        durante o teste.
        */

        const testWindow =
            new BrowserWindow({
                show: false,
                width: 400,
                height: 300
            });


        try {

            await testarJanelas();

        }
        catch (error) {

            console.error("");
            console.error(
                "ERRO NO TESTE:"
            );

            console.error(
                error
            );

            setTimeout(
                () => {
                    app.quit();
                },
                3000
            );

        }

    }
);


app.on(
    "window-all-closed",
    () => {

        app.quit();

    }
);