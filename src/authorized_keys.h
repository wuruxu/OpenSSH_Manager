#pragma once
#include <string>
#include <vector>
namespace manager {
struct AuthorizedKeyEntry {
    std::string text, type, comment;
    size_t begin = 0, end = 0;
};
// Preserve the source bytes and offsets, including options, comments and line endings.
std::vector<AuthorizedKeyEntry> authorizedKeyEntries(const std::string& source);
std::string eraseAuthorizedKeyEntry(const std::string& source, size_t index);
}
