#pragma once
#include <string>

namespace manager {
enum class LoginKind { Other, Success, Failure, Connection };
struct LoginSummary {
    LoginKind kind = LoginKind::Other;
    std::string user, address, explanation;
};
LoginSummary summarizeLogin(const std::string& message);
}
