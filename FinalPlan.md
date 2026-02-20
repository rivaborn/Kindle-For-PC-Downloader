# FinalPlan.md — Complete Session Summary

## Project Goal

Rewrite **Kindle Downloader**, a Win32 C++ application that automates bulk downloading
from Kindle for PC, replacing its old modal "About" dialog workflow with a single
control dialog featuring:

- Configurable delay (spin box, default 3 s)
- Pause / Resume with visual status
- Stop with visual status
- "Select Kindle Book List" that waits indefinitely for the user to click into
  the Kindle window (no fixed countdown)
- Exit

All automation was to run on a worker thread (not the UI thread), using Win32 events
and `std::atomic` for thread safety.

---

## Phase 1 — Build System Repair

### Problem 1: `&&` not valid in PowerShell

**Symptom:**
```
At line:1 char:91
The token '&&' is not a valid statement separator in this version.
```

**Cause:**
The `.vscode/tasks.json` had `"type": "shell"` which defaults to PowerShell on
Windows. The task command used `&&` to chain `VsDevCmd.bat && cl.exe ...`, but
PowerShell versions before 7.0 do not support `&&` as a statement separator.
PowerShell intercepted and choked on the `&&` before `cmd.exe` ever saw it.

**First attempted fix:** Changed `"type": "shell"` → `"type": "process"`.

**Result:** Failed. VS Code in process mode passed args with single-quote wrapping,
so cmd received `'/C'` literally, which it does not understand.

**Second attempted fix:** Kept `"type": "shell"` but added:
```json
"options": {
  "shell": { "executable": "cmd.exe", "args": ["/C"] }
}
```
and moved the entire build command into `"command"`.

**Result:** Failed. `cmd.exe /C "C:\Program Files\..."` — the space in the VS path
caused cmd to split the path at the space and fail with `'C:\Program' is not
recognized`.

**Root cause of path-splitting:** The classic `cmd /C "..."` quoting rule. When the
first token after `/C` is a quoted string with an embedded space, cmd splits it.
The extra outer-quotes trick (`cmd /C ""path with space\..." && ...`) was the
correct theoretical fix, but VS Code's escaping layer kept collapsing the extra
quotes before cmd saw them.

**Final fix:** Created `build.bat` containing the build commands. The VS Code task
simply calls `build.bat` via `cmd.exe /C`. This sidesteps all shell quoting issues
entirely.

**Final `tasks.json`:**
```json
{
  "version": "2.0.0",
  "tasks": [{
    "label": "Build (MSVC)",
    "type": "shell",
    "command": "build.bat",
    "options": {
      "shell": { "executable": "cmd.exe", "args": ["/C"] },
      "cwd": "${workspaceFolder}"
    },
    "group": { "kind": "build", "isDefault": true },
    "problemMatcher": "$msCompile"
  }]
}
```

---

### Problem 2: `LNK1136` — invalid or corrupt file (rc passed to linker)

**Symptom:**
```
Kindle Downloader.rc : fatal error LNK1136: invalid or corrupt file
```

**Cause:**
The original `build.bat` passed the raw `.rc` file directly to the linker.
The linker cannot consume `.rc` files; they must first be compiled by `rc.exe`
into a `.res` binary, which the linker can then accept.

**Fix:** Added a separate `rc.exe` step before the `cl.exe` step:
```bat
rc.exe /fo "build\Kindle Downloader.res" "Kindle Downloader.rc"
if errorlevel 1 exit /b %errorlevel%
cl.exe ... /link ... "build\Kindle Downloader.res"
```

---

### Problem 3: `LNK1104` — cannot open output exe (file locked)

**Symptom:**
```
LINK : fatal error LNK1104: cannot open file 'build\Kindle Downloader.exe'
```

**Cause:**
The exe from a previous successful build was locked by Windows (either still
running, or held by Windows Defender/Search Indexer).

**Attempted fixes:**
- Added `taskkill /f /im "Kindle Downloader.exe" >nul 2>&1` — did not help
  (process not found or AV still held it)
- Added `del /f /q "build\Kindle Downloader.exe" >nul 2>&1` — could not delete
  from WSL either (`Permission denied`)

**Final fix:** Changed the output executable name to remove the space:
```
/Fe:"build\KindleDownloader.exe"
```
This outputs to a new filename that was never locked, avoiding the conflict
entirely. `taskkill` and `del` were updated to match the new name.

---

### Final `build.bat`

```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not exist build mkdir build
taskkill /f /im "KindleDownloader.exe" >nul 2>&1
del /f /q "build\KindleDownloader.exe" >nul 2>&1
rc.exe /fo "build\Kindle Downloader.res" "Kindle Downloader.rc"
if errorlevel 1 exit /b %errorlevel%
cl.exe /EHsc /W4 /std:c++17 /D_UNICODE /DUNICODE /DWIN32 /D_WINDOWS ^
    "Kindle Downloader.cpp" KWindow.cpp ^
    /Fe:"build\KindleDownloader.exe" ^
    /link user32.lib kernel32.lib comctl32.lib shell32.lib ^
    "build\Kindle Downloader.res"
```

---

## Phase 2 — Code Rewrite

### What existed before

- `wWinMain` registered a window class, created a main window, ran a message loop.
- Clicking a menu item opened `IDD_ABOUTBOX` via `DialogBox`.
- The `About()` dialog proc ran the entire automation loop **on the UI thread**,
  blocking it completely.
- A fixed `Sleep(3000)` was used to give the user time to click into Kindle.
- A single `HANDLE hExit` event was used for stop signalling.
- F9 keyboard hook ran on a separate thread (`executeHook`) that waited on `hExit`.
- The loop sent `VK_RETURN` then `VK_UP` with `Sleep(1000)` between iterations
  (hardcoded, not configurable).
- No pause capability existed.

### Plan: target UX and architecture

**Dialog (`IDD_MAINDLG`):**
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

**State machine:**
```
Idle ──[Select]──► WaitingForKindle ──[Kindle detected]──► Running
                         │                                    │    │
                    [Stop/F9]                          [Pause] [Stop/F9]
                         │                                    │    │
                         ▼                                  Paused  │
                       Stopped ◄────────────────────────────────────┘
                         │
                    [Select again]
                         │
                    WaitingForKindle  (new run)
```

**Thread model:**
- UI thread: `DialogBox` message loop only; never blocks.
- Worker thread: `WorkerProc` — waits for Kindle, runs automation loop.
- Hook thread: `HookThreadProc` — dedicated `GetMessage` loop for `WH_KEYBOARD_LL`.

**Sync primitives:**

| Name | Type | SET means | RESET means |
|---|---|---|---|
| `g_hStopEvent` | manual-reset event | stop requested | still running |
| `g_hPauseEvent` | manual-reset event | running (not paused) | paused — worker blocks |

**Worker→UI communication:** `PostMessage(hDlg, WM_APP_SET_STATE, newState, 0)`
UI thread handles `WM_APP_SET_STATE`, calls `JoinWorker()` on Stopped, then
`UpdateUiState()`.

---

### Files changed

#### `resource.h` — full replacement

Removed all old IDs (`IDD_ABOUTBOX`, `IDC_TargLoc`, `IDM_ABOUT`, `IDM_EXIT`,
`IDC_ExitWindow`, `IDC_StartDL`, `IDC_StopDL`, etc.).

Added:
```c
#define IDD_MAINDLG          200
#define IDC_DELAY_EDIT       1010
#define IDC_DELAY_SPIN       1011
#define IDC_STATUS_LABEL     1012
#define IDC_PAUSE            1013
#define IDC_STOP             1014
#define IDC_SELECT           1015
#define IDC_EXIT_BTN         1016
```

Kept: `IDS_APP_TITLE`, `IDC_KINDLEDOWNLOADER`, `IDI_KINDLEDOWNLOADER`, `IDI_SMALL`.

---

#### `Kindle Downloader.rc` — full replacement

**Important discovery:** The original `.rc` file was stored as **UTF-16 LE**
(Visual Studio's default). This caused the file to appear with spaces between
every character when read as ASCII/UTF-8. The file was rewritten from scratch
as plain **ANSI/UTF-8**, which `rc.exe` (VS 2022) handles correctly for
all-ASCII content.

The old dialogs (`IDD_ABOUTBOX`, `IDD_KindleView`) were removed. The menu and
accelerators were removed (no longer needed; app is dialog-only). The new dialog
`IDD_MAINDLG` was defined.

**Spin control style note:** Rather than including `commctrl.h` in the `.rc` file
(which can cause INCLUDE path issues), the spin control style was expressed as a
computed hex literal:
```
WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ARROWKEYS = 0x50000022
```
The buddy association (`UDM_SETBUDDY`) is done in code at `WM_INITDIALOG` rather
than relying on `UDS_AUTOBUDDY` in the resource, making it explicit and reliable.

---

#### `Kindle Downloader.cpp` — full replacement

All old code removed. New structure:

**Globals:**
```cpp
static HINSTANCE           g_hInst;
static HWND                g_hDlg;
static AppState            g_state = Idle;
static std::thread         g_workerThread;
static HANDLE              g_hStopEvent  = NULL;
static HANDLE              g_hPauseEvent = NULL;
static std::atomic<DWORD>  g_hookThreadId{ 0 };
static HANDLE              g_hookReadyEvent = NULL;
```

**`wWinMain`:**
Initialises common controls, starts hook thread (waits up to 2 s for it to prime
its message queue), calls `DialogBox(IDD_MAINDLG)`, then shuts down the hook
thread with `PostThreadMessage(WM_QUIT)` and joins it.

**`MainDlgProc`:**
Handles `WM_INITDIALOG`, `WM_APP_SET_STATE`, `WM_COMMAND` (Select, Pause, Stop,
Exit/Cancel), `WM_CLOSE`. Never blocks.

**`UpdateUiState(hDlg, state)`:**
Single function that owns all button enable/disable logic and status text. Called
from the UI thread only (directly or via `WM_APP_SET_STATE`).

**`StartRun(hDlg, delaySec)`:**
Creates both events and launches `WorkerProc` thread.

**`RequestStop()`:**
Sets both events. Setting `g_hPauseEvent` is essential — it unblocks a paused
worker so it can see the stop event rather than waiting forever.

**`SetPaused(bool)`:**
Resets or sets `g_hPauseEvent`.

**`JoinWorker()`:**
Joins the thread, closes and nulls both event handles. Safe to call repeatedly.

**`WorkerProc(hDlg, delaySec)`:**
Worker thread body. Calls `WaitForKindle` (posts `Stopped` and returns if
stopped), then posts `Running`, then runs automation loop (posts `Stopped` when
done). Never touches UI directly — all UI updates via `PostMessage`.

**`WaitForKindle(hDlg)`:**
Polls `GetForegroundWindow()` every 200 ms. Requires 3 consecutive polls where
the foreground differs from the dialog (~600 ms of stability) before returning.
Checks `g_hStopEvent` on each poll.

**`WaitInterruptible(ms)`:**
Key design: tracks `remaining` milliseconds explicitly (not a wall-clock
deadline) so the timer **freezes while paused**. Uses 50 ms sleep slices.
On each iteration:
1. Check stop → return `true` (stopped)
2. Check `g_hPauseEvent` (RESET = paused) → block on `WaitForMultipleObjects`
   until resume or stop
3. If remaining == 0 → return `false` (elapsed)
4. Sleep a slice, subtract actual elapsed time from remaining

**`SendKey(vk)`:**
Sends key-down + key-up `INPUT` struct pair via `SendInput`.

**`KeyboardProc(nCode, wParam, lParam)`:**
Low-level keyboard hook. On F9 key-down, calls `RequestStop()`.

**`HookThreadProc()`:**
Primes message queue via `PeekMessage`, signals `g_hookReadyEvent`, installs
`WH_KEYBOARD_LL` hook, runs `GetMessage` loop. Removes hook on exit.

---

### Key design decisions and rationale

| Decision | Rationale |
|---|---|
| Dialog-only app (no main window) | Spec says "single control dialog"; eliminates unneeded boilerplate |
| `PostMessage` for worker→UI state changes | Never safe to call Win32 UI functions from a non-UI thread |
| Manual-reset event for pause (SET = running) | `WaitForSingleObject` returning WAIT_TIMEOUT means paused; semantics are clear and efficient |
| `RequestStop` sets both events | Without setting `g_hPauseEvent`, a paused worker would wait forever before seeing stop |
| Timer tracks `remaining` not deadline | Ensures configured delay is honoured even across pause/resume cycles |
| 3-poll Kindle detection (~600 ms) | Debounces accidental brief focus changes; keeps wait-time short |
| Spin buddy set in code, not resource | Avoids `UDS_AUTOBUDDY` quirks and INCLUDE dependency on `commctrl.h` in the `.rc` |
| Output exe renamed to `KindleDownloader.exe` | Avoids linker LNK1104 caused by Windows locking the space-named exe from a prior build |

---

### Build result

```
Microsoft (R) C/C++ Optimizing Compiler Version 19.44.35211 for x86
Kindle Downloader.cpp
KWindow.cpp
Generating Code...
Microsoft (R) Incremental Linker Version 14.44.35211.0
/out:build\KindleDownloader.exe
... (all libs) ...
[no errors]
```

Output: `build\KindleDownloader.exe` ✓

---

## Files Produced / Modified

| File | Action | Summary |
|---|---|---|
| `.vscode/tasks.json` | Modified (3 iterations) | Fixed shell/quoting; final form calls `build.bat` via `cmd.exe` |
| `build.bat` | Created | Runs VsDevCmd, rc.exe, cl.exe; handles locked-exe problem |
| `resource.h` | Replaced | New control IDs for new dialog |
| `Kindle Downloader.rc` | Replaced | New ANSI .rc with `IDD_MAINDLG`; removed old dialogs and menu |
| `Kindle Downloader.cpp` | Replaced | Full rewrite: dialog-only, threaded, state machine |
| `architecture.md` | Created | Full developer documentation of the rewritten app |
| `FinalPlan.md` | Created | This file |

Files **not changed:** `KWindow.cpp`, `framework.h`, `Kindle Downloader.h`,
`targetver.h`, `small.ico`, `Kindle Downloader.ico`, `Kindle Downloader.sln`,
`Kindle Downloader.vcxproj`, `Kindle Downloader.vcxproj.filters`
