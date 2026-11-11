#include "i18n.h"
#include "bootstrap.h"
#include "platform.h"
#include "engine_release.h"
#include <winhttp.h>
#include <shlobj.h>
#include <process.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
namespace manager {
namespace {
constexpr size_t archiveLimit = 64 * 1024 * 1024;
void require(bool ok, const char* context) {
    if (!ok) throw std::runtime_error(std::string(context) + ": " + utf8(errorText(GetLastError())));
}
struct File { HANDLE h; ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } };
struct Internet { HINTERNET h; ~Internet() { if (h) WinHttpCloseHandle(h); } };
std::wstring folder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &value))) throw std::runtime_error(i18n::utf8Text(i18n::Text::WindowsFolderMissing));
    std::wstring result(value); CoTaskMemFree(value); return result;
}
std::wstring directory(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }
bool exists(const std::wstring& path) {
    DWORD flags = GetFileAttributesW(path.c_str());
    if (flags != INVALID_FILE_ATTRIBUTES) return !(flags & FILE_ATTRIBUTE_DIRECTORY);
    DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) require(false, i18n::utf8Text(i18n::Text::OpEngineFile));
    return false;
}
void checkDigest(const std::string& bytes) {
    if (bytes.empty() || bytes.size() > archiveLimit || digest(bytes) != engineRelease::sha256)
        throw std::runtime_error(i18n::utf8Text(i18n::Text::ZipChecksum));
}
}
std::wstring managedEngineDirectory() { return folder(FOLDERID_ProgramFiles) + L"\\OpenSSHManager\\engine"; }
std::string verifiedEngineArchive(const std::wstring& path) {
    File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    require(file.h != INVALID_HANDLE_VALUE, i18n::utf8Text(i18n::Text::OpOpenZip));
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(file.h, &info), i18n::utf8Text(i18n::Text::OpZipAttributes));
    if (GetFileType(file.h) != FILE_TYPE_DISK || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        throw std::runtime_error(i18n::utf8Text(i18n::Text::ZipRegularFile));
    LARGE_INTEGER size{};
    require(GetFileSizeEx(file.h, &size), i18n::utf8Text(i18n::Text::OpZipSize));
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(archiveLimit)) throw std::runtime_error(i18n::utf8Text(i18n::Text::ZipSize));
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD got = 0;
        require(ReadFile(file.h, bytes.data() + offset, static_cast<DWORD>((std::min)(bytes.size() - offset, size_t(65536))), &got, nullptr) && got, i18n::utf8Text(i18n::Text::OpReadZip));
        offset += got;
    }
    checkDigest(bytes); return bytes;
}
std::string setupResource(int id) {
    auto module = GetModuleHandleW(nullptr);
    auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    require(resource != nullptr, i18n::utf8Text(i18n::Text::OpFindInstaller));
    auto memory = LoadResource(module, resource);
    auto data = static_cast<const char*>(LockResource(memory));
    DWORD size = SizeofResource(module, resource);
    require(data && size, i18n::utf8Text(i18n::Text::OpReadInstaller));
    return {data, size};
}
EngineSetup::~EngineSetup() {
    canceled = true;
    if (thread) { WaitForSingleObject(thread, INFINITE); CloseHandle(thread); }
}
void EngineSetup::publish(SetupStage stage, const std::wstring& message, int percent, const std::wstring& archive) {
    std::lock_guard<std::mutex> lock(mutex);
    update = {stage, message, archive, percent};
}
SetupUpdate EngineSetup::snapshot() { std::lock_guard<std::mutex> lock(mutex); return update; }
void EngineSetup::cancel() { canceled = true; }
void EngineSetup::start(const std::wstring& path, bool download) {
    if (thread) {
        if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) throw std::runtime_error(i18n::utf8Text(i18n::Text::PreparationRunning));
        CloseHandle(thread); thread = nullptr;
    }
    canceled = false; offline = path; allowDownload = download;
    publish(SetupStage::Preparing, i18n::text(i18n::Text::CheckingEngine));
    thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, entry, this, 0, nullptr));
    if (!thread) { publish(SetupStage::Failed, i18n::text(i18n::Text::DownloadWorkerFailed)); throw std::runtime_error(i18n::utf8Text(i18n::Text::EngineWorkerFailed)); }
}
unsigned __stdcall EngineSetup::entry(void* context) {
    auto& self = *static_cast<EngineSetup*>(context);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try { self.prepare(); }
    catch (const std::exception& e) {
        self.publish(self.canceled ? SetupStage::Canceled : SetupStage::Failed,
            self.canceled ? i18n::text(i18n::Text::PreparationCanceled) : wide(e.what()));
    }
    CoUninitialize(); return 0;
}
void EngineSetup::prepare() {
    auto cancelPoint = [&] { if (canceled) throw std::runtime_error(i18n::utf8Text(i18n::Text::Canceled)); };
    auto service = serviceInfo();
    if (service.installed && !service.repairAllowed) { publish(SetupStage::Existing, i18n::text(i18n::Text::ServiceDetected)); return; }
    auto engine = managedEngineDirectory();
    bool reusable = exists(directory(engine) + L"\\InstallState.json");
    for (auto file : {L"sshd.exe", L"sshd-auth.exe", L"sshd-session.exe", L"sftp-server.exe", L"ssh-keygen.exe", L"ssh-agent.exe", L"libcrypto.dll", L"sshd_config_default", L"openssh-events.man"})
        reusable = exists(engine + L"\\" + file) && reusable;
    cancelPoint();
    if (!service.installed && reusable && offline.empty()) { publish(SetupStage::Ready, i18n::text(i18n::Text::LocalEngineReady)); return; }
    auto cache = folder(FOLDERID_LocalAppData) + L"\\OpenSSHManager";
    auto archive = cache + L"\\OpenSSH-" + wide(engineRelease::sha256) + L".zip";
    std::string bytes;
    if (!offline.empty()) {
        publish(SetupStage::Preparing, i18n::text(i18n::Text::VerifyOffline));
        bytes = verifiedEngineArchive(offline);
    } else {
        auto local = directory(modulePath()) + L"\\OpenSSH-Win64.zip";
        if (exists(local)) {
            publish(SetupStage::Preparing, i18n::text(i18n::Text::VerifyAdjacent));
            try {
                bytes = verifiedEngineArchive(local);
                if (!allowDownload) { cancelPoint(); publish(SetupStage::Ready, i18n::text(i18n::Text::OfflineReady), -1, local); return; }
            } catch (...) { if (!allowDownload) throw; bytes.clear(); }
        }
        if (bytes.empty() && exists(archive)) {
            try { bytes = verifiedEngineArchive(archive); } catch (...) { bytes.clear(); }
        }
    }
    if (!allowDownload && offline.empty()) {
        cancelPoint();
        if (bytes.empty()) publish(SetupStage::NeedsDownload, i18n::text(i18n::Text::EngineNotReady));
        else publish(SetupStage::Ready, i18n::text(i18n::Text::CacheReady), -1, archive);
        return; // Detection never enters the network or installation path.
    }
    if (bytes.empty()) {
        publish(SetupStage::Preparing, i18n::text(i18n::Text::DownloadingMicrosoft) + std::wstring(engineRelease::tag) + L"…", 0);
        URL_COMPONENTS url{}; url.dwStructSize = sizeof(url); url.dwHostNameLength = url.dwUrlPathLength = url.dwExtraInfoLength = static_cast<DWORD>(-1);
        require(WinHttpCrackUrl(engineRelease::url, 0, 0, &url), i18n::utf8Text(i18n::Text::OpDownloadUrl));
        if (url.nScheme != INTERNET_SCHEME_HTTPS) throw std::runtime_error(i18n::utf8Text(i18n::Text::HttpsRequired));
        std::wstring host(url.lpszHostName, url.dwHostNameLength), path(url.lpszUrlPath, url.dwUrlPathLength);
        if (url.dwExtraInfoLength) path.append(url.lpszExtraInfo, url.dwExtraInfoLength);
        Internet session{WinHttpOpen(L"OpenSSHManager/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
        require(session.h != nullptr, i18n::utf8Text(i18n::Text::OpDownloadSession));
        require(WinHttpSetTimeouts(session.h, 10000, 10000, 10000, 10000), i18n::utf8Text(i18n::Text::OpDownloadTimeouts));
        Internet connection{WinHttpConnect(session.h, host.c_str(), url.nPort, 0)};
        require(connection.h != nullptr, i18n::utf8Text(i18n::Text::OpConnectHost));
        Internet request{WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        require(request.h != nullptr, i18n::utf8Text(i18n::Text::OpDownloadRequest));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        require(WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)), i18n::utf8Text(i18n::Text::OpHttpsRedirect));
        cancelPoint();
        require(WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request.h, nullptr), i18n::utf8Text(i18n::Text::OpDownloadEngine));
        DWORD status = 0, size = sizeof(status);
        require(WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX), i18n::utf8Text(i18n::Text::OpHttpStatus));
        if (status != 200) throw std::runtime_error(i18n::utf8Text(i18n::Text::DownloadHttp) + std::to_string(status));
        DWORD length = 0; size = sizeof(length);
        WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX);
        if (length > archiveLimit) throw std::runtime_error(i18n::utf8Text(i18n::Text::DownloadTooLarge));
        ULONGLONG began = GetTickCount64();
        for (;;) {
            cancelPoint();
            if (GetTickCount64() - began > 10 * 60 * 1000) throw std::runtime_error(i18n::utf8Text(i18n::Text::DownloadTimeout));
            char chunk[65536]; DWORD got = 0;
            require(WinHttpReadData(request.h, chunk, sizeof(chunk), &got), i18n::utf8Text(i18n::Text::OpReadDownload));
            if (!got) break;
            if (bytes.size() + got > archiveLimit) throw std::runtime_error(i18n::utf8Text(i18n::Text::DownloadTooLarge));
            bytes.append(chunk, got);
            int percent = length ? static_cast<int>((std::min)(size_t(99), bytes.size() * 100 / length)) : -1;
            publish(SetupStage::Preparing, i18n::text(i18n::Text::DownloadingBytes) + std::to_wstring(bytes.size() / 1024) + L" KB", percent);
        }
    }
    cancelPoint(); publish(SetupStage::Preparing, i18n::text(i18n::Text::VerifyingHash), 100); checkDigest(bytes);
    // Cache is untrusted input to the elevated helper, which rechecks the digest
    // before copying into an administrator-only staging directory.
    if (offline != archive) {
        if (!CreateDirectoryW(cache.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) require(false, i18n::utf8Text(i18n::Text::OpCreateCache));
        GUID guid{}; require(SUCCEEDED(CoCreateGuid(&guid)), i18n::utf8Text(i18n::Text::OpCacheName));
        wchar_t name[40]{}; StringFromGUID2(guid, name, 40);
        auto temporary = cache + L"\\" + name + L".download";
        struct Remove { std::wstring path; ~Remove() { DeleteFileW(path.c_str()); } } remove{temporary};
        {
            File file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            require(file.h != INVALID_HANDLE_VALUE, i18n::utf8Text(i18n::Text::OpCacheFile));
            DWORD written = 0;
            require(WriteFile(file.h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size(), i18n::utf8Text(i18n::Text::OpSaveZip));
            require(FlushFileBuffers(file.h), i18n::utf8Text(i18n::Text::OpFlushZip));
        }
        require(MoveFileExW(temporary.c_str(), archive.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH), i18n::utf8Text(i18n::Text::OpCommitCache));
    }
    cancelPoint(); publish(SetupStage::Ready, i18n::text(i18n::Text::EngineVerified), 100, archive);
}
}
