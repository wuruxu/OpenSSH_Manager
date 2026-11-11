#pragma once
#include "config.h"
#include "authorized_keys.h"
#include <windows.h>
#include <string>
#include <vector>

namespace manager {
std::wstring wide(const std::string& utf8);
std::string utf8(const std::wstring& text);
std::wstring errorText(DWORD error);
std::wstring modulePath();
std::wstring dataPath();
std::string readFile(const std::wstring& path);
std::string digest(const std::string& data);
struct AccountIdentity {
    std::wstring name;
    std::wstring qualifiedName;
    bool administrator = false;
    bool elevated = false;
};
AccountIdentity currentAccountIdentity();
std::wstring publicKeyFilePath(bool administrators);
struct ServiceInfo {
    bool installed = false, engineMissing = false, repairAllowed = false;
    DWORD state = SERVICE_STOPPED;
    DWORD startType = SERVICE_DEMAND_START;
    DWORD pid = 0;
    std::wstring exe;
    std::wstring version;
    std::wstring config;
    std::wstring problem;
};
ServiceInfo serviceInfo();
std::wstring executableVersion(const std::wstring& path);
struct NetworkAddress { std::wstring name, address; };
std::vector<NetworkAddress> networkAddresses();
std::wstring stateName(DWORD state);
std::wstring defaultShell();
struct EventRecord {
    ULONGLONG ticks = 0;
    std::wstring time, channel, user, address, result, explanation, message, details;
};
struct EventBatch { std::vector<EventRecord> records; std::wstring warning; };
EventBatch readEvents();
std::wstring managePublicKey(const std::string& key, bool remove, bool administrators, const std::string& revision = {}, size_t row = 0);
std::string readPublicKeys(bool administrators);
bool isElevated();
HANDLE elevate(HWND owner, const std::vector<std::wstring>& arguments);
// Internal elevated worker. Whitelisted operations; no arbitrary command runner.
int adminMain(const std::vector<std::wstring>& args);
}
