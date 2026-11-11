#pragma once
#include <optional>
#include <string>
#include <vector>

namespace manager::i18n {
enum class Language { English, SimplifiedChinese, TraditionalChinese };
enum class Text {
#define I18N_TEXT(id, zh, tw, en) id,
#include "i18n_strings.inc"
#undef I18N_TEXT
    Count
};
// Exact supported language tags, with script taking precedence over region.
std::optional<Language> matchLanguage(std::wstring tag);
Language selectLanguage(const std::vector<std::wstring>& preferred);
Language language();
void setLanguage(Language value);
void initialize(); // Call before creating windows or starting worker threads.
const wchar_t* languageTag();
const wchar_t* fontFamily();
const wchar_t* text(Text id);
const char* utf8Text(Text id);
const wchar_t* text(Text id, Language value);
const char* utf8Text(Text id, Language value);
}
