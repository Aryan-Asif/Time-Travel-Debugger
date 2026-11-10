#pragma once

// helper.h: everything that is used by more than one of server.cpp, client.cpp, load_balancer.cpp

#include <string>
#include <cstdint>
#include <cstring>
using namespace std;

// ---- Constants used in all three programs ----
const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;
const int IO_BUFFER_SIZE = 64 * 1024; // fixed buffer for moving bytes
const int CLIENT_TIMEOUT_SEC = 30;    // client gives up after this long, load balancer drops clients older than this

// ---- Structs used by server and client (the client reads back what the server wrote) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Server bookkeeping for resolve.bin ----
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};

// =============================================================================================
// CHECKSUM: CRC-32 (the one used by zip and png), written bit by bit so it is easy to read.
// Every message on every hop is  [status: 1 byte][body][CRC-32 of status+body: 4 bytes]
// =============================================================================================

// Used by a SENDER: add every chunk while sending, then send result() as the last 4 bytes.
class Crc32
{
    uint32_t value;

public:
    Crc32()
    {
        value = 0xFFFFFFFF;
    }
    void add(const char *data, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            value = value ^ (unsigned char)data[i];
            for (int bit = 0; bit < 8; bit++)
            {
                if (value & 1)
                    value = (value >> 1) ^ 0xEDB88320;
                else
                    value = value >> 1;
            }
        }
    }
    uint32_t result() const
    {
        return value ^ 0xFFFFFFFF;
    }
};

// Used by a RECEIVER (or the load balancer while forwarding).
// The end of a message is only known when the connection closes, so the last 4 bytes
// received so far are always held back: they might be the checksum.
class TrailerChecker
{
    Crc32 crc;
    char tail[4]; // the last (up to) 4 bytes received so far
    int tailLen;

public:
    TrailerChecker()
    {
        tailLen = 0;
    }

    // The status byte is the first byte of a message and is covered by the checksum.
    // A receiver that reads the status byte on its own calls this once, before feed().
    void addStatus(char status)
    {
        crc.add(&status, 1);
    }

    // Give it n received bytes. It copies to 'out' every byte that is surely NOT part of
    // the trailer and returns how many bytes that is. 'out' needs room for n bytes.
    int feed(const char *in, int n, char *out)
    {
        int outCount = 0;
        for (int i = 0; i < n; i++)
        {
            if (tailLen == 4)
            {
                // the oldest held byte is now surely body
                out[outCount] = tail[0];
                outCount++;
                crc.add(&tail[0], 1);
                tail[0] = tail[1];
                tail[1] = tail[2];
                tail[2] = tail[3];
                tail[3] = in[i];
            }
            else
            {
                tail[tailLen] = in[i];
                tailLen++;
            }
        }
        return outCount;
    }

    // Call when the stream has ended: do the held-back 4 bytes equal the checksum of everything before them?
    bool verify() const
    {
        if (tailLen < 4)
            return false;
        uint32_t received;
        memcpy(&received, tail, 4);
        return received == crc.result();
    }

    // The held-back 4 bytes (the load balancer forwards them only if verify() is true)
    void getTrailer(char out[4]) const
    {
        memcpy(out, tail, 4);
    }
};
