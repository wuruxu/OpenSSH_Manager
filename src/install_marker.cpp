#include "i18n.h"
#include "install_marker.h"
#include <map>
#include <stdexcept>
namespace manager {
namespace {
struct Value { std::wstring text; bool boolean = false, isBool = false; };
class Parser {
    const std::wstring& input; size_t pos = 0;
    [[noreturn]] void fail() { throw std::runtime_error(i18n::utf8Text(i18n::Text::MarkerInvalid)); }
    void space() { while (pos < input.size() && (input[pos] == L' ' || input[pos] == L'\r' || input[pos] == L'\n' || input[pos] == L'\t')) ++pos; }
    bool take(wchar_t c) { space(); if (pos < input.size() && input[pos] == c) { ++pos; return true; } return false; }
    std::wstring string() {
        if (!take(L'"')) fail();
        std::wstring out;
        while (pos < input.size()) {
            wchar_t c = input[pos++];
            if (c == L'"') return out;
            if (c < 32) fail();
            if (c == L'\\') {
                if (pos == input.size()) fail();
                c = input[pos++];
                switch (c) {
                case L'"': case L'\\': case L'/': break;
                case L'b': c = L'\b'; break; case L'f': c = L'\f'; break;
                case L'n': c = L'\n'; break; case L'r': c = L'\r'; break; case L't': c = L'\t'; break;
                case L'u': {
                    unsigned n = 0;
                    for (int i = 0; i < 4; ++i) {
                        if (pos == input.size()) fail();
                        wchar_t h = input[pos++];
                        int d = h >= L'0' && h <= L'9' ? h - L'0' : h >= L'a' && h <= L'f' ? h - L'a' + 10 : h >= L'A' && h <= L'F' ? h - L'A' + 10 : -1;
                        if (d < 0) fail();
                        n = n * 16 + static_cast<unsigned>(d);
                    }
                    c = static_cast<wchar_t>(n); break;
                }
                default: fail();
                }
            }
            if (!c) fail(); // Paths and keys must not contain embedded NULs.
            out += c;
        }
        fail();
    }
public:
    explicit Parser(const std::wstring& value) : input(value) { if (!input.empty() && input[0] == 0xfeff) ++pos; }
    InstallMarker parse() {
        if (!take(L'{')) fail();
        std::map<std::wstring, Value> fields;
        if (!take(L'}')) for (;;) {
            auto key = string();
            for (auto& c : key) if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
            if (!take(L':')) fail();
            space(); Value value;
            if (pos < input.size() && input[pos] == L'"') value.text = string();
            else if (input.compare(pos, 4, L"true") == 0) { pos += 4; value.isBool = value.boolean = true; }
            else if (input.compare(pos, 5, L"false") == 0) { pos += 5; value.isBool = true; }
            else fail();
            if (!fields.emplace(key, value).second) fail();
            if (take(L'}')) break;
            if (!take(L',')) fail();
        }
        space(); if (pos != input.size()) fail();
        if (!fields.count(L"product") || !fields.count(L"directory") || !fields.count(L"ownsservice")) fail();
        const auto& product = fields.at(L"product"); const auto& directory = fields.at(L"directory"); const auto& owns = fields.at(L"ownsservice");
        if (product.isBool || directory.isBool || !owns.isBool) fail();
        return {product.text, directory.text, owns.boolean};
    }
};
}
InstallMarker parseInstallMarker(const std::wstring& json) { return Parser(json).parse(); }
}
