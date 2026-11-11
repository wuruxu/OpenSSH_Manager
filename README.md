# OpenSSH Manager for Windows

A standalone C++17 / Win32 tray application and configuration manager. The SSH service is provided by
Microsoft's Win32-OpenSSH, with no Cygwin, Qt, Electron, or .NET GUI runtime dependency.
This directory does not modify the parent Portable OpenSSH build or protocol implementation.

The current version is **0.1.23, an initial implementation**. Configuration, public-key entry deletion,
log parsing, scroll calculations, and proportional layout tests have run on Linux, and the x64 Windows
GUI has been cross-compiled. **The UI, UAC, installation, rollback, and SSH sign-in have not yet been
validated on a real Windows machine.** Use a test machine first; validation steps are provided below.

## Features

- A native interface inspired by Chrome Settings, following the system light/dark mode: sidebar navigation,
  rounded groups, blue primary actions, and status indicators. Uses Win32 controls with selective GDI+
  drawing provided by Windows, without a browser engine.
- Native Simplified Chinese, Traditional Chinese, and English interfaces. At startup, the app reads the
  current user's Windows UI language preferences and selects the first supported language in preference order:
  `zh-Hans` / mainland China / Singapore use Simplified Chinese; `zh-Hant` / Taiwan / Hong Kong / Macao use
  Traditional Chinese; English uses English. If no preference matches, the app falls back to English.
  Script tags take precedence over regions. Selection does not depend on IP address, time zone, or the
  system locale for non-Unicode programs. After changing the system language, exit the tray app and restart it.
  The main UI, tray, popups, installation progress, and application-generated errors/log explanations share
  a built-in translation catalog. Original OpenSSH logs, system errors, account names, paths, and configuration
  directives remain unchanged. Elevated helpers and installation scripts inherit the initiating user's selected
  language, even when UAC uses a different administrator account. Installation and uninstallation scripts also
  select their language automatically. Translations are embedded in the EXE; release packages include
  `Localization.ps1` for the scripts. No online translation is required.
- Tray menu, close-to-tray behavior, icon recovery after Explorer restarts, and DPI awareness.
  The window is resizable. The main window has no horizontal or vertical scrollbars; control coordinates and
  dimensions scale proportionally to the client area's width and height. Fonts, icons, and drawing details use
  the smaller of the two scale factors to prevent distortion, updating when the window size or DPI changes.
- Arrowless capsule scrollbars inside public-key/log lists and multiline detail fields, approximately 50% of
  the system scrollbar thickness at the current DPI. Wrapping description fields allow vertical scrolling only
  and ignore default or stale scroll ranges on disabled axes; event details still support horizontal scrolling.
  Supports thumb dragging, track clicks for paging, and the mouse wheel, while retaining native text/list
  scrolling logic. ListView keeps the original system scrollbar gutter and draws a half-width capsule inside it.
  The outer list frame does not resize when scrollbars appear or disappear, preventing layout loops.
  Window resizing caches the background, batches control positioning, and then refreshes the result.
  Mouse-wheel scrolling applies only within lists and text fields.
- Padding around the right-hand pages is halved to give content more room, retaining proportional layout
  without main-window scrollbars. Page, sidebar, and import-popup action buttons use Chrome-style capsule
  shapes, a baseline height of 38 pixels, and 16 pixels of horizontal padding.
  "Start service", "Save & apply", and "Import" use blue primary buttons without emphasized borders.
  Secondary buttons have a subtle light fill, blue text, and a low-contrast one-pixel outline; hover/pressed
  states primarily strengthen the fill instead of adding a dark gray border. Dark mode uses corresponding
  muted colors. Destructive actions use red text and a pale red border.
  Disabled buttons fade both text and border and do not respond to hover. Sidebar selection remains borderless.
  Light, dark, and high-contrast modes share the same hover/pressed/disabled state rules.
  Mouse clicks do not draw dotted focus rectangles; keyboard focus uses a short underline. Buttons are drawn
  in memory before being presented in one pass to avoid white flashes during background erasure.
  Service polling sets each button's final enabled state once and does not call EnableWindow when it is unchanged.
  Refreshing identical public-key content does not rebuild the list, preserving selection and scroll position.
- Automatically follows the Windows app color mode, with system high-contrast colors taking priority.
  System theme notifications are coalesced; unchanged palettes do not trigger a redraw.
  The main UI, lists/headers, edit fields, checkboxes, dropdowns, and import popup switch together while
  preserving input, selection, and column widths. The title bar uses the system-supported dark-mode attribute;
  on older Windows versions without that attribute, the client area still changes theme.
  Implementation references: [Windows app color mode settings](https://learn.microsoft.com/en-us/windows/apps/develop/settings/settings-common)
  and [Win32 light/dark themes](https://learn.microsoft.com/windows/apps/desktop/modernize/apply-windows-themes).
- Selected navigation items use a solid fill without a border; keyboard focus is indicated by a short underline.
  The sidebar divides its available height among four rows so rounding during scaling does not scroll the first
  item out of view when the last item is selected. All pages, including hidden pages, are laid out in advance.
  Page switches synchronize lists and headers before showing them, avoiding movement on first entry or after resizing.
  Only the page area and changed visibility are updated. Lists use their own double buffering; top-level
  descendant compositing has been removed. The ListView scroll host changes the outer list frame only when
  the page size changes; queries, scrolling, and scrollbar visibility changes only synchronize capsule state.
  The built-in ListView header is hidden, with a separate Header control above the list. A cached bitmap draws
  the light gray header, smaller text, vertical column separators, and bottom border instead of calling the
  built-in header's themed drawing. Column dragging updates the list and header cache; horizontal scrolling
  keeps headings aligned with data columns. After manual resizing, refreshes, page switches, and window resizing
  preserve column widths for the current run. Restarting the app restores the default automatic layout.
  Public-key and log lists share row drawing: a baseline height of 36 pixels, light gray hover, light blue
  selection, and a continuous background across the entire row, including the delete column and trailing space.
  A short marker on the left indicates keyboard focus. Native ListView still handles data, selection, keyboard
  input, column widths, and scrolling. Mouse movement redraws only affected rows. Batch data updates suspend
  both list and header redraw until completion; headers are not actively redrawn while idle.
- A blue OpenSSH-themed icon, with source in `resources/app.svg` and `OpenSSHManager.svg` included in release packages.
  Redrawn from the sunglasses-wearing pufferfish in the [OpenSSH logo](https://www.openssh.org/images/openssh.gif),
  using Windows-style blue. The ICO contains sizes from 16 to 256 pixels for the EXE, window, taskbar, tray, and UI branding.
- Arrow-key navigation, native keyboard behavior in edit fields/dropdowns/checkboxes, and system colors in high-contrast mode.
- Selected sidebar items have a straight left edge and rounded right corners, without an additional border.
- Version and build time (`MM-DD HH:mm`) appear below the top-left icon area. The version comes from the CMake
  project version; the build timestamp is recorded during compilation and displayed in the runtime computer's
  Windows local time. The separate "Current Windows account" and "Service status" headings have been removed.
- The top area shows the current app account, administrator/standard-user identity, and a separate elevation state
  before the service status. An administrator running without UAC elevation still appears as
  "Administrator · Not elevated". Identity detection is not used to grant permissions.
- Startup only detects `sshd` and the local engine. The primary action progresses through
  "Download → Install service → Start service". Download and installation each require a click, with progress,
  cancellation of preparation, retries, and offline ZIP support.
- View the standard `sshd` service status; start, stop, restart, and select automatic/manual startup.
- The service overview shows the registered `sshd.exe` file version below the service name, or unknown if it cannot be read.
- The current user's tray startup at sign-in is independent of system SSH service startup at boot.
  "Show tray at Windows sign-in" and "Start SSH at boot" appear as side-by-side checkboxes with a gap of
  22 design pixels, scaled with the window. The service startup option is synchronized with the actual state
  after the elevated operation finishes.
- Edit the global port, one listen IP, password/public-key authentication, TCP forwarding, and allowed users/groups.
  The listen-IP dropdown lists connected interface names and IPv4 / IPv6 addresses, refreshing when opened.
  Only listed entries can be selected, including an all-addresses option. A configured address missing from
  the current interface list remains available as "Current setting (interface unavailable)", preventing unrelated
  saves from changing the listening scope. The selected concrete IP is saved, including interface indices for
  IPv6 link-local addresses. If an interface address changes, select and apply it again.
  Authentication/forwarding and network listening share a card, with authentication/forwarding above.
  Each heading shares a row with its controls, and the listen hint aligns with the heading's left edge.
  Port and listen-IP labels leave a 12px gap to the visible control border, scaled with the font; the listen-IP
  dropdown is 30% narrower. The port label's left edge aligns with the first checkbox in the row above.
  Clicking blank page space or switching to another window removes control focus and closes dropdown lists.
- Validate candidate configurations, detect external changes using SHA-256, replace atomically, retain backups,
  and restore the configuration if restarting fails.
- Preserve existing comments, unknown directives, and `Match` blocks unchanged. Global `Include`, multiple
  ports/listen addresses, and other configurations the form cannot represent reliably disable form-based
  overwriting. The configuration file can still be edited manually.
- Set the default shell to CMD or Windows PowerShell. The description, shell dropdown, and apply button share a row.
- A native ListView shows public-key comments, types, and a delete icon on the right. The delete icon retains
  the row's selection background, uses a red destructive-action symbol, becomes bolder on hover, and uses a
  disabled color during operations. Hover shows the complete public key; Delete removes the selected entry.
  The key-target dropdown sits to the right of the "Public keys" heading with a gap of 22 design pixels.
  Its expanded list is wider to show full target paths; the space previously used by the dropdown increases
  list height. Deletion asks for confirmation and checks the file revision. Only the selected entry is removed,
  preserving other entries with the same key, restriction options, comments, and line endings.
- "Import key" opens an input popup with a pointer tip. Paste one public key and confirm; local validation
  failures or canceled UAC prompts preserve the input. Import still performs actual validation with `ssh-keygen`
  and sets ACLs using SIDs rather than group names specific to English Windows installations.
- When reading the shared administrator key file requires elevation, the UI shows "Authorize". After UAC,
  a local, one-time pipe returns the list without relaxing key-file permissions or writing temporary export files.
  Administrator import/deletion returns the updated list after the same authorization.
- On first use, the public-key page selects a default target based on the current account and displays the full
  path computed by the same function used for writing. Manual selections are preserved. Administrators choosing
  the personal file are warned that the default `Match` rules may prevent it from taking effect.
- Merge the latest 200 events from Operational / Admin channels in descending time order, showing time, result,
  account, source IP, and explanation. Select an event for its full message and XML. The UI remains usable while
  events are read asynchronously.
- Manage the dedicated `OpenSSHManager-Server` firewall rule: domain/private profiles, local subnet, and a specific
  executable and port. If the rule exists, configuration saves update it. Other applications' and system rules
  are not modified.

Does not include a live session list, session termination, traffic statistics, arbitrary account creation/deletion,
a graphical SFTP chroot editor, or automatic engine updates. Personal public-key management is limited to the current
signed-in user; other users should import keys from their own desktop sessions. Existing entries with options such as
`command=` can be displayed and deleted individually; creating or changing those restrictions still requires manual editing.

## Pinned SSH engine

`engine.lock.json` pins the x64 release ZIP and SHA-256 for Microsoft's official `10.4.0.0p1-Preview`.

The installer verifies `sftp-server.exe` and appends the protected `engine` directory to the system PATH so this
Win32-OpenSSH version's `internal-sftp` can find the actual executable. Repeated installation does not add duplicates.
A normal reinstall repairs PATH for a service owned by this manager when both the installation marker and service path
match, without restarting the service. Explicit `-GuiOnly` installation and external SSH services do not trigger PATH
repair. Existing manual PATH entries are not claimed; ownership of newly added entries is recorded in `InstallState.json`.
**Preview is the upstream release label, not a stable-release designation.** The app does not silently follow `latest`;
this engine is also distinct from Portable 10.6p2 in the parent directory. The engine can be replaced independently,
but the lock file must be updated and regression tests completed first.

Packages use official binaries and retain their licenses and notices. To build the engine yourself, use
[Microsoft's Windows source branch](https://github.com/PowerShell/openssh-portable) and the
[official build instructions](https://github.com/PowerShell/Win32-OpenSSH/wiki/Building-OpenSSH-for-Windows-%28using-LibreSSL-crypto%29).

## Build

On Windows, install Visual Studio's Desktop development with C++ tools, the Windows SDK, and CMake,
then run these commands from the repository root:

```powershell
cmake -S windows -B windows/build -A x64
cmake --build windows/build --config Release
ctest --test-dir windows/build -C Release --output-on-failure
```

Output: `windows/build/Release/OpenSSHManager.exe`. The program uses the static C++ runtime.
Its manifest specifies `asInvoker`, so the resident GUI does not require administrator privileges.

Cross-compile on Linux (requires `x86_64-w64-mingw32-g++` and `windres`):

```sh
cmake -S windows -B windows/build-mingw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x64.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build windows/build-mingw
```

Run the standalone configuration tests on Linux:

```sh
cmake -S windows -B /tmp/openssh-manager-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/openssh-manager-tests
ctest --test-dir /tmp/openssh-manager-tests --output-on-failure
```

## Create a release package

Requires Python 3. Download the pinned version and verify its SHA-256:

```sh
python3 windows/scripts/package.py \
  --manager-exe windows/build-mingw/OpenSSHManager.exe
```

For offline packaging, also specify `--engine-zip /path/to/OpenSSH-Win64.zip`. ZIP files that do not match the
lock file are rejected. The output is `windows/dist/OpenSSHManager-0.1.23-win64.zip`, with a SHA-256 file alongside it.
The package contains the GUI, complete OpenSSH engine, installation/uninstallation/validation scripts, and per-file hashes.

The GUI is not yet code-signed. Before an official release, sign the GUI and repackage it after signing.
The installation script checks copied-file hashes and verifies Microsoft Authenticode signatures on key engine files.

## Installation and startup

Target: 64-bit Windows 10/11 or Windows Server 2019 and later, with an interactive desktop.
Refer to the corresponding Microsoft release notes for the engine's exact OS support.
Windows 11 x64 is recommended for initial validation.

Run `OpenSSHManager.exe` directly to detect the service; there is no need to run the installation script first:

- If `sshd` already exists, including a stopped or disabled service, inspect the engine files referenced by the
  service. If they are complete, manage the existing service without automatically upgrading it.
- If the service remains registered but this manager's engine is missing, check both the service command line
  and `InstallState.json`. "Download → Repair" is available only when the service points exactly to this manager's
  `engine\sshd.exe` and the marker's `Product`, `Directory`, and boolean `OwnsService=true` all match.
  If a valid cache is already available, show "Repair" directly.
- If the marker is missing, corrupt, or has mismatched ownership, or the service comes from another directory,
  report the problem without automatically repairing or overwriting it. Ownership is checked again after elevation,
  and by the script before stopping the service, writing the engine, and restoring the service.
- Repair verifies the pinned ZIP hash and Microsoft signatures, stops this manager's service, and restores the engine.
  It preserves the existing service registration, startup type, configuration, and all keys. A disabled service stays
  disabled; otherwise, the service starts after configuration validation. Missing or invalid configuration/keys cause
  an error rather than automatic regeneration.
- If no service exists, first reuse a complete engine with an installation marker under
  `Program Files\OpenSSHManager\engine`; otherwise, validate `OpenSSH-Win64.zip` beside the app or the download cache.
  A usable engine enables "Install service" directly; otherwise, the app shows "Download".
  Startup itself does not download anything.
- Clicking "Download" downloads and verifies the engine in the background, with progress on the button and support
  for cancellation. Completion only changes the action to "Install service"; it does not automatically request elevation.
  Clicking "Install service" requests UAC, installs into `Program Files\OpenSSHManager\engine`, and registers and starts
  the service.
- Download, verification, or installation failures display the cause. Click "Download" or "Install service" again to retry.
  Canceling UAC preserves the verified cache and restores the "Install service" action.
- If GitHub is unreachable, click "Offline ZIP" to select the official ZIP version specified by `engine.lock.json`.
  After successful verification, the app waits for "Install service" to be clicked. Other versions and corrupt files
  are rejected.
- Service-query errors stop the automatic flow. Checks are repeated after download and after elevation to avoid
  overwriting a service created in the meantime.

Installation scripts, the version lock, and the hash are embedded in the EXE, so the GUI also works when distributed
alone. Downloads use Windows WinHTTP with the system proxy. The cache is stored in the current user's
`%LOCALAPPDATA%\OpenSSHManager`. After elevation, the ZIP's SHA-256 is checked again. Extraction and Microsoft signature
verification take place in a directory writable only by administrators and SYSTEM before installation proceeds.
Existing configuration and keys are preserved. Engine installation does not move or replace the running GUI and does not
automatically create a Start menu shortcut for it. Initial installation enables service startup at boot and creates the
firewall rule; the SSH service keeps running after the GUI exits. This is not automatic engine updating; the lock-file
version does not change with `latest`.

Startup performs detection only, without downloading or installing. The legacy `--no-auto-install` option remains for
compatibility and has the same behavior as the default; `--tray` is still supported. The Windows smoke-test script checks
that default startup does not enter the download or installation stage. The app cannot exit during installation;
during downloading, cancel the download before exiting. If the service is registered but startup or firewall setup fails,
the UI shows the failure and actual state. Subsequent launches only manage the existing service. Check the configuration,
start the service, or synchronize the firewall as appropriate; the app does not automatically overwrite a registered service.

To install the GUI into Program Files and create a Start menu shortcut as well, fully extract the release package and
**double-click `Install.cmd`**. The standalone installer behaves as follows.

Approve the UAC administrator prompt. The installer window shows detection results, progress, and the final result;
press Enter to close it. There is no need to open an administrator PowerShell window or select an installation mode manually.

- No registered `sshd`: install the engine and GUI, then register and start the service.
- Existing `sshd`, whether running or stopped: automatically install or update only the GUI, preserving the existing
  engine, service state, configuration, and keys.
- Canceled UAC: cancel installation. A service-query failure reports an error and exits; it is not treated as an absent service.
- The installation path is fixed at `Program Files\OpenSSHManager`. Exit the old GUI from its tray menu before updating.

`Install.cmd` is the double-click entry point; the actual installation is performed by the reviewable `Install.ps1` script.
When needed, it automatically switches to 64-bit administrator PowerShell. It can also be run from a normal PowerShell window:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1
```

The installation directory is fixed at `Program Files\OpenSSHManager`. The installer registers only the `sshd` service
and preserves any existing `ssh-agent`. It generates host keys and a default configuration, enables service startup at boot,
and creates the firewall rule described above. SSH sign-in is independent of the GUI process; exiting the tray does not
stop the service.

Automatic detection is the default. To explicitly install only the GUI, even when no SSH service exists, use:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -GuiOnly
```

`-GuiOnly` does not replace the engine or change the service or keys. It can also update the GUI in an existing installation.
The generated Start menu shortcut includes `--no-auto-install` to prevent a GUI-only installation from triggering engine
installation on first launch. Exit the GUI through its tray menu before updating. There is currently no automatic engine
upgrade flow.

An existing service automatically selects GUI-update mode. An initial full installation preserves and reuses an existing
`ProgramData\ssh` directory. Before proceeding, the installer backs up the existing configuration, host keys, and shared
administrator authorized keys to `ProgramData\ssh.manager-backups\<random-directory>`. Only SYSTEM and administrators can
access the backup. Existing configuration contents remain unchanged; only missing host keys are generated, and ACLs on
managed files are corrected. An orphaned host public key without its private key, or an invalid configuration, stops the
installation with an explicit error instead of silently replacing user data.

If initial installation fails before service registration, fix the error and retry without deleting configuration or keys.
If the service is registered but not running, use the GUI to check the configuration and start it. Double-clicking the installer
again only updates the GUI; it does not automatically repair or replace the existing service.

After installation, launch OpenSSH Manager from the Start menu as a **normal desktop user**.
For system changes, the GUI launches a restricted administration mode of the same EXE with UAC elevation.
There is no resident management service or local HTTP control port. Canceling UAC preserves the current form.

### The "Start service" button is disabled

"Not installed" means Windows has no registered service named `sshd`. Startup performs detection only: the primary action
is "Download" when the engine is not ready, and "Install service" when it is ready. Download and installation each require
a click. Placing an `sshd_config` file alone does not register a service. Before installation, Stop and Restart are hidden,
while the offline ZIP action remains available.

The installer starts the service, so successful installation should show "Running". A disabled "Start service" button is
normal in this state; use "Stop service" or "Restart" as needed. The service page explains why an action is unavailable
and offers installation when the service is absent. If service information cannot be read, the actual error is shown
instead of continuing to display the previous service state.

## Configuration behavior

Only a standard `sshd` service registered with an absolute path to `sshd.exe`, without additional arguments, is supported.
The configuration must be at `ProgramData\ssh\sshd_config`. Custom service names, `-f` arguments, and multiple instances
are explicitly rejected for editing.

Save sequence: validate input → check the old configuration hash → write a protected temporary file → run the matching
engine's `sshd -t -f` → recheck the hash → atomically replace and back up with `ReplaceFileW` → restart if the service was
running → synchronize an existing dedicated firewall rule.

On failure, the app attempts to restore the previous configuration, firewall rule, and service state. Both the original
failure and any recovery failure are displayed. Saving settings does not start a service that was previously stopped.
Recovery copies are stored as `.manager-{GUID}.backup` in the configuration directory; administrators currently need to
clean up old copies manually.

`Match` rules can override global values. User and group conditions are also subject to Windows OpenSSH account-naming rules.
A successful configuration check does not guarantee that a particular user can sign in; validate sign-in with actual accounts.
Restarting the service may interrupt existing connections. Seamless hot reload is not guaranteed.

The default shell is configured through the system-wide `HKLM\SOFTWARE\OpenSSH\DefaultShell` setting.
The default administrator authorized-keys file is shared by the Administrators group, so an imported key may authorize
multiple administrator accounts. The app follows upstream defaults and does not treat this as one administrator's personal
key file. Check custom `AuthorizedKeysFile` settings yourself. The UI does not handle Windows sign-in passwords.

The default firewall rule does not allow public-network access. Other existing rules may allow broader access; this app
does not tighten them. Administrators must explicitly configure Windows Firewall scopes for access across subnets or
from the public Internet.

## Sign-in logs

On the "Event logs" page, click "Refresh logs" to read the Operational and Admin channels.
Times use the current computer's time zone. Missing accounts/IP addresses are shown as `—`.
When a password or public-key failure has no stated cause, the explanation explicitly says it is unknown.
A closed connection is not automatically classified as an authentication failure. Selecting an event shows its full
message and XML below for copying and troubleshooting.

If reading one channel fails, results from the other channel are retained and the error is shown. For access-denied errors,
run the GUI as administrator and read again. The app only reads existing logs; it does not clear them or change the logging
level. Logs configured for file output are not included in this list.

## Windows validation

Run `Test-Windows.ps1` to check window startup, four-page navigation, arrow-key switching, the absence of main-window
scrollbars, proportional sizing and bounds of controls on all four pages, internal half-width scrollbars, text-scroll
synchronization, and close-to-tray behavior. In administrator PowerShell, add `-CheckService` to check the installed service
configuration. The script terminates only the GUI it launched and does not stop the SSH service.

Also validate the following on a test machine:

1. Double-click first installation, automatic GUI updates, and explicit `-GuiOnly`; launch from a standard user,
   administrator, and 32-bit PowerShell. Cancel UAC or switch credentials. Test paths containing spaces, Chinese characters,
   and special characters. Error windows must remain visible and exit codes must be correct.
   Existing services, whether running or stopped, must not be replaced or started; existing configuration and keys must
   be preserved. Check installation-directory and host-key ACLs. Detection failures, a busy installation directory,
   repeated installation, and incomplete release packages must produce clear errors.
2. All four pages at 100%/150%/200% DPI; scaling across monitors, small windows, and maximized proportional layout.
   The main window must have no scrollbars, and all main controls must stay within the client area.
   Test Tab/Shift+Tab, navigation arrow keys, Space/Enter on buttons, borderless navigation selection, disabled states,
   and high-contrast readability. Continuously resize width and height: text and icons must scale uniformly and buttons
   must stay within bounds. Continuous internal list scrolling must not cause repeated layout or white flashes.
   Drag thumbs to both ends, hold the scrollbar track, and scroll long text and log lists.
   Independent headers must not flash white while idle, on hover, or during data updates. After resizing columns or
   scrolling horizontally, headings must remain aligned with data columns.
   Internal scroll ranges must update after page/DPI changes, and icons must remain clear in the window and tray.
   Test tray right-click, double-click, and recovery after Explorer restarts.
   Check the top account identity as a standard user, a non-elevated administrator, and an elevated administrator.
   Long account names must not cover the service status. The default public-key target must match the account;
   refreshing after a manual target change must preserve that selection, and the actual import path must match the hint.
3. Cancel UAC and confirm that the form is preserved. A standard user must not be able to execute `--admin` directly.
   Exiting during an apply operation must be prevented.
4. Change the port, authentication, and user rules. Preserve `Match` blocks. After an external configuration edit,
   saving from the stale form must be rejected.
5. Invalid configurations must not replace the existing file. Use an occupied port to simulate service-start failure
   and verify restoration of the previous configuration and service state.
6. Validate password/public-key authentication separately with a standard local account, an administrator account,
   and a domain account. Incorrect passwords must be rejected.
7. Test interactive CMD/PowerShell terminals, window resizing, remote commands, SFTP uploads/downloads, and Unicode filenames.
8. Check public-key comments/types, full-key tooltips for long keys, the delete icon, and the Delete key.
   Only the selected row should be deleted; duplicate keys and entries with options must not be removed accidentally.
   External file changes must cause deletion from a stale list to be rejected. Test target switching, missing files,
   insufficient permissions, and empty lists. Check the paste popup's pointer position, DPI across monitors,
   Tab/Enter/Esc, and closing by clicking outside. Reject private keys, multiline input, and invalid public keys;
   canceling UAC must preserve input. Test administrator-authorized reading and list refresh after import/deletion.
   Confirm that shared administrator key-file ACLs have not been relaxed.
9. Firewall changes must affect only this product's rule and synchronize after port changes.
   If domain policy prevents changes, the app must report the error and attempt recovery.
10. Generate successful sign-in, incorrect-password, invalid-account, and closed-connection events.
    Compare timestamps and messages with both channels in Event Viewer. Missing accounts/IPs must not be guessed.
    The UI must remain responsive during refresh. If one channel denies access, the other channel's results and
    the error must remain available.
11. SSH must remain available after desktop sign-out, and the service must start after restarting Windows.
    Uninstalling the GUI must not delete host keys.

## Uninstallation

Exit the tray app first, then run `Uninstall.ps1` from the installation directory or release package in administrator PowerShell.
It removes only the `sshd` service owned by this product whose path still matches, its firewall rule, and the GUI entry points.
External services managed through `-GuiOnly` are not removed.

Only the engine PATH entry recorded as added by this installer is removed. Pre-existing entries and paths subsequently
added by other software are preserved. Old installation markers without ownership information do not cause PATH removal.
Keys, configuration, and backups under `ProgramData\ssh`, along with engine event resources, are preserved to avoid breaking
historical logs or an existing agent. Other Windows users can remove their tray auto-start entries in their own Startup Apps settings.

## Code structure

- `src/main.cpp`: sidebar navigation, native controls, proportional window layout, tray, DPI, and asynchronous waiting
  for elevated processes.
- `src/theme_controls.cpp`: buffered drawing and theme colors for native checkboxes/dropdowns, preserving native interaction.
- `src/theme.cpp`: shared palettes, rounded buttons, list-row drawing, navigation icons, keyboard focus, and system high-contrast support.
- `src/layout_math.h` / `tests/layout_tests.cpp`: proportional viewport mapping, uniform font scaling, and boundary tests
  for small, wide, and tall windows.
- `src/scrollbar.cpp` / `src/scroll_math.h`: capsule scrollbars, independent headers and cached drawing, native control hosts,
  scroll ranges, and drag mapping.
- `tests/scroll_tests.cpp`: regression tests for scroll ranges, tiny tracks, nonzero origins, positions beyond 16 bits, and boundaries.
- `resources/app.svg` / `app.ico`: application icons; `scripts/build-icon.py` can regenerate the ICO using Inkscape and Pillow.
- `src/authorized_keys.cpp` / `tests/authorized_keys_tests.cpp`: entry parsing that preserves original bytes, exact-row deletion,
  and regression tests.
- `src/key_popup.cpp`: native public-key paste popup with a pointer tip, input validation, and input preservation on cancellation.
- `src/key_channel.cpp`: local, one-time public-key response pipes with restricted ACLs, process verification, and integrity checks.
- `tests/native_controls_tests.cpp`: Windows tests for empty/full lists, automatic column widths, scrollbar visibility thresholds,
  page show/hide, stable outer frames, independent-header drawing and idle redraw behavior, bidirectional column-width synchronization,
  horizontal alignment, GDI resource cleanup, scroll synchronization, popup validation/cancellation, and response-pipe transfer.
  Does not modify real public keys.
- `src/platform.cpp`: SCM, UAC, process output, configuration transactions, public keys, ACLs, firewall, and registry access.
- `src/events.cpp`: structured Windows Event Log reads, merging of both channels, and event details.
- `src/diagnostics.cpp`: sign-in log classification and explanations.
- `tests/diagnostics_tests.cpp`: regression tests for log classification, account/IP extraction, and message-length bounds.
- `src/i18n.h` / `src/i18n.cpp` / `src/i18n_strings.inc`: language matching, Windows UI language detection, and built-in
  Simplified Chinese, Traditional Chinese, and English text catalogs.
- `tests/i18n_tests.cpp`: language preference order, script precedence, English fallback, translation completeness,
  file filters, and log/configuration behavior tests.
- `scripts/Localization.ps1` / `tests/localization_tests.ps1`: installer translations, argument formatting, and language-selection tests.
  When CMake detects `pwsh` or `powershell`, CTest also runs the script tests. Set `-DPOWERSHELL_EXECUTABLE=full-path` to specify a runtime.
  For Windows UI regression testing, run `scripts/Test-Windows.ps1 -Manager executable-path -UiLanguage zh-Hans`,
  `-UiLanguage zh-Hant`, and `-UiLanguage en` separately. These checks exercise the UI without changing service configuration.
- `src/config.cpp`: platform-independent configuration parsing/merging, input validation, and Windows argument quoting.
- `tests/config_tests.cpp`: configuration preservation, idempotence, rejection of unsupported configurations, and injection validation;
  Windows additionally runs argument round-trip tests.
- `tests/install_mode_tests.ps1`: automatic installation modes, service-query failures, and elevation-argument quoting regression tests.
- `scripts/EnginePath.ps1` / `tests/engine_path_tests.ps1`: preservation of raw system PATH values and types, deduplication,
  ownership of added entries, removal, and retry tests after failures.
- `scripts/`: double-click installation entry point, automatic elevation/detection, uninstallation, packaging, and Windows smoke tests.

This project is not affiliated with Microsoft. The GUI uses the BSD 2-Clause license in this directory;
the OpenSSH engine is covered by its bundled licenses and notices.

### Staged installation validation

On a Windows test machine, verify startup with no service and no network access or elevation; downloading followed by
waiting at "Install service"; download cancellation; restoring "Install service" after UAC rejection; selecting the pinned
ZIP offline and waiting for an installation click; rejection of incorrect ZIPs; direct management of existing stopped/running
system OpenSSH services; preservation of configuration and host keys; and actual SSH/SFTP connections after installation.

For script-level checks, run `windows/tests/engine_archive_tests.ps1` (temporary files only, with no elevation or service changes).
Add `-OfficialArchive <ZIP-path>` to validate the real pinned ZIP. Actual installation and UAC still require Windows validation.
