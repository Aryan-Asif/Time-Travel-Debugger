// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <sstream>  // For reading words from string directly and swiftly during Tokenization
#include <unistd.h> 
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>   // runtime_error (errors are thrown as exceptions)
#include <sys/time.h>
#include <netinet/in.h>
#include "helper.h"    // shared structs, constants and checksum code
using namespace std;

// ---- Constants ----
// MAX_VARS_PER_FRAME, MAX_STACK_DEPTH and IO_BUFFER_SIZE now live in helper.h
const int32_t MAX_FUNCS = 128;
const int32_t CALL_OFFSET_WIDTH = 20; // to avoid offset shifting during resolving
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // applied as SO_RCVTIMEO on the client socket (see main)

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:

    Stack(): top(nullptr), count(0)
    { 

    }
    void push(const T& val)
    {
        if (count < MAX_STACK_DEPTH)
        {
            Node* node = new Node{ val, nullptr };
            if (count == 0)
            {
                top = node;
                count++;
                return;
            }

            node->next = top;
            top = node;
            count++;
            return;
        }
    }
    T pop()
    {
        T val = top->data;
        if (count == 1)
        {
            delete top;
            top = nullptr;
            count--;
            return val;
        }

        Node* temp = top->next;
        delete top;
        top = temp;
        count--;
        return val;
    }
    T& peek()
    {
        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int ct = 0;
        Node* current = top;
        if (top != nullptr)
        {
            while (current != nullptr && ct < maxLen)
            {
                out[ct++] = current->data;
                current = current->next;
            }
        }

        return ct;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:

    Timeline(): head(nullptr), tail(nullptr), stepCount(0)
    {

    }
    void record(Snapshot* s)
    {
        TimelineNode* node = new TimelineNode{ s, nullptr, nullptr };
        if (stepCount == 0)
        {
            head = node;
            tail = node;
            stepCount++;
            return;
        }

        TimelineNode* temp = tail;
        tail->next = node;
        tail = node;
        tail->prev = temp;
        stepCount++;
    }
    TimelineNode* begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
// Variable, Frame, Snapshot and TTDBHeader are now in helper.h (the client needs them too)
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping: FuncEntry and PendingPatch are now in helper.h



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    bool isBlank = true;
    while (getline(in, out))
    {
        for (char c : out)
        {
            if (!isspace(c))
            {
                isBlank = false;
                break;
            }
        }

        if (isBlank)
            continue;

        return true;
    }

    return false;
}
string firstWord(const string& line)
{
    string result = "";
    for (char c: line)
    {
        if (isspace(c))
            break;

        result += c;
    }

    return result;
}
string secondWord(const string& line)
{
    bool firstWordDone = false;
    string result = "";
    for (char c: line)
    {
        if (isspace(c))
        {
            if (!firstWordDone)
            {
                firstWordDone = true;
                result = "";
                continue;
            }
            else
                break;
        }

        

        result += c;
    }
    return result;
}

bool validateProgram(const char* sourcePath)
{
    Stack<int> st;
    ifstream fin(sourcePath);
    string line = "";
    string word = "";
    if (!fin)
    {
        throw runtime_error("Could not open file.");
    }

    while (readSourceLine(fin, line))
    {
        word = firstWord(line);

        if (word == "func")
        {
            if (!st.isEmpty())
            {
                throw runtime_error("Invalid Code: nested func declaration (func inside another func).");
            }
            st.push(1);
            continue;
        }

        if (word == "func_end")
        {
            if (st.isEmpty())
            {
                throw runtime_error("Invalid Code: func_end without a matching func.");
            }
            st.pop();
            continue;
        }
    }

     if (st.isEmpty())
            return true;
        else
            throw runtime_error("Invalid Code: func without a matching func_end.");
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int32_t size = text.length();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    fwrite(text.data(), 1, size, f);
    return offsetField;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offset;
    int32_t size;
    fread(&offset, sizeof(int64_t), 1, f);
    fread(&size, sizeof(int32_t), 1, f);
    outText.resize(size);
    fread(outText.data(), 1, size, f);
    return offset;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    int64_t main_offset = 0;
    bool main_found = false;
    ifstream fin(sourcePath);
    FILE* fout = fopen(resolveBinPath, "wb+");
    string line = "";
    string first_word = "";
    string second_word = "";
    int64_t offset = 0;
    if (!fin)
    {
        if (fout)
            fclose(fout);
        throw runtime_error("Could not open file.");
    }
    if (!fout)
    {
        throw runtime_error("Could not open resolve file.");
    }
    while (readSourceLine(fin, line))
    {
        first_word = firstWord(line);
        second_word = secondWord(line);

        if (first_word == "func" && second_word == "main")
        {
            main_offset = offset;
            main_found = true;
        }
        if (first_word == "func")
        {
            if (funcCount >= MAX_FUNCS)
            {
                fclose(fout);
                throw runtime_error("Function limit exceeded");
            }

            funcArray[funcCount].funcName = second_word;
            funcArray[funcCount].byteOffsetInResolveBin = offset;
            funcCount++;
        }

        if  (first_word == "call")
        {
            int64_t temp = 0;
            bool flag = true;
            for (int i = 0; i < funcCount; i++)
            {
                if (second_word == funcArray[i].funcName)
                {
                    flag = false;
                    temp = funcArray[i].byteOffsetInResolveBin;
                    break;
                }
            }

            if (flag)
            {
                if (patchCount < MAX_PATCHES)
                {
                patches[patchCount].targetFuncName = second_word;
                patches[patchCount].byteOffsetOfOffsetField = offset;
                patchCount++;

                size_t pos = line.find(second_word, first_word.length()); // search after the word "call", so a function named "a" or "c" is not found inside "call"
                if (pos != string::npos)
                    line.replace(pos,second_word.length(),string(CALL_OFFSET_WIDTH, '0'));
        

                writeResolveRecord(fout, offset, line);
                offset += 8 + 4 + line.length();
                continue;
                }
                else
                {
                    fclose(fout);
                    throw runtime_error("Patch limit exceeded");
                }
            }
            else
            {
                size_t pos = line.find(second_word, first_word.length()); // search after the word "call", so a function named "a" or "c" is not found inside "call"

                if (pos != string::npos)
                    line.replace(pos, second_word.length(), to_string(temp));
            }
        }

        writeResolveRecord(fout, offset, line);
        offset += 8 + 4 + line.length();
    }

    for (int i = 0; i < patchCount; i++)
    {
        string patchLine = "";
        int64_t patchOffset = patches[i].byteOffsetOfOffsetField;
        fseek(fout, patchOffset, SEEK_SET);
        readResolveRecord(fout, patchLine);
        int64_t func_offset = -1;
        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                func_offset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (func_offset == -1)
        {
            fclose(fout);
            throw runtime_error("Undefined Function: call to function '" + patches[i].targetFuncName + "'");
        }

        string offsetString = to_string(func_offset);
        offsetString = string(CALL_OFFSET_WIDTH - offsetString.length(), '0') + offsetString;

        size_t pos = patchLine.find(string(CALL_OFFSET_WIDTH, '0'));

        if (pos != string::npos)
            patchLine.replace(pos,CALL_OFFSET_WIDTH,offsetString);
    
        fseek(fout, patchOffset, SEEK_SET);
        writeResolveRecord(fout, patchOffset, patchLine);
    }

    if (main_found)
    {
        fclose(fout);
        return main_offset;
    }
    else
    {
        fclose(fout);
        throw runtime_error("No Main found");
    }
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    stringstream ss(line);
    int32_t ct = 0;
    string word;
    while (ss >> word && ct < maxTokens)
    {
        tokens[ct].text = word;
        if (ct == 0)
            tokens[ct].type = KEYWORD;
        else if (ct == 1)
            tokens[ct].type = IDENTIFIER;
        else
            tokens[ct].type = PARAM;

        ct++;
    }

    return ct;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* snapshot = new Snapshot;
    snapshot->stackDepth = callStack.snapshot_into(snapshot->callStack, MAX_STACK_DEPTH);
    return snapshot;
}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    Stack<Frame> callStack;
    string line = "";
    Token tokens[MAX_TOKENS];

    FILE* fin = fopen(resolveBinPath, "rb");
    if (!fin)
    {
        throw runtime_error("Could not open resolve file.");
    }

    fseek(fin, mainOffset, SEEK_SET);

    int64_t current_offset = readResolveRecord(fin, line);

    Frame main_frame;
    main_frame.func_name = "main";
    main_frame.returnLine = 0;
    main_frame.argc = 0;
    main_frame.localCount = 0;

    callStack.push(main_frame);

    while (!callStack.isEmpty())
    {
        Frame& current_frame = callStack.peek();

        current_offset = readResolveRecord(fin, line);

        tokenizeLine(line, tokens, MAX_TOKENS);

        string instruction = tokens[0].text;

        if (instruction == "set")
        {
            bool flag = true;
            int idx = -1;

            for (int i = 0; i < current_frame.localCount; i++)
            {
                if (tokens[1].text == current_frame.locals[i].name)
                {
                    flag = false;
                    idx = i;
                    break;
                }
            }

            if (flag)
            {
                Variable new_var;
                new_var.name = tokens[1].text;
                new_var.value = stoi(tokens[2].text);

                current_frame.locals[current_frame.localCount] = new_var;
                current_frame.localCount++;
            }
            else
            {
                current_frame.locals[idx].value = stoi(tokens[2].text);
            }
        }

        else if (instruction == "add" || instruction == "sub" || instruction == "mul" || instruction == "div")
        {
            int idx1 = -1;
            int idx2 = -1;

            for (int i = 0; i < current_frame.localCount; i++)
            {
                if (tokens[1].text == current_frame.locals[i].name)
                    idx1 = i;

                if (tokens[2].text == current_frame.locals[i].name)
                    idx2 = i;
            }

            if (idx1 == -1 || idx2 == -1)
            {
                fclose(fin);
                throw runtime_error("Undefined Variable Used");
            }

            if (instruction == "add")
            {
                current_frame.locals[idx1].value += current_frame.locals[idx2].value;
            }
            else if (instruction == "sub")
            {
                current_frame.locals[idx1].value -= current_frame.locals[idx2].value;
            }
            else if (instruction == "mul")
            {
                current_frame.locals[idx1].value *= current_frame.locals[idx2].value;
            }
            else
            {
                if (current_frame.locals[idx2].value == 0)
                {
                    fclose(fin);
                    throw runtime_error("Cannot divide by zero");
                }

                current_frame.locals[idx1].value /= current_frame.locals[idx2].value;
            }
        }

        else if (instruction == "call")
        {
            int64_t callee_offset = stoi(tokens[1].text);

            if (callStack.depth() >= MAX_STACK_DEPTH)
            {
                fclose(fin);
                throw runtime_error("Stack overflow");
            }

            Frame new_frame;
            new_frame.argc = 0;
            new_frame.localCount = 0;
            new_frame.returnLine = current_offset;
            
            fseek(fin, callee_offset, SEEK_SET);
            
            string funcLine = "";
            readResolveRecord(fin, funcLine);
            
            Token funcTokens[MAX_TOKENS];
            int32_t funcTokenCount = tokenizeLine(funcLine, funcTokens, MAX_TOKENS);
            new_frame.func_name = funcTokens[1].text;

            for (int i = 2; i < funcTokenCount; i++)
            {
                new_frame.argv[new_frame.argc].name = tokens[i].text;

                if (isdigit(tokens[i].text[0]) || (tokens[i].text[0] == '-' && tokens[i].text.size() > 1))
                {
                    new_frame.argv[new_frame.argc].value = stoi(tokens[i].text);
                }
                else
                {
                    bool found = false;

                    for (int j = 0; j < current_frame.localCount; j++)
                    {
                        if (current_frame.locals[j].name == tokens[i].text)
                        {
                            new_frame.argv[new_frame.argc].value = current_frame.locals[j].value;

                            found = true;
                            break;
                        }
                    }

                    if (!found)
                    {
                        fclose(fin);
                        throw runtime_error("Undefined Variable Used");
                    }
                }

                new_frame.locals[new_frame.localCount].name = funcTokens[i].text;

                new_frame.locals[new_frame.localCount].value = new_frame.argv[new_frame.argc].value;

                new_frame.localCount++;
                new_frame.argc++;
            }

            callStack.push(new_frame);
        }

        else if (instruction == "func_end")
        {
            Frame finished_frame = callStack.pop();

            if (!callStack.isEmpty())
            {
                Frame& caller_frame = callStack.peek();

                for (int i = 0; i < finished_frame.argc; i++)
                {
                    for (int j = 0; j < caller_frame.localCount; j++)
                    {
                        if (caller_frame.locals[j].name == finished_frame.argv[i].name)
                        {
                            caller_frame.locals[j].value = finished_frame.locals[i].value;

                            break;
                        }
                    }
                }

                fseek(fin, finished_frame.returnLine, SEEK_SET);
                readResolveRecord(fin, line);
            }

        }
    
    Snapshot* snapshot = buildSnapshot(callStack);
    timeline.record(snapshot);
    }
    fclose(fin);
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* f = fopen(tdbgPath, "wb");

    if (!f)
    {
        throw runtime_error("Could not create the .tdbg file.");
    }

    TTDBHeader header;

    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';

    header.version = 1;
    header.stepCount = timeline.getStepCount();
    header.indexOffset = 0;
    writeHeader(f, header);

    int64_t* index = new int64_t[header.stepCount];


    TimelineNode* current = timeline.begin();

    for (int32_t i = 0; i < header.stepCount; i++) {
    index[i] = ftell(f);

    Snapshot* snapshot = current->data;

    fwrite(&snapshot->stackDepth, sizeof(int32_t), 1, f);

    for (int j = 0; j < snapshot->stackDepth; j++)
    {
        Frame& frame = snapshot->callStack[j];

        int32_t funcNameSize = frame.func_name.length();
        fwrite(&funcNameSize, sizeof(int32_t), 1, f);
        fwrite(frame.func_name.data(), 1, funcNameSize, f);

        fwrite(&frame.argc, sizeof(int32_t), 1, f);

        for (int k = 0; k < frame.argc; k++)
        {
            int32_t nameSize = frame.argv[k].name.length();
            fwrite(&nameSize, sizeof(int32_t), 1, f);
            fwrite(frame.argv[k].name.data(), 1, nameSize, f);

            fwrite(&frame.argv[k].value, sizeof(int32_t), 1, f);
        }

        fwrite(&frame.returnLine, sizeof(int32_t), 1, f);

        fwrite(&frame.localCount, sizeof(int32_t), 1, f);

        for (int k = 0; k < frame.localCount; k++)
        {
            int32_t nameSize = frame.locals[k].name.length();
            fwrite(&nameSize, sizeof(int32_t), 1, f);
            fwrite(frame.locals[k].name.data(), 1, nameSize, f);

            fwrite(&frame.locals[k].value, sizeof(int32_t), 1, f);
        }
    }

    current = current->next;
}


    header.indexOffset = ftell(f);

    fwrite(index, sizeof(int64_t), header.stepCount, f);

    fseek(f, 0, SEEK_SET);

    writeHeader(f, header);

    delete[] index;

    fclose(f);
}

// =============================================================================================
// PHASE 2: NETWORK PART (stage 0 receive + sending the reply)
// The load balancer starts us with "./server <port> &" and sends one client at a time.
// Message format on the wire: [status: 1 byte][body][CRC-32: 4 bytes]
//   upload from client:  status 0, body = source code
//   our reply:           status 0, body = .tdbg file     OR     status 1, body = error text
// =============================================================================================

const int DEFAULT_PORT = 5001;
// The upload is saved under its own name so it never overwrites the client's own source.bin
// when everything runs in one folder.
const char *SOURCE_PATH = "received_source.bin";
const char *RESOLVE_PATH = "resolve.bin";
const char *TDBG_PATH = "session.tdbg";

bool sendAll(int sock, const char *buf, size_t n)
{
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

int createListenSocket(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        throw runtime_error("socket() failed");

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        throw runtime_error("bind() failed, is the port already used?");
    }
    if (listen(sock, 8) < 0)
    {
        close(sock);
        throw runtime_error("listen() failed");
    }
    return sock;
}

// Stage 0: read [status][source][crc] until the client side is closed, save the source to disk.
// Throws if anything is wrong.
void receiveSource(int sock, const char *path)
{
    char status;
    if (recv(sock, &status, 1, MSG_WAITALL) != 1)
        throw runtime_error("nothing received from the client");
    if (status != 0)
        throw runtime_error("unexpected status byte in the upload");

    FILE *f = fopen(path, "wb");
    if (!f)
        throw runtime_error("server could not create the source file");

    TrailerChecker checker;
    checker.addStatus(status);

    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];
    uint64_t total = 0;

    while (true)
    {
        ssize_t n = recv(sock, in, IO_BUFFER_SIZE, 0);
        if (n < 0)
        {
            fclose(f);
            throw runtime_error("connection error or timeout while receiving the source");
        }
        if (n == 0)
            break; // the other side finished sending

        int released = checker.feed(in, n, out);
        total += released;
        if (total > MAX_SOURCE_BYTES)
        {
            fclose(f);
            throw runtime_error("source file is too big");
        }
        fwrite(out, 1, released, f);
    }
    fclose(f);

    if (!checker.verify())
        throw runtime_error("upload checksum mismatch");
}

bool sendErrorReply(int sock, const string &message)
{
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

bool sendTdbgReply(int sock, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return sendErrorReply(sock, "server could not open the .tdbg file");

    char status = 0;
    Crc32 crc;
    crc.add(&status, 1);
    bool ok = sendAll(sock, &status, 1);

    char buffer[IO_BUFFER_SIZE];
    while (ok)
    {
        size_t n = fread(buffer, 1, IO_BUFFER_SIZE, f);
        if (n == 0)
            break;
        crc.add(buffer, n);
        ok = sendAll(sock, buffer, n);
    }
    fclose(f);

    if (!ok)
        return false;
    uint32_t sum = crc.result();
    return sendAll(sock, (const char *)&sum, 4);
}

// The server stays alive between clients, so the snapshots must be freed after every request.
void freeTimeline(Timeline &timeline)
{
    TimelineNode *current = timeline.begin();
    while (current != nullptr)
    {
        TimelineNode *next = current->next;
        delete current->data;
        delete current;
        current = next;
    }
}

// One whole request: receive, run the 4 passes, reply. Never throws.
void handleClient(int sock)
{
    Timeline timeline;
    try
    {
        receiveSource(sock, SOURCE_PATH);                                    // stage 0
        validateProgram(SOURCE_PATH);                                        // pass 0x0
        int64_t mainOffset = resolveProgram(SOURCE_PATH, RESOLVE_PATH);      // pass 0x1
        executeProgram(RESOLVE_PATH, mainOffset, timeline);                  // pass 0x2
        writeTdbg(timeline, TDBG_PATH);                                      // pass 0x3
    }
    catch (const exception &e)
    {
        cout << "request failed: " << e.what() << endl;
        freeTimeline(timeline);
        sendErrorReply(sock, e.what());
        return;
    }

    cout << "request ok: " << timeline.getStepCount() << " steps, sending .tdbg" << endl;
    freeTimeline(timeline);
    sendTdbgReply(sock, TDBG_PATH);
}

// main section
int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    if (argc > 1)
        port = atoi(argv[1]);

    int listenSock = -1;
    try
    {
        listenSock = createListenSocket(port);
    }
    catch (const exception &e)
    {
        cout << "server could not start: " << e.what() << endl;
        return 1;
    }
    cout << "server listening on port " << port << endl;

    while (true)
    {
        int client = accept(listenSock, NULL, NULL);
        if (client < 0)
        {
            usleep(100 * 1000);
            continue;
        }

        // a client that goes silent must not hang the server forever
        timeval tv;
        tv.tv_sec = SOCKET_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        handleClient(client);
        close(client);
    }
    return 0;
}
