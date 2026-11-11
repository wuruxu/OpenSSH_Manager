#include "diagnostics.h"
#include <iostream>
#include <stdexcept>
using namespace manager;
static void check(bool ok, const char* msg) { if (!ok) throw std::runtime_error(msg); }
int main() {
    try {
        auto event = summarizeLogin("Accepted publickey for contoso\\alice from 2001:db8::1 port 55123 ssh2: ED25519 SHA256:abc");
        check(event.kind == LoginKind::Success && event.user == "contoso\\alice" && event.address == "2001:db8::1", "Success/IPv6 parsing failed");
        event = summarizeLogin("Failed password for invalid user unknown from 192.0.2.1 port 123 ssh2");
        check(event.kind == LoginKind::Failure && event.user == "unknown", "Invalid-user authentication parsing failed");
        event = summarizeLogin("User alice from 192.0.2.3 not allowed because not listed in AllowUsers");
        check(event.user == "alice" && event.address == "192.0.2.3" && event.kind == LoginKind::Failure, "Policy denial parsing failed");
        event = summarizeLogin("Connection closed by authenticating user Alice Smith 192.0.2.2 port 1111 [preauth]");
        check(event.kind == LoginKind::Connection && event.user == "Alice Smith", "Connection close was misclassified");
        event = summarizeLogin("Server listening on :: port 22.");
        check(event.user.empty() && event.address.empty() && event.kind == LoginKind::Other, "Missing fields were invented");
        event = summarizeLogin("Failed publickey for alice from 192.0.2.3 port 2200 ssh2");
        check(event.kind == LoginKind::Failure && event.explanation.find("日志未说明具体原因") != std::string::npos, "Unknown failure reason was guessed");
        event = summarizeLogin("Authentication refused: account policy");
        check(event.explanation.find("权限/属主") == std::string::npos, "Generic refusal was mislabeled as an ACL failure");
        event = summarizeLogin(std::string(50000, 'x'));
        check(event.explanation.size() <= 2048, "Oversized event was not bounded");
        std::cout << "PASS: login event interpretation.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
