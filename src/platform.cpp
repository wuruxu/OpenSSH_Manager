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
            throw std::runtime_error("Reparse-point paths are not supported: " + utf8(p));
        p = parent(p);
    }
}
struct Security {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(const std::wstring& sddl) {
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr), L"Create ACL");
        sa.lpSecurityDescriptor = sd;
    }
    ~Security() { LocalFree(sd); }
};
const wchar_t* configAcl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)";
void writeNew(const std::wstring& path, const std::string& text, const std::wstring& acl = configAcl) {
    regularPath(path);
    Security security(acl);
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, &security.sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(file.h != INVALID_HANDLE_VALUE, L"Create file: " + path);
    DWORD written = 0;
    require(WriteFile(file.h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size(), L"Write file");
    require(FlushFileBuffers(file.h), L"Flush file");
}
std::wstring uniquePath(const std::wstring& directory, const std::wstring& suffix) {
    GUID guid{};
    require(SUCCEEDED(CoCreateGuid(&guid)), L"Generate temporary filename");
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
    require(CreatePipe(&read, &write, &sa, 0), L"Create process output pipe");
    Handle reader(read), writer(write);
    require(SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0), L"Configure pipe");
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
    require(input.h != INVALID_HANDLE_VALUE, L"Open NUL");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> attributeBytes(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBytes.data());
    require(InitializeProcThreadAttributeList(attributes, 1, 0, &size), L"Create process attributes");
    struct AttributesGuard { LPPROC_THREAD_ATTRIBUTE_LIST p; ~AttributesGuard() { DeleteProcThreadAttributeList(p); } } guard{attributes};
    HANDLE inherited[] = {write, input.h};
    require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr), L"Restrict inherited handles");
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
    require(job.h != nullptr, L"Create process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), L"Configure process job");
    require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, parent(exe).c_str(), &startup.StartupInfo, &pi), L"Run " + exe);
    Handle process(pi.hProcess), thread(pi.hThread);
    if (!AssignProcessToJobObject(job.h, process.h)) { auto e = GetLastError(); TerminateProcess(process.h, 1); fail(L"Assign validation process to job", e); }
    require(ResumeThread(thread.h) != static_cast<DWORD>(-1), L"Resume process");
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
            require(GetExitCodeProcess(process.h, &result.code), L"Read process result");
            return result;
        }
        if (GetTickCount64() - started > timeout) throw std::runtime_error("OpenSSH operation timed out; the child process was terminated.");
    }
}
void validate(const ServiceInfo& service, const std::wstring& file) {
    auto result = execute(service.exe, {L"-t", L"-f", file});
    if (result.code != 0) throw std::runtime_error("sshd configuration check failed:\r\n" + result.output);
}
SERVICE_STATUS_PROCESS query(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{}; DWORD needed = 0;
    require(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed), L"Query sshd status");
    return status;
}
void awaitState(SC_HANDLE service, DWORD desired) {
    auto started = GetTickCount64();
    for (;;) {
        auto state = query(service);
        if (state.dwCurrentState == desired) return;
        if (desired == SERVICE_RUNNING && state.dwCurrentState == SERVICE_STOPPED)
            fail(L"sshd stopped during startup. Check OpenSSH event logs.", state.dwWin32ExitCode ? state.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE);
        if (GetTickCount64() - started > 30000) throw std::runtime_error("Timed out waiting for sshd service.");
        Sleep(200);
    }
}
void stop(SC_HANDLE service) {
    auto state = query(service).dwCurrentState;
    if (state == SERVICE_STOPPED) return;
    if (state != SERVICE_STOP_PENDING) { SERVICE_STATUS status{}; require(ControlService(service, SERVICE_CONTROL_STOP, &status), L"Stop sshd"); }
    awaitState(service, SERVICE_STOPPED);
}
void start(SC_HANDLE service) {
    if (query(service).dwCurrentState != SERVICE_RUNNING) {
        require(StartServiceW(service, 0, nullptr), L"Start sshd");
        awaitState(service, SERVICE_RUNNING);
    }
}
ServiceInfo usableService() {
    auto info = serviceInfo();
    if (!info.installed) throw std::runtime_error("OpenSSH sshd service is not installed. Run Install.ps1 first.");
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
            throw std::runtime_error("Invalid IPv4/IPv6 listen address.");
    }
    auto original = readFile(info.config);
    if (digest(original) != revision) throw std::runtime_error("Configuration changed since it was loaded. Reload it before saving.");
    auto text = renderConfig(parseConfig(original), settings);
    TemporaryFile candidate{uniquePath(parent(info.config), L".conf")};
    writeNew(candidate.path, text);
    validate(info, candidate.path);
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    require(scm.h != nullptr, L"Open service manager");
    ServiceHandle service(OpenServiceW(scm.h, L"sshd", SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP));
    require(service.h != nullptr, L"Open sshd");
    auto state = query(service.h).dwCurrentState;
    if (state != SERVICE_RUNNING && state != SERVICE_STOPPED) throw std::runtime_error("Service is changing state. Wait and try again.");
    bool running = state == SERVICE_RUNNING;
    const bool syncFirewall = firewallOwned();
    if (digest(readFile(info.config)) != revision) throw std::runtime_error("Configuration changed during validation. Nothing was saved.");
    const auto backup = uniquePath(parent(info.config), L".backup");
    // ReplaceFile preserves the original file's ACL and makes a recovery copy.
    require(ReplaceFileW(info.config.c_str(), candidate.path.c_str(), backup.c_str(), 0, nullptr, nullptr), L"Replace sshd configuration");
    candidate.path.clear();
    try {
        if (running) { stop(service.h); start(service.h); }
        if (syncFirewall) firewall(info);
    } catch (const std::exception& e) {
        std::string message = e.what();
        // Restore config even if stopping the new service fails.
        BOOL restored = ReplaceFileW(info.config.c_str(), backup.c_str(), nullptr, 0, nullptr, nullptr);
        if (!restored) message += "\r\nROLLBACK FAILED. Recovery copy: " + utf8(backup) + "\r\n" + utf8(errorText(GetLastError()));
        else {
            message += "\r\nPrevious configuration restored.";
            if (syncFirewall) {
                try { firewall(info); }
                catch (const std::exception& recovery) { message += "\r\nFirewall recovery failed: " + std::string(recovery.what()); }
            }
            if (running) {
                try { stop(service.h); start(service.h); }
                catch (const std::exception& recovery) { message += "\r\nService recovery failed: " + std::string(recovery.what()); }
            }
        }
        throw std::runtime_error(message);
    }
}
std::wstring systemDirectory() {
    wchar_t buffer[MAX_PATH]{};
    require(GetSystemDirectoryW(buffer, MAX_PATH) != 0, L"Locate Windows system directory");
    return buffer;
}
void setShell(const std::wstring& choice) {
    std::wstring shell;
    if (choice == L"cmd") shell = systemDirectory() + L"\\cmd.exe";
    else if (choice == L"powershell") shell = systemDirectory() + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    else throw std::runtime_error("Unknown shell selection.");
    require(GetFileAttributesW(shell.c_str()) != INVALID_FILE_ATTRIBUTES, L"Shell is not installed");
    HKEY key = nullptr;
    auto code = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OpenSSH", 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (code != ERROR_SUCCESS) fail(L"Open shell registry setting", code);
    code = RegSetValueExW(key, L"DefaultShell", 0, REG_SZ, reinterpret_cast<const BYTE*>(shell.c_str()), static_cast<DWORD>((shell.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (code != ERROR_SUCCESS) fail(L"Save shell setting", code);
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
    comCheck(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy.p)), L"Open Windows Firewall policy");
    ComPtr<INetFwRules> rules;
    comCheck(policy->get_Rules(&rules.p), L"Read firewall rules");
    ComPtr<INetFwRule> existing;
    Bstr name(L"OpenSSHManager-Server");
    auto hr = rules->Item(name, &existing.p);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return false;
    comCheck(hr, L"Read OpenSSH Manager firewall rule");
    return existing.p != nullptr;
}
void firewall(const ServiceInfo& service) {
    auto config = parseConfig(readFile(service.config));
    if (!config.problem.empty()) throw std::runtime_error(config.problem);
    validateSettings(config.settings);
    ComPtr<INetFwPolicy2> policy;
    comCheck(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), reinterpret_cast<void**>(&policy.p)), L"Open Windows Firewall policy");
    ComPtr<INetFwRules> rules;
    comCheck(policy->get_Rules(&rules.p), L"Read firewall rules");
    ComPtr<INetFwRule> rule;
    comCheck(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), reinterpret_cast<void**>(&rule.p)), L"Create firewall rule");
    Bstr name(L"OpenSSHManager-Server"), description(L"OpenSSH Manager: SSH on domain/private profiles, local subnet only."),
        executable(service.exe), ports(wide(config.settings.port)), remote(L"LocalSubnet");
    comCheck(rule->put_Name(name), L"Set firewall name");
    comCheck(rule->put_Description(description), L"Set firewall description");
    comCheck(rule->put_ApplicationName(executable), L"Set firewall application");
    comCheck(rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP), L"Set firewall protocol");
    comCheck(rule->put_LocalPorts(ports), L"Set firewall port");
    comCheck(rule->put_RemoteAddresses(remote), L"Set firewall remote scope");
    comCheck(rule->put_Direction(NET_FW_RULE_DIR_IN), L"Set firewall direction");
    comCheck(rule->put_Profiles(NET_FW_PROFILE2_DOMAIN | NET_FW_PROFILE2_PRIVATE), L"Set firewall profiles");
    comCheck(rule->put_Action(NET_FW_ACTION_ALLOW), L"Set firewall action");
    comCheck(rule->put_Enabled(VARIANT_TRUE), L"Enable firewall rule");
    // Add replaces a rule with the same name, leaving unrelated rules intact.
    comCheck(rules->Add(rule.p), L"Save OpenSSH Manager firewall rule");
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
    std::wstring result = buffer ? buffer : L"Unknown Windows error";
    LocalFree(buffer);
    return result + L" (" + std::to_wstring(error) + L")";
}
std::wstring modulePath() {
    std::vector<wchar_t> path(32768);
    auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(n && n < path.size(), L"Locate manager executable");
    return std::wstring(path.data(), n);
}
std::wstring dataPath() {
    PWSTR path = nullptr;
    auto hr = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path);
    if (FAILED(hr)) fail(L"Locate ProgramData", static_cast<DWORD>(hr));
    std::wstring out(path); CoTaskMemFree(path);
    return out + L"\\ssh";
}
AccountIdentity currentAccountIdentity() {
    HANDLE tokenValue = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenValue), L"Read current account");
    Handle token(tokenValue);
    auto readToken = [&](TOKEN_INFORMATION_CLASS type) {
        DWORD size = 0;
        GetTokenInformation(token.h, type, nullptr, 0, &size);
        require(size != 0, L"Query account information size");
        std::vector<BYTE> buffer(size);
        require(GetTokenInformation(token.h, type, buffer.data(), size, &size), L"Read account information");
        return buffer;
    };
    AccountIdentity result;
    auto userBytes = readToken(TokenUser);
    PSID sid = reinterpret_cast<TOKEN_USER*>(userBytes.data())->User.Sid;
    DWORD nameSize = 0, domainSize = 0;
    SID_NAME_USE use{};
    LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &use);
    require(nameSize != 0, L"Resolve current account name");
    std::vector<wchar_t> name(nameSize), domain((std::max)(domainSize, DWORD(1)));
    require(LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &use), L"Resolve current account name");
    result.name = name.data();
    result.qualifiedName = domain[0] ? std::wstring(domain.data()) + L"\\" + result.name : result.name;

    auto groupBytes = readToken(TokenGroups);
    auto groups = reinterpret_cast<TOKEN_GROUPS*>(groupBytes.data());
    alignas(DWORD) BYTE administrators[SECURITY_MAX_SID_SIZE]{};
    DWORD sidSize = sizeof(administrators);
    require(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &sidSize), L"Create Administrators SID");
    for (DWORD i = 0; i < groups->GroupCount; ++i) {
        // UAC-filtered administrator tokens retain this SID as DENY_ONLY.
        // This is an account identity hint, never an authorization decision.
        if (EqualSid(groups->Groups[i].Sid, administrators)) { result.administrator = true; break; }
    }
    TOKEN_ELEVATION elevation{}; DWORD size = 0;
    require(GetTokenInformation(token.h, TokenElevation, &elevation, sizeof(elevation), &size), L"Read elevation state");
    result.elevated = elevation.TokenIsElevated != 0;
    return result;
}
std::wstring publicKeyFilePath(bool administrators) {
    if (administrators) return dataPath() + L"\\administrators_authorized_keys";
    PWSTR profile = nullptr;
    comCheck(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile), L"Locate current profile");
    std::wstring path = std::wstring(profile) + L"\\.ssh\\authorized_keys";
    CoTaskMemFree(profile);
    return path;
}
std::string readFile(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(file.h != INVALID_HANDLE_VALUE, L"Read file: " + path);
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.h, &info), L"Inspect file");
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Expected a regular file.");
    if (info.nFileSizeHigh || info.nFileSizeLow > 4 * 1024 * 1024) throw std::runtime_error("File exceeds 4 MiB.");
    std::string text(info.nFileSizeLow, 0); DWORD got = 0;
    require(ReadFile(file.h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr), L"Read file contents");
    if (got != text.size()) throw std::runtime_error("File changed while being read.");
    return text;
}
std::string digest(const std::string& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("SHA-256 unavailable.");
    unsigned char hash[32]{};
    auto result = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash, sizeof(hash));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) throw std::runtime_error("SHA-256 failed.");
    std::string out;
    const char* hex = "0123456789abcdef";
    for (auto b : hash) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
}
ServiceInfo serviceInfo() {
    ServiceInfo info; info.config = dataPath() + L"\\sshd_config";
    ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    require(scm.h != nullptr, L"Open service manager");
    ServiceHandle service(OpenServiceW(scm.h, L"sshd", SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!service.h) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return info;
        fail(L"Read sshd service");
    }
    info.installed = true;
    auto status = query(service.h);
    info.state = status.dwCurrentState; info.pid = status.dwProcessId;
    DWORD needed = 0;
    QueryServiceConfigW(service.h, nullptr, 0, &needed);
    std::vector<BYTE> bytes(needed);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
    require(QueryServiceConfigW(service.h, config, needed, &needed), L"Read sshd command line");
    info.startType = config->dwStartType;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(config->lpBinaryPathName, &count);
    LocalMemory arguments; arguments.p = argv;
    if (!argv || count < 1) { info.problem = L"无法解析 sshd 服务命令行。"; return info; }
    info.exe = argv[0];
    auto name = info.exe.substr(info.exe.find_last_of(L"\\/") + 1);
    if (_wcsicmp(name.c_str(), L"sshd.exe") != 0 || info.exe.size() < 3 || info.exe[1] != L':')
        info.problem = L"只支持使用绝对路径注册的原生 sshd.exe 服务。";
    else info.version = executableVersion(info.exe);
    // V1 deliberately manages the standard configuration only. Never silently edit a different instance.
    if (count != 1) info.problem = L"此 sshd 服务带自定义启动参数。第一版仅管理使用默认配置的标准服务。";
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
            if (!owned) ownershipProblem = L"安装标记与服务归属不匹配。";
        } catch (...) { ownershipProblem = L"安装标记缺失、无效或不可读取。"; }
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
                info.problem = L"无法检查引擎文件：" + file + L"\r\n" + errorText(code); return info;
            }
        } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        info.engineMissing = true;
        if (!missing.empty()) missing += L"、";
        missing += file.substr(file.find_last_of(L"\\/") + 1);
    }
    if (info.engineMissing) {
        info.repairAllowed = managedPath && owned;
        info.problem = L"引擎缺失：" + missing + L"。\r\n" + (info.repairAllowed ?
            std::wstring(L"服务路径与安装标记匹配，可下载引擎并修复安装。") :
            ownershipProblem + L"无法确认属于本程序，不允许自动修复或覆盖。请使用原安装方式恢复引擎。");
    }
    return info;
}
std::wstring stateName(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING: return L"运行中";
    case SERVICE_STOPPED: return L"已停止";
    case SERVICE_START_PENDING: return L"正在启动";
    case SERVICE_STOP_PENDING: return L"正在停止";
    case SERVICE_PAUSED: return L"已暂停";
    default: return L"状态变化中";
    }
}
std::wstring defaultShell() {
    wchar_t buffer[32768]{}; DWORD size = sizeof(buffer);
    auto code = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OpenSSH", L"DefaultShell", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buffer, &size);
    return code == ERROR_SUCCESS ? buffer : L"cmd.exe（系统默认）";
}
std::string readPublicKeys(bool administrators) {
    auto path = publicKeyFilePath(administrators);
    regularPath(path);
    auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return {};
        fail(L"Read authorized_keys", code);
    }
    return readFile(path);
}
std::wstring managePublicKey(const std::string& input, bool remove, bool administrators, const std::string& revision, size_t row) {
    const bool exact = remove && !revision.empty();
    auto key = exact ? std::string{} : validatePublicKey(input);
    if (administrators != isElevated()) throw std::runtime_error(administrators ? "Administrator elevation is required." :
        "Import personal keys from a non-elevated GUI. This prevents editing another identity's profile.");
    auto service = usableService();
    const auto target = publicKeyFilePath(administrators);
    std::wstring directory = parent(target), acl;
    if (administrators) {
        acl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
    } else {
        HANDLE tokenValue = nullptr;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenValue), L"Read account SID");
        Handle token(tokenValue); DWORD size = 0;
        GetTokenInformation(token.h, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> bytes(size);
        require(GetTokenInformation(token.h, TokenUser, bytes.data(), size, &size), L"Read account SID");
        LPWSTR sidText = nullptr;
        require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sidText), L"Format account SID");
        std::wstring sid(sidText); LocalFree(sidText);
        acl = L"O:" + sid + L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" + sid + L")";
    }
    regularPath(directory);
    Security security(acl);
    if (!CreateDirectoryW(directory.c_str(), &security.sa) && GetLastError() != ERROR_ALREADY_EXISTS) fail(L"Create SSH key directory");
    regularPath(target);
    std::string original;
    DWORD attr = GetFileAttributesW(target.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) original = readFile(target);
    else if (GetLastError() != ERROR_FILE_NOT_FOUND) fail(L"Read authorized_keys");
    std::string next;
    ProcessResult result{};
    if (exact) {
        if (digest(original) != revision) throw std::runtime_error("公钥文件已被修改，请刷新列表后重新删除。");
        next = eraseAuthorizedKeyEntry(original, row);
    } else {
        TemporaryFile publicFile{uniquePath(directory, L".pub")};
        writeNew(publicFile.path, key + "\n", acl);
        auto keygen = parent(service.exe) + L"\\ssh-keygen.exe";
        result = execute(keygen, {L"-l", L"-f", publicFile.path});
        if (result.code != 0) throw std::runtime_error("ssh-keygen rejected the public key:\r\n" + result.output);
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
        if (remove && !found) throw std::runtime_error("Key was not found. Entries with authorized_keys options must be edited manually.");
        if (!remove && found) return L"该公钥已存在。\r\n" + target + L"\r\n" + wide(result.output);
        if (!remove) { if (!next.empty() && next.back() != '\n') next += "\n"; next += key + "\n"; }
    }
    TemporaryFile candidate{uniquePath(directory, L".keys")};
    writeNew(candidate.path, next, acl);
    auto current = GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES ? std::string{} : readFile(target);
    if (current != original) throw std::runtime_error("authorized_keys changed during validation. Retry the operation.");
    require(MoveFileExW(candidate.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), L"Save authorized_keys");
    candidate.path.clear();
    return std::wstring(remove ? L"已删除公钥。\r\n" : L"已导入公钥。\r\n") + target + L"\r\n" + wide(result.output);
}
bool isElevated() {
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), L"Read process token");
    Handle handle(token); TOKEN_ELEVATION value{}; DWORD size = 0;
    require(GetTokenInformation(token, TokenElevation, &value, sizeof(value), &size), L"Read elevation state");
    return value.TokenIsElevated != 0;
}
HANDLE elevate(HWND owner, const std::vector<std::wstring>& args) {
    auto executable = modulePath(); auto parameters = joinArgs(args);
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.hwnd = owner; info.lpVerb = L"runas"; info.lpFile = executable.c_str();
    info.lpParameters = parameters.c_str(); info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        if (GetLastError() == ERROR_CANCELLED) return nullptr;
        fail(L"Launch administrator operation");
    }
    return info.hProcess;
}
namespace {
std::string installEngine(const std::wstring& archive, bool repair) {
    auto info = serviceInfo();
    if (repair && (!info.installed || !info.repairAllowed)) throw std::runtime_error("Repair refused: service path, ownership marker or missing-engine state changed. Reload first.");
    if (!repair && info.installed) return "An existing sshd service was detected; no changes were made.";
    auto root = parent(parent(managedEngineDirectory()));
    regularPath(root);
    auto stage = uniquePath(root, L".setup");
    Security directoryAcl(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
    require(CreateDirectoryW(stage.c_str(), &directoryAcl.sa), L"Create protected installer directory");
    struct Cleanup {
        std::wstring path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{stage};
    const wchar_t* privateAcl = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)";
    const wchar_t* files[] = {L"Install.ps1", L"EnginePath.ps1", L"EngineArchive.ps1", L"Uninstall.ps1", L"engine.lock.json"};
    for (int i = 0; i < 5; ++i) writeNew(stage + L"\\" + files[i], setupResource(301 + i), privateAcl);
    std::vector<std::wstring> args{L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File",
        stage + L"\\Install.ps1", L"-EngineOnly", L"-Elevated"};
    if (repair) {
        if (archive.empty()) throw std::runtime_error("A verified engine ZIP is required for repair.");
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
    require(GetSystemDirectoryW(system, MAX_PATH) != 0, L"Locate PowerShell");
    auto result = execute(std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe", args, 10 * 60 * 1000);
    if (result.code != 0) throw std::runtime_error("Engine installation failed:\r\n" + result.output);
    return result.output;
}
}
int adminMain(const std::vector<std::wstring>& args) {
    try {
        if (!isElevated()) throw std::runtime_error("This operation requires administrator elevation.");
        Security security(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
        Handle mutex(CreateMutexW(&security.sa, FALSE, L"Global\\OpenSSHManager.AdminOperation"));
        require(mutex.h != nullptr, L"Open configuration transaction lock");
        DWORD lock = WaitForSingleObject(mutex.h, 0);
        if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) throw std::runtime_error("Another management operation is still running.");
        struct LockGuard { HANDLE h; ~LockGuard() { ReleaseMutex(h); } } lockGuard{mutex.h};
        if (args.empty()) throw std::runtime_error("Missing operation.");
        if ((args[0] == L"key-list" && args.size() == 2) || (args[0] == L"key-import" && args.size() == 3) ||
            (args[0] == L"key-delete" && args.size() == 4)) {
            // Connect and authenticate the requesting GUI before any mutation.
            KeyChannelWriter response(args.back());
            if (args[0] == L"key-import") managePublicKey(utf8(args[1]), false, true);
            if (args[0] == L"key-delete") {
                auto index = utf8(args[2]);
                if (index.empty() || index.size() > 10 || index.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("Invalid public key row.");
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
            for (size_t i = 4; i <= 6; ++i) if (args[i] != L"yes" && args[i] != L"no") throw std::runtime_error("Invalid boolean.");
            s.password = args[4] == L"yes"; s.pubkey = args[5] == L"yes"; s.forwarding = args[6] == L"yes";
            s.users = utf8(args[7]); s.groups = utf8(args[8]);
            apply(info, s, utf8(args[1]));
        } else if (args[0] == L"validate" && args.size() == 1) {
            validate(info, info.config);
            MessageBoxW(nullptr, L"sshd 配置检查通过。", L"OpenSSH Manager", MB_OK | MB_ICONINFORMATION);
        } else if (args[0] == L"shell" && args.size() == 2) {
            setShell(args[1]);
        } else if (args[0] == L"firewall" && args.size() == 1) {
            firewall(info);
        } else if ((args[0] == L"key-add" || args[0] == L"key-remove") && args.size() == 2) {
            auto result = managePublicKey(utf8(args[1]), args[0] == L"key-remove", true);
            MessageBoxW(nullptr, result.c_str(), L"授权公钥", MB_OK | MB_ICONINFORMATION);
        } else if (args.size() == 1 && (args[0] == L"start" || args[0] == L"stop" || args[0] == L"restart" || args[0] == L"auto" || args[0] == L"manual")) {
            ServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
            require(scm.h != nullptr, L"Open service manager");
            ServiceHandle service(OpenServiceW(scm.h, L"sshd", SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG));
            require(service.h != nullptr, L"Open sshd");
            if (args[0] == L"auto" || args[0] == L"manual") {
                require(ChangeServiceConfigW(service.h, SERVICE_NO_CHANGE, args[0] == L"auto" ? SERVICE_AUTO_START : SERVICE_DEMAND_START,
                    SERVICE_NO_CHANGE, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr), L"Change startup type");
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
        } else throw std::runtime_error("Unknown administrator operation or invalid arguments.");
        return 0;
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, wide(e.what()).c_str(), L"OpenSSH Manager — 操作失败", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 1;
    }
}
}
