#include "key_channel.h"
#include "platform.h"
#include <objbase.h>
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace manager {
namespace {
constexpr DWORD limit = 4 * 1024 * 1024;
void require(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + utf8(errorText(GetLastError())));
}
std::wstring pipeName(const std::wstring& token) {
    GUID guid{}; wchar_t canonical[40]{};
    if (token.size() != 38 || FAILED(CLSIDFromString(token.c_str(), &guid))) throw std::runtime_error("Invalid response token.");
    StringFromGUID2(guid, canonical, 40);
    if (token != canonical) throw std::runtime_error("Invalid response token.");
    return L"\\\\.\\pipe\\OpenSSHManager.Keys." + token;
}
}
KeyChannel::~KeyChannel() { close(); }
void KeyChannel::close() {
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    pipe = INVALID_HANDLE_VALUE; token.clear(); bytes.clear(); verified = false;
}
void KeyChannel::open() {
    close(); GUID guid{}; wchar_t value[40]{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Could not create response token.");
    StringFromGUID2(guid, value, 40); token = value;
    HANDLE raw = nullptr; require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), "Read account");
    DWORD size = 0; GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    bool success = GetTokenInformation(raw, TokenUser, user.data(), size, &size) != FALSE; CloseHandle(raw);
    require(success, "Read account SID");
    LPWSTR sid = nullptr; require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid), "Format SID");
    auto acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &sd, nullptr), "Create channel ACL");
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    pipe = CreateNamedPipeW(pipeName(token).c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &sa);
    auto code = GetLastError(); LocalFree(sd); SetLastError(code);
    require(pipe != INVALID_HANDLE_VALUE, "Create response channel");
    ConnectNamedPipe(pipe, nullptr); // Nonblocking: helper connects after ShellExecute's UAC approval.
}
void KeyChannel::poll(HANDLE process) {
    if (pipe == INVALID_HANDLE_VALUE) return;
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        auto code = GetLastError();
        if (code == ERROR_PIPE_LISTENING || code == ERROR_PIPE_NOT_CONNECTED || code == ERROR_BROKEN_PIPE) return;
        require(false, "Read response channel");
    }
    if (!verified) {
        ULONG client = 0; require(GetNamedPipeClientProcessId(pipe, &client), "Verify response process");
        if (client != GetProcessId(process)) throw std::runtime_error("Unexpected public-key response process.");
        verified = true;
    }
    while (available) {
        char buffer[16384]; DWORD count = 0;
        require(ReadFile(pipe, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &count, nullptr), "Read public keys");
        if (!count) break;
        if (bytes.size() + count > limit + sizeof(DWORD)) throw std::runtime_error("Public-key response exceeds 4 MiB.");
        bytes.append(buffer, count); available -= count;
    }
}
std::string KeyChannel::result() const {
    DWORD size = 0;
    if (bytes.size() < sizeof(size)) throw std::runtime_error("Public-key response was not received. Refresh the list.");
    std::memcpy(&size, bytes.data(), sizeof(size));
    if (size > limit || bytes.size() != sizeof(size) + size) throw std::runtime_error("Incomplete public-key response. Refresh the list.");
    return bytes.substr(sizeof(size));
}
KeyChannelWriter::KeyChannelWriter(const std::wstring& token) {
    pipe = CreateFileW(pipeName(token).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    require(pipe != INVALID_HANDLE_VALUE, "Connect public-key response");
    try {
        DWORD mode = PIPE_WAIT; require(SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr), "Set response wait mode");
        ULONG server = 0; require(GetNamedPipeServerProcessId(pipe, &server), "Verify requesting process");
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server);
        require(process != nullptr, "Open requesting process");
        wchar_t executable[32768]{}; DWORD size = 32768;
        bool ok = QueryFullProcessImageNameW(process, 0, executable, &size) != FALSE; CloseHandle(process);
        require(ok, "Read requesting application");
        if (_wcsicmp(executable, modulePath().c_str()) != 0) throw std::runtime_error("Unexpected public-key requesting application.");
    } catch (...) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; throw; }
}
KeyChannelWriter::~KeyChannelWriter() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }
void KeyChannelWriter::send(const std::string& bytes) {
    if (bytes.size() > limit) throw std::runtime_error("Public-key file exceeds 4 MiB.");
    DWORD size = static_cast<DWORD>(bytes.size());
    std::string frame(reinterpret_cast<const char*>(&size), sizeof(size)); frame += bytes;
    size_t offset = 0;
    while (offset < frame.size()) {
        DWORD written = 0;
        require(WriteFile(pipe, frame.data() + offset, static_cast<DWORD>((std::min)(frame.size() - offset, size_t(16384))), &written, nullptr) && written != 0, "Send public keys");
        offset += written;
    }
    // Keep the client alive until the GUI has consumed/authenticated the response.
    require(FlushFileBuffers(pipe), "Finish public-key response");
}
}
