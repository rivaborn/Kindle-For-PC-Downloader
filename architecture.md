# Kindle Downloader — Architecture

## Overview

Kindle Downloader is a plain Win32 C++ desktop application that automates bulk downloading of books in **Kindle for PC**. It works by sending synthetic keystrokes (`Enter` to trigger a download, `Up Arrow` to move to the next book) to whatever window is in the foreground, with a user-configurable delay between the two key events.

The application has no main window. It launches directly as a modal dialog, runs its automation on a dedicated worker thread, and uses Win32 events for thread synchronisation.

---

## Source Files

| File | Purpose |
|---|---|
| `Kindle Downloader.cpp` | All application logic — entry point, dialog proc, worker thread, helpers |
| `Kindle Downloader.rc` | Dialog layout, icons, string table (ANSI/UTF-8) |
| `resource.h` | Numeric IDs for dialog and controls |
| `framework.h` | Precompiled-header stub; includes `windows.h` and standard C headers |
| `Kindle Downloader.h` | Includes `resource.h`; included by the main `.cpp` |
| `KWindow.cpp` | Empty placeholder (left over from earlier version) |
| `targetver.h` | Sets the minimum Windows SDK version via `SDKDDKVer.h` |

---

## State Machine

The application tracks one of five states stored in `g_state`:

```
Idle ──[Select clicked]──► WaitingForKindle
                                │
                    [Kindle window gains focus]
                                │
                                ▼
                 ┌──────────── Running ◄────────────────┐
                 │              │                        │
          [Pause clicked]  [Stop / F9]            [Resume clicked]
                 │              │                        │
                 ▼              ▼                        │
               Paused ──────► Stopped              (from Paused)
                 └───────────────────────────────────────┘

From Stopped or Idle:  Select can be clicked to start a new run.
```

| State | Description |
|---|---|
| `Idle` | Initial state; no run has been started yet |
| `WaitingForKindle` | "Select" was clicked; watching for the Kindle window to gain focus |
| `Running` | Worker thread is actively sending keystrokes |
| `Paused` | Worker thread is blocked; no keystrokes are sent |
| `Stopped` | Run has ended (naturally or by user); a new run may be started |

---

## Thread Model

```
Main thread (UI)                     Worker thread              Hook thread
─────────────────────                ──────────────             ───────────
DialogBox() message loop             WorkerProc()               HookThreadProc()
  handles WM_COMMAND                 WaitForKindle()            GetMessage() loop
  handles WM_APP_SET_STATE  ◄──────  PostMessage()              KeyboardProc()
  UpdateUiState()                    SendKey()                    └─ F9 → RequestStop()
  StartRun() / RequestStop()         WaitInterruptible()
```

Two Win32 manual-reset events coordinate the worker thread:

| Event | Signalled meaning | Reset meaning |
|---|---|---|
| `g_hStopEvent` | Stop has been requested | Still running |
| `g_hPauseEvent` | Running (not paused) | Paused — worker blocks |

---

## Main Program Flows

### 1. Application startup

1. `wWinMain` initialises the Common Controls library (needed for the spin/up-down control).
2. Creates `g_hookReadyEvent`, then launches `HookThreadProc` on a new thread and waits up to 2 s for it to signal that its message queue is ready.
3. Calls `DialogBox(IDD_MAINDLG)` which blocks, running the dialog's own internal message loop.
4. When the dialog closes, posts `WM_QUIT` to the hook thread and joins it.

### 2. Dialog initialisation (`WM_INITDIALOG`)

1. Saves the dialog handle in `g_hDlg`.
2. Associates the spin control (`IDC_DELAY_SPIN`) with the edit control (`IDC_DELAY_EDIT`) as its buddy via `UDM_SETBUDDY`.
3. Sets the spin range to 1–60 and the default position to 3 (seconds).
4. Calls `UpdateUiState(Idle)` to set button enable/disable and status label text.

### 3. Starting a run (`IDC_SELECT` clicked)

1. Reads the delay value from the edit control; clamps to [1, 60].
2. Calls `UpdateUiState(WaitingForKindle)` — disables Select, Pause, and the delay controls; sets status to "Waiting for Kindle…".
3. Calls `StartRun()`:
   - Creates `g_hStopEvent` (not signalled) and `g_hPauseEvent` (signalled = running).
   - Launches `WorkerProc` on a new `std::thread`.

### 4. Worker thread — waiting phase

`WaitForKindle()` polls `GetForegroundWindow()` every 200 ms. Once the foreground window differs from the dialog for three consecutive polls (~600 ms), it considers the Kindle window active and returns. If `g_hStopEvent` fires during this loop, it returns early.

On success (Kindle detected):
→ Posts `WM_APP_SET_STATE(Running)` to the dialog.

On stop:
→ Posts `WM_APP_SET_STATE(Stopped)` and exits.

### 5. Worker thread — automation loop

Iterates up to 800 times (one iteration = one book):

```
Check stop/pause (WaitInterruptible 0 ms)
  └─ if stop → break
  └─ if paused → block until resumed or stopped
SendKey(VK_RETURN)           ← triggers "Download" in Kindle
WaitInterruptible(delay × 1000 ms)
  └─ timer freezes while paused
  └─ if stop → break
SendKey(VK_UP)               ← moves selection to the previous book
WaitInterruptible(100 ms)    ← brief debounce
  └─ if stop → break
```

When the loop ends (by completing all iterations or by stop):
→ Posts `WM_APP_SET_STATE(Stopped)` and exits.

### 6. Pause and resume

- **Pause button**: calls `SetPaused(true)` which calls `ResetEvent(g_hPauseEvent)`. The worker is blocked inside `WaitInterruptible` at the `WaitForMultipleObjects` call. `UpdateUiState(Paused)` flips the button text to "Resume".
- **Resume button**: calls `SetPaused(false)` which calls `SetEvent(g_hPauseEvent)`. The worker unblocks and the delay timer continues from where it left off (remaining milliseconds are tracked separately from wall-clock time). `UpdateUiState(Running)` flips the button text back to "Pause".

### 7. Stop

- **Stop button or F9**: calls `RequestStop()` which sets both `g_hStopEvent` and `g_hPauseEvent` (to unblock a paused worker). The worker detects the stop event at its next `WaitInterruptible` check and exits, then posts `WM_APP_SET_STATE(Stopped)`.
- The UI thread receives `WM_APP_SET_STATE(Stopped)`, calls `JoinWorker()` (joining the thread and closing both event handles), then calls `UpdateUiState(Stopped)`.

### 8. Exit

Clicking Exit, pressing Escape, or closing the window:
1. Calls `RequestStop()` and `JoinWorker()` (safe to call even if nothing is running).
2. Calls `EndDialog()`.
3. Control returns to `wWinMain`, which shuts down the hook thread and returns.

---

## Function Reference

### `wWinMain`
Entry point. Initialises common controls, starts the keyboard hook thread, runs the app as a modal dialog via `DialogBox`, then cleans up the hook thread before returning.

---

### `MainDlgProc`
Dialog procedure for `IDD_MAINDLG`. Handles:
- `WM_INITDIALOG` — wires spin buddy, sets range/default, initialises UI state.
- `WM_APP_SET_STATE` — marshalled from worker thread; joins worker on Stopped, then calls `UpdateUiState`.
- `WM_COMMAND` — dispatches button clicks (Select, Pause, Stop, Exit/Cancel).
- `WM_CLOSE` — stops worker, joins, ends dialog.

---

### `UpdateUiState(hDlg, state)`
Sets `g_state`, updates the status label text, and enables/disables controls according to the state machine rules:
- **Select** and the delay spin/edit are enabled only in `Idle` or `Stopped`.
- **Pause** is enabled only while `Running` or `Paused`; its label toggles between "Pause" and "Resume".
- **Stop** is enabled while `Running`, `Paused`, or `WaitingForKindle`.

---

### `StartRun(hDlg, delaySec)`
Creates `g_hStopEvent` (manual-reset, initially unsignalled) and `g_hPauseEvent` (manual-reset, initially signalled = running), then launches `WorkerProc` on a new `std::thread`.

---

### `RequestStop()`
Sets both `g_hStopEvent` and `g_hPauseEvent`. Setting `g_hPauseEvent` ensures a paused worker unblocks and sees the stop event rather than waiting forever.

---

### `SetPaused(bool paused)`
Resets `g_hPauseEvent` to block the worker (pause) or sets it to unblock the worker (resume). No-op if `g_hPauseEvent` is NULL (no run active).

---

### `JoinWorker()`
Joins `g_workerThread` (blocks until it has exited), then closes and nulls `g_hStopEvent` and `g_hPauseEvent`. Safe to call when no worker is running. Always called before starting a new run or exiting.

---

### `WorkerProc(hDlg, delaySec)`
Worker thread body. Two phases:
1. **Wait phase**: calls `WaitForKindle`; posts `Stopped` and returns if stop fires, or posts `Running` and continues.
2. **Automation loop**: up to 800 iterations of `SendKey(Enter) → WaitInterruptible(delay) → SendKey(Up) → WaitInterruptible(100)`. Posts `Stopped` when done.

---

### `WaitForKindle(hDlg)`
Polls `GetForegroundWindow()` every 200 ms. Returns `false` (Kindle detected) once the foreground window differs from `hDlg` for `KINDLE_STABLE_POLLS` (3) consecutive polls. Returns `true` if `g_hStopEvent` is signalled before that happens.

---

### `WaitInterruptible(ms)`
Waits for up to `ms` milliseconds, with two interruption paths:
- **Stop**: `g_hStopEvent` signalled → returns `true` immediately.
- **Pause**: `g_hPauseEvent` reset → blocks on `WaitForMultipleObjects` until either stop (returns `true`) or resume (continues counting down).

The remaining time is tracked explicitly so the delay timer is frozen while paused. Uses 50 ms slices so stop/pause are detected promptly. Returns `false` when the full duration has elapsed.

---

### `SendKey(vk)`
Sends a key-down then key-up `INPUT` event for the given virtual key code via `SendInput`. Targets whatever window currently holds foreground focus.

---

### `KeyboardProc(nCode, wParam, lParam)`
Low-level keyboard hook callback. Calls `RequestStop()` when F9 (`VK_F9`) key-down is detected. Passes all events to the next hook via `CallNextHookEx`.

---

### `HookThreadProc()`
Runs the keyboard hook on a dedicated thread so it has its own message queue (required for `WH_KEYBOARD_LL`). Primes the message queue with `PeekMessage`, signals `g_hookReadyEvent` so `wWinMain` knows the hook is ready, installs the hook with `SetWindowsHookEx`, then runs a `GetMessage` loop until `WM_QUIT` is posted by `wWinMain` at exit. Removes the hook with `UnhookWindowsHookEx` before returning.

---

## Resource Layout (`IDD_MAINDLG`)

```
┌─────────────────────────────────────────┐
│  Kindle Downloader                  [X] │
├─────────────────────────────────────────┤
│  Delay (seconds):  [ 3 ][▲]            │
│                                         │
│  Status: Idle                           │
│                                         │
│       [ Select Kindle Book List ]       │
│                                         │
│    [ Pause ]          [ Stop ]          │
│                                         │
│              [ Exit ]                   │
└─────────────────────────────────────────┘
```

| Control ID | Type | Purpose |
|---|---|---|
| `IDC_DELAY_EDIT` | `EDITTEXT` | Numeric input for delay (1–60 s) |
| `IDC_DELAY_SPIN` | `msctls_updown32` | Spin arrows; buddy is `IDC_DELAY_EDIT` |
| `IDC_STATUS_LABEL` | `LTEXT` | Displays current state text |
| `IDC_SELECT` | `PUSHBUTTON` | Starts a run (arms the waiting phase) |
| `IDC_PAUSE` | `PUSHBUTTON` | Toggles pause/resume; label changes accordingly |
| `IDC_STOP` | `PUSHBUTTON` | Stops the current run |
| `IDC_EXIT_BTN` | `PUSHBUTTON` | Exits the application |
