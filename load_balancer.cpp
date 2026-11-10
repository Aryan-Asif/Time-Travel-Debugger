// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code).
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not understand the messages. It only moves bytes, but it checks the checksum of every
// stream while forwarding (if the check fails the 4 checksum bytes are NOT forwarded, so the next
// receiver rejects the message).
// The only messages it writes itself are errors ("server busy", "backend server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
// Usage: ./load_balancer <server_ip> <server_port>

#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <mutex>
#include <stdexcept>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *SERVER_PATH = "./server";     // the program we start when the server is down
// (IO_BUFFER_SIZE and CLIENT_TIMEOUT_SEC are in helper.h)
const int LISTEN_BACKLOG = 16;            // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;        // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;      // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100; // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;

// ---- Queue: here it holds client sockets (int). ----
// Implemented by hand (linked list), no std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;

public:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (!isEmpty())
            dequeue();
    }
    void enqueue(const T &val)
    {
        Node *node = new Node;
        node->data = val;
        node->next = nullptr;

        if (rear == nullptr) // queue was empty
        {
            front = node;
            rear = node;
        }
        else
        {
            rear->next = node;
            rear = node;
        }
        count++;
    }
    T dequeue()
    {
        if (front == nullptr)
            throw runtime_error("dequeue on an empty queue");

        Node *node = front;
        T val = node->data;
        front = front->next;
        if (front == nullptr) // queue is empty now
            rear = nullptr;
        delete node;
        count--;
        return val;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;         // the connected socket
    string address;   // the client's IP as text, e.g. "127.0.0.1"
    int port;         // the client's port
    time_t arrivedAt; // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1;

// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // MSG_NOSIGNAL: a client that already left gives an error instead of killing the program.
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t r = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0)
            return false;
        sent += r;
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload][checksum: 4 bytes].
    // For an error: status 1, then the message text, then the CRC-32 of both.
    char status = 1;
    Crc32 crc;
    crc.add(&status, 1);
    crc.add(message.data(), message.size());
    uint32_t sum = crc.result();

    if (!sendAll(sock, &status, 1))
        return false;
    if (!sendAll(sock, message.data(), message.size()))
        return false;
    return sendAll(sock, (const char *)&sum, 4);
}
int createListenSocket(int port)
{
    // SOCK_CLOEXEC: the server that we start with system() must not inherit this socket.
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        perror("socket");
        exit(1);
    }

    // SO_REUSEADDR: we can restart the load balancer right away
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }
    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        exit(1);
    }
    return sock;
}
int connectToServer(const char *host, int port)
{
    // Return the socket, or -1 if the connection fails
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
    {
        close(sock);
        return -1;
    }
    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}
void setReceiveTimeout(int sock, int seconds)
{
    // a silent client or server must not freeze the load balancer forever
    timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// If we want to send an error before we have read the client's upload, we first read the upload to its end.
// Otherwise close() would reset the connection and the client would lose our error message.
void discardUpload(int sock)
{
    setReceiveTimeout(sock, CLIENT_TIMEOUT_SEC);
    char junk[IO_BUFFER_SIZE];
    while (recv(sock, junk, sizeof(junk), 0) > 0)
    {
    }
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================

bool startServer(int port)
{
    // Command text, for example: "./server 5001 &"
    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    int result = system(command.c_str());
    return result == 0;
}
int connectToServerWithWakeup()
{
    // 1. If the server is alive, we connect at once.
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0)
        return sock;

    // 2. Otherwise start it.
    cout << "server is down, starting it..." << endl;
    if (!startServer(serverPort))
        return -1;

    // 3. Give it time to open its port: up to 50 tries, 100 ms apart.
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
            return sock;
    }

    // 4. It never came up.
    return -1;
}

// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================

void relayOneClient(ClientInfo &c)
{
    // 1. Connect to the server (start it if needed).
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        discardUpload(c.sock);
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }
    setReceiveTimeout(c.sock, CLIENT_TIMEOUT_SEC);
    setReceiveTimeout(serverSock, CLIENT_TIMEOUT_SEC);

    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];

    // 2. Client -> server. Everything except the last 4 bytes (the checksum) is forwarded at once.
    TrailerChecker upload;
    while (true)
    {
        ssize_t n = recv(c.sock, in, IO_BUFFER_SIZE, 0);
        if (n <= 0) // 0 = the client finished sending, below 0 = error or timeout
            break;

        int released = upload.feed(in, n, out);
        if (released > 0 && !sendAll(serverSock, out, released))
        {
            discardUpload(c.sock);
            sendError(c.sock, "lost the connection to the backend server");
            close(serverSock);
            close(c.sock);
            return;
        }
    }

    // 3. Only a good upload gets its checksum forwarded. Without it the server rejects the message.
    bool uploadGood = upload.verify();
    if (uploadGood)
    {
        char trailer[4];
        upload.getTrailer(trailer);
        sendAll(serverSock, trailer, 4);
    }

    // 4. Tell the server that the string is complete.
    shutdown(serverSock, SHUT_WR);

    if (!uploadGood)
    {
        cout << "bad upload from " << c.address << ":" << c.port << " (checksum mismatch)" << endl;
        sendError(c.sock, "upload checksum mismatch");
        close(serverSock);
        close(c.sock);
        return;
    }

    // 5. Server -> client, the same way. If the reply is damaged we do not forward its checksum,
    //    so the client reports "response corrupted".
    TrailerChecker reply;
    bool gotAnything = false;
    bool clientGone = false;
    while (true)
    {
        ssize_t n = recv(serverSock, in, IO_BUFFER_SIZE, 0);
        if (n <= 0) // 0 = the server sent its whole reply and closed
            break;

        gotAnything = true;
        int released = reply.feed(in, n, out);
        if (released > 0 && !sendAll(c.sock, out, released))
        {
            clientGone = true;
            break;
        }
    }

    if (!clientGone)
    {
        if (!gotAnything)
        {
            sendError(c.sock, "backend server closed the connection without a reply");
        }
        else if (reply.verify())
        {
            char trailer[4];
            reply.getTrailer(trailer);
            sendAll(c.sock, trailer, 4);
        }
        else
        {
            cout << "reply from the server is damaged (checksum mismatch)" << endl;
        }
    }

    // 6. Close both sockets.
    close(serverSock);
    close(c.sock);
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================

void acceptLoop(int listenSock)
{
    while (flag)
    {
        sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);

        // accept4 with SOCK_CLOEXEC: the server started by system() must not inherit client sockets,
        // otherwise a client would never see the end of its connection.
        int sock = accept4(listenSock, (sockaddr *)&addr, &addrLen, SOCK_CLOEXEC);
        if (sock < 0)
        {
            if (flag)
                perror("accept");
            usleep(100 * 1000); // do not spin if accept() keeps failing
            continue;
        }

        ClientInfo c;
        c.sock = sock;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        c.address = ip;
        c.port = ntohs(addr.sin_port);
        c.arrivedAt = time(NULL);

        bool full = false;
        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
            full = true;
        else
            pendingClients.enqueue(c);
        queueMutex.unlock();

        if (full)
        {
            cout << "queue full, refusing " << c.address << ":" << c.port << endl;
            sendError(sock, "server busy, try again later");

            // throw away what the client already sent, so that close() does not reset the connection
            // (a reset could destroy the error message before the client reads it). Never waits.
            char junk[1024];
            while (recv(sock, junk, sizeof(junk), MSG_DONTWAIT) > 0)
            {
            }
            close(sock);
        }
        else
        {
            cout << "client " << c.address << ":" << c.port << " queued" << endl;
        }
    }
}
void dispatcherLoop()
{
    // This runs in the main thread until Ctrl+C.
    while (flag)
    {
        ClientInfo c;
        bool gotClient = false;

        queueMutex.lock();
        if (!pendingClients.isEmpty())
        {
            c = pendingClients.dequeue();
            gotClient = true;
        }
        queueMutex.unlock();

        if (!gotClient)
        {
            usleep(50 * 1000); // nothing to do, look again in 50 ms
            continue;
        }

        // The client gave up waiting after CLIENT_TIMEOUT_SEC seconds, so do not serve it: close, no reply.
        if (time(NULL) - c.arrivedAt > CLIENT_TIMEOUT_SEC)
        {
            cout << "client " << c.address << ":" << c.port << " waited too long, dropped" << endl;
            close(c.sock);
            continue;
        }

        cout << "serving " << c.address << ":" << c.port << endl;
        relayOneClient(c); // the next client is taken only after this one is finished
        cout << "finished " << c.address << ":" << c.port << endl;
    }
}

void handler(int sg)
{
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{
    // usage: ./load_balancer <server_ip> <server_port>
    if (argc >= 2)
        serverHost = argv[1];
    if (argc >= 3)
        serverPort = atoi(argv[2]);
    if (argc > 3 || serverPort < 1 || serverPort > 65535)
    {
        cout << "usage: ./load_balancer <server_ip> <server_port>" << endl;
        return 1;
    }

    signal(SIGINT, handler); // Ctrl+C sets the flag, the loops see it and we close everything safely
    signal(SIGPIPE, SIG_IGN);

    int listenSock = createListenSocket(lbPort);
    cout << "load balancer listening on port " << lbPort << ", server at " << serverHost << ":" << serverPort << endl;

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    // Ctrl+C: close the listening socket and every client that is still waiting.
    cout << "shutting down" << endl;
    close(listenSock);
    queueMutex.lock();
    while (!pendingClients.isEmpty())
    {
        ClientInfo c = pendingClients.dequeue();
        close(c.sock);
    }
    queueMutex.unlock();
    return 0;
}
