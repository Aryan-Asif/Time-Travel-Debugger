// ======================= TIME-TRAVEL DEBUGGER - CLIENT =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <vector>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
// (IO_BUFFER_SIZE, MAX_VARS_PER_FRAME, MAX_STACK_DEPTH and the structs Variable, Frame, Snapshot,
//  TTDBHeader are in helper.h because the server uses the same ones)
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg";     // where the downloaded .tdbg is saved
const char *TDBG_TEMP_FILE = "ttdb_new.tdbg"; // the download goes here first, so a failed download cannot damage the old session
const int MAX_JUMP = 1000000000;              // biggest n accepted by jmp
string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index; // file descriptor of the open .tdbg file (-1 when closed)
    TTDBHeader header;
    vector<int64_t> index; // index[i] = byte position of snapshot i in the file
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; // 0-based inside the program, shown to the user as 1-based
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = recv(sock, buf + got, n - got, 0);
        if (r <= 0)
            return false;
        got += r;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // MSG_NOSIGNAL: if the other side is gone we get an error back instead of a crash.
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

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int sock = socket(AF_INET, SOCK_STREAM, 0);
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
bool uploadSource(int sock, const char *sourcePath)
{
    // Message: [status 0][file bytes][CRC-32 of status + file bytes]
    // The checksum is updated chunk by chunk, so the whole file is never loaded into memory.
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE *f = fopen(sourcePath, "rb");
    if (!f)
        return false;

    Crc32 crc;
    char status = 0;
    crc.add(&status, 1);
    if (!sendAll(sock, &status, 1))
    {
        fclose(f);
        return false;
    }

    char buffer[IO_BUFFER_SIZE];
    while (true)
    {
        size_t n = fread(buffer, 1, IO_BUFFER_SIZE, f);
        if (n == 0)
            break;
        crc.add(buffer, n);
        if (!sendAll(sock, buffer, n))
        {
            fclose(f);
            return false;
        }
    }
    fclose(f);

    uint32_t sum = crc.result();
    if (!sendAll(sock, (const char *)&sum, 4))
        return false;

    shutdown(sock, SHUT_WR);
    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Every received byte (status included) goes through a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    outError = "";

    char status;
    if (!recvAll(sock, &status, 1))
    {
        outError = "no reply from the load balancer (connection closed or timed out)";
        return false;
    }
    if (status != 0 && status != 1)
    {
        outError = "unknown reply from the load balancer";
        return false;
    }

    FILE *file = NULL;
    if (status == 0)
    {
        file = fopen(outTdbgPath, "wb");
        if (!file)
        {
            outError = "cannot create the .tdbg file on disk";
            return false;
        }
    }

    TrailerChecker checker;
    checker.addStatus(status);

    string message = "";
    string failText = ""; // not empty when the connection broke or the disk write failed
    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];

    while (true)
    {
        ssize_t n = recv(sock, in, IO_BUFFER_SIZE, 0);
        if (n < 0)
        {
            failText = "connection lost while receiving the response";
            break;
        }
        if (n == 0)
            break; // the load balancer closed the connection: the reply is complete

        int released = checker.feed(in, n, out); // bytes that are surely not the trailer
        if (status == 1)
        {
            message.append(out, released);
        }
        else if (released > 0)
        {
            if (fwrite(out, 1, released, file) != (size_t)released)
            {
                failText = "cannot write the .tdbg file on disk";
                break;
            }
        }
    }

    if (file)
        fclose(file);

    if (failText != "")
    {
        if (status == 0)
            remove(outTdbgPath);
        outError = failText;
        return false;
    }
    if (!checker.verify())
    {
        if (status == 0)
            remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == 1)
    {
        outError = message;
        return false;
    }
    return true;
}
bool requestDebugFile(const char *host, int port, const char *sourcePath, const char *outTdbgPath, string &outError)
{
    // The whole network part of "start": connect, upload the source, read the reply.
    // Returns true only if a checked .tdbg file was saved to outTdbgPath.
    FILE *test = fopen(sourcePath, "rb");
    if (!test)
    {
        outError = string("cannot open source file: ") + sourcePath;
        return false;
    }
    fclose(test);

    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "cannot connect to the load balancer";
        return false;
    }

    // do not wait forever: after CLIENT_TIMEOUT_SEC seconds recv() gives up
    timeval tv;
    tv.tv_sec = CLIENT_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // If the upload fails, the load balancer may still have sent us an error ("server busy"),
    // so we read the reply in every case.
    uploadSource(sock, sourcePath);
    bool ok = receiveResponse(sock, outTdbgPath, outError);
    close(sock);
    return ok;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

// read() can also return less than asked for, so loop until n bytes are read
bool readFully(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = read(fd, p + got, n - got);
        if (r <= 0)
            return false;
        got += r;
    }
    return true;
}
bool readInt32(int fd, int32_t &value)
{
    return readFully(fd, &value, sizeof(int32_t));
}
bool readName(int fd, string &text) // [size: 4 bytes][characters]
{
    int32_t size;
    if (!readInt32(fd, size))
        return false;
    if (size < 0 || size > 10000) // a damaged file must not make us allocate a huge string
        return false;
    text.resize(size);
    if (size == 0)
        return true;
    return readFully(fd, &text[0], size);
}
bool readVariable(int fd, Variable &v)
{
    if (!readName(fd, v.name))
        return false;
    return readInt32(fd, v.value);
}
bool readFrame(int fd, Frame &f)
{
    if (!readName(fd, f.func_name))
        return false;

    if (!readInt32(fd, f.argc))
        return false;
    if (f.argc < 0 || f.argc > MAX_VARS_PER_FRAME)
        return false;
    for (int i = 0; i < f.argc; i++)
    {
        if (!readVariable(fd, f.argv[i]))
            return false;
    }

    if (!readInt32(fd, f.returnLine))
        return false;

    if (!readInt32(fd, f.localCount))
        return false;
    if (f.localCount < 0 || f.localCount > MAX_VARS_PER_FRAME)
        return false;
    for (int i = 0; i < f.localCount; i++)
    {
        if (!readVariable(fd, f.locals[i]))
            return false;
    }
    return true;
}
bool readHeaderAndIndex(int fd, TTDBHeader &h, vector<int64_t> &index)
{
    const int64_t HEADER_SIZE = 20; // 4 + 4 + 4 + 8 bytes, as written by writeHeader()

    if (!readFully(fd, h.magic, 4))
        return false;
    if (!readInt32(fd, h.version))
        return false;
    if (!readInt32(fd, h.stepCount))
        return false;
    if (!readFully(fd, &h.indexOffset, sizeof(int64_t)))
        return false;

    if (memcmp(h.magic, "TTDB", 4) != 0)
        return false;
    if (h.version != 1)
        return false;
    if (h.stepCount <= 0)
        return false;

    // The index must be exactly the last part of the file
    int64_t fileSize = lseek(fd, 0, SEEK_END);
    if (h.indexOffset < HEADER_SIZE)
        return false;
    if (h.indexOffset + (int64_t)h.stepCount * 8 != fileSize)
        return false;

    lseek(fd, h.indexOffset, SEEK_SET);
    index.resize(h.stepCount);
    if (!readFully(fd, &index[0], (size_t)h.stepCount * 8))
        return false;

    for (int i = 0; i < h.stepCount; i++)
    {
        if (index[i] < HEADER_SIZE || index[i] >= h.indexOffset)
            return false;
    }
    return true;
}

bool openTdbg(const char *path, TdbgReader &r)
{
    // Open the file, read the header and the whole index into r. Return false if the file is damaged.
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;

    TTDBHeader header;
    vector<int64_t> index;
    if (!readHeaderAndIndex(fd, header, index))
    {
        close(fd);
        return false;
    }

    r.file_index = fd;
    r.header = header;
    r.index = index;
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    vector<int64_t> empty;
    r.index.swap(empty);
    if (r.file_index >= 0)
        close(r.file_index);
    r.file_index = -1;
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    // Jump to the snapshot with the index and read it. The caller must delete the result.
    // Returns nullptr if the step does not exist or the file is damaged.
    if (step < 0 || step >= r.header.stepCount)
        return nullptr;
    if (lseek(r.file_index, r.index[step], SEEK_SET) < 0)
        return nullptr;

    Snapshot *s = new Snapshot;
    bool ok = readInt32(r.file_index, s->stackDepth);
    if (ok && (s->stackDepth < 0 || s->stackDepth > MAX_STACK_DEPTH))
        ok = false;
    for (int i = 0; ok && i < s->stackDepth; i++)
    {
        ok = readFrame(r.file_index, s->callStack[i]);
    }

    if (!ok)
    {
        delete s;
        return nullptr;
    }
    return s;
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    s.active_flag = 0;
    s.reader.file_index = -1;
    memset(&s.reader.header, 0, sizeof(TTDBHeader));
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (snap == nullptr)
    {
        cout << "damaged .tdbg file\n";
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    // Move delta steps (negative = backwards) and show the new step.
    // If the move is not possible, the session stays exactly as it was.
    if (!s.active_flag)
    {
        cout << "no session yet, first use: start <source file>\n";
        return false;
    }

    int64_t total = s.reader.header.stepCount;
    int64_t target = s.currentStep + delta;
    if (target < 0)
    {
        cout << "cannot go back that far, you are at step " << s.currentStep + 1 << " of " << total << "\n";
        return false;
    }
    if (target >= total)
    {
        cout << "cannot go forward that far, you are at step " << s.currentStep + 1 << " of " << total << "\n";
        return false;
    }

    int64_t oldStep = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s))
    {
        s.currentStep = oldStep;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port (the file is saved under a temporary name first).
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    string error;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_TEMP_FILE, error))
    {
        cout << "error: " << error << "\n";
        return false;
    }

    TdbgReader newReader;
    newReader.file_index = -1;
    if (!openTdbg(TDBG_TEMP_FILE, newReader))
    {
        cout << "server sent a damaged .tdbg file\n";
        remove(TDBG_TEMP_FILE);
        return false;
    }

    if (s.active_flag)
        closeTdbg(s.reader);
    s.reader = newReader; // newReader has no destructor, so the file stays open in s.reader
    s.currentStep = 0;
    s.active_flag = 1;

    // The new file is good, give it its final name (the open file keeps working after a rename).
    rename(TDBG_TEMP_FILE, TDBG_FILE);

    cout << "session started, the program has " << s.reader.header.stepCount << " steps\n";
    return showCurrent(s);
}
bool cmdNext(DebugSession &s)
{
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession &s)
{
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession &s, int32_t n)
{
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    // callStack[0] is the function that is running now, the last one is main.
    cout << "\n===== Step " << step << " of " << totalSteps << " =====\n";
    if (s.stackDepth == 0)
    {
        cout << "(the program has finished, the call stack is empty)\n";
        return;
    }

    for (int i = 0; i < s.stackDepth; i++)
    {
        const Frame &f = s.callStack[i];

        cout << "#" << i << " " << f.func_name << "(";
        for (int k = 0; k < f.argc; k++)
        {
            if (k > 0)
                cout << ", ";
            cout << f.argv[k].name << "=" << f.argv[k].value;
        }
        cout << ")";
        if (i == 0)
            cout << "   <- running now";
        cout << "\n";

        cout << "     return line: " << f.returnLine << "\n";
        cout << "     locals: ";
        if (f.localCount == 0)
            cout << "(none)";
        for (int k = 0; k < f.localCount; k++)
        {
            if (k > 0)
                cout << ", ";
            cout << f.locals[k].name << "=" << f.locals[k].value;
        }
        cout << "\n";
    }
}
void printHelp()
{
    cout << "Commands:\n";
    cout << "  start <file>      send the C-- source file to the server and show step 1\n";
    cout << "  next              go one step forward\n";
    cout << "  prv               go one step back\n";
    cout << "  jmp next <n>      go n steps forward\n";
    cout << "  jmp prv <n>       go n steps back\n";
    cout << "  help              show this text\n";
    cout << "  quit | exit       leave\n";
    cout << "(you can also write \"" << PROGRAM_NAME << "\" in front of a command, e.g. \"" << PROGRAM_NAME << " next\")\n";
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

// true if word is a whole number from 1 to MAX_JUMP
bool parsePositive(const string &word, int32_t &number)
{
    if (word.empty() || word.size() > 10)
        return false;
    long long value = 0;
    for (char c : word)
    {
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + (c - '0');
    }
    if (value < 1 || value > MAX_JUMP)
        return false;
    number = (int32_t)value;
    return true;
}

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    vector<string> words;
    istringstream stream(line);
    string word;
    while (stream >> word)
        words.push_back(word);

    // 2. If the first word is "ttdb", drop it.
    if (!words.empty() && words[0] == PROGRAM_NAME)
        words.erase(words.begin());

    if (words.empty()) // empty line: nothing to do
        return true;

    // 3. Match the first word.
    string cmd = words[0];
    if (cmd == "start" && words.size() == 2)
        return cmdStart(s, words[1]);
    if (cmd == "next" && words.size() == 1)
        return cmdNext(s);
    if (cmd == "prv" && words.size() == 1)
        return cmdPrv(s);
    if (cmd == "jmp" && words.size() == 3)
    {
        int32_t n;
        if (parsePositive(words[2], n))
        {
            if (words[1] == "next")
                return cmdJmpNext(s, n);
            if (words[1] == "prv")
                return cmdJmpPrv(s, n);
        }
    }
    if (cmd == "help" && words.size() == 1)
    {
        printHelp();
        return true;
    }
    if ((cmd == "quit" || cmd == "exit") && words.size() == 1)
    {
        quitRequested = true;
        return true;
    }

    // 4. For anything else, or a wrong number of words:
    cout << "unknown command\n";
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    // usage: ./client <lb_ip> <lb_port>
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2)
        LB_HOST = argv[1];
    if (argc >= 3)
        LB_PORT = atoi(argv[2]);
    if (argc > 3 || LB_PORT < 1 || LB_PORT > 65535)
    {
        cout << "usage: ./client <lb_ip> <lb_port>\n";
        return 1;
    }

    DebugSession session;
    initSession(session);

    cout << "Time-Travel Debugger client (load balancer at " << LB_HOST << ":" << LB_PORT << ")\n";
    printHelp();

    bool quitRequested = false;
    string line;
    while (true)
    {
        if (quitRequested)
            break;

        cout << PROMPT << flush;
        if (!getline(cin, line)) // end of the input
            break;

        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    return 0;
}
