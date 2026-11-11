#include "authorized_keys.h"
#include "config.h"
#include <stdexcept>
namespace manager {
std::vector<AuthorizedKeyEntry> authorizedKeyEntries(const std::string& source) {
    std::vector<AuthorizedKeyEntry> entries;
    size_t start = source.compare(0, 3, "\xef\xbb\xbf") == 0 ? 3 : 0;
    while (start < source.size()) {
        auto end = source.find('\n', start); end = end == std::string::npos ? source.size() : end + 1;
        auto line = trim(source.substr(start, end - start));
        if (!line.empty() && line[0] != '#') {
            AuthorizedKeyEntry entry{line, "—", "", start, end};
            // Options may contain quoted spaces and escaped quotes. Locate the key type
            // without interpreting or rewriting restrictions such as command=/from=.
            size_t pos = 0;
            for (int field = 0; field < 2 && pos < line.size(); ++field) {
                while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
                size_t begin = pos; bool quoted = false;
                while (pos < line.size()) {
                    char c = line[pos];
                    if (c == '\\' && quoted && pos + 1 < line.size()) { pos += 2; continue; }
                    if (c == '"') quoted = !quoted;
                    if (!quoted && (c == ' ' || c == '\t')) break;
                    ++pos;
                }
                auto token = line.substr(begin, pos - begin);
                if (token.rfind("ssh-", 0) == 0 || token.rfind("ecdsa-", 0) == 0 || token.rfind("sk-", 0) == 0) {
                    entry.type = token;
                    pos = line.find_first_not_of(" \t", pos);
                    if (pos != std::string::npos) pos = line.find_first_of(" \t", pos);
                    if (pos != std::string::npos) entry.comment = trim(line.substr(pos));
                    break;
                }
            }
            entries.push_back(std::move(entry));
        }
        start = end;
    }
    return entries;
}
std::string eraseAuthorizedKeyEntry(const std::string& source, size_t index) {
    auto entries = authorizedKeyEntries(source);
    if (index >= entries.size()) throw std::runtime_error("Public key entry no longer exists. Refresh the list.");
    const auto& entry = entries[index];
    return source.substr(0, entry.begin) + source.substr(entry.end);
}
}
