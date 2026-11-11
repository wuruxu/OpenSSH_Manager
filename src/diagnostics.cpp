#include "i18n.h"
#include "diagnostics.h"
#include <regex>

namespace manager {
LoginSummary summarizeLogin(const std::string& message) {
    LoginSummary out; out.explanation = message;
    if (message.size() > 2048) { out.explanation = message.substr(0, 2048); return out; }
    std::smatch match;
    static const std::regex auth(R"((Accepted|Failed) ([^\s]{1,256}) for (invalid user )?(.{1,256}?) from ([^\s]{1,256}) port ([0-9]{1,5}))");
    static const std::regex invalid(R"(Invalid user (.{1,256}?) from ([^\s]{1,256}) port ([0-9]{1,5}))");
    static const std::regex denied(R"(User (.{1,256}?)(?: from ([^\s]{1,256}))? not allowed because (.{1,1024}))");
    static const std::regex closeUser(R"((?:Connection closed by|Disconnected from|Connection reset by) (?:authenticating user |invalid user |user )(.{1,256}?) ([^\s]{1,256}) port ([0-9]{1,5}))");
    static const std::regex connection(R"((?:Connection from|Connection closed by|Connection reset by|Received disconnect from) ([^\s]{1,256}) port ([0-9]{1,5}))");
    if (std::regex_search(message, match, auth)) {
        out.user = match[4]; out.address = match[5];
        out.kind = match[1] == "Accepted" ? LoginKind::Success : LoginKind::Failure;
        auto method = match[2] == "publickey" ? std::string(i18n::utf8Text(i18n::Text::PublicKeyMethod)) : match[2] == "password" ? std::string(i18n::utf8Text(i18n::Text::PasswordMethod)) : match[2].str();
        if (out.kind == LoginKind::Success) out.explanation = method + i18n::utf8Text(i18n::Text::AuthSuccess);
        else if (match[3].matched) out.explanation = i18n::utf8Text(i18n::Text::AuthInvalidUser);
        else out.explanation = method + i18n::utf8Text(i18n::Text::AuthFailure);
    } else if (std::regex_search(message, match, invalid)) {
        out.kind = LoginKind::Failure; out.user = match[1]; out.address = match[2]; out.explanation = i18n::utf8Text(i18n::Text::AccountUnknown);
    } else if (std::regex_search(message, match, denied)) {
        out.kind = LoginKind::Failure; out.user = match[1]; out.address = match[2]; out.explanation = i18n::utf8Text(i18n::Text::AccountDenied) + match[3].str();
    } else if (message.find("Authentication refused") != std::string::npos || message.find("Bad permissions") != std::string::npos) {
        out.kind = LoginKind::Failure;
        out.explanation = (message.find("permission") != std::string::npos || message.find("ownership") != std::string::npos ? i18n::utf8Text(i18n::Text::AuthPermissions) : i18n::utf8Text(i18n::Text::AuthRefused)) + message;
    } else if (std::regex_search(message, match, closeUser)) {
        out.kind = LoginKind::Connection; out.user = match[1]; out.address = match[2]; out.explanation = i18n::utf8Text(i18n::Text::ConnectionClosed);
    } else if (std::regex_search(message, match, connection)) {
        out.kind = LoginKind::Connection; out.address = match[1]; out.explanation = message;
    }
    return out;
}
}
