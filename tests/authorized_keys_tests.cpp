#include "authorized_keys.h"
#include <iostream>
#include <stdexcept>
using namespace manager;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    try {
        const std::string first = "  ssh-ed25519 AAAABBBBBBBBBBBBBBBB laptop\r\n";
        const std::string restricted = "command=\"echo hello \\\"world\\\"\",no-port-forwarding ssh-rsa AAAACCCCCCCCCCCCCCCC backup host\n";
        const std::string tail = "ssh-ed25519 AAAABBBBBBBBBBBBBBBB same key different entry";
        const std::string source = "\xef\xbb\xbf# heading\r\n\r\n" + first + "# preserve\n" + restricted + tail;
        auto entries = authorizedKeyEntries(source);
        check(entries.size() == 3, "comments/empty lines were counted as keys");
        check(entries[0].text == "ssh-ed25519 AAAABBBBBBBBBBBBBBBB laptop", "full key text lost");
        check(entries[1].type == "ssh-rsa" && entries[1].comment == "backup host", "quoted options parsing failed");
        check(eraseAuthorizedKeyEntry(source, 0) == "\xef\xbb\xbf# heading\r\n\r\n# preserve\n" + restricted + tail, "deleting a row changed duplicates or surrounding bytes");
        check(eraseAuthorizedKeyEntry(source, 1) == "\xef\xbb\xbf# heading\r\n\r\n" + first + "# preserve\n" + tail, "restricted key removal changed other entries");
        check(eraseAuthorizedKeyEntry(source, 2) == source.substr(0, source.size() - tail.size()), "last row without newline deletion failed");
        check(eraseAuthorizedKeyEntry("\xef\xbb\xbf" + tail, 0) == "\xef\xbb\xbf", "BOM was deleted");
        check(authorizedKeyEntries("# only comments\n\r\n").empty(), "empty file entries");
        bool failed = false; try { eraseAuthorizedKeyEntry(source, 3); } catch (const std::exception&) { failed = true; }
        check(failed, "invalid row accepted");
        auto unknown = authorizedKeyEntries("future-key AAAAA custom\n");
        check(unknown.size() == 1 && unknown[0].text == "future-key AAAAA custom", "unknown entry silently hidden");
        check(authorizedKeyEntries("ssh-ed25519 " + std::string(8192, 'A')).front().text.size() > 8192, "long key tooltip data truncated");
        std::cout << "Exact row deletion, duplicate/restricted keys, BOM, CRLF, comments and full text preserved.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
