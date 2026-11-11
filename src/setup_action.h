#pragma once
namespace manager {
enum class SetupActivity { Idle, Checking, Preparing, Installing };
enum class PrimaryAction { Unavailable, Checking, Preparing, Installing, Download, Install, Repair, Start };
// Both the button and its command handler use this decision, so a queued click
// cannot start an installation while preparation is still running.
constexpr PrimaryAction primaryAction(bool serviceKnown, bool installed, bool ready, SetupActivity activity, bool engineMissing = false, bool repairAllowed = false) {
    if (activity == SetupActivity::Installing) return PrimaryAction::Installing;
    if (activity == SetupActivity::Checking) return PrimaryAction::Checking;
    if (activity == SetupActivity::Preparing) return PrimaryAction::Preparing;
    if (!serviceKnown) return PrimaryAction::Unavailable;
    if (installed && engineMissing) {
        if (!repairAllowed) return PrimaryAction::Unavailable;
        return ready ? PrimaryAction::Repair : PrimaryAction::Download;
    }
    if (installed) return PrimaryAction::Start;
    return ready ? PrimaryAction::Install : PrimaryAction::Download;
}
}
