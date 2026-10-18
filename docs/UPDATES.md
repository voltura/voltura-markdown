# Automatic updates

**Automatic updates** is an unchecked option in the reader's three-dot menu. It is stored in the existing per-user preferences. Turning it off cancels work and prevents installation.

The first check runs five seconds after a successful document or empty-window paint. Subsequent checks run hourly while the reader remains open. All requests, downloads, signature verification, hashing, and cache maintenance run on a worker below normal priority. Closing never joins this worker or waits for a download.

The choice is saved immediately. Closing a different window cannot restore an older choice, and the helper checks the saved opt-in again before installing.

## Selection and verification

The feed is the latest public stable release in `voltura/voltura-markdown`. Numeric versions must be newer than the running reader. Drafts, previews, incomplete releases, and downgrades are rejected. Installed copies select `VolturaMarkdown-Setup-<version>-win-x64.exe`; portable copies select `vmd.exe`. Installed identity requires an exact normalized executable-path match with the existing HKCU uninstall registration.

Each payload must match `SHA256SUMS.txt`, product/company/version metadata, its expected reader/installer identity and architecture, and the running application's signing certificate. Authenticode must verify the signature and timestamp. The only trust exception is an untrusted root for that same pinned self-signed publisher; other signature errors fail. Verification uses cached certificate retrieval so it cannot hold up shutdown with network requests. Unsigned applications cannot automatically update. Changing signing certificates requires a manual update to establish the new publisher pin.

Requests use HTTPS and an allowlist of GitHub release hosts, bounded redirects, short individual timeouts, a two-minute transfer deadline, and a 64 MiB payload limit. Errors remain quiet and retry on the next scheduled check.

## Staging and close

Downloads live under `%LOCALAPPDATA%\Voltura\Markdown\Updates`, separated by executable path. A new release invalidates the previous ready marker before downloading. Partial downloads are never installable. Completion atomically publishes a marker after verification and prepares a copy of the signed reader as the helper. Superseded owned files and abandoned partial downloads are cleaned by later checks; unrelated files and reparse points are retained.

A newer published tag blocks the older download even while its assets are still being uploaded. If its marker cannot be saved, in-memory eligibility and waiting helpers are cancelled; installation defers to a later reader session.

A normal close hands an already prepared candidate to the helper. The helper waits up to 30 seconds for readers of that executable to exit, then verifies the candidate again. A per-target mutex serializes installation and coordinates reopening. A reopened reader cancels a waiting helper; an installation already applying may briefly defer that reader's initialization. Other readers are never forcibly closed.

The reader and a hidden top-level helper window handle session-ending messages. A shutdown/sign-out query cancels installation even if the shutdown is later cancelled. An inherited event preserves cancellation across the reader/helper handoff; the installer checks that event and Windows shutdown state before replacement. Automatic updates never restart the reader or request elevation/reboot.

Installed updates run NSIS silently against the registered directory. Setup stages the executable and retains the original if replacement fails. Portable updates replace the original filename with a same-directory backup/rollback step and never create an installation. Locked or unwritable targets defer without a prompt. Diagnostics are limited to a 64 KiB log in the target's update cache. No task, service, or updater persists between reader sessions.

Setup serializes manual and automatic installer processes before staging replacement files. This adds no installer text or pages.

## Manual updates

Interactive and explicitly launched silent installers use Windows Restart Manager to close readers of the target executable gracefully. Other copies and unrelated applications are not registered for shutdown. There is no forced termination. After replacement, Restart Manager reopens the saved windows; a failed replacement also reopens them with the original executable. Automatic updates retain their separate no-restart behavior.

On a manual update request, each reader saves its document path, source view, zoom, window placement/fullscreen state, horizontal position, and top block plus exact offset within that block. A digest of the loaded source is compared on the restarted document worker. Unchanged content resumes at its saved position without laying out the whole document. Changed content opens at the beginning; a removed document returns the ordinary loading error. Empty windows are restored too. Snapshot hashing and disk writes happen only on an explicit update shutdown request, outside ordinary opening, scrolling, and closing.

Restart snapshots are single-use per-user files with executable identity, bounded content, a one-day validity limit, and reparse-point checks. Abandoned files are bounded and pruned on later manual update requests. A cancelled session end unregisters that restart. The helper cancels installation/reopening on Windows session-ending messages and never asks for a reboot.

Exact session restoration requires the running reader to include this support. Older released readers cannot retroactively supply their current reading position. Windows also cannot restart readers launched under another Windows account.

## Verification

- `scripts/build.ps1`: Release build, CTest policy/cache/replacement tests, and production instrumentation checks.
- `scripts/test-updates.ps1`: isolated signed staging, replacement, cancellation, reader reopening, modified/wrong-publisher rejection, inherited handoff, and real silent NSIS update. Requires the existing local signing configuration and NSIS. It does not update the normal installation.
- `scripts/test-restart.py`: real Restart Manager and NSIS tests with copied readers, an isolated installer identity and fallback directory, Unicode/spaces, multiple documents, empty windows, source/zoom/fullscreen, exact offsets, replacement failure, and changed/removed content. Restarted test readers return to the private desktop before creating any windows.
- `scripts/benchmark-updates.py`: alternating startup comparisons and scrolling with updates disabled, enabled, and downloading a signed local fixture. Full samples stay under ignored `build/`.
- Existing CommonMark, native startup, search, display, and installer lifecycle checks remain required.

NSIS `/D=` and `_?=` values must be the final unquoted command-line argument, including paths with spaces. Manual-update test packages also compile an isolated `TEST_INSTALL_DIRECTORY` with a separate `PRODUCT_KEY`, preventing an ignored `/D=` from falling back to the normal installation.

Session-end tests send Windows messages to task-owned test windows. They do not shut down Windows or establish physical shutdown timing guarantees. Performance measurements are local warm-cache evidence, not cold-start proof.
