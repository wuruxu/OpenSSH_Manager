#include "platform.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <tuple>

namespace manager {
std::wstring executableVersion(const std::wstring& path) {
    // Read resources, without launching sshd on every status tick. Invalidate
    // when the registered path, executable size or modification time changes.
    static std::mutex mutex;
    static std::wstring cachedPath, cachedVersion;
    static WIN32_FILE_ATTRIBUTE_DATA cached{};
    std::lock_guard<std::mutex> lock(mutex);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    if (path == cachedPath && attributes.nFileSizeHigh == cached.nFileSizeHigh && attributes.nFileSizeLow == cached.nFileSizeLow &&
        CompareFileTime(&attributes.ftLastWriteTime, &cached.ftLastWriteTime) == 0) return cachedVersion;
    std::wstring version;
    DWORD unused = 0, size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (size && size <= 8 * 1024 * 1024) {
        std::vector<BYTE> bytes(size);
        if (GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) {
            VS_FIXEDFILEINFO* info = nullptr; UINT length = 0;
            if (VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) &&
                length >= sizeof(*info) && info->dwSignature == 0xFEEF04BD) {
                version = std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionMS)) +
                    L"." + std::to_wstring(HIWORD(info->dwFileVersionLS)) + L"." + std::to_wstring(LOWORD(info->dwFileVersionLS));
            }
        }
    }
    cachedPath = path; cached = attributes; cachedVersion = version;
    return version;
}
std::vector<NetworkAddress> networkAddresses() {
    ULONG size = 15000, result = ERROR_BUFFER_OVERFLOW;
    std::vector<BYTE> bytes;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        bytes.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data()), &size);
    }
    if (result == ERROR_NO_DATA) return {};
    if (result != NO_ERROR) throw std::runtime_error(utf8(L"读取网络接口失败：" + errorText(result)));
    std::vector<NetworkAddress> addresses;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(bytes.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        std::wstring name = adapter->FriendlyName ? adapter->FriendlyName : L"网络接口";
        for (auto* entry = adapter->FirstUnicastAddress; entry; entry = entry->Next) {
            auto* socket = entry->Address.lpSockaddr;
            if (!socket || (entry->DadState != IpDadStatePreferred && entry->DadState != IpDadStateDeprecated)) continue;
            wchar_t buffer[INET6_ADDRSTRLEN]{};
            ULONG scope = 0;
            if (socket->sa_family == AF_INET && entry->Address.iSockaddrLength >= static_cast<int>(sizeof(sockaddr_in))) {
                auto* address = reinterpret_cast<sockaddr_in*>(socket);
                if (address->sin_addr.s_addr == INADDR_ANY ||
                    !InetNtopW(AF_INET, &address->sin_addr, buffer, INET6_ADDRSTRLEN)) continue;
            } else if (socket->sa_family == AF_INET6 && entry->Address.iSockaddrLength >= static_cast<int>(sizeof(sockaddr_in6))) {
                auto* address = reinterpret_cast<sockaddr_in6*>(socket);
                if (IN6_IS_ADDR_UNSPECIFIED(&address->sin6_addr) ||
                    !InetNtopW(AF_INET6, &address->sin6_addr, buffer, INET6_ADDRSTRLEN)) continue;
                if (IN6_IS_ADDR_LINKLOCAL(&address->sin6_addr)) {
                    scope = address->sin6_scope_id ? address->sin6_scope_id : adapter->Ipv6IfIndex;
                    if (!scope) continue;
                }
            } else continue;
            std::wstring address = buffer;
            if (scope) address += L"%" + std::to_wstring(scope);
            addresses.push_back({name, address});
        }
    }
    std::sort(addresses.begin(), addresses.end(), [](const auto& a, const auto& b) {
        return std::tie(a.name, a.address) < std::tie(b.name, b.address);
    });
    addresses.erase(std::unique(addresses.begin(), addresses.end(), [](const auto& a, const auto& b) {
        return a.name == b.name && a.address == b.address;
    }), addresses.end());
    return addresses;
}
}
