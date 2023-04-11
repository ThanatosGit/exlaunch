#include <lib.hpp>

#include "remote_api.hpp"

#include "static_thread.hpp"
#include "event_helper.hpp"

#include <nn.hpp>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include <atomic>


/**
 * Ryujinx networking is sadly very limited.
 * 1. Any blocking operation on any socket blocks all other operations on any socket. e.g. you can not recv on one thread and send on another.
 *      It doesn't matter if you use different operations like recv/send/accept and even not if you try to use it on different sockets.
 *      => only one blocking call
 * 2. errno does not work correctly. It does not return EWOULDBLOCK or EAGAIN.
 *      It returns length = -1 on recv for a connected socket and 0 for a non connected but only if gracefully shutdown
 *      otherwise we stuck forever and can not reconnect.
 *      That's why a manual keep-alive is implemented
 * 3. Setting the socket to non-blocking doesn't work (at least not via fcntl)
 *      Non-blocking works for recv as flag!
*/

struct ClientSubscriptions RemoteApi::clientSubs;

namespace {
    static s32 g_TcpSocket = -1;
    static s32 clientSocket = -1;
    static std::atomic<bool> readyForGameThread = false;
    u8 requestNumber = 0;

    constexpr inline auto DefaultTcpAutoBufferSizeMax      = 192 * 1024; /* 192kb */
    constexpr inline auto MinTransferMemorySize            = (2 * DefaultTcpAutoBufferSizeMax + (128 * 1024));
    constexpr inline auto MinSocketAllocatorSize           = 128 * 1024;

    constexpr inline auto SocketAllocatorSize = MinSocketAllocatorSize * 1;
    constexpr inline auto TransferMemorySize = MinTransferMemorySize * 1;

    constexpr inline auto SocketPoolSize = SocketAllocatorSize + TransferMemorySize;
    constexpr inline auto SocketPort = 6969;

    constexpr inline auto ResetValueAliveTimer = 5000; // client should send alive packet every 2 seconds. loops all 2 ms to decrease. invalidate afte 10 s => 10 000 ms / 2 ms = 5000 cycles

    static shimmer::util::StaticThread<0x4000> SocketSpawnThread;
    static RemoteApi::CommandBuffer RecvBuffer;
    static size_t RecvBufferLength = 0;
    SendBuffer sendBuffer;
    static int keepAlive;
}; // namespace
static nn::os::MutexType sendBufferLock;


void PrepareThread() {
    nn::os::InitializeMutex(&sendBufferLock, false, 0);

    void* pool = aligned_alloc(0x4000, SocketPoolSize);
    R_ABORT_UNLESS(nn::socket::Initialize(pool, SocketPoolSize, SocketAllocatorSize, 14));

    /* Open socket. */
    g_TcpSocket = nn::socket::Socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_TcpSocket < 0) EXL_ABORT(69);

    /* Set socket to keep-alive. */
    // int flags = true;
    // nn::socket::SetSockOpt(g_TcpSocket, SOL_SOCKET, SO_KEEPALIVE, &flags, sizeof(flags));

    /* Open and wait for connection. */
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = nn::socket::InetHtons(SocketPort);

    int rval = nn::socket::Bind(g_TcpSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    if (rval < 0) EXL_ABORT(69);

    rval = nn::socket::Listen(g_TcpSocket, 1);
    if (rval < 0) EXL_ABORT(69);
}

void AddPacketToSendBuffer(PacketBuffer& buffer) {
    nn::os::LockMutex(&sendBufferLock);
    sendBuffer.push_back(std::move(buffer));
    nn::os::UnlockMutex(&sendBufferLock);
}

void ReceiveLogic() {
    // don't recv new packets while we waiting for game loop otherwise we need to make a copy of our receive buffer
    if (readyForGameThread.load()) return;
    keepAlive--;
    memset(RecvBuffer.data(), 0, RecvBuffer.size());

    ssize_t length = nn::socket::Recv(clientSocket, RecvBuffer.data(), 1, MSG_DONTWAIT);
    RecvBufferLength = length;
    if (length > 0) {
        // data received
        RemoteApi::ParseClientPacket();
    }
}

void SendLogic() {
    nn::os::LockMutex(&sendBufferLock);
    for (auto& pb : sendBuffer) {
        ssize_t ret = nn::socket::Send(clientSocket, pb->data(), pb->size(), 0);
        // mark them for removal
        if (ret > 0) {
            pb->clear();
        }
    }
    // erase everything with size of 0
    sendBuffer.erase(std::remove_if(sendBuffer.begin(), sendBuffer.end(), [](auto& pb) { 
        return pb->size() == 0;
    }),
                           sendBuffer.end());
    nn::os::UnlockMutex(&sendBufferLock);
}

void SocketSpawn(void *) {
    SocketSpawnThread.SetName("SocketSpawnThread");
    PrepareThread();

    bool looping = true;
    while (looping) {
        struct sockaddr clientAddr;
        u32 addrLen;
        requestNumber = 0;
        memset(&RemoteApi::clientSubs, 0, sizeof(struct ClientSubscriptions));
        clientSocket = nn::socket::Accept(g_TcpSocket, &clientAddr, &addrLen);
        keepAlive = ResetValueAliveTimer;
        while (looping) {
            // poll every 2 ms = 2000000 ns
            svcSleepThread(2000000);
            ReceiveLogic();
            SendLogic();

            if (keepAlive == 0) {
                // clean packet because there is no connection anymore
                nn::os::LockMutex(&sendBufferLock);
                svcSleepThread(100);
                sendBuffer.clear();
                nn::socket::Close(clientSocket);
                clientSocket = -1;
                nn::os::UnlockMutex(&sendBufferLock);
                break;
            }
        }
    }

    nn::socket::Close(g_TcpSocket);
    svcExitThread();
}

void RemoteApi::Init() {
    R_ABORT_UNLESS(SocketSpawnThread.Create(SocketSpawn));
    SocketSpawnThread.Start();
    // /* Inject hook. */
}

/* Checks if readyForGameThread is true and executes the callback in that case */
/* readyForGameThread is set to true if a packet with type PACKET_REMOTE_LUA_EXEC is received  */
void RemoteApi::ProcessCommand(lua_State* L, const std::function<PacketBuffer(lua_State* L, CommandBuffer &RecvBuffer, size_t RecvBufferLength)> &processor) {
    if (readyForGameThread.load()) {
        PacketBuffer buffer = processor(L, RecvBuffer, RecvBufferLength);
        buffer->insert(buffer->begin() + 1, requestNumber++);
        AddPacketToSendBuffer(buffer);
        readyForGameThread.store(false);
    }
}

void RemoteApi::SendMessage(lua_State* L, PacketType packetType, const std::function<PacketBuffer(lua_State* L, PacketType packetType)> &processor) {
    if (clientSocket > 0) {
        PacketBuffer buffer = processor(L, packetType);
        AddPacketToSendBuffer(buffer);
    }
}

void RemoteApi::ParseHandshake() {
    const char interestByte = RecvBuffer.data()[1];
    RemoteApi::clientSubs.logging = interestByte & 0x1;
    RemoteApi::clientSubs.multiWorld = (interestByte & 0x2) >> 1;

    PacketBuffer buffer(new std::vector<u8>());
    buffer->push_back(PACKET_HANDSHAKE);
    buffer->push_back(requestNumber++);

    AddPacketToSendBuffer(buffer);
}

void RemoteApi::ParseRemoteLuaExec() {
    // will be parsed in the next cycle by the game loop
    readyForGameThread.store(true);
}

void RemoteApi::ParseClientPacket() {
    int remainingBytes = 0;
    switch (RecvBuffer.data()[0]) {
    case PACKET_HANDSHAKE:
        RecvBufferLength += 1;
        nn::socket::Recv(clientSocket, RecvBuffer.data() + 1, 1, MSG_DONTWAIT);
        RemoteApi::ParseHandshake();
        break;
    case PACKET_REMOTE_LUA_EXEC:
        nn::socket::Recv(clientSocket, RecvBuffer.data() + 1, 4, MSG_DONTWAIT);
        memcpy(&remainingBytes, RecvBuffer.data() + 1, 4);
        nn::socket::Recv(clientSocket, RecvBuffer.data() + 5, remainingBytes, MSG_DONTWAIT);
        RecvBufferLength += 4 + remainingBytes;
        RemoteApi::ParseRemoteLuaExec();
        break;
    case PACKET_KEEP_ALIVE:
        keepAlive = ResetValueAliveTimer;
        break;
    }
}