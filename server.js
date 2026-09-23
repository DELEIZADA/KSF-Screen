const express = require("express");
const http = require("http");
const { Server } = require("socket.io");

const app = express();
const server = http.createServer(app);

const io = new Server(server, {
    cors: {
        origin: "*"
    }
});

// Local = 3000
// Online = usa automaticamente a porta fornecida pelo servidor
const PORT = process.env.PORT || 3000;

const rooms = new Map();


/*
================================
PÁGINA DE TESTE
================================
*/

app.get("/", (req, res) => {

    res.send(`
        <h1>KSF Screen Server</h1>
        <p>Servidor online e funcionando!</p>
    `);

});


/*
================================
SOCKET.IO
================================
*/

io.on("connection", (socket) => {

    console.log(
        "Novo usuário conectado:",
        socket.id
    );


    /*
    ================================
    CRIAR SALA
    ================================
    */

    socket.on(
        "create-room",
        (roomCode) => {

            roomCode =
                String(roomCode)
                    .trim()
                    .toUpperCase();


            if (rooms.has(roomCode)) {

                socket.emit(
                    "room-error",
                    "Essa sala já existe."
                );

                return;
            }


            rooms.set(
                roomCode,
                {
                    host: socket.id,
                    guest: null,

                    // ID de quem está transmitindo.
                    // null = ninguém transmitindo.
                    broadcaster: null
                }
            );


            socket.join(roomCode);


            socket.data.roomCode =
                roomCode;

            socket.data.isHost =
                true;


            console.log(
                "Sala criada:",
                roomCode
            );


            socket.emit(
                "room-created",
                roomCode
            );

        }
    );


    /*
    ================================
    ENTRAR NA SALA
    ================================
    */

    socket.on(
        "join-room",
        (roomCode) => {

            roomCode =
                String(roomCode)
                    .trim()
                    .toUpperCase();


            const room =
                rooms.get(roomCode);


            if (!room) {

                socket.emit(
                    "room-error",
                    "Sala não encontrada."
                );

                return;
            }


            if (room.guest) {

                socket.emit(
                    "room-error",
                    "Essa sala já está cheia."
                );

                return;
            }


            room.guest =
                socket.id;


            socket.join(
                roomCode
            );


            socket.data.roomCode =
                roomCode;

            socket.data.isHost =
                false;


            console.log(
                "Amigo entrou na sala:",
                roomCode
            );


            socket.emit(
                "room-joined",
                roomCode
            );


            io.to(
                room.host
            ).emit(
                "friend-joined"
            );


            /*
            Informa ao novo usuário
            se alguém já está transmitindo.
            */

            if (room.broadcaster) {

                socket.emit(
                    "broadcast-busy"
                );

            }

        }
    );


    /*
    ================================
    PEDIR PARA TRANSMITIR
    ================================
    */

    socket.on(
        "request-broadcast",
        () => {

            const roomCode =
                socket.data.roomCode;


            if (!roomCode) {
                return;
            }


            const room =
                rooms.get(roomCode);


            if (!room) {
                return;
            }


            /*
            Se outra pessoa já estiver
            transmitindo, bloqueia.
            */

            if (
                room.broadcaster &&
                room.broadcaster !== socket.id
            ) {

                socket.emit(
                    "broadcast-denied"
                );

                return;
            }


            /*
            Reserva a transmissão
            para este usuário.
            */

            room.broadcaster =
                socket.id;


            console.log(
                "Transmissão iniciada:",
                roomCode,
                socket.id
            );


            /*
            Quem pediu recebe autorização.
            */

            socket.emit(
                "broadcast-granted"
            );


            /*
            O outro usuário fica sabendo
            que a sala está ocupada.
            */

            socket
                .to(roomCode)
                .emit(
                    "broadcast-started"
                );

        }
    );


    /*
    ================================
    PARAR TRANSMISSÃO
    ================================
    */

    socket.on(
        "stop-broadcast",
        () => {

            const roomCode =
                socket.data.roomCode;


            if (!roomCode) {
                return;
            }


            const room =
                rooms.get(roomCode);


            if (!room) {
                return;
            }


            /*
            Somente quem está transmitindo
            pode liberar a transmissão.
            */

            if (
                room.broadcaster ===
                socket.id
            ) {

                room.broadcaster =
                    null;


                console.log(
                    "Transmissão encerrada:",
                    roomCode
                );


                /*
                Avisa o outro usuário que
                agora ele pode transmitir.
                */

                socket
                    .to(roomCode)
                    .emit(
                        "broadcast-stopped"
                    );

            }

        }
    );


    /*
    ================================
    SINALIZAÇÃO WEBRTC
    ================================
    */

    socket.on(
        "signal",
        (data) => {

            const roomCode =
                socket.data.roomCode;


            if (!roomCode) {
                return;
            }


            socket
                .to(roomCode)
                .emit(
                    "signal",
                    data
                );

        }
    );


    /*
    ================================
    DESCONEXÃO
    ================================
    */

    socket.on(
        "disconnect",
        () => {

            const roomCode =
                socket.data.roomCode;


            if (!roomCode) {
                return;
            }


            const room =
                rooms.get(roomCode);


            if (!room) {
                return;
            }


            /*
            Se quem saiu estava transmitindo,
            libera a transmissão.
            */

            if (
                room.broadcaster ===
                socket.id
            ) {

                room.broadcaster =
                    null;


                socket
                    .to(roomCode)
                    .emit(
                        "broadcast-stopped"
                    );

            }


            /*
            HOST SAIU
            */

            if (
                socket.data.isHost
            ) {

                socket
                    .to(roomCode)
                    .emit(
                        "host-left"
                    );


                rooms.delete(
                    roomCode
                );


                console.log(
                    "Sala encerrada:",
                    roomCode
                );

            }


            /*
            AMIGO SAIU
            */

            else {

                room.guest =
                    null;


                if (room.host) {

                    io.to(
                        room.host
                    ).emit(
                        "friend-left"
                    );

                }


                console.log(
                    "Amigo saiu da sala:",
                    roomCode
                );

            }

        }
    );

});


/*
================================
INICIAR SERVIDOR
================================
*/

server.listen(
    PORT,
    "0.0.0.0",
    () => {

        console.log("");
        console.log(
            "=============================="
        );

        console.log(
            "   KSF SCREEN SERVER V0.3"
        );

        console.log(
            "=============================="
        );

        console.log("");

        console.log(
            "Servidor funcionando na porta",
            PORT
        );

        console.log("");

    }
);