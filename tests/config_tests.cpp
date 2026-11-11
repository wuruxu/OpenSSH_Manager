#include "config.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
using namespace manager;
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void rejects(const std::function<void()>& fn) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "unsafe/unsupported input was accepted");
}
int main() {
    try {
        std::string match = "Match Group administrators\r\n    AuthorizedKeysFile __PROGRAMDATA__/ssh/administrators_authorized_keys\r\nMatch User restricted\r\n    PasswordAuthentication no\r\n";
        auto base = parseConfig("# original comment\r\nPort 22\r\nPasswordAuthentication yes\r\nPasswordAuthentication no\r\nSubsystem sftp sftp-server.exe\r\n" + match);
        auto s = base.settings; s.port = "2222"; s.users = "domain\\user localuser";
        auto updated = renderConfig(base, s);
        check(updated.find(match) != std::string::npos, "Match blocks changed");
        check(updated.find("# original comment\r\n") != std::string::npos, "comment lost");
        check(updated.find("Subsystem sftp sftp-server.exe") != std::string::npos, "subsystem lost");
        check(updated.find("Port 22\r\n") == std::string::npos, "old port retained");
        auto reparsed = parseConfig(updated);
        check(reparsed.settings.port == "2222" && reparsed.settings.password, "settings did not round trip");
        check(renderConfig(reparsed, reparsed.settings) == updated, "repeated saves add/change content");
        rejects([&] { auto c = parseConfig("Include conf.d/*.conf\n"); renderConfig(c, s); });
        rejects([&] { auto c = parseConfig("Port 22\nPort 2222\n"); renderConfig(c, s); });
        rejects([&] { auto c = parseConfig("AllowTcpForwarding local\n"); renderConfig(c, s); });
        rejects([&] { auto bad = s; bad.users = "user\nForceCommand evil"; validateSettings(bad); });
        rejects([&] { auto bad = s; bad.password = bad.pubkey = false; validateSettings(bad); });
        rejects([&] { auto bad = s; bad.port = "65536"; validateSettings(bad); });
        rejects([&] { auto bad = s; bad.listen = "127.0.0.1:22 #comment"; validateSettings(bad); });
        auto scoped = s; scoped.listen = "fe80::abcd%12";
        validateSettings(scoped);
        check(parseConfig(renderConfig(base, scoped)).settings.listen == scoped.listen, "IPv6 interface scope was lost");
        for (const auto& address : {"fe80::1%0", "fe80::1%4294967296", "fe80::1%eth0", "fe80::1%1%2", "127.0.0.1%12", "fe80::1%12\nPort 2222"})
            rejects([&] { auto bad = s; bad.listen = address; validateSettings(bad); });
        check(!parseConfig(std::string("Port\0 22", 8)).problem.empty(), "NUL not rejected");
        check(parseConfig("\xef\xbb\xbfPort=2222 # comment\n").settings.port == "2222", "BOM/equal/comment parsing failed");
        check(quoteArgument(L"C:\\Program Files\\") == L"\"C:\\Program Files\\\\\"", "trailing backslash escaping failed");
        check(quoteArgument(L"a\"b") == L"\"a\\\"b\"", "quote escaping failed");
#ifdef _WIN32
        for (const auto& value : {L"", L"domain\\user", L"C:\\Program Files\\", L"a\\\"b", L"space and \"quotes\"", L"\\\\"}) {
            auto command = L"app.exe " + quoteArgument(value);
            int count = 0; auto argv = CommandLineToArgvW(command.c_str(), &count);
            bool same = argv && count == 2 && std::wstring(argv[1]) == value;
            LocalFree(argv);
            check(same, "Windows argument round trip failed");
        }
#endif
        rejects([] { validatePublicKey("-----BEGIN OPENSSH PRIVATE KEY-----"); });
        rejects([] { validatePublicKey("command=bad ssh-ed25519 AAAABBBBBBBBBBBBBBBB"); });
        rejects([] { validatePublicKey("ssh-ed25519 AAAABBBBBBBBBBBBBBBB\nssh-rsa AA"); });
        check(validatePublicKey("ssh-ed25519 AAAABBBBBBBBBBBBBBBB test") == "ssh-ed25519 AAAABBBBBBBBBBBBBBBB test", "valid key rejected");
        std::cout << "Configuration preservation, restrictions, injection checks and quoting passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
