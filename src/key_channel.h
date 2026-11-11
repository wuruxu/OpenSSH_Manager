#pragma once
#include <windows.h>
#include <string>
namespace manager {
// Authenticated one-shot local response channel for key data and installer results.
// The GUI verifies the helper PID; the helper verifies the requesting executable.
struct KeyChannel {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::wstring token;
    std::string bytes;
    bool verified = false;
    ~KeyChannel();
    void open();
    void close();
    void poll(HANDLE process);
    std::string result() const;
};
class KeyChannelWriter {
    HANDLE pipe = INVALID_HANDLE_VALUE;
public:
    explicit KeyChannelWriter(const std::wstring& token);
    ~KeyChannelWriter();
    void send(const std::string& bytes);
};
}
