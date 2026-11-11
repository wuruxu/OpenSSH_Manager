#include "i18n.h"
#include <algorithm>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#endif

namespace manager::i18n {
namespace {
std::atomic<Language> current{Language::English};
struct Entry {
    const wchar_t* wide[3];
    const char* utf8[3];
};
constexpr Entry catalog[] = {
#define I18N_TEXT(id, zh, tw, en) {{L##en, L##zh, L##tw}, {en, zh, tw}},
#include "i18n_strings.inc"
#undef I18N_TEXT
};
static_assert(sizeof(catalog) / sizeof(catalog[0]) == static_cast<size_t>(Text::Count));
}
std::optional<Language> matchLanguage(std::wstring tag) {
    for (auto& ch : tag) {
        if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
        if (ch == L'_') ch = L'-';
    }
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < tag.size()) {
        auto end = tag.find(L'-', start);
        parts.push_back(tag.substr(start, end == std::wstring::npos ? end : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (parts.empty()) return {};
    if (parts[0] == L"en") return Language::English;
    if (parts[0] != L"zh") return {};
    // Only the script/region subtags before any extension affect selection.
    std::wstring region;
    for (size_t i = 1; i < parts.size(); ++i) {
        if (parts[i].size() == 1) break;
        if (parts[i] == L"hans") return Language::SimplifiedChinese;
        if (parts[i] == L"hant") return Language::TraditionalChinese;
        if (parts[i].size() == 2) region = parts[i];
        if (parts[i] == L"cht") return Language::TraditionalChinese;
        if (parts[i] == L"chs") return Language::SimplifiedChinese;
    }
    if (region == L"tw" || region == L"hk" || region == L"mo") return Language::TraditionalChinese;
    return Language::SimplifiedChinese;
}
Language selectLanguage(const std::vector<std::wstring>& preferred) {
    for (const auto& tag : preferred) if (auto value = matchLanguage(tag)) return *value;
    return Language::English;
}
Language language() { return current.load(std::memory_order_relaxed); }
void setLanguage(Language value) { current.store(value, std::memory_order_relaxed); }
void initialize() {
    std::vector<std::wstring> preferred;
#ifdef _WIN32
    ULONG count = 0, size = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) && size) {
        std::vector<wchar_t> buffer(size);
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &size)) {
            for (const auto* p = buffer.data(); *p; p += wcslen(p) + 1) preferred.emplace_back(p);
        }
    }
    if (preferred.empty()) {
        wchar_t tag[LOCALE_NAME_MAX_LENGTH]{};
        if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), tag, LOCALE_NAME_MAX_LENGTH, 0))
            preferred.emplace_back(tag);
    }
#endif
    setLanguage(selectLanguage(preferred));
}
const wchar_t* languageTag() {
    switch (language()) {
    case Language::SimplifiedChinese: return L"zh-Hans";
    case Language::TraditionalChinese: return L"zh-Hant";
    default: return L"en";
    }
}
const wchar_t* fontFamily() {
    switch (language()) {
    case Language::SimplifiedChinese: return L"Microsoft YaHei UI";
    case Language::TraditionalChinese: return L"Microsoft JhengHei UI";
    default: return L"Segoe UI";
    }
}
const wchar_t* text(Text id, Language value) { return catalog[static_cast<size_t>(id)].wide[static_cast<size_t>(value)]; }
const char* utf8Text(Text id, Language value) { return catalog[static_cast<size_t>(id)].utf8[static_cast<size_t>(value)]; }
const wchar_t* text(Text id) { return text(id, language()); }
const char* utf8Text(Text id) { return utf8Text(id, language()); }
}
