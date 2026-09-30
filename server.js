const express = require("express");
const http = require("http");
const { Server } = require("socket.io");
const { Pool } = require("pg");
const bcrypt = require("bcryptjs");

const app = express();

app.use(
    express.json({
        limit: "16kb"
    })
);

const server = http.createServer(app);


/*
================================
BANCO DE DADOS POSTGRESQL
================================
*/

const databaseUrl =
    process.env.DATABASE_URL;

const db = databaseUrl
    ? new Pool({
        connectionString:
            databaseUrl,

        ssl: {
            rejectUnauthorized:
                false
        }
    })
    : null;


async function testDatabaseConnection() {

    if (!db) {

        console.log(
            "Banco de dados PostgreSQL: DATABASE_URL não configurada"
        );

        return;
    }

    try {

        await db.query(
            "SELECT 1"
        );

        console.log(
            "Banco de dados PostgreSQL: CONECTADO"
        );

    }

    catch (error) {

        console.error(
            "Banco de dados PostgreSQL: ERRO DE CONEXÃO",
            error.message
        );

    }

}


/*
================================
SOCKET.IO
================================
*/

const io =
    new Server(
        server,
        {
            cors: {
                origin: "*"
            }
        }
    );


// Local = 3000
// Online = usa automaticamente a porta fornecida pelo servidor
const PORT =
    process.env.PORT ||
    3000;


/*
================================
SALAS
================================
*/

const rooms =
    new Map();


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
            <p>KSF Screen Server V0.7.0</p>
        `);

    }
);


/*
================================
CADASTRAR CONTA
================================
*/

app.post(
    "/api/auth/register",
    async (req, res) => {

        if (!db) {

            return res
                .status(503)
                .json({
                    success: false,
                    message:
                        "Banco de dados indisponível."
                });

        }


        const username =
            typeof req.body?.username ===
                "string"

                ? req.body.username.trim()
                : "";


        const password =
            typeof req.body?.password ===
                "string"

                ? req.body.password
                : "";


        if (
            username.length < 3 ||
            username.length > 24
        ) {

            return res
                .status(400)
                .json({
                    success: false,
                    message:
                        "O nome de usuário deve ter entre 3 e 24 caracteres."
                });

        }


        if (
            !/^[A-Za-z0-9_.-]+$/.test(
                username
            )
        ) {

            return res
                .status(400)
                .json({
                    success: false,
                    message:
                        "O nome de usuário pode usar apenas letras, números, ponto, hífen e underline."
                });

        }


        if (
            password.length < 8 ||
            password.length > 72
        ) {

            return res
                .status(400)
                .json({
                    success: false,
                    message:
                        "A senha deve ter entre 8 e 72 caracteres."
                });

        }


        try {

            const existingUser =
                await db.query(
                    `
                    SELECT id
                    FROM ksf_users
                    WHERE LOWER(username) = LOWER($1)
                    LIMIT 1
                    `,
                    [
                        username
                    ]
                );


            if (
                existingUser.rowCount >
                0
            ) {

                return res
                    .status(409)
                    .json({
                        success: false,
                        message:
                            "Esse nome de usuário já está em uso."
                    });

            }


            const passwordHash =
                await bcrypt.hash(
                    password,
                    12
                );


            const result =
                await db.query(
                    `
                    INSERT INTO ksf_users
                    (
                        username,
                        password_hash
                    )
                    VALUES
                    (
                        $1,
                        $2
                    )
                    RETURNING
                        ksf_id,
                        username,
                        created_at
                    `,
                    [
                        username,
                        passwordHash
                    ]
                );


            const user =
                result.rows[0];


            console.log(
                "Nova conta criada:",
                user.ksf_id
            );


            return res
                .status(201)
                .json({
                    success: true,

                    user: {
                        ksfId:
                            user.ksf_id,

                        username:
                            user.username,

                        createdAt:
                            user.created_at
                    }
                });

        }

        catch (error) {

            console.error(
                "Erro ao criar conta:",
                error.message
            );


            if (
                error.code ===
                "23505"
            ) {

                return res
                    .status(409)
                    .json({
                        success: false,
                        message:
                            "Esse nome de usuário já está em uso."
                    });

            }


            return res
                .status(500)
                .json({
                    success: false,
                    message:
                        "Não foi possível criar a conta."
                });

        }

    }
);


/*
================================
ENTRAR NA CONTA
================================
*/

app.post(
    "/api/auth/login",
    async (req, res) => {

        if (!db) {

            return res
                .status(503)
                .json({
                    success: false,
                    message:
                        "Banco de dados indisponível."
                });

        }


        const identifier =
            typeof req.body?.identifier ===
                "string"

                ? req.body.identifier.trim()
                : "";


        const password =
            typeof req.body?.password ===
                "string"

                ? req.body.password
                : "";


        if (
            !identifier ||
            !password
        ) {

            return res
                .status(400)
                .json({
                    success: false,
                    message:
                        "Informe seu nome de usuário ou ID KSF e sua senha."
                });

        }


        try {

            const result =
                await db.query(
                    `
                    SELECT
                        ksf_id,
                        username,
                        password_hash,
                        recovery_email,
                        recovery_email_verified,
                        created_at
                    FROM ksf_users
                    WHERE
                        LOWER(username) = LOWER($1)
                        OR UPPER(ksf_id) = UPPER($1)
                    LIMIT 1
                    `,
                    [
                        identifier
                    ]
                );


            if (
                result.rowCount ===
                0
            ) {

                return res
                    .status(401)
                    .json({
                        success: false,
                        message:
                            "Nome de usuário, ID KSF ou senha incorretos."
                    });

            }


            const user =
                result.rows[0];


            const passwordMatches =
                await bcrypt.compare(
                    password,
                    user.password_hash
                );


            if (!passwordMatches) {

                return res
                    .status(401)
                    .json({
                        success: false,
                        message:
                            "Nome de usuário, ID KSF ou senha incorretos."
                    });

            }


            console.log(
                "Login realizado:",
                user.ksf_id
            );


            return res
                .status(200)
                .json({
                    success: true,

                    user: {
                        ksfId:
                            user.ksf_id,

                        username:
                            user.username,

                        recoveryEmailVerified:
                            Boolean(
                                user.recovery_email_verified
                            ),

                        createdAt:
                            user.created_at
                    }
                });

        }

        catch (error) {

            console.error(
                "Erro ao entrar na conta:",
                error.message
            );


            return res
                .status(500)
                .json({
                    success: false,
                    message:
                        "Não foi possível entrar na conta."
                });

        }

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
        roomCode ||
        ""
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
            rooms.get(
                roomCode
            ) ||
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


    room.nextParticipantNumber +=
        1;


    return {

        id:
            socket.id,

        name:
            "Amigo " +
            participantNumber,

        isHost:
            Boolean(
                isHost
            ),

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
        rooms.get(
            roomCode
        );


    if (!room) {
        return;
    }


    io
        .to(
            roomCode
        )
        .emit(
            "room-state",
            {
                roomCode,

                participants:
                    getRoomState(
                        room
                    )
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
                    roomCode.length !==
                    6
                ) {

                    socket.emit(
                        "room-error",
                        "Código de sala inválido."
                    );

                    return;
                }


                if (
                    rooms.has(
                        roomCode
                    )
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
                    .to(
                        roomCode
                    )
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
                    .to(
                        broadcasterId
                    )
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
                    .to(
                        broadcasterId
                    )
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


                if (
                    !isParticipantInRoom(
                        room,
                        targetId
                    )
                ) {

                    return;
                }


                io
                    .to(
                        targetId
                    )
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
        SAIR DA SALA SEM DESCONECTAR
        ================================
        */

        socket.on(
            "leave-room",
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

                    socket.data.roomCode =
                        null;

                    socket.data.isHost =
                        false;

                    socket.emit(
                        "room-left"
                    );

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
                    "Participante saiu voluntariamente:",
                    roomCode,
                    participant
                        ? participant.name
                        : socket.id
                );


                socket
                    .to(
                        roomCode
                    )
                    .emit(
                        "participant-left",
                        {
                            participantId:
                                socket.id
                        }
                    );


                if (
                    room.hostId ===
                        socket.id
                ) {

                    socket
                        .to(
                            roomCode
                        )
                        .emit(
                            "host-left"
                        );


                    for (
                        const participantId
                        of room.participants.keys()
                    ) {

                        const participantSocket =
                            io.sockets.sockets.get(
                                participantId
                            );


                        if (
                            !participantSocket
                        ) {
                            continue;
                        }


                        participantSocket.data.roomCode =
                            null;

                        participantSocket.data.isHost =
                            false;

                        participantSocket.leave(
                            roomCode
                        );

                    }


                    rooms.delete(
                        roomCode
                    );


                    console.log(
                        "Sala encerrada pelo dono:",
                        roomCode
                    );

                }

                else {

                    socket.leave(
                        roomCode
                    );


                    if (
                        room.participants.size ===
                            0
                    ) {

                        rooms.delete(
                            roomCode
                        );

                    }

                    else {

                        emitRoomState(
                            roomCode
                        );

                    }

                }


                socket.data.roomCode =
                    null;

                socket.data.isHost =
                    false;


                socket.leave(
                    roomCode
                );


                socket.emit(
                    "room-left"
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
                    .to(
                        roomCode
                    )
                    .emit(
                        "participant-left",
                        {
                            participantId:
                                socket.id
                        }
                    );


                if (
                    room.hostId ===
                    socket.id
                ) {

                    socket
                        .to(
                            roomCode
                        )
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
    async () => {

        await testDatabaseConnection();

        console.log("");

        console.log(
            "=============================="
        );

        console.log(
            "   KSF SCREEN SERVER V0.7.0"
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