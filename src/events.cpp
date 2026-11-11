#include "i18n.h"
#include "platform.h"
#include "diagnostics.h"
#include <winevt.h>
#include <algorithm>
#include <stdexcept>

namespace manager {
namespace {
struct EventHandle {
    EVT_HANDLE h;
    ~EventHandle() { if (h) EvtClose(h); }
};
std::vector<BYTE> render(EVT_HANDLE context, EVT_HANDLE event, DWORD flags) {
    DWORD bytes = 0, count = 0;
    EvtRender(context, event, flags, 0, nullptr, &bytes, &count);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) throw std::runtime_error(utf8(errorText(GetLastError())));
    std::vector<BYTE> buffer(bytes);
    if (!EvtRender(context, event, flags, bytes, buffer.data(), &bytes, &count))
        throw std::runtime_error(utf8(errorText(GetLastError())));
    return buffer;
}
std::wstring localTime(ULONGLONG ticks) {
    FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME utcTime{}, time{};
    if (!FileTimeToSystemTime(&utc, &utcTime) || !SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &time)) return i18n::text(i18n::Text::Unknown);
    wchar_t text[64]{};
    swprintf_s(text, L"%04u-%02u-%02u %02u:%02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return text;
}
std::wstring valueString(const EVT_VARIANT& value) {
    return value.Type == EvtVarTypeString && value.StringVal ? value.StringVal : L"";
}
}
EventBatch readEvents() {
    EventBatch result;
    const wchar_t* paths[] = {L"Event/EventData/Data[@Name='process']", L"Event/EventData/Data[@Name='payload']"};
    EventHandle user{EvtCreateRenderContext(2, paths, EvtRenderContextValues)};
    EventHandle system{EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem)};
    if (!user.h || !system.h) throw std::runtime_error(i18n::utf8Text(i18n::Text::EventContextFailed));
    for (const auto* channel : {L"OpenSSH/Operational", L"OpenSSH/Admin"}) {
        try {
            EventHandle query{EvtQuery(nullptr, channel, L"*", EvtQueryChannelPath | EvtQueryReverseDirection)};
            if (!query.h) throw std::runtime_error(utf8(errorText(GetLastError())));
            for (int i = 0; i < 200; ++i) {
                EVT_HANDLE handle = nullptr; DWORD count = 0;
                if (!EvtNext(query.h, 1, &handle, 1000, 0, &count)) {
                    if (GetLastError() != ERROR_NO_MORE_ITEMS) throw std::runtime_error(utf8(errorText(GetLastError())));
                    break;
                }
                EventHandle event{handle};
                auto sys = render(system.h, handle, EvtRenderEventValues);
                auto data = render(user.h, handle, EvtRenderEventValues);
                auto xml = render(nullptr, handle, EvtRenderEventXml);
                const auto* s = reinterpret_cast<const EVT_VARIANT*>(sys.data());
                const auto* u = reinterpret_cast<const EVT_VARIANT*>(data.data());
                EventRecord record;
                record.ticks = s[EvtSystemTimeCreated].FileTimeVal;
                record.time = localTime(record.ticks);
                record.channel = channel;
                record.message = valueString(u[1]);
                if (record.message.empty()) record.message = i18n::text(i18n::Text::EventNoPayload);
                auto summary = summarizeLogin(utf8(record.message));
                record.kind = summary.kind;
                record.user = summary.user.empty() ? L"—" : wide(summary.user);
                record.address = summary.address.empty() ? L"—" : wide(summary.address);
                record.explanation = wide(summary.explanation);
                record.result = summary.kind == LoginKind::Success ? i18n::text(i18n::Text::Success) : summary.kind == LoginKind::Failure ? i18n::text(i18n::Text::Failure) : summary.kind == LoginKind::Connection ? i18n::text(i18n::Text::Connection) : i18n::text(i18n::Text::Event);
                if (summary.kind == LoginKind::Other && s[EvtSystemLevel].Type == EvtVarTypeByte && s[EvtSystemLevel].ByteVal <= 2 && s[EvtSystemLevel].ByteVal > 0)
                    { record.result = i18n::text(i18n::Text::Error); record.error = true; }
                record.details = i18n::text(i18n::Text::EventTime) + record.time + i18n::text(i18n::Text::EventChannel) + channel +
                    i18n::text(i18n::Text::EventId) + std::to_wstring(s[EvtSystemEventID].UInt16Val) + i18n::text(i18n::Text::ProcessId) + std::to_wstring(s[EvtSystemProcessID].UInt32Val) +
                    i18n::text(i18n::Text::EventProcess) + valueString(u[0]) + i18n::text(i18n::Text::EventMessage) + record.message + i18n::text(i18n::Text::EventXml) + reinterpret_cast<const wchar_t*>(xml.data());
                result.records.push_back(std::move(record));
            }
        } catch (const std::exception& e) {
            result.warning += std::wstring(channel) + i18n::text(i18n::Text::Colon) + wide(e.what()) + L"\r\n";
            result.warning += i18n::text(i18n::Text::EventAccessHint);
        }
    }
    std::stable_sort(result.records.begin(), result.records.end(), [](const EventRecord& a, const EventRecord& b) { return a.ticks > b.ticks; });
    if (result.records.size() > 200) result.records.resize(200);
    return result;
}
}
