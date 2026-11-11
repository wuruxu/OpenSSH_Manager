#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
namespace manager {
enum class SetupStage { Idle, Preparing, NeedsDownload, Ready, Existing, Failed, Canceled };
struct SetupUpdate {
    SetupStage stage = SetupStage::Idle;
    std::wstring message, archive;
    int percent = -1;
};
// Only the worker touches the network/files; the GUI polls snapshots.
class EngineSetup {
    std::mutex mutex;
    std::atomic_bool canceled{false};
    HANDLE thread = nullptr;
    SetupUpdate update;
    std::wstring offline;
    bool allowDownload = false;
    static unsigned __stdcall entry(void* self);
    void prepare();
    void publish(SetupStage stage, const std::wstring& message, int percent = -1, const std::wstring& archive = {});
public:
    ~EngineSetup();
    void start(const std::wstring& offlineArchive = {}, bool download = false);
    void cancel();
    SetupUpdate snapshot();
};
std::wstring managedEngineDirectory();
std::string verifiedEngineArchive(const std::wstring& path);
std::string setupResource(int id);
}
