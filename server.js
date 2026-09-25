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
const PORT =
    process.env.PORT || 3000;


/*
================================
SALAS

Cada sala possui:

{
    hostId: "socket-id",

    nextParticipantNumber: 4,

    participants: Map {
        socketId => {
            id,
            name,
            isHost,
            broadcasting
        }
    }
}
================================
*/

const rooms = new Map();


/*
================================
PÁGINA DE TESTE
================================
*/

app.get(
    "/",
    (req, res) => {

        res.send(`
            <h1>KSF Screen Server</h1>
            <p>Servidor Multi-Stream online e funcionando!</p>
            <p>KSF Screen Server V0.6.1 FIX</p>
        `);

    }
);


/*
================================
NORMALIZAR CÓDIGO DA SALA
================================
*/

function normalizeRoomCode(
    roomCode
) {

    return String(
        roomCode || ""
    )
        .trim()
        .toUpperCase();

}


/*
================================
PEGAR SALA DO SOCKET
================================
*/

function getSocketRoom(
    socket
) {

    const roomCode =
        socket.data.roomCode;


    if (!roomCode) {

        return {
            roomCode: null,
            room: null
        };

    }


    return {
        roomCode,
        room:
            rooms.get(roomCode) ||
            null
    };

}


/*
================================
CRIAR DADOS DO PARTICIPANTE
================================
*/

function createParticipant(
    socket,
    room,
    isHost
) {

    const participantNumber =
        room.nextParticipantNumber;


    room.nextParticipantNumber += 1;


    return {

        id:
            socket.id,

        name:
            "Amigo " +
            participantNumber,

        isHost:
            Boolean(isHost),

        broadcasting:
            false

    };

}


/*
================================
ESTADO PÚBLICO DA SALA
================================
*/

function getRoomState(
    room
) {

    return Array
        .from(
            room.participants.values()
        )
        .map(
            participant => ({

                id:
                    participant.id,

                name:
                    participant.name,

                isHost:
                    participant.isHost,

                broadcasting:
                    participant.broadcasting

            })
        );

}


/*
================================
ENVIAR ESTADO DA SALA
================================
*/

function emitRoomState(
    roomCode
) {

    const room =
        rooms.get(roomCode);


    if (!room) {
        return;
    }


    io
        .to(roomCode)
        .emit(
            "room-state",
            {
                roomCode,

                participants:
                    getRoomState(room)
            }
        );

}


/*
================================
VALIDAR PARTICIPANTE DA SALA
================================
*/

function isParticipantInRoom(
    room,
    socketId
) {

    return Boolean(
        room &&
        socketId &&
        room.participants.has(
            socketId
        )
    );

}


/*
================================
SOCKET.IO
================================
*/

io.on(
    "connection",
    socket => {

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
            rawRoomCode => {

                const roomCode =
                    normalizeRoomCode(
                        rawRoomCode
                    );


                if (
                    roomCode.length !== 6
                ) {

                    socket.emit(
                        "room-error",
                        "Código de sala inválido."
                    );

                    return;
                }


                if (
                    rooms.has(roomCode)
                ) {

                    socket.emit(
                        "room-error",
                        "Essa sala já existe."
                    );

                    return;
                }


                const room = {

                    hostId:
                        socket.id,

                    nextParticipantNumber:
                        1,

                    participants:
                        new Map()

                };


                const participant =
                    createParticipant(
                        socket,
                        room,
                        true
                    );


                room.participants.set(
                    socket.id,
                    participant
                );


                rooms.set(
                    roomCode,
                    room
                );


                socket.join(
                    roomCode
                );


                socket.data.roomCode =
                    roomCode;

                socket.data.isHost =
                    true;


                console.log(
                    "Sala criada:",
                    roomCode,
                    "| Host:",
                    socket.id
                );


                socket.emit(
                    "room-created",
                    {
                        roomCode,

                        selfId:
                            socket.id,

                        participant
                    }
                );


                emitRoomState(
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
            rawRoomCode => {

                const roomCode =
                    normalizeRoomCode(
                        rawRoomCode
                    );


                const room =
                    rooms.get(
                        roomCode
                    );


                if (!room) {

                    socket.emit(
                        "room-error",
                        "Sala não encontrada."
                    );

                    return;
                }


                if (
                    room.participants.has(
                        socket.id
                    )
                ) {

                    socket.emit(
                        "room-joined",
                        {
                            roomCode,

                            selfId:
                                socket.id,

                            participant:
                                room.participants.get(
                                    socket.id
                                )
                        }
                    );

                    emitRoomState(
                        roomCode
                    );

                    return;
                }


                const participant =
                    createParticipant(
                        socket,
                        room,
                        false
                    );


                room.participants.set(
                    socket.id,
                    participant
                );


                socket.join(
                    roomCode
                );


                socket.data.roomCode =
                    roomCode;

                socket.data.isHost =
                    false;


                console.log(
                    "Participante entrou:",
                    roomCode,
                    participant.name,
                    socket.id
                );


                socket.emit(
                    "room-joined",
                    {
                        roomCode,

                        selfId:
                            socket.id,

                        participant
                    }
                );


                emitRoomState(
                    roomCode
                );

            }
        );


        /*
        ================================
        COMEÇAR TRANSMISSÃO
        ================================
        */

        socket.on(
            "start-broadcast",
            () => {

                const {
                    roomCode,
                    room
                } =
                    getSocketRoom(
                        socket
                    );


                if (
                    !roomCode ||
                    !room
                ) {
                    return;
                }


                const participant =
                    room.participants.get(
                        socket.id
                    );


                if (!participant) {
                    return;
                }


                participant.broadcasting =
                    true;


                console.log(
                    "Transmissão iniciada:",
                    roomCode,
                    participant.name,
                    socket.id
                );


                socket.emit(
                    "broadcast-started-self"
                );


                emitRoomState(
                    roomCode
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

                const {
                    roomCode,
                    room
                } =
                    getSocketRoom(
                        socket
                    );


                if (
                    !roomCode ||
                    !room
                ) {
                    return;
                }


                const participant =
                    room.participants.get(
                        socket.id
                    );


                if (!participant) {
                    return;
                }


                if (
                    !participant.broadcasting
                ) {
                    return;
                }


                participant.broadcasting =
                    false;


                console.log(
                    "Transmissão encerrada:",
                    roomCode,
                    participant.name,
                    socket.id
                );


                socket
                    .to(roomCode)
                    .emit(
                        "participant-broadcast-stopped",
                        {
                            participantId:
                                socket.id
                        }
                    );


                emitRoomState(
                    roomCode
                );

            }
        );


        /*
        ================================
        PEDIR PARA ASSISTIR
        ================================
        */

        socket.on(
            "watch-stream",
            data => {

                const {
                    roomCode,
                    room
                } =
                    getSocketRoom(
                        socket
                    );


                if (
                    !roomCode ||
                    !room
                ) {
                    return;
                }


                const broadcasterId =
                    data &&
                    typeof data.broadcasterId ===
                        "string"

                        ? data.broadcasterId
                        : null;


                if (
                    !broadcasterId ||
                    broadcasterId ===
                        socket.id
                ) {

                    return;
                }


                const broadcaster =
                    room.participants.get(
                        broadcasterId
                    );


                if (
                    !broadcaster ||
                    !broadcaster.broadcasting
                ) {

                    socket.emit(
                        "watch-error",
                        {
                            broadcasterId,

                            message:
                                "Essa transmissão não está mais disponível."
                        }
                    );

                    return;
                }


                console.log(
                    "Pedido para assistir:",
                    socket.id,
                    "->",
                    broadcasterId
                );


                io
                    .to(broadcasterId)
                    .emit(
                        "viewer-request",
                        {
                            viewerId:
                                socket.id
                        }
                    );

            }
        );


        /*
        ================================
        PARAR DE ASSISTIR
        ================================
        */

        socket.on(
            "stop-watching",
            data => {

                const {
                    roomCode,
                    room
                } =
                    getSocketRoom(
                        socket
                    );


                if (
                    !roomCode ||
                    !room
                ) {
                    return;
                }


                const broadcasterId =
                    data &&
                    typeof data.broadcasterId ===
                        "string"

                        ? data.broadcasterId
                        : null;


                if (
                    !broadcasterId ||
                    !isParticipantInRoom(
                        room,
                        broadcasterId
                    )
                ) {

                    return;
                }


                console.log(
                    "Parou de assistir:",
                    socket.id,
                    "->",
                    broadcasterId
                );


                io
                    .to(broadcasterId)
                    .emit(
                        "viewer-left",
                        {
                            viewerId:
                                socket.id
                        }
                    );

            }
        );


        /*
        ================================
        SINALIZAÇÃO WEBRTC DIRECIONADA

        V0.6.1:
        connectionId diferencia duas conexões
        simultâneas entre os mesmos usuários.

        Exemplo:
        A transmite para B
        enquanto
        B transmite para A.
        ================================
        */

        socket.on(
            "signal",
            data => {

                const {
                    room,
                    roomCode
                } =
                    getSocketRoom(
                        socket
                    );


                if (
                    !room ||
                    !roomCode ||
                    !data
                ) {
                    return;
                }


                const targetId =
                    typeof data.targetId ===
                        "string"

                        ? data.targetId
                        : null;


                if (
                    !targetId ||
                    targetId ===
                        socket.id
                ) {

                    return;
                }


                /*
                Impede sinalização para alguém
                que não pertence à mesma sala.
                */

                if (
                    !isParticipantInRoom(
                        room,
                        targetId
                    )
                ) {

                    return;
                }


                /*
                IMPORTANTE:

                Além do sourceId, repassamos
                connectionId.

                Assim o cliente consegue saber
                a qual RTCPeerConnection pertencem
                offer, answer e ICE.

                Isso evita misturar:

                transmissão A -> B

                com

                transmissão B -> A.
                */

                io
                    .to(targetId)
                    .emit(
                        "signal",
                        {
                            sourceId:
                                socket.id,

                            type:
                                data.type,

                            connectionId:
                                data.connectionId,

                            sdp:
                                data.sdp,

                            candidate:
                                data.candidate
                        }
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
                    rooms.get(
                        roomCode
                    );


                if (!room) {
                    return;
                }


                const participant =
                    room.participants.get(
                        socket.id
                    );


                room.participants.delete(
                    socket.id
                );


                console.log(
                    "Participante saiu:",
                    roomCode,
                    participant
                        ? participant.name
                        : socket.id
                );


                socket
                    .to(roomCode)
                    .emit(
                        "participant-left",
                        {
                            participantId:
                                socket.id
                        }
                    );


                /*
                HOST SAIU
                */

                if (
                    room.hostId ===
                    socket.id
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


                    return;
                }


                if (
                    room.participants.size ===
                    0
                ) {

                    rooms.delete(
                        roomCode
                    );

                    return;
                }


                emitRoomState(
                    roomCode
                );

            }
        );

    }
);


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
            "   KSF SCREEN SERVER V0.6.1 FIX"
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

        console.log(
            "Multi-Stream: ATIVO"
        );

        console.log("");

    }
);