#include "i18n.h"
#include "platform.h"
#include "key_channel.h"
#include "bootstrap.h"
#include "install_marker.h"
#include <filesystem>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <winevt.h>
#include <userenv.h>
#include <lm.h>
#include <netfw.h>
#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace manager {
namespace {
struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE v = nullptr) : h(v) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct ServiceHandle {
    SC_HANDLE h;
    explicit ServiceHandle(SC_HANDLE v) : h(v) {}
    ~ServiceHandle() { if (h) CloseServiceHandle(h); }
};
struct LocalMemory {
    void* p = nullptr;
    ~LocalMemory() { if (p) LocalFree(p); }
};
[[noreturn]] void fail(const std::wstring& context, DWORD error = GetLastError()) {
    throw std::runtime_error(utf8(context + L"\r\n" + errorText(error)));
}
void require(bool condition, const std::wstring& context) { if (!condition) fail(context); }
std::wstring parent(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }
std::wstring joinArgs(const std::vector<std::wstring>& args) {
    std::wstring command;
    for (const auto& arg : args) { if (!command.empty()) command += L' '; command += quoteArgument(arg); }
    return command;
}
void regularPath(const std::wstring& path) {
    // Fail closed for junction/symlink paths before privileged writes.
    auto p = path;
    while (p.size() > 3) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error(i18n::utf8Text(i18n::Text::ReparseUnsupported) + utf8(p));
        p = parent(p);
    }
}
struct Security {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(const std::wstring& sddl) {
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr), i18n::text(i18n::Text::OpCreateAcl));
        sa.lpSecurityDescriptor = sd;
    }
    ~Security() { LocalFree(sd); }
};
const wchar_t* configAcl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)";
void writeNew(const std::wstring& path, const std::string& text, const std::wstring& acl = configAcl) {
    regularPath(path);
    Security security(acl);
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security.sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(file.h != INVALID_HANDLE_VALUE, i18n::text(i18n::Text::OpCreateFile) + path);
    DWORD written = 0;
    require(WriteFile(file.h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size(), i18n::text(i18n::Text::OpWriteFile));
    require(FlushFileBuffers(file.h), i18n::text(i18n::Text::OpFlushFile));
}
std::wstring uniquePath(const std::wstring& directory, const std::wstring& suffix) {
    GUID guid{};
    require(SUCCEEDED(CoCreateGuid(&guid)), i18n::text(i18n::Text::OpTempName));
    wchar_t buffer[40]{};
    StringFromGUID2(guid, buffer, 40);
    return directory + L"\\.manager-" + buffer + suffix;
}
struct TemporaryFile {
    std::wstring path;
    ~TemporaryFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
};
struct ProcessResult { DWORD code; std::string output; bool truncated = false; };
ProcessResult execute(const std::wstring& exe, const std::vector<std::wstring>& args, DWORD timeout = 20000) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    require(CreatePipe(&read, &write, &sa, 0), i18n::text(i18n::Text::OpCreatePipe));
    Handle reader(read), writer(write);
    require(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0), i18n::text(i18n::Text::OpConfigurePipe));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    require(input.h != INVALID_HANDLE_VALUE, i18n::text(i18n::Text::OpOpenNul));
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attributeBytes(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBytes.data());
    require(InitializeProcThreadAttributeList(attributes, 1, 0, &size), i18n::text(i18n::Text::OpProcessAttributes));
    struct AttributesGuard { LPPROC_THREAD_ATTRIBUTE_LIST p; ~AttributesGuard() { DeleteProcThreadAttributeList(p); } } guard{attributes};
    HANDLE inherited[] = {write, input.h};
    require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr), i18n::text(i18n::Text::OpRestrictHandles));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.h;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION pi{};
    std::vector<std::wstring> argv{exe};
    argv.insert(argv.end(), args.begin(), args.end());
    auto command = joinArgs(argv);
    Handle job(CreateJobObjectW(nullptr, nullptr));
    require(job.h != nullptr, i18n::text(i18n::Text::OpCreateJob));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), i18n::text(i18n::Text::OpConfigureJob));
    require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, parent(exe).c_str(), &startup.StartupInfo, &pi), i18n::text(i18n::Text::OpRun) + exe);
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.h, process.h)) { auto e = GetLastError(); TerminateProcess(process.h, 1); fail(i18n::text(i18n::Text::OpAssignJob), e); }
    require(ResumeThread(thread.h) != static_cast<DWORD>(-1), i18n::text(i18n::Text::OpResume));
    CloseHandle(writer.h); writer.h = nullptr;
    ProcessResult result{};
    auto appendOutput = [&result](const char* data, DWORD size) {
        const size_t room = 128 * 1024 - result.output.size();
        result.output.append(data, (std::min)(room, static_cast<size_t>(size)));
        if (size > room) result.truncated = true;
    };
    auto started = GetTickCount64();
    for (;;) {
        DWORD available = 0;
        while (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available) {
            char buffer[4096]; DWORD got = 0;
            if (!ReadFile(read, buffer, (std::min)(available, DWORD(sizeof(buffer))), &got, nullptr) || !got) break;
            appendOutput(buffer, got);
        }
        if (WaitForSingleObject(process.h, 30) == WAIT_OBJECT_0) {
            // One final drain after the process closed its output handles.
            while (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available) {
                char buffer[4096]; DWORD got = 0;
                if (!ReadFile(read, buffer, (std::min)(available, DWORD(sizeof(buffer))), &got, nullptr) || !got) break;
                appendOutput(buffer, got);
            }
            require(GetExitCodeProcess(process.h, &result.code), i18n::text(i18n::Text::OpProcessResult));
            return result;
        }
        if (GetTickCount64() - started > timeout) throw std::runtime_error(i18n::utf8Text(i18n::Text::OperationTimeout));
    }
}
void validate(const ServiceInfo& service, const std::wstring& file) {
    auto result = execute(service.exe, {L"-t", L"-f", file});
    if (result.code != 0) throw std::runtime_error(i18n::utf8Text(i18n::Text::ConfigCheckFailed) + result.output);
}
SERVICE_STATUS_PROCESS query(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{}; DWORD needed = 0;
    require(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed), i18n::text(i18n::Text::OpQueryService));
    return status;
}
void awaitState(SC_HANDLE service, DWORD desired) {
    auto started = GetTickCount64();
    for (;;) {
        auto state = query(service);
        if (state.dwCurrentState == desired) return;
        if (desired == SERVICE_RUNNING && state.dwCurrentState == SERVICE_STOPPED)
            fail(i18n::text(i18n::Text::ServiceStoppedStarting), state.dwWin32ExitCode ? state.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
        if (GetTickCount64() - started > 30000) throw std::runtime_error(i18n::utf8Text(i18n::Text::ServiceTimeout));
        Sleep(200);
    }
}
void stop(SC_HANDLE service) {
    auto state = query(service).dwCurrentState;
    if (state == SERVICE_STOPPED) return;
    if (state != SERVICE_STOP_PENDING) { SERVICE_STATUS status{}; require(ControlService(service, SERVICE_CONTROL_STOP, &status), i18n::text(i18n::Text::OpStopSshd)); }
    awaitState(service, SERVICE_STOPPED);
}
void start(SC_HANDLE service) {
    if (query(service).dwCurrentState != SERVICE_RUNNING) {
        require(StartServiceW(service, 0, nullptr), i18n::text(i18n::Text::OpStartSshd));
        awaitState(service, SERVICE_RUNNING);
    }
}
ServiceInfo usableService() {
    auto info = serviceInfo();
    if (!info.installed) throw std::runtime_error(i18n::utf8Text(i18n::Text::ServiceNotInstalled));
    if (!info.problem.empty()) throw std::runtime_error(utf8(info.problem));
    regularPath(info.config);
    return info;
}
void firewall(const ServiceInfo& service);
bool firewallOwned();
void apply(const ServiceInfo& info, const Settings& settings, const std::string& revision) {
    validateSettings(settings);
    if (!settings.listen.empty()) {
        auto address = settings.listen.substr(0, settings.listen.find('%'));
        IN_ADDR v4{}; IN6_ADDR v6{};
        if (InetPtonA(AF_INET, address.c_str(), &v4) != 1 && InetPtonA(AF_INET6, address.c_str(), &v6) != 1)
            throw std::runtime_error(i18n::utf8Text(i18n::Text::ListenAddressInvalid));
    }
    auto original = readFile(info.config);
    if (digest(original) != revision) throw std::runtime_error(i18n::utf8Text(i18n::Text::ConfigChanged));
    auto text = renderConfig(parseConfig(original), settings);
    TemporaryFile candidate{uniquePath(parent(info.config), L".conf")};
    writeNew(candidate.path, text);
    validate(info, candidate.path);
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    require(scm.h != nullptr, i18n::text(i18n::Text::OpOpenScm));
    ServiceHandle service(OpenServiceW(scm.h, L"sshd", SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP));
    require(service.h != nullptr, i18n::text(i18n::Text::OpOpenSshd));
    auto state = query(service.h).dwCurrentState;
    if (state != SERVICE_RUNNING && state != SERVICE_STOPPED) throw std::runtime_error(i18n::utf8Text(i18n::Text::ServiceTransition));
    bool running = state == SERVICE_RUNNING;
    const bool syncFirewall = firewallOwned();
    if (digest(readFile(info.config)) != revision) throw std::runtime_error(i18n::utf8Text(i18n::Text::ConfigValidationChanged));
    const auto backup = uniquePath(parent(info.config), L".backup");
    // ReplaceFile preserves the original file's ACL and makes a recovery copy.
    require(ReplaceFileW(info.config.c_str(), candidate.path.c_str(), backup.c_str(), 0, nullptr, nullptr), i18n::text(i18n::Text::OpReplaceConfig));
    candidate.path.clear();
    try {
        if (running) { stop(service.h); start(service.h); }
        if (syncFirewall) firewall(info);
    } catch (const std::exception& e) {
        std::string message = e.what();
        // Restore config even if stopping the new service fails.
        BOOL restored = ReplaceFileW(info.config.c_str(), backup.c_str(), nullptr, 0, nullptr, nullptr);
        if (!restored) message += i18n::utf8Text(i18n::Text::RollbackFailed) + utf8(backup) + "\r\n" + utf8(errorText(GetLastError()));
        else {
            message += i18n::utf8Text(i18n::Text::ConfigRestored);
            if (syncFirewall) {
                try { firewall(info); }
                catch (const std::exception& recovery) { message += i18n::utf8Text(i18n::Text::FirewallRecoveryFailed) + std::string(recovery.what()); }
            }
            if (running) {
                try { stop(service.h); start(service.h); }
                catch (const std::exception& recovery) { message += i18n::utf8Text(i18n::Text::ServiceRecoveryFailed) + std::string(recovery.what()); }
            }
        }
        throw std::runtime_error(message);
    }
}
std::wstring systemDirectory() {
    wchar_t buffer[MAX_PATH]{};
    require(GetSystemDirectoryW(buffer, MAX_PATH) != 0, i18n::text(i18n::Text::OpSystemDirectory));
    return buffer;
}
void setShell(const std::wstring& choice) {
    std::wstring shell;
    if (choice == L"cmd") shell = systemDirectory() + L"\\cmd.exe";
    else if (choice == L"powershell") shell = systemDirectory() + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    else throw std::runtime_error(i18n::utf8Text(i18n::Text::UnknownShell));
    require(GetFileAttributesW(shell.c_str()) != INVALID_FILE_ATTRIBUTES, i18n::text(i18n::Text::ShellMissing));
    HKEY key = nullptr;
    auto code = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OpenSSH", 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (code != ERROR_SUCCESS) fail(i18n::text(i18n::Text::OpOpenShellSetting), code);
    code = RegSetValueExW(key, L"DefaultShell", 0, REG_SZ, reinterpret_cast<const BYTE*>(shell.c_str()), static_cast<DWORD>((shell.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (code != ERROR_SUCCESS) fail(i18n::text(i18n::Text::OpSaveShell), code);
}
template<class T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T* operator->() const { return p; }
};
struct Bstr {
    BSTR p;
    explicit Bstr(const std::wstring& s) : p(SysAllocString(s.c_str())) { if (!p) throw std::bad_alloc(); }
    ~Bstr() { SysFreeString(p); }
    operator BSTR() const { return p; }
};
void comCheck(HRESULT result, const wchar_t* operation) {
    if (FAILED(result)) fail(operation, static_cast<DWORD>(result));
}
bool firewallOwned() {
    ComPtr<INetFwPolicy2> policy;
    comCheck(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy.p)), i18n::text(i18n::Text::OpFirewallPolicy));
    ComPtr<INetFwRules> rules;
    comCheck(policy->get_Rules(&rules.p), i18n::text(i18n::Text::OpFirewallRules));
    ComPtr<INetFwRule> existing;
    Bstr name(L"OpenSSHManager-Server");
    auto hr = rules->Item(name, &existing.p);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return false;
    comCheck(hr, i18n::text(i18n::Text::OpReadFirewall));
    return existing.p != nullptr;
}
void firewall(const ServiceInfo& service) {
    auto config = parseConfig(readFile(service.config));
    if (!config.problem.empty()) throw std::runtime_error(config.problem);
    validateSettings(config.settings);
    ComPtr<INetFwPolicy2> policy;
    comCheck(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy.p)), i18n::text(i18n::Text::OpFirewallPolicy));
    ComPtr<INetFwRules> rules;
    comCheck(policy->get_Rules(&rules.p), i18n::text(i18n::Text::OpFirewallRules));
    ComPtr<INetFwRule> rule;
    comCheck(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), reinterpret_cast<void**>(&rule.p)), i18n::text(i18n::Text::OpCreateFirewall));
    Bstr name(L"OpenSSHManager-Server"), description(L"OpenSSH Manager: SSH on domain/private profiles, local subnet only."),
        executable(service.exe), ports(wide(config.settings.port)), remote(L"LocalSubnet");
    comCheck(rule->put_Name(name), i18n::text(i18n::Text::OpFirewallName));
    comCheck(rule->put_Description(description), i18n::text(i18n::Text::OpFirewallDescription));
    comCheck(rule->put_ApplicationName(executable), i18n::text(i18n::Text::OpFirewallApp));
    comCheck(rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP), i18n::text(i18n::Text::OpFirewallProtocol));
    comCheck(rule->put_LocalPorts(ports), i18n::text(i18n::Text::OpFirewallPort));
    comCheck(rule->put_RemoteAddresses(remote), i18n::text(i18n::Text::OpFirewallScope));
    comCheck(rule->put_Direction(NET_FW_RULE_DIR_IN), i18n::text(i18n::Text::OpFirewallDirection));
    comCheck(rule->put_Profiles(NET_FW_PROFILE2_DOMAIN | NET_FW_PROFILE2_PRIVATE), i18n::text(i18n::Text::OpFirewallProfiles));
    comCheck(rule->put_Action(NET_FW_ACTION_ALLOW), i18n::text(i18n::Text::OpFirewallAction));
    comCheck(rule->put_Enabled(VARIANT_TRUE), i18n::text(i18n::Text::OpEnableFirewall));
    // Add replaces a rule with the same name, leaving unrelated rules intact.
    comCheck(rules->Add(rule.p), i18n::text(i18n::Text::OpSaveFirewall));
}
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    auto n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    auto n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring errorText(DWORD error) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result = buffer ? buffer : i18n::text(i18n::Text::UnknownWindowsError);
    LocalFree(buffer);
    return result + L" (" + std::to_wstring(error) + L")";
}
std::wstring modulePath() {
    std::vector<wchar_t> path(32768);
    auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(n && n < path.size(), i18n::text(i18n::Text::OpManagerPath));
    return std::wstring(path.data(), n);
}
std::wstring dataPath() {
    PWSTR path = nullptr;
    auto hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path);
    if (FAILED(hr)) fail(i18n::text(i18n::Text::OpProgramData), static_cast<DWORD>(hr));
    std::wstring out(path); CoTaskMemFree(path);
    return out + L"\\ssh";
}
AccountIdentity currentAccountIdentity() {
    HANDLE tokenValue = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenValue), i18n::text(i18n::Text::OpReadCurrentAccount));
    Handle token(tokenValue);
    auto readToken = [&](TOKEN_INFORMATION_CLASS type) {
        DWORD size = 0;
        GetTokenInformation(token.h, type, nullptr, 0, &size);
        require(size != 0, i18n::text(i18n::Text::OpAccountInfoSize));
        std::vector<BYTE> buffer(size);
        require(GetTokenInformation(token.h, type, buffer.data(), size, &size), i18n::text(i18n::Text::OpReadAccountInfo));
        return buffer;
    };
    AccountIdentity result;
    auto userBytes = readToken(TokenUser);
    PSID sid = reinterpret_cast<TOKEN_USER*>(userBytes.data())->User.Sid;
    DWORD nameSize = 0, domainSize = 0;
    SID_NAME_USE use{};
    LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &use);
    require(nameSize != 0, i18n::text(i18n::Text::OpResolveAccount));
    std::vector<wchar_t> name(nameSize), domain((std::max)(domainSize, DWORD(1)));
    require(LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &use), i18n::text(i18n::Text::OpResolveAccount));
    result.name = name.data();
    result.qualifiedName = domain[0] ? std::wstring(domain.data()) + L"\\" + result.name : result.name;

    auto groupBytes = readToken(TokenGroups);
    auto groups = reinterpret_cast<TOKEN_GROUPS*>(groupBytes.data());
    alignas(DWORD) BYTE administrators[SECURITY_MAX_SID_SIZE]{};
    DWORD sidSize = sizeof(administrators);
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &sidSize), i18n::text(i18n::Text::OpAdminSid));
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        // UAC-filtered administrator tokens retain this SID as DENY_ONLY.
        // This is an account identity hint, never an authorization decision.
        if (EqualSid(groups->Groups[i].Sid, administrators)) { result.administrator = true; break; }
    }
    TOKEN_ELEVATION elevation{}; DWORD size = 0;
    require(GetTokenInformation(token.h, TokenElevation, &elevation, sizeof(elevation), &size), i18n::text(i18n::Text::OpReadElevation));
    result.elevated = elevation.TokenIsElevated != 0;
    return result;
}
std::wstring publicKeyFilePath(bool administrators) {
    if (administrators) return dataPath() + L"\\administrators_authorized_keys";
    PWSTR profile = nullptr;
    comCheck(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile), i18n::text(i18n::Text::OpCurrentProfile));
    std::wstring path = std::wstring(profile) + L"\\.ssh\\authorized_keys";
    CoTaskMemFree(profile);
    return path;
}
std::string readFile(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(file.h != INVALID_HANDLE_VALUE, i18n::text(i18n::Text::OpReadFile) + path);
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.h, &info), i18n::text(i18n::Text::OpInspectFile));
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error(i18n::utf8Text(i18n::Text::RegularFileRequired));
    if (info.nFileSizeHigh || info.nFileSizeLow > 4 * 1024 * 1024) throw std::runtime_error(i18n::utf8Text(i18n::Text::FileTooLarge));
    std::string text(info.nFileSizeLow, 0); DWORD got = 0;
    require(ReadFile(file.h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr), i18n::text(i18n::Text::OpReadContents));
    if (got != text.size()) throw std::runtime_error(i18n::utf8Text(i18n::Text::FileChanged));
    return text;
}
std::string digest(const std::string& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error(i18n::utf8Text(i18n::Text::HashUnavailable));
    unsigned char hash[32]{};
    auto result = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) throw std::runtime_error(i18n::utf8Text(i18n::Text::HashFailed));
    std::string out;
    const char* hex = "0123456789abcdef";
    for (auto b : hash) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}
ServiceInfo serviceInfo() {
    ServiceInfo info; info.config = dataPath() + L"\\sshd_config";
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    require(scm.h != nullptr, i18n::text(i18n::Text::OpOpenScm));
    ServiceHandle service(OpenServiceW(scm.h, L"sshd", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service.h) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return info;
        fail(i18n::text(i18n::Text::OpReadService));
    }
    info.installed = true;
    auto status = query(service.h);
    info.state = status.dwCurrentState; info.pid = status.dwProcessId;
    DWORD needed = 0;
    QueryServiceConfigW(service.h, nullptr, 0, &needed);
    std::vector<BYTE> bytes(needed);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
    require(QueryServiceConfigW(service.h, config, needed, &needed), i18n::text(i18n::Text::OpReadCommand));
    info.startType = config->dwStartType;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(config->lpBinaryPathName, &count);
    LocalMemory arguments; arguments.p = argv;
    if (!argv || count < 1) { info.problem = i18n::text(i18n::Text::ServiceCommandInvalid); return info; }
    info.exe = argv[0];
    auto name = info.exe.substr(info.exe.find_last_of(L"\\/") + 1);
    if (_wcsicmp(name.c_str(), L"sshd.exe") != 0 || info.exe.size() < 3 || info.exe[1] != L':')
        info.problem = i18n::text(i18n::Text::ServiceAbsolutePath);
    else info.version = executableVersion(info.exe);
    // V1 deliberately manages the standard configuration only. Never silently edit a different instance.
    if (count != 1) info.problem = i18n::text(i18n::Text::ServiceCustomArgs);
    if (!info.problem.empty()) return info;
    const auto managed = managedEngineDirectory();
    bool managedPath = _wcsicmp(config->lpBinaryPathName, (L"\"" + managed + L"\\sshd.exe\"").c_str()) == 0;
    bool owned = false;
    std::wstring ownershipProblem;
    if (managedPath) {
        try {
            const auto markerPath = parent(managed) + L"\\InstallState.json";
            regularPath(markerPath); regularPath(managed);
            auto marker = parseInstallMarker(wide(readFile(markerPath)));
            owned = marker.product == L"OpenSSHManager" && marker.ownsService &&
                _wcsicmp(marker.directory.c_str(), parent(managed).c_str()) == 0;
            if (!owned) ownershipProblem = i18n::text(i18n::Text::OwnershipMismatch);
        } catch (...) { ownershipProblem = i18n::text(i18n::Text::OwnershipMissing); }
    }
    std::vector<std::wstring> files{info.exe};
    if (managedPath) for (auto name : {L"sshd-auth.exe", L"sshd-session.exe", L"ssh-keygen.exe", L"sftp-server.exe", L"ssh-agent.exe", L"libcrypto.dll", L"sshd_config_default", L"openssh-events.man"})
        files.push_back(managed + L"\\" + name);
    std::wstring missing;
    for (const auto& file : files) {
        DWORD attr = GetFileAttributesW(file.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                info.problem = i18n::text(i18n::Text::EngineInspectFailed) + file + L"\r\n" + errorText(code); return info;
            }
        } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        info.engineMissing = true;
        if (!missing.empty()) missing += i18n::text(i18n::Text::ListSeparator);
        missing += file.substr(file.find_last_of(L"\\/") + 1);
    }
    if (info.engineMissing) {
        info.repairAllowed = managedPath && owned;
        info.problem = i18n::text(i18n::Text::EngineMissingPrefix) + missing + i18n::text(i18n::Text::SentenceNewline) + (info.repairAllowed ?
            std::wstring(i18n::text(i18n::Text::EngineRepairAllowed)) :
            ownershipProblem + i18n::text(i18n::Text::EngineRepairDenied));
    }
    return info;
}
std::wstring stateName(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING: return i18n::text(i18n::Text::Running);
    case SERVICE_STOPPED: return i18n::text(i18n::Text::Stopped);
    case SERVICE_START_PENDING: return i18n::text(i18n::Text::Starting);
    case SERVICE_STOP_PENDING: return i18n::text(i18n::Text::Stopping);
    case SERVICE_PAUSED: return i18n::text(i18n::Text::Paused);
    default: return i18n::text(i18n::Text::StateChanging);
    }
}
std::wstring defaultShell() {
    wchar_t buffer[32768]{}; DWORD size = sizeof(buffer);
    auto code = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OpenSSH", L"DefaultShell", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size);
    return code == ERROR_SUCCESS ? buffer : i18n::text(i18n::Text::DefaultCmd);
}
std::string readPublicKeys(bool administrators) {
    auto path = publicKeyFilePath(administrators);
    regularPath(path);
    auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return {};
        fail(i18n::text(i18n::Text::OpReadKeys), code);
    }
    return readFile(path);
}
std::wstring managePublicKey(const std::string& input, bool remove, bool administrators, const std::string& revision, size_t row) {
    const bool exact = remove && !revision.empty();
    auto key = exact ? std::string{} : validatePublicKey(input);
    if (administrators != isElevated()) throw std::runtime_error(administrators ? i18n::utf8Text(i18n::Text::AdminRequired) :
        i18n::utf8Text(i18n::Text::PersonalKeyElevation));
    auto service = usableService();
    const auto target = publicKeyFilePath(administrators);
    std::wstring directory = parent(target), acl;
    if (administrators) {
        acl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
    } else {
        HANDLE tokenValue = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenValue), i18n::text(i18n::Text::OpReadSid));
        Handle token(tokenValue); DWORD size = 0;
        GetTokenInformation(token.h, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> bytes(size);
        require(GetTokenInformation(token.h, TokenUser, bytes.data(), size, &size), i18n::text(i18n::Text::OpReadSid));
        LPWSTR sidText = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sidText), i18n::text(i18n::Text::OpFormatAccountSid));
        std::wstring sid(sidText); LocalFree(sidText);
        acl = L"O:" + sid + L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" + sid + L")";
    }
    regularPath(directory);
    Security security(acl);
    if (!CreateDirectoryW(directory.c_str(), &security.sa) && GetLastError() != ERROR_ALREADY_EXISTS) fail(i18n::text(i18n::Text::OpCreateKeyDir));
    regularPath(target);
    std::string original;
    DWORD attr = GetFileAttributesW(target.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) original = readFile(target);
    else if (GetLastError() != ERROR_FILE_NOT_FOUND) fail(i18n::text(i18n::Text::OpReadKeys));
    std::string next;
    ProcessResult result{};
    if (exact) {
        if (digest(original) != revision) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyChanged));
        next = eraseAuthorizedKeyEntry(original, row);
    } else {
        TemporaryFile publicFile{uniquePath(directory, L".pub")};
        writeNew(publicFile.path, key + "\n", acl);
        auto keygen = parent(service.exe) + L"\\ssh-keygen.exe";
        result = execute(keygen, {L"-l", L"-f", publicFile.path});
        if (result.code != 0) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyRejected) + result.output);
        auto identity = [](const std::string& line) { std::istringstream in(line); std::string a, b; in >> a >> b; return a + " " + b; };
        const auto wanted = identity(key);
        bool found = false;
        size_t pos = 0;
        while (pos < original.size()) {
            size_t end = original.find('\n', pos); end = end == std::string::npos ? original.size() : end + 1;
            auto line = original.substr(pos, end - pos);
            bool matches = identity(line) == wanted;
            found = found || matches;
            if (!(remove && matches)) next += line;
            pos = end;
        }
        if (remove && !found) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyNotFound));
        if (!remove && found) return i18n::text(i18n::Text::KeyExists) + target + L"\r\n" + wide(result.output);
        if (!remove) { if (!next.empty() && next.back() != '\n') next += "\n"; next += key + "\n"; }
    }
    TemporaryFile candidate{uniquePath(directory, L".keys")};
    writeNew(candidate.path, next, acl);
    auto current = GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string{} : readFile(target);
    if (current != original) throw std::runtime_error(i18n::utf8Text(i18n::Text::KeysChangedValidation));
    require(MoveFileExW(candidate.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), i18n::text(i18n::Text::OpSaveKeys));
    candidate.path.clear();
    return std::wstring(remove ? i18n::text(i18n::Text::KeyDeleted) : i18n::text(i18n::Text::KeyImported)) + target + L"\r\n" + wide(result.output);
}
bool isElevated() {
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), i18n::text(i18n::Text::OpReadToken));
    Handle handle(token); TOKEN_ELEVATION value{}; DWORD size = 0;
    require(GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size), i18n::text(i18n::Text::OpReadElevation));
    return value.TokenIsElevated != 0;
}
HANDLE elevate(HWND owner, const std::vector<std::wstring>& args) {
    std::vector<std::wstring> localizedArgs{std::wstring(L"--ui-language=") + i18n::languageTag()};
    localizedArgs.insert(localizedArgs.end(), args.begin(), args.end());
    auto executable = modulePath(); auto parameters = joinArgs(localizedArgs);
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = owner; info.lpVerb = L"runas"; info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str(); info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        if (GetLastError() == ERROR_CANCELLED) return nullptr;
        fail(i18n::text(i18n::Text::OpLaunchAdmin));
    }
    return info.hProcess;
}
namespace {
std::string installEngine(const std::wstring& archive, bool repair) {
    auto info = serviceInfo();
    if (repair && (!info.installed || !info.repairAllowed)) throw std::runtime_error(i18n::utf8Text(i18n::Text::RepairRefused));
    if (!repair && info.installed) return i18n::utf8Text(i18n::Text::ExistingServiceUnchanged);
    auto root = parent(parent(managedEngineDirectory()));
    regularPath(root);
    auto stage = uniquePath(root, L".setup");
    Security directoryAcl(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
    require(CreateDirectoryW(stage.c_str(), &directoryAcl.sa), i18n::text(i18n::Text::OpProtectedSetup));
    struct Cleanup {
        std::wstring path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{stage};
    const wchar_t* privateAcl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
    const wchar_t* files[] = {L"Install.ps1", L"EnginePath.ps1", L"EngineArchive.ps1", L"Uninstall.ps1", L"engine.lock.json", L"Localization.ps1"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) writeNew(stage + L"\\" + files[i], setupResource(301 + static_cast<int>(i)), privateAcl);
    std::vector<std::wstring> args{L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File",
        stage + L"\\Install.ps1", L"-EngineOnly", L"-Elevated", L"-UiLanguage", i18n::languageTag()};
    if (repair) {
        if (archive.empty()) throw std::runtime_error(i18n::utf8Text(i18n::Text::RepairZipRequired));
        args.push_back(L"-Repair");
    }
    if (!archive.empty()) {
        // Read a locked snapshot and verify again after elevation. Never execute
        // scripts or binaries from the caller's writable download directory.
        auto bytes = verifiedEngineArchive(archive);
        auto copy = stage + L"\\engine.zip";
        writeNew(copy, bytes, privateAcl);
        args.insert(args.end(), {L"-Archive", copy});
    }
    wchar_t system[MAX_PATH]{};
    require(GetSystemDirectoryW(system, MAX_PATH) != 0, i18n::text(i18n::Text::OpLocatePowerShell));
    auto result = execute(std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe", args, 10 * 60 * 1000);
    if (result.code != 0) throw std::runtime_error(i18n::utf8Text(i18n::Text::EngineInstallFailed) + result.output);
    return result.output;
}
}
int adminMain(const std::vector<std::wstring>& args) {
    try {
        if (!isElevated()) throw std::runtime_error(i18n::utf8Text(i18n::Text::ElevationRequired));
        Security security(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
        Handle mutex(CreateMutexW(&security.sa, FALSE, L"Global\\OpenSSHManager.AdminOperation"));
        require(mutex.h != nullptr, i18n::text(i18n::Text::OpConfigLock));
        DWORD lock = WaitForSingleObject(mutex.h, 0);
        if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) throw std::runtime_error(i18n::utf8Text(i18n::Text::AnotherOperation));
        struct LockGuard { HANDLE h; ~LockGuard() { ReleaseMutex(h); } } lockGuard{mutex.h};
        if (args.empty()) throw std::runtime_error(i18n::utf8Text(i18n::Text::MissingOperation));
        if ((args[0] == L"key-list" && args.size() == 2) || (args[0] == L"key-import" && args.size() == 3) ||
            (args[0] == L"key-delete" && args.size() == 4)) {
            // Connect and authenticate the requesting GUI before any mutation.
            KeyChannelWriter response(args.back());
            if (args[0] == L"key-import") managePublicKey(utf8(args[1]), false, true);
            if (args[0] == L"key-delete") {
                auto index = utf8(args[2]);
                if (index.empty() || index.size() > 10 || index.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error(i18n::utf8Text(i18n::Text::InvalidKeyRow));
                managePublicKey({}, true, true, utf8(args[1]), static_cast<size_t>(std::stoull(index)));
            }
            response.send(readPublicKeys(true));
            return 0;
        }
        if ((args[0] == L"engine-install" || args[0] == L"engine-repair") && args.size() == 3) {
            KeyChannelWriter response(args[2]);
            try { response.send("OK\n" + installEngine(args[1], args[0] == L"engine-repair")); return 0; }
            catch (const std::exception& e) { response.send("ERROR\n" + std::string(e.what())); return 1; }
        }
        auto info = usableService();
        if (args[0] == L"apply" && args.size() == 9) {
            Settings s; s.port = utf8(args[2]); s.listen = utf8(args[3]);
            for (size_t i = 4; i <= 6; ++i) if (args[i] != L"yes" && args[i] != L"no") throw std::runtime_error(i18n::utf8Text(i18n::Text::InvalidBoolean));
            s.password = args[4] == L"yes"; s.pubkey = args[5] == L"yes"; s.forwarding = args[6] == L"yes";
            s.users = utf8(args[7]); s.groups = utf8(args[8]);
            apply(info, s, utf8(args[1]));
        } else if (args[0] == L"validate" && args.size() == 1) {
            validate(info, info.config);
            MessageBoxW(nullptr, i18n::text(i18n::Text::ConfigCheckPassed), L"OpenSSH Manager", MB_OK | MB_ICONINFORMATION);
        } else if (args[0] == L"shell" && args.size() == 2) {
            setShell(args[1]);
        } else if (args[0] == L"firewall" && args.size() == 1) {
            firewall(info);
        } else if ((args[0] == L"key-add" || args[0] == L"key-remove") && args.size() == 2) {
            auto result = managePublicKey(utf8(args[1]), args[0] == L"key-remove", true);
            MessageBoxW(nullptr, result.c_str(), i18n::text(i18n::Text::AuthorizedKeys), MB_OK | MB_ICONINFORMATION);
        } else if (args.size() == 1 && (args[0] == L"start" || args[0] == L"stop" || args[0] == L"restart" || args[0] == L"auto" || args[0] == L"manual")) {
            ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
            require(scm.h != nullptr, i18n::text(i18n::Text::OpOpenScm));
            ServiceHandle service(OpenServiceW(scm.h, L"sshd", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG));
            require(service.h != nullptr, i18n::text(i18n::Text::OpOpenSshd));
            if (args[0] == L"auto" || args[0] == L"manual") {
                require(ChangeServiceConfigW(service.h, SERVICE_NO_CHANGE, args[0] == L"auto" ? SERVICE_AUTO_START : SERVICE_DEMAND_START,
                    SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr), i18n::text(i18n::Text::OpChangeStartup));
            } else {
                if (args[0] != L"stop") {
                    DWORD attributes = GetFileAttributesW(info.config.c_str());
                    // A newly enabled Windows optional feature initializes its default config on first start.
                    if (args[0] != L"start" || attributes != INVALID_FILE_ATTRIBUTES ||
                        (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)) validate(info, info.config);
                }
                if (args[0] != L"start") stop(service.h);
                if (args[0] != L"stop") start(service.h);
            }
        } else throw std::runtime_error(i18n::utf8Text(i18n::Text::UnknownOperation));
        return 0;
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, wide(e.what()).c_str(), i18n::text(i18n::Text::OperationFailedTitle), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }
}
}
