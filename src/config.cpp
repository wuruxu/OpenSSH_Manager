#include "i18n.h"
#include "config.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>

namespace manager {
std::string trim(const std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
static std::pair<std::string, std::string> directive(const std::string& line) {
    auto s = trim(line);
    if (s.empty() || s[0] == '#') return {};
    auto p = s.find_first_of(" \t=");
    if (p == std::string::npos) return {lower(s), ""};
    auto value = trim(s.substr(p));
    if (!value.empty() && value[0] == '=') value = trim(value.substr(1));
    bool quoted = false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '"') quoted = !quoted;
        if (value[i] == '#' && !quoted) { value.resize(i); break; }
    }
    return {lower(s.substr(0, p)), trim(value)};
}
static bool managed(const std::string& key) {
    return key == "port" || key == "listenaddress" || key == "passwordauthentication" ||
        key == "pubkeyauthentication" || key == "allowtcpforwarding" || key == "allowusers" || key == "allowgroups";
}
static void patterns(const std::string& s) {
    if (s.size() > 2048) throw std::runtime_error(i18n::utf8Text(i18n::Text::AccountListLong));
    bool quoted = false;
    for (unsigned char c : s) {
        if (c == '"') { quoted = !quoted; continue; }
        if (c < 32 || c == 127 || c == '#' || c == '=')
            throw std::runtime_error(i18n::utf8Text(i18n::Text::AccountPatternInvalid));
    }
    if (quoted) throw std::runtime_error(i18n::utf8Text(i18n::Text::AccountQuoteOpen));
}
void validateSettings(const Settings& s) {
    if (s.port.empty() || s.port.size() > 5 ||
        s.port.find_first_not_of("0123456789") != std::string::npos ||
        std::stoul(s.port) == 0 || std::stoul(s.port) > 65535)
        throw std::runtime_error(i18n::utf8Text(i18n::Text::PortInvalid));
    auto scopeAt = s.listen.find('%');
    auto address = s.listen.substr(0, scopeAt);
    if (s.listen.size() > 64 || address.find_first_not_of("0123456789abcdefABCDEF:.") != std::string::npos)
        throw std::runtime_error(i18n::utf8Text(i18n::Text::ListenInvalid));
    if (scopeAt != std::string::npos) {
        auto scope = s.listen.substr(scopeAt + 1);
        if (address.find(':') == std::string::npos || scope.empty() || scope.size() > 10 ||
            scope.find_first_not_of("0123456789") != std::string::npos || std::stoull(scope) == 0 || std::stoull(scope) > 0xffffffffULL)
            throw std::runtime_error(i18n::utf8Text(i18n::Text::ScopeInvalid));
    }
    if (!s.password && !s.pubkey) throw std::runtime_error(i18n::utf8Text(i18n::Text::AuthRequired));
    patterns(s.users);
    patterns(s.groups);
}
Config parseConfig(const std::string& input) {
    Config out;
    out.text = input;
    if (out.text.compare(0, 3, "\xef\xbb\xbf") == 0) out.text.erase(0, 3);
    if (out.text.find('\0') != std::string::npos) out.problem = i18n::utf8Text(i18n::Text::ConfigNul);
    std::map<std::string, std::vector<std::string>> values;
    std::istringstream stream(out.text);
    std::string line;
    bool inMatch = false;
    while (std::getline(stream, line)) {
        auto [key, value] = directive(line);
        if (key == "match") { inMatch = true; out.hasMatch = true; }
        if (!inMatch && key == "include") out.problem = i18n::utf8Text(i18n::Text::ConfigInclude);
        if (!inMatch && managed(key)) values[key].push_back(value);
    }
    for (const auto& name : {"port", "listenaddress", "allowusers", "allowgroups"}) {
        if (values[name].size() > 1) out.problem = i18n::utf8Text(i18n::Text::ConfigMultiplePrefix) + std::string(name) + i18n::utf8Text(i18n::Text::ConfigMultipleSuffix);
    }
    auto get = [&](const std::string& key, const std::string& fallback) {
        return values[key].empty() ? fallback : values[key].front();
    };
    out.settings.port = get("port", "22");
    out.settings.listen = get("listenaddress", "");
    out.settings.users = get("allowusers", "");
    out.settings.groups = get("allowgroups", "");
    auto boolean = [&](const std::string& key) {
        auto value = lower(get(key, "yes"));
        if (value != "yes" && value != "no") out.problem = i18n::utf8Text(i18n::Text::ConfigUnsupported) + key + ": " + value;
        return value != "no";
    };
    out.settings.password = boolean("passwordauthentication");
    out.settings.pubkey = boolean("pubkeyauthentication");
    out.settings.forwarding = boolean("allowtcpforwarding");
    try { validateSettings(out.settings); }
    catch (const std::exception& e) { out.problem = e.what(); }
    return out;
}
std::string renderConfig(const Config& base, const Settings& s) {
    if (!base.problem.empty()) throw std::runtime_error(base.problem);
    validateSettings(s);
    const std::string nl = base.text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string header = "# OpenSSH Manager: global settings" + nl;
    header += "Port " + s.port + nl;
    if (!s.listen.empty()) header += "ListenAddress " + s.listen + nl;
    header += "PasswordAuthentication " + std::string(s.password ? "yes" : "no") + nl;
    header += "PubkeyAuthentication " + std::string(s.pubkey ? "yes" : "no") + nl;
    header += "AllowTcpForwarding " + std::string(s.forwarding ? "yes" : "no") + nl;
    if (!trim(s.users).empty()) header += "AllowUsers " + trim(s.users) + nl;
    if (!trim(s.groups).empty()) header += "AllowGroups " + trim(s.groups) + nl;
    header += "# End OpenSSH Manager global settings" + nl;
    std::string result = header;
    bool inMatch = false;
    // Preserve all non-managed lines, including byte-for-byte Match sections.
    size_t pos = 0;
    while (pos < base.text.size()) {
        size_t end = base.text.find('\n', pos);
        end = end == std::string::npos ? base.text.size() : end + 1;
        auto line = base.text.substr(pos, end - pos);
        auto [key, value] = directive(line);
        (void)value;
        if (key == "match") inMatch = true;
        const auto stripped = trim(line);
        bool marker = stripped == "# OpenSSH Manager: global settings" || stripped == "# End OpenSSH Manager global settings";
        if (inMatch || (!managed(key) && !marker)) result += line;
        pos = end;
    }
    return result;
}
std::wstring quoteArgument(const std::wstring& arg) {
    // CommandLineToArgvW / Microsoft CRT escaping, including trailing slashes.
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
std::string validatePublicKey(const std::string& input) {
    auto text = trim(input);
    if (text.size() > 32768 || text.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        throw std::runtime_error(i18n::utf8Text(i18n::Text::KeySingleLine));
    std::istringstream in(text);
    std::string type, data;
    in >> type >> data;
    if (type != "ssh-ed25519" && type != "ssh-rsa" && type != "ecdsa-sha2-nistp256" &&
        type != "ecdsa-sha2-nistp384" && type != "ecdsa-sha2-nistp521" &&
        type != "sk-ssh-ed25519@openssh.com" && type != "sk-ecdsa-sha2-nistp256@openssh.com")
        throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyUnsupported));
    if (data.size() < 16 || data.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != std::string::npos)
        throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyEncoding));
    for (unsigned char c : text) if (c < 32 && c != '\t') throw std::runtime_error(i18n::utf8Text(i18n::Text::KeyControlCharacter));
    return text;
}
}
