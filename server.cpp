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
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t CALL_OFFSET_WIDTH = 20; // to avoid offset shifting during resolving
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

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
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
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
        cout << "Could not open file.\n";
        return false;
    }

    while (readSourceLine(fin, line))
    {
        word = firstWord(line);

        if (word == "func")
        {
            if (!st.isEmpty())
            {
                cout << "Invalid Code.\n";
                return false;
            }
            st.push(1);
            continue;
        }

        if (word == "func_end")
        {
            if (st.isEmpty())
            {
                cout << "Invalid Code.\n";
                return false;
            }
            st.pop();
            continue;
        }
    }

     if (st.isEmpty())
            return true;
        else
            return false;
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
    if (!fin)     // Will modify to give exception after whole project
    {
        cout << "Could not open file.\n";
        return -1;
    }
    if (!fout)  // Will modify to give exception after whole project
    {
        cout << "Could not open resolve file.\n";
        return -1;
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
                cout << "Function limit exceeded";
                return -1;
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

                size_t pos = line.find(second_word);
                if (pos != string::npos)
                    line.replace(pos,second_word.length(),string(CALL_OFFSET_WIDTH, '0'));
        

                writeResolveRecord(fout, offset, line);
                offset += 8 + 4 + line.length();
                continue;
                }
                else
                {
                    cout << "Patch limit exceeded";
                    return -1;
                }
            }
            else
            {
                size_t pos = line.find(second_word);

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
        if (func_offset == -1) // Will modify to give exception after whole project
        {
            cout << "Undefined Function";
            return -1;
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
        cout << "No Main found";
        return -1;
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
    if (!fin) // Will change to exception later
    {
        cout << "File not opened";
        return;
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
                cout << "Undefined Variable Used";
                return;
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
                    cout << "Cannot divide by zero";
                    return;
                }

                current_frame.locals[idx1].value /= current_frame.locals[idx2].value;
            }
        }

        else if (instruction == "call")
        {
            int64_t callee_offset = stoi(tokens[1].text);

            if (callStack.depth() >= MAX_STACK_DEPTH)
            {
                cout << "Stack overflow";
                return;
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
                        cout << "Undefined Variable Used";
                        return;
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
        cout << "File not opened";
        return;
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
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}