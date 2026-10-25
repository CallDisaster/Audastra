#pragma once
#include "audastra_japi.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace audastra::japi {
inline constexpr std::string_view Prefix = "ajapi:2;";
struct Value {
    int32_t kind = AJ_VOID;
    int32_t integer = 0;
    float real = 0;
    std::string text;
};
struct Request {
    uint32_t epoch = 0;
    uint32_t id = 0;
    std::string command;
    std::vector<Value> arguments;
};
struct Response {
    bool consumed = true;
    uint32_t epoch = 0;
    uint32_t id = 0;
    int32_t status = AJ_OK;
    Value value;
};
bool ValidText(std::string_view text);
bool ValidName(std::string_view name, size_t limit = 32);
bool ValidCommand(std::string_view name);
bool ValidFunction(std::string_view name);
bool ValidVersion(std::string_view version);
std::string EncodeText(std::string_view text);
int32_t DecodeText(std::string_view text, std::string& out, size_t limit);
int32_t ParseRequest(std::string_view wire, Request& out);
std::string EncodeResponse(const Response& response);
int32_t ParseResponse(std::string_view wire, Response& out);
char TypeChar(int32_t kind);
int32_t TypeKind(char type);
const char* StatusText(int32_t status);
}
