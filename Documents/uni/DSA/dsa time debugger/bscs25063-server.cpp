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
//#include <unistd.h>
//#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
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
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {    // pushes the value on the stack if max limit is not reached yet.

        if (count >= MAX_STACK_DEPTH) {
            throw std::overflow_error("The Stack is FUll");


        }

        Node* n = new Node;
        n->data = val;
        if (isEmpty()) {
            top = n;
            top->next = nullptr;
            count++;
            return;
        }


        n->next = top;
        top = n;

        count++;

    }
    T pop()
    {

        if (isEmpty()) {
            throw std::underflow_error("The Stack is Empty");

        }
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;

    }
    T& peek()
    {
        // returns the top value on the stack
        return top->data;
    }
    bool isEmpty()
    {
        if (count == 0) {
            return true;
        }
        return false;

    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written

        int32_t  i = 0;
        Node* temp = top;

        while (temp != nullptr and i < maxLen) {
            out[i] = temp->data;
            i++;
            temp = temp->next;

        }
        return i;


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
    // Implement these functions
    Timeline()
    {
        head = nullptr;
        tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s)
    {
        // add record in the timeline
        TimelineNode* n = new TimelineNode();
        n->data = s;

        if (head == nullptr) {
            head = tail = n;
            head->next = nullptr;
            tail->prev = nullptr;
            stepCount++;
            return;

        }

        tail->next = n;
        n->prev = tail;
        n->next = nullptr;
        tail = n;
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



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    // reads the next nonblank line

    string temp = "";

    while (getline(in, temp)) {

        bool isBlank = true;

        for (int i = 0; i < temp.length(); i++) {

            if (temp[i] != '\t' and temp[i] != ' ' and temp[i] != '\r') {
                isBlank = false;
                break;
            }

        }
        if (isBlank) {
            continue;
        }

        out = temp;
        return true;

    }
    return false;


}
string firstWord(const string& line)
{
    // returns first word from the input string

    string temp = "";

    int len = line.length();
    int i = 0;

    while (i < len and (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
        i++;
    }

    for (; i < len; i++) {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') {
            break;

        }
        temp += line[i];
    }
    return temp;

}
string secondWord(const string& line)
{
    // returns the second word

    int i = 0;
    string temp = "";

    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
        i++;
    }

    while (i < line.length() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {

        i++;
    }

    while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {

        i++;
    }



    for (; i < line.length(); i++) {
        if (line[i] == ' ') {
            break;

        }
        temp += line[i];
    }
    return temp;


}
bool validateProgram(const char* sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream fin(sourcePath);
    if (!fin) {
        return false;
    }

    Stack<string> st;
    string temp = "";
    bool isValid = false;

    while (readSourceLine(fin, temp)) {
        string t = firstWord(temp);
        if (t == "func") {
            if (st.depth() == 0) {
                st.push(t);
            }
            else {
                fin.close();
                return false;
            }
        }
        else if (t == "func_end") {
            if (st.depth() == 0) {
                fin.close();
                return false;
            }
            else {
                st.pop();
                isValid = true;
            }
        }
        else {
            if (st.depth() == 0) {
                fin.close();
                return false;
            }
        }

    }
    if (st.depth() != 0) {
        fin.close();
        return false;
    }
    fin.close();
    return isValid;

}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position

    fwrite(&offsetField, sizeof(offsetField), 1, f); // memory of raw data , size of the data , how many are we writing, where we are writing
    int32_t s = text.length(); // means 4 byte variable , incase i forget
    fwrite(&s, sizeof(s), 1, f); // same as 1 
    fwrite(text.c_str(), s, 1, f); // c_str convert the string into c style string discarding stuff like size etc


    return offsetField;


}
int64_t readResolveRecord(FILE* f, string& outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.

    int64_t offSet = 0;
    fread(&offSet, sizeof(offSet), 1, f);

    int32_t length;
    fread(&length, sizeof(length), 1, f);
    outText.resize(length);
    fread(&outText[0], 1, length, f); // telling fread to  dump the raw data at which index/position
    // same as we do write and read in offstream and ifstream file

    return offSet + sizeof(int64_t) + sizeof(int32_t) + length;


}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error
    ifstream fin(sourcePath);
    if (!fin) {
        cout << "Error: File Not Found" << endl;
        return -1;
    }

    FILE* file = fopen(resolveBinPath, "wb+");
    if (!file) {
        cout << "Error: File Not Found" << endl;
        return -1;
    }
    int64_t curr_offSet = 0;
    string temp = "";
    while (readSourceLine(fin, temp)) {
        int64_t totalBytes = writeResolveRecord(file, curr_offSet, temp);
        string first_word = firstWord(temp);

        if (first_word == "func") {
            if (funcCount >= MAX_FUNCS) {
                throw runtime_error("Error: Maximum FUnction Limit reached");

            }
            funcArray[funcCount].byteOffsetInResolveBin = curr_offSet;
            funcArray[funcCount].funcName = secondWord(temp);
            funcCount++;
        }

        else if (first_word == "call") {
            if (patchCount >= MAX_PATCHES) {
                throw runtime_error("Error: MAximum Patch Limit Reached");
            }


            string t = secondWord(temp);

            patches[patchCount].byteOffsetOfOffsetField = curr_offSet;
            patches[patchCount].targetFuncName = t;
            patchCount++;

        }

        curr_offSet = curr_offSet + 8 + 4 + temp.length();

    }
    fin.close();

    for (int i = 0; i < patchCount; i++) {
        bool isFound = false;
        for (int j = 0; j < funcCount; j++) {

            if (patches[i].targetFuncName == funcArray[j].funcName) {
                isFound = true;

                fseek(file, patches[i].byteOffsetOfOffsetField, 0);
                fwrite(&funcArray[j].byteOffsetInResolveBin, sizeof(int64_t), 1, file);
                break;
            }
        }
        if (!isFound) {
            fclose(file);
            return -1;
        }
    }


    for (int i = 0; i < funcCount; i++) {
        if (funcArray[i].funcName == "main") {
            fclose(file);
            return funcArray[i].byteOffsetInResolveBin;
        }
    }
    fclose(file);
    return -1;

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
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated

    int32_t totalToken = 0;
    int32_t len = line.length();
    int32_t char_index = 0;
    string temp = "";
    while (char_index < len) {

        if (line[char_index] != ' ' and line[char_index] != '\t' and line[char_index] != '\r') {
            temp += line[char_index];


        }

        else if (line[char_index] == ' ' or line[char_index] == '\t' or line[char_index] == '\r') {
            if (!temp.empty()) {



                if (totalToken >= maxTokens) {
                    throw runtime_error("Error: Token Limit Exceeded");
                }
                if (totalToken == 0) {
                    tokens[totalToken].type = KEYWORD;
                    tokens[totalToken].text = temp;
                    totalToken++;
                }
                else if (totalToken == 1) {
                    tokens[totalToken].type = IDENTIFIER;
                    tokens[totalToken].text = temp;
                    totalToken++;
                }
                else {
                    tokens[totalToken].type = PARAM;
                    tokens[totalToken].text = temp;
                    totalToken++;
                }
                temp.clear();
            }
        }
        char_index++;

    }
    if (!temp.empty()) {
        if (totalToken >= maxTokens) {
            throw runtime_error("Error: Token Capacity Exceeded");
        }
        if (totalToken == 0) {
            tokens[totalToken].type = KEYWORD;
            tokens[totalToken].text = temp;
            totalToken++;
        }
        else if (totalToken == 1) {
            tokens[totalToken].type = IDENTIFIER;
            tokens[totalToken].text = temp;
            totalToken++;
        }
        else {
            tokens[totalToken].type = PARAM;
            tokens[totalToken].text = temp;
            totalToken++;
        }
    }

    return totalToken;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)

{
    // build the snapshot based on the callStack given
    Snapshot* ss = new Snapshot();
    int32_t depth = callStack.snapshot_into(ss->callStack, MAX_STACK_DEPTH);
    ss->stackDepth = depth;
    return ss;

}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
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