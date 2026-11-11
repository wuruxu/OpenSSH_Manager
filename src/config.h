#pragma once
#include <string>
#include <vector>

namespace manager {
struct Settings {
    std::string port = "22";
    std::string listen; // empty: OpenSSH default, all interfaces
    bool password = true;
    bool pubkey = true;
    bool forwarding = true;
    std::string users;
    std::string groups;
};
struct Config {
    Settings settings;
    std::string text;
    std::string problem;
    bool hasMatch = false;
};
Config parseConfig(const std::string& text);
void validateSettings(const Settings& settings);
std::string renderConfig(const Config& base, const Settings& settings);
std::wstring quoteArgument(const std::wstring& arg);
std::string trim(const std::string& value);
// Only plain public keys, without authorized_keys options or private key data.
std::string validatePublicKey(const std::string& text);
}
