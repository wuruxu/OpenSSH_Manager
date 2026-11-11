#include "i18n.h"
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
    if (token.size() != 38 || FAILED(CLSIDFromString(token.c_str(), &guid))) throw std::runtime_error(i18n::utf8Text(i18n::Text::ResponseTokenInvalid));
    StringFromGUID2(guid, canonical, 40);
    if (token != canonical) throw std::runtime_error(i18n::utf8Text(i18n::Text::ResponseTokenInvalid));
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
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error(i18n::utf8Text(i18n::Text::ResponseTokenFailed));
    StringFromGUID2(guid, value, 40); token = value;
    HANDLE raw = nullptr; require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw), i18n::utf8Text(i18n::Text::OpReadAccount));
    DWORD size = 0; GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    bool success = GetTokenInformation(raw, TokenUser, user.data(), size, &size) != FALSE; CloseHandle(raw);
    require(success, i18n::utf8Text(i18n::Text::OpReadSid));
    LPWSTR sid = nullptr; require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid), i18n::utf8Text(i18n::Text::OpFormatSid));
    auto acl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &sd, nullptr), i18n::utf8Text(i18n::Text::OpChannelAcl));
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    pipe = CreateNamedPipeW(pipeName(token).c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &sa);
    auto code = GetLastError(); LocalFree(sd); SetLastError(code);
    require(pipe != INVALID_HANDLE_VALUE, i18n::utf8Text(i18n::Text::OpCreateChannel));
    ConnectNamedPipe(pipe, nullptr); // Nonblocking: helper connects after ShellExecute's UAC approval.
}
void KeyChannel::poll(HANDLE process) {
    if (pipe == INVALID_HANDLE_VALUE) return;
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        auto code = GetLastError();
        if (code == ERROR_PIPE_LISTENING || code == ERROR_PIPE_NOT_CONNECTED || code == ERROR_BROKEN_PIPE) return;
        require(false, i18n::utf8Text(i18n::Text::OpReadChannel));
    }
    if (!verified) {
        ULONG client = 0; require(GetNamedPipeClientProcessId(pipe, &client), i18n::utf8Text(i18n::Text::OpVerifyProcess));
        if (client != GetProcessId(process)) throw std::runtime_error(i18n::utf8Text(i18n::Text::UnexpectedKeyProcess));
        verified = true;
    }
    while (available) {
        char buffer[16384]; DWORD count = 0;
        require(ReadFile(pipe, buffer, (std::min)(available, static_cast<DWORD>(sizeof(buffer))), &count, nullptr), i18n::utf8Text(i18n::Text::OpReadPublicKeys));
        if (!count) break;
        if (bytes.size() + count > limit + sizeof(DWORD)) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyResponseTooLarge));
        bytes.append(buffer, count); available -= count;
    }
}
std::string KeyChannel::result() const {
    DWORD size = 0;
    if (bytes.size() < sizeof(size)) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyResponseMissing));
    std::memcpy(&size, bytes.data(), sizeof(size));
    if (size > limit || bytes.size() != sizeof(size) + size) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyResponseIncomplete));
    return bytes.substr(sizeof(size));
}
KeyChannelWriter::KeyChannelWriter(const std::wstring& token) {
    pipe = CreateFileW(pipeName(token).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    require(pipe != INVALID_HANDLE_VALUE, i18n::utf8Text(i18n::Text::OpConnectKeyResponse));
    try {
        DWORD mode = PIPE_WAIT; require(SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr), i18n::utf8Text(i18n::Text::OpResponseWait));
        ULONG server = 0; require(GetNamedPipeServerProcessId(pipe, &server), i18n::utf8Text(i18n::Text::OpVerifyRequester));
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server);
        require(process != nullptr, i18n::utf8Text(i18n::Text::OpOpenRequester));
        wchar_t executable[32768]{}; DWORD size = 32768;
        bool ok = QueryFullProcessImageNameW(process, 0, executable, &size) != FALSE; CloseHandle(process);
        require(ok, i18n::utf8Text(i18n::Text::OpReadRequester));
        if (_wcsicmp(executable, modulePath().c_str()) != 0) throw std::runtime_error(i18n::utf8Text(i18n::Text::UnexpectedKeyApp));
    } catch (...) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; throw; }
}
KeyChannelWriter::~KeyChannelWriter() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }
void KeyChannelWriter::send(const std::string& bytes) {
    if (bytes.size() > limit) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyFileTooLarge));
    DWORD size = static_cast<DWORD>(bytes.size());
    std::string frame(reinterpret_cast<const char*>(&size), sizeof(size)); frame += bytes;
    size_t offset = 0;
    while (offset < frame.size()) {
        DWORD written = 0;
        require(WriteFile(pipe, frame.data() + offset, static_cast<DWORD>((std::min)(frame.size() - offset, size_t(16384))), &written, nullptr) && written != 0, i18n::utf8Text(i18n::Text::OpSendKeys));
        offset += written;
    }
    // Keep the client alive until the GUI has consumed/authenticated the response.
    require(FlushFileBuffers(pipe), i18n::utf8Text(i18n::Text::OpFinishKeys));
}
}
