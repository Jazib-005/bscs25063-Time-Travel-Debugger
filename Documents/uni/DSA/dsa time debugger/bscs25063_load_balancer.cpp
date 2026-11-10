// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 
#include <iostream>
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <signal.h>

#ifdef _WIN32
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include "helper.h"
using namespace std;

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif


// ---- Constants ----
const char* SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front, * rear;
    int count;


public:
    // Implement these functions:
    Queue() {
        count = 0;
        front = rear = nullptr;
    }
    ~Queue()
    {
        while (front != nullptr) {
            Node* temp = front->next;
            delete front;
            front = temp;
        }

    }
    void enqueue(const T& val)
    {
        Node* new_node = new Node(val);
        if (isEmpty()) {
            front = rear = new_node;
            count++;
            return;
        }
        rear->next = new_node;
        rear = new_node;
        count++;
        return;

    }
    T dequeue()
    {
        if (isEmpty()) {
            throw::underflow_error("Error: Queue is Empty...");
        }
        Node* temp = front;
        front = front->next;
        if (front == nullptr) {
            rear = nullptr;
        }
        T value = temp->data;
        delete temp;
        count--;
        return value;

    }
    bool isEmpty()
    {
        if (count == 0) {
            return true;
        }
        return false;

    }
    int size()
    {
        return count;

    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char* buf, size_t n)
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.

    size_t total_sent = 0;
    while (total_sent < n) {
        int sent = send(sock, buf + total_sent, (int)(n - total_sent), MSG_NOSIGNAL);
        if (sent <= 0) {
            return false;
        }
        total_sent += sent;
    }
    return true;



}
bool sendError(int sock, const string& message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 

    char status = 1;
    int32_t checksum = calculateChecksum(0, &status, 1);
    checksum = calculateChecksum(checksum, message.c_str(), (int32_t)message.length());

    if (!sendAll(sock, &status, 1)) return false;
    if (!message.empty() && !sendAll(sock, message.c_str(), message.length())) return false;
    if (!sendAll(sock, (const char*)&checksum, sizeof(checksum))) return false;

    return true;



}
int createListenSocket(int port)
{
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.

    int Socket = socket(AF_INET, SOCK_STREAM, 0);

    if (Socket < 0) {
        cout << "Error:: Creation of listening socket failed" << endl;
        exit(1);
    }

    int opt = 1;
    if (setsockopt(Socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
        cout << "Error: Setsockopt SO_REUSEADDR failed" << endl;
#ifdef _WIN32
        closesocket(Socket);
#else
        close(Socket);
#endif

        exit(1);
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(Socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        cout << "bind failed" << endl;
#ifdef _WIN32
        closesocket(Socket);
#else
        close(sock);
#endif
        exit(1);
    }

    if (listen(Socket, LISTEN_BACKLOG) < 0) {
        cout << "listen failed" << endl;
#ifdef _WIN32
        closesocket(Socket);
#else
        close(sock);
#endif
        exit(1);
    }

    return Socket;

}
int connectToServer(const char* host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    int Socket = socket(AF_INET, SOCK_STREAM, 0);
    if (Socket < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
#ifdef _WIN32
        closesocket(Socket);
#else
        close(sock);
#endif
        exit(1);
    }

    if (connect(Socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(Socket);
#else
        close(sock);
#endif
        return -1;
    }
    return Socket;



}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.

    string command_text = string(SERVER_PATH) + " " + to_string(port) + " &";
    int system_result = system(command_text.c_str());

    if (system_result == 0) {
        return true;
    }
    else {
        return false;
    }

}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.

    int Socket = connectToServer(serverHost.c_str(), serverPort);

    if (Socket >= 0) {
        return Socket;
    }

    if (!startServer(serverPort)) {
        return -1;
    }

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) {
#ifdef _WIN32
        Sleep(STARTUP_POLL_INTERVAL_MS);
#else
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
#endif
        Socket = connectToServer(serverHost.c_str(), serverPort);
        if (Socket >= 0) {
            return Socket;
        }
    }

    return -1;


}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo& c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.

    if (time(NULL) - c.arrivedAt > 30) {
        sendError(c.sock, "client waited too long in queue");
#ifdef _WIN32
        closesocket(c.sock);
#else
        close(c.sock);
#endif
        return;
    }


    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0) {
        sendError(c.sock, "backend server failed to start");
#ifdef _WIN32
        closesocket(c.sock);
#else
        close(c.sock);
#endif
        return;
    }


    char buffer[IO_BUFFER_SIZE];
    int bytesRead = 0;

    while ((bytesRead = recv(c.sock, buffer, sizeof(buffer), 0)) > 0) {
        if (!sendAll(serverSock, buffer, bytesRead)) {
            break;
        }
    }

    shutdown(serverSock, SHUT_WR);

    while ((bytesRead = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
        if (!sendAll(c.sock, buffer, bytesRead)) {
            break;
        }
    }

#ifdef _WIN32
    closesocket(serverSock);
    closesocket(c.sock);
#else
    close(serverSock);
    close(c.sock);
#endif
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    while (flag) {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        int clientSock = accept(listenSock, (struct sockaddr*)&clientAddr, &addrLen);

        if (clientSock < 0) {
            if (!flag) break;
            continue;
        }

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        int port = ntohs(clientAddr.sin_port);

        ClientInfo c;
        c.sock = clientSock;
        c.address = string(ipStr);
        c.port = port;
        c.arrivedAt = time(NULL);

        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS) {
            queueMutex.unlock();
            sendError(clientSock, "server busy, try again later");
#ifdef _WIN32
            closesocket(clientSock);
#else
            close(clientSock);
#endif
        }
        else {
            pendingClients.enqueue(c);
            queueMutex.unlock();
        }
    }
}

void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.

    while (flag) {
        ClientInfo c;
        bool hasClient = false;

        queueMutex.lock();
        if (!pendingClients.isEmpty()) {
            c = pendingClients.dequeue();
            hasClient = true;
        }
        queueMutex.unlock();

        if (hasClient) {
            relayOneClient(c);
        }
        else {
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
        }
    }

}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char* argv[])
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().

    if (argc >= 2) serverHost = argv[1];
    if (argc >= 3) serverPort = atoi(argv[2]);
    if (argc >= 4) lbPort = atoi(argv[3]);

    int listenSock = createListenSocket(lbPort);
    cout << "Load Balancer running on port " << lbPort << endl;

    thread accThread(acceptLoop, listenSock);
    accThread.detach();

    dispatcherLoop();

#ifdef _WIN32
    closesocket(listenSock);
    WSACleanup();
#else
    close(listenSock);
#endif


    return 0;
}