// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

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

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <vector>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;



// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t total_received = 0;
    while (total_received < n) {
        int received = recv(sock, buf + total_received, (int)(n - total_received), 0);
        if (received <= 0) {
            return false;
        }
        total_received += received;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
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

int connectToHost(const char *host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return -1;
    }

    return sock;
}
bool uploadSource(int sock, const char *sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.

    FILE* fp = fopen(sourcePath, "rb");
    if (!fp) {
        return false;
    }

    char status = 0;
    int32_t checksum = calculateChecksum(0, &status, 1);
    if (!sendAll(sock, &status, 1)) {
        fclose(fp);
        return false;
    }

    char buffer[IO_BUFFER_SIZE];
    size_t bytesRead;
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        checksum = calculateChecksum(checksum, buffer, (int32_t)bytesRead);
        if (!sendAll(sock, buffer, bytesRead)) {
            fclose(fp);
            return false;
        }
    }
    fclose(fp);

    if (!sendAll(sock, (const char*)&checksum, sizeof(checksum))) {
        return false;
    }

    shutdown(sock, SHUT_WR);
    return true;


}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    char status;
    if (!recvAll(sock, &status, 1)) {
        return false;
    }

    string networkData;
    char buffer[IO_BUFFER_SIZE];
    int bytesGrabbed;

    while ((bytesGrabbed = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        networkData.append(buffer, bytesGrabbed);
    }

    if (networkData.length() < 4) {
        outError = "Error: Response too short, missing checksum";
        return false;
    }

    string payloadData = networkData.substr(0, networkData.length() - 4);

    int32_t receivedCheck;
    memcpy(&receivedCheck, networkData.data() + networkData.length() - 4, 4);

    int32_t myCheck = calculateChecksum(0, &status, 1);
    myCheck = calculateChecksum(myCheck, payloadData.c_str(), (int32_t)payloadData.length());

    if (myCheck != receivedCheck) {
        outError = "Error: CheckSUm mismatch";
        return false;
    }

    if (status == 1) {
        outError = payloadData;
        return false;
    }

    FILE* fp = fopen(outTdbgPath, "wb");
    if (!fp) {
        outError = "Error: File not found";
        return false;
    }

    fwrite(payloadData.c_str(), 1, payloadData.length(), fp);
    fclose(fp);
    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char *path, TdbgReader &r)
{
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;

    if (fread(&r.header, 1, sizeof(TTDBHeader), fp) != sizeof(TTDBHeader)) {
        fclose(fp);
        return false;
    }

    if (strncmp(r.header.magic, "TTDB", 4) != 0 || r.header.stepCount <= 0) {
        fclose(fp);
        return false;
    }

    r.index.resize(r.header.stepCount);
    if (fseek(fp, r.header.indexOffset, SEEK_SET) != 0 ||
        fread(r.index.data(), sizeof(int64_t), r.header.stepCount, fp) != (size_t)r.header.stepCount) {
        fclose(fp);
        return false;
    }

    r.file_index = (size_t)fp;
    return true;
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
    r.index.clear();
    if (r.file_index != 0) {
        FILE* fp = (FILE*)r.file_index;
        fclose(fp);
        r.file_index = 0;
    }
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (step < 0 || step >= r.header.stepCount || r.file_index == 0) {
        return nullptr;
    }

    FILE* fp = (FILE*)r.file_index;

    fseek(fp, r.index[step], SEEK_SET);

    Snapshot* s = new Snapshot;

    if (fread(&s->stackDepth, sizeof(int32_t), 1, fp) != 1) {
        delete s;
        return nullptr;
    }

    for (int i = 0; i < s->stackDepth; i++) {
        int32_t strLen;

        fread(&strLen, sizeof(int32_t), 1, fp);

        s->callStack[i].func_name.resize(strLen);
        fread(&s->callStack[i].func_name[0], sizeof(char), strLen, fp);

        fread(&s->callStack[i].argc, sizeof(int32_t), 1, fp);

        for (int j = 0; j < s->callStack[i].argc; j++) {
            fread(&strLen, sizeof(int32_t), 1, fp);

            s->callStack[i].argv[j].name.resize(strLen);
            fread(&s->callStack[i].argv[j].name[0], sizeof(char), strLen, fp);
            fread(&s->callStack[i].argv[j].value, sizeof(int32_t), 1, fp);
        }

        fread(&s->callStack[i].returnLine, sizeof(int64_t), 1, fp);

        fread(&s->callStack[i].localCount, sizeof(int32_t), 1, fp);

        for (int j = 0; j < s->callStack[i].localCount; j++) {
            fread(&strLen, sizeof(int32_t), 1, fp);
            s->callStack[i].locals[j].name.resize(strLen);
            fread(&s->callStack[i].locals[j].name[0], sizeof(char), strLen, fp);
            fread(&s->callStack[i].locals[j].value, sizeof(int32_t), 1, fp);
        }
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
    s.currentStep = 0;
    s.reader.file_index = 0;
   
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag) {
        closeTdbg(s.reader);
        s.active_flag = 0;
    }
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot* currentSnap = readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (!currentSnap) {
        cout << "Corrupted/Damaged .tbg file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep, s.reader.header.stepCount, *currentSnap);
    delete currentSnap;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
    if (!s.active_flag) {
        return false;
    }

    int destination = s.currentStep + delta;
    if (destination < 0 || destination >= s.reader.header.stepCount) {
        return false;
    }

    s.currentStep = destination;
    showCurrent(s);
    return true;
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    int serverSock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if (serverSock < 0) {
        cout << "Failed to establish connection with the server" << endl;
        return false;
    }

    if (!uploadSource(serverSock, sourcePath.c_str())) {
        cout << "Failed to upload file" << endl;
#ifdef _WIN32
        closesocket(serverSock);
#else
        close(serverSock);
#endif
        return false;
    }

    string errorMsg;
    bool isOk = receiveResponse(serverSock, TDBG_FILE, errorMsg);

#ifdef _WIN32
    closesocket(serverSock);
#else
    close(serverSock);
#endif

    if (!isOk) {
        if (!errorMsg.empty()) {
            cout << "\n" << errorMsg << "\n";
        }
        else {
            cout << "Response Failure" << endl;
        }
        return false;
    }

    TdbgReader tempReader;
    if (!openTdbg(TDBG_FILE, tempReader)) {
        cout << "Received Damaged .tbg file from server" << endl;
        closeTdbg(tempReader);
        return false;
    }

    if (s.active_flag) {
        closeTdbg(s.reader);
    }

    s.reader = tempReader;
    s.currentStep = 0;
    s.active_flag = 1;

    showCurrent(s);
    return true;
}
bool cmdNext(DebugSession &s) {
    return moveBy(s, 1);

}
bool cmdPrv(DebugSession &s) {
    return moveBy(s, -1);

}
bool cmdJmpNext(DebugSession &s, int32_t n) {
    return moveBy(s, n);
}
bool cmdJmpPrv(DebugSession &s, int32_t n) {
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
    cout << "Current Step : " << step << endl;
    cout << "Total Steps : " << totalSteps << endl;

    for (int i = 0; i < s.stackDepth; i++) {
        cout << "Function " << i + 1 << endl;
        cout << "Function Name : " << s.callStack[i].func_name << endl;

        cout << "Arguments : " << endl;
        for (int j = 0; j < s.callStack[i].argc; j++) {
            cout << j + 1 << ". " << s.callStack[i].argv[j].name << " = " << s.callStack[i].argv[j].value << endl;
        }

        cout << "Return Line : " << s.callStack[i].returnLine << endl;

        cout << "Locals : " << endl;
        for (int j = 0; j < s.callStack[i].localCount; j++) {
            cout << j + 1 << ". " << s.callStack[i].locals[j].name << " = " << s.callStack[i].locals[j].value << endl;
        }
        cout << endl;
    }
}
void printHelp()
{
    // Print the commands
    cout << "\nCommands : \n";
    cout << "start <file_name> -- Connects to server, compiles/runs the source file, loads generated .tdbg file.\n";
    cout << "next -- Steps forward to next state.\n";
    cout << "prv -- Steps to previous state.\n";
    cout << "jmp next <n> -- Jumps  by n steps forward (n must be a +ve integer).\n";
    cout << "jmp prv <n> -- Jumps by n steps backwards (n must be a +ve integer).\n";
    cout << "help -- Prints help menu.\n";
    cout << "quit, exit -- Exits the debugger session.\n";
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    stringstream ss(line);
    vector<string> args;
    string word;

    while (ss >> word) {
        args.push_back(word);
    }

    if (args.empty()) {
        return false;
    }

    if (args[0] == "ttdb") {
        args.erase(args.begin());
    }

    if (args.empty()) {
        return false;
    }

    string command = args[0];

    if (command == "start") {
        if (args.size() != 2) {
            cout << "Nor defined command" << endl;
            printHelp();
            return false;
        }
        cmdStart(s, args[1]);
        return true;
    }
    else if (command == "next") {
        if (args.size() > 1) {
            cout << "Undefined command" << endl;
            printHelp();
            return false;
        }
        cmdNext(s);
        return true;
    }
    else if (command == "prv") {
        if (args.size() > 1) {
            cout << "Undefined command" << endl;
            printHelp();
            return false;
        }
        cmdPrv(s);
        return true;
    }
    else if (command == "jmp") {
        if (args.size() != 3) {
            cout << "Undefined command" << endl;
            printHelp();
            return false;
        }
        try {
            int jumps = stoi(args[2]);
            if (jumps <= 0) {
                cout << "Undefined command" << endl;
                printHelp();
                return false;
            }

            if (args[1] == "next") {
                cmdJmpNext(s, jumps);
            }
            else if (args[1] == "prv") {
                cmdJmpPrv(s, jumps);
            }
            else {
                cout << "Undefined command" << endl;
                printHelp();
                return false;
            }
        }
        catch (...) {
            cout << "Undefined command" << endl;
            printHelp();
            return false;
        }
        return true;
    }
    else if (command == "help") {
        printHelp();
        return true;
    }
    else if (command == "quit" || command == "exit") {
        quitRequested = true;
        return true;
    }

    cout << "Undefined command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    if (argc >= 2) {
        LB_HOST = argv[1];
    }
    if (argc >= 3) {
        LB_PORT = atoi(argv[2]);
    }

    DebugSession session;
    initSession(session);

    cout << "Welcome to the Time-Travel Debugger (TTDB)" << endl;
    printHelp();
    cout << "\n";

    string inputLine;
    bool quit = false;

    while (!quit) {
        cout << PROMPT;
        if (!getline(cin, inputLine)) {
            break;
        }
        if (inputLine.empty()) {
            continue;
        }
        runCommand(session, inputLine, quit);
    }

    closeSession(session);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;

}
