#pragma once
#include <string>
#include <cstdint>
#include <vector>

// Constants used across the project
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;

struct FuncEntry
{
    std::string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    std::string targetFuncName;
};

struct Variable
{
    std::string name;
    int32_t value;
};

struct Frame
{
    std::string func_name;
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

#pragma pack(push, 1)
struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
#pragma pack(pop)

inline int32_t calculateChecksum(int32_t currentSum, const char* buf, int32_t len)
{
    int32_t sum = currentSum;
    for (int32_t i = 0; i < len; i++) {
        sum = sum + buf[i];
        sum = sum % 1000000;
    }
    return sum;
}