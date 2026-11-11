#include "i18n.h"
#include "diagnostics.h"
#include "config.h"
#include <codecvt>
#include <cstring>
#include <iostream>
#include <vector>
#include <stdexcept>
using namespace manager;
using namespace manager::i18n;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        for (const auto* tag : {L"zh-CN", L"zh-SG", L"zh-Hans", L"ZH_hAnS_TW", L"zh", L"zh-CHS"})
            check(matchLanguage(tag) == Language::SimplifiedChinese, "Simplified/script precedence failed");
        for (const auto* tag : {L"zh-TW", L"zh-HK", L"zh-MO", L"zh-Hant", L"zh-Hant-CN", L"zh-CHT"})
            check(matchLanguage(tag) == Language::TraditionalChinese, "Traditional/script precedence failed");
        check(matchLanguage(L"en-GB") == Language::English, "English variant failed");
        check(!matchLanguage(L"english") && !matchLanguage(L"zhfoo") && !matchLanguage(L""), "Unsupported language accepted");
        check(matchLanguage(L"zh-CN-x-hant") == Language::SimplifiedChinese, "Private-use tag changed script");
        check(selectLanguage({L"fr-FR", L"zh-HK", L"en-US"}) == Language::TraditionalChinese, "Preference ordering failed");
        check(selectLanguage({L"en-US", L"zh-CN"}) == Language::English, "First supported preference ignored");
        check(selectLanguage({L"ja-JP", L"de-DE"}) == Language::English && selectLanguage({}) == Language::English, "English fallback failed");
        std::codecvt_utf8<wchar_t> conversion;
        for (auto lang : {Language::English, Language::SimplifiedChinese, Language::TraditionalChinese}) {
            setLanguage(lang);
            check(matchLanguage(languageTag()) == lang, "Worker language tag does not round-trip");
            for (size_t i = 0; i < static_cast<size_t>(Text::Count); ++i) {
                auto id = static_cast<Text>(i);
                check(*text(id) && *utf8Text(id), "Missing translation");
                const char* bytes = utf8Text(id), *end = bytes + std::strlen(bytes), *next = nullptr;
                std::vector<wchar_t> decoded(std::strlen(bytes) + 1);
                wchar_t* decodedEnd = nullptr;
                std::mbstate_t state{};
                auto result = conversion.in(state, bytes, end, next, decoded.data(), decoded.data() + decoded.size(), decodedEnd);
                check(result == std::codecvt_base::ok && next == end && std::wstring(decoded.data(), decodedEnd) == text(id),
                    "Wide and UTF-8 translations differ");
            }
            const auto* filter = text(Text::ZipFilter);
            const auto* pattern = filter + wcslen(filter) + 1;
            check(std::wstring(pattern) == L"*.zip" && pattern[6] == 0, "File dialog double-NUL filter corrupted");
            auto result = summarizeLogin("Failed publickey for alice from 192.0.2.1 port 22 ssh2");
            check(result.kind == LoginKind::Failure && result.user == "alice" && result.address == "192.0.2.1", "Translation changed event parsing");
            check(result.explanation == std::string(utf8Text(Text::PublicKeyMethod)) + utf8Text(Text::AuthFailure), "Event explanation not localized");
            result = summarizeLogin("Server listening on :: port 22.");
            check(result.explanation == "Server listening on :: port 22.", "Original log was translated");
            Settings settings; settings.port = "0";
            bool rejected = false;
            try { validateSettings(settings); } catch (const std::runtime_error& e) {
                rejected = std::string(e.what()) == utf8Text(Text::PortInvalid);
            }
            check(rejected, "Validation error not localized");
            settings.port = "2222";
            const auto rendered = renderConfig(parseConfig("Port 22\n"), settings);
            check(rendered.find("Port 2222\n") != std::string::npos, "Translation changed SSH directive");
        }
        check(std::wstring(text(Text::ImportKey, Language::TraditionalChinese)) == L"匯入公鑰", "Traditional terminology regressed");
        std::cout << "PASS: language matching, complete catalog, filters, diagnostics and validation.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
