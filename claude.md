```md
# claude.md — Kindle-For-PC-Downloader (mw)

This repo is a tiny Win32 C++ app that automates **Kindle for PC** library downloads by sending keystrokes to the Kindle window (currently: **Enter** to trigger “Download” for the selected book, then **Up Arrow** to move selection to the book above, repeat).

You are modifying the program to replace the current “About”/simple modal workflow with a **single control dialog** that supports:
- configurable delay (spin box) between the “download” step and the “move selection” step
- pause/resume (with a clear visual paused state)
- stop (with a clear visual stopped state)
- “Select Kindle Book List” remains, but **no longer uses a fixed 3s countdown**; instead it waits as long as needed for the user to click on the Kindle app
- Exit remains

---

## Repo context (what exists today)

- The program currently launches a Win32 window with a menu item that opens a modal dialog (historically the About dialog).
- Core automation is done via `SendInput()` with virtual keys:
  - `VK_RETURN` (Enter) to trigger “Download” on the currently-selected book
  - `VK_UP` to move selection upward
- There is a global-ish `hExit` event used to break out of the loop and to stop via F9 keyboard hook.

Key files you’ll likely touch:
- `Kindle Downloader.cpp` — menu wiring + dialog procedure + automation loop
- `resource.h` / `Kindle Downloader.rc` — dialog layout and control IDs (you will add/modify resources here)
- (Maybe) `Kindle Downloader.h` / `framework.h` — common Win32 includes and declarations

---

## Target UX (new dialog)

The app should start by showing a single dialog with:

1) **Delay (seconds)**  
   - A numeric **spin box** (Up-Down control) with a buddy edit field  
   - Default: **3 seconds**  
   - Range: pick a sane range (e.g., 1–60 seconds)  
   - This delay controls the time **between step 3 (download action) and step 4 (move up)**.

2) **Pause / Resume button**  
   - Clicking toggles paused state  
   - Visual indication required:
     - button text toggles “Pause” ↔ “Resume”
     - plus a status label like “Running / Paused / Stopped”

3) **Stop button**  
   - Stops the automation run immediately (or at the next safe point)  
   - Visual indication required:
     - status label becomes “Stopped”
     - disable Pause (or convert Pause back to initial state)
     - allow a fresh “Select Kindle Book List” to start again

4) **Select Kindle Book List button (start)**  
   - When clicked, the program enters “arming” mode:
     - status label: “Waiting for Kindle…”
     - it should **wait indefinitely** for the user to click into the Kindle for PC app and select a book in the list
   - After the Kindle window is active, begin the automation loop using the configured Delay.

5) **Exit button**  
   - Closes dialog/app (also stops any running automation cleanly)

---

## Implementation requirements

### 1) Don’t use a fixed countdown
Remove the `Sleep(3000)` style behavior.
Instead, after “Select Kindle Book List”:
- Wait until the user activates the Kindle app window.
- “Activated” can be defined as:
  - `GetForegroundWindow()` is not this dialog, and
  - (ideally) foreground window title/class matches Kindle for PC (best effort),
  - OR simplest acceptable: `GetForegroundWindow()` changes away from the dialog and stays away for N polls.
- Once foreground is Kindle, proceed.

### 2) Keep UI responsive
Automation must not run on the UI thread.
Use a worker thread for the download loop.
Use thread-safe primitives:
- `std::atomic<bool>` for `running`, `paused`, `stopRequested`
- Win32 events (recommended):
  - `HANDLE hStopEvent` (manual-reset) to signal stop
  - `HANDLE hPauseEvent` (auto-reset or manual-reset) to manage pause gating
  - or one event for stop + one for pause resume

UI thread updates status via:
- `SetDlgItemText`
- `EnableWindow`
- If updating from worker thread, marshal via `PostMessage(hDlg, WM_APP+X, ...)` and handle in dialog proc.

### 3) Pause semantics
When paused:
- worker thread must stop sending inputs
- worker thread should block efficiently (wait on an event), not busy-loop
- UI shows “Paused”

### 4) Stop semantics
When stop is pressed:
- signal stop event
- worker breaks out quickly
- clear `running`
- update UI state to “Stopped”
- ensure handles are closed/reset so you can start again without restarting the app

### 5) Delay semantics (important)
Interpret “Delay between steps 3 and 4” literally:
- Send Enter (download)
- wait Delay seconds
- send Up Arrow (move to previous title)
- optional small debounce after Up Arrow (could be 50–150ms) if needed, but keep it minimal and do not hardcode multi-second sleeps besides the configured delay.

### 6) Keep the old stop hotkey (optional but nice)
If F9 stop already exists, keep it working, but it must map to the same stop pathway as the Stop button.
Don’t leave orphaned hooks or threads running after stop/exit.

---

## Suggested state machine (use this mental model)

- **Idle**: nothing running. Buttons enabled: Select, Exit. Pause disabled.
- **WaitingForKindle**: after Select clicked; status says waiting. Stop enabled (optional), Pause disabled.
- **Running**: worker loop active. Pause enabled, Stop enabled, Select disabled.
- **Paused**: worker blocked. Pause button becomes Resume. Stop enabled.
- **Stopped**: terminal for last run; allows Select again.

---

## Acceptance criteria

A build is considered correct when:

- Launching the app shows the new dialog immediately.
- Delay defaults to 3 and changes via spin control.
- Clicking Select:
  - status changes to “Waiting for Kindle…”
  - it waits indefinitely until the user activates the Kindle app and highlights a book
  - then begins sending Enter then Up with Delay seconds in between
- Pause toggles:
  - status shows Paused/Running
  - while paused, no keys are sent
- Stop halts immediately and status shows Stopped.
- Exit closes cleanly even if Running/Paused.

---

## Coding/style constraints

- Keep this as plain Win32 C++ (no MFC dependency).
- Prefer small, readable functions:
  - `StartRun(...)`, `RequestStop()`, `SetPaused(bool)`, `UpdateUiState(...)`
- Don’t hardcode magic numbers in multiple places:
  - store delay seconds in one place (read from dialog control when starting; optionally allow live update)
- Make thread lifetime safe:
  - join/cleanup on stop and on exit
  - never call `EndDialog` while worker thread may still reference the dialog handle without first stopping.

---

## Notes on window targeting

This tool relies on Kindle for PC behavior where:
- Enter triggers the download action on the selected list item
- Up moves selection upward

Because `SendInput()` targets the active foreground window, ensure:
- the Kindle window stays foreground during running
- if it loses focus, you may either:
  - keep going (user responsibility), OR
  - detect focus loss and auto-pause (nice-to-have; not required unless it’s causing misfires)

---

## What to deliver in the PR

- Updated resources for the new dialog (spin control + buttons + status label)
- Updated dialog procedure and automation logic to match the state machine above
- Removal of the fixed 3-second countdown behavior
- Clean stop/pause handling without freezing UI
```
