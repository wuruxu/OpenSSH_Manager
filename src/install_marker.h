#pragma once
#include <string>
namespace manager {
struct InstallMarker {
    std::wstring product, directory;
    bool ownsService = false;
};
// Parse the flat, typed JSON format written by Install.ps1. Malformed JSON,
// duplicate (including differently cased) keys and non-scalar values fail closed.
InstallMarker parseInstallMarker(const std::wstring& json);
}
