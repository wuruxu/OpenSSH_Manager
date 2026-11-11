#include "setup_action.h"
#include <iostream>
#include <stdexcept>
using namespace manager;
void expect(PrimaryAction actual, PrimaryAction expected) {
    if (actual != expected) throw std::runtime_error("Unexpected primary action in setup lifecycle");
}
int main() {
    // First launch -> detection -> download -> ready. A prepared archive is not
    // an installed service; it must require a separate install click.
    expect(primaryAction(true, false, false, SetupActivity::Checking), PrimaryAction::Checking);
    expect(primaryAction(true, false, false, SetupActivity::Idle), PrimaryAction::Download);
    expect(primaryAction(true, false, false, SetupActivity::Preparing), PrimaryAction::Preparing);
    expect(primaryAction(true, false, true, SetupActivity::Idle), PrimaryAction::Install);
    // UAC rejection / installer failure before registration preserves Install.
    expect(primaryAction(true, false, true, SetupActivity::Installing), PrimaryAction::Installing);
    expect(primaryAction(true, false, true, SetupActivity::Idle), PrimaryAction::Install);
    // Registration can appear before the helper finishes. Keep the button busy.
    expect(primaryAction(true, true, true, SetupActivity::Installing), PrimaryAction::Installing);
    expect(primaryAction(true, true, true, SetupActivity::Idle), PrimaryAction::Start);
    // Existing service always wins over stale archive state; detection errors
    // never expose Download, Install or Start as executable actions.
    expect(primaryAction(true, true, false, SetupActivity::Idle), PrimaryAction::Start);
    for (bool installed : {false, true}) for (bool ready : {false, true}) {
        expect(primaryAction(false, installed, ready, SetupActivity::Idle), PrimaryAction::Unavailable);
        expect(primaryAction(true, installed, ready, SetupActivity::Preparing), PrimaryAction::Preparing);
    }
    std::cout << "Staged setup, explicit install, retry, registration races and detection errors passed.\n";
}
