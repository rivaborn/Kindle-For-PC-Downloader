# Kindle Downloader — Architecture

## Overview

Kindle Downloader is a plain Win32 C++ desktop application that automates bulk downloading of a
Kindle library. It has no main window: it launches directly as a modal dialog, runs its automation
on a dedicated worker thread, and uses Win32 events for thread synchronisation.

It ships **two automation engines**, selected by the radio buttons at the top of the dialog:

| Engine                                | Target                                               | How it works                                                                                                                        |
| ------------------------------------- | ---------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| **New Kindle app (Microsoft Store)**  | `AMZNKindle.AmazonKindleReadingApp` (`Kindle.exe`)   | Reads the library through UI Automation and presses each book's own **Download** button. Knows every title, so it can stop at one.   |
| **Classic Kindle for PC**             | The old desktop app                                  | The original behaviour — send `Enter` to download the selected book, `Up` to move to the one above, relying on a Recent-sorted list. |

The new engine is the default and is the reason for most of what follows. The classic engine is
unchanged from earlier versions and is kept for people still running the old desktop app.

---

## Why the new app needs a different approach

The Store Kindle app is **React Native for Windows hosted in WinUI 3**, not a classic Win32 list.
Blind `Enter`/`Up` keystrokes are unreliable against it, and they can never satisfy "stop when you
reach book X" because the program never learns what it is looking at.

UI Automation solves both problems, because the app publishes its whole library:

| UIA node                       | What it gives us                                                                 |
| ------------------------------ | -------------------------------------------------------------------------------- |
| `library-items-flatlist`       | The scroll viewport. Supports `ScrollPattern`.                                    |
| `library-item-container`       | One per book. `Name` is `"<Title> by <Author>"` (plus `", New"` once downloaded). |
| two `Text` children            | The clean title, then the author.                                                 |
| `download-button-<ASIN>`       | **Present only while the book still needs downloading.**                          |
| `library-more-menu-<ASIN>`     | Always present; used to identify the row.                                         |

So "does this book need downloading?" is simply "does a `download-button-…` exist?", and the button
disappears when the download **completes** — a free progress signal.

### Seven quirks that shape the code

These were all measured against app version 1.0.23620.0 and are the reason several pieces of
`KindleUia.cpp` look more defensive than they otherwise would. Numbers 5–7 were each found only by
reading a run log after the app looked like it was working.

1. **There is no programmatic press.** The Download buttons expose no `InvokePattern`, and
   `LegacyIAccessible::DoDefaultAction()` returns `S_OK` while doing nothing at all — React Native
   publishes the accessibility shell but never wires it to its touch handler. A synthesized mouse
   click is the only thing that works.
2. **The click needs a hover first.** Move the pointer, wait ~150 ms so React Native processes
   pointer-enter, and only then press. Pressing 30–40 ms after the move is silently ignored.
3. **`VerticalScrollPercent` is always 0.** List movement therefore has to be detected from the
   content, by matching a named row between two snapshots and comparing where it sits.
4. **Only on-screen rows can be clicked.** The list keeps ~90–170 rows realised in the UIA tree but
   only ~8 are visible; off-screen rows have real-looking coordinates that would click the wrong
   thing.
5. **`ElementFromPoint` must be retried, not believed first time.** While the list is re-rendering —
   right after a download completes, or after a scroll — it returns *nothing at all* for a point
   that is perfectly clickable half a second later. Taking that first answer at face value silently
   failed 16 of 41 clicks. It is polled 12 × 100 ms, and only a *different, non-empty* AutomationId
   counts as genuine occlusion.
6. **Row pitch is the smallest gap between neighbours, not `items[1] - items[0]`.** The list parks a
   few recycled rows far above the rest, so the first gap can read 1683 px when the true pitch is
   153 px. Getting this wrong turned a deliberately-short scroll into 1.5 screens that stepped over
   books entirely.
7. **`ScrollAmount_LargeIncrement` moves exactly one viewport** (measured: 1198 px against a 1202 px
   viewport). Zero overlap means a half-visible bottom row lands half-visible at the top and is
   never pressed. The wheel moves a much smaller, measurable step and is used instead.

---

## Source Files

| File                    | Purpose                                                                        |
| ----------------------- | ------------------------------------------------------------------------------ |
| `Kindle Downloader.cpp` | Entry point, dialog proc, both worker threads, UI helpers                       |
| `KindleUia.h/.cpp`      | UI Automation driver for the new Store app (`kuia::Session`, title matching)    |
| `Kindle Downloader.rc`  | Dialog layout, icons, string table (UTF-16LE, no BOM)                          |
| `resource.h`            | Numeric IDs for dialog and controls                                            |
| `framework.h`           | Include stub; `windows.h` and standard C headers                                |
| `Kindle Downloader.h`   | Includes `resource.h`; included by the main `.cpp`                              |
| `KWindow.cpp`           | Empty placeholder (left over from an earlier version)                           |
| `targetver.h`           | Sets the Windows SDK version via `SDKDDKVer.h`                                  |
| `build.bat`             | Command-line build; links `ole32`, `oleaut32`, `uiautomationcore`               |

---

## State Machine

Unchanged in shape from the previous version; `WaitingForKindle` is now also re-entered mid-run
whenever the Kindle window loses focus.

```
Idle ──[Start clicked]──► WaitingForKindle
                                │
                    [Kindle window is foreground]
                                │
                                ▼
                 ┌──────────── Running ◄────────────────┐
                 │              │                        │
          [Pause clicked]  [Stop / F9]            [Resume clicked]
                 │              │                        │
                 ▼              ▼                        │
               Paused ──────► Stopped              (from Paused)
                 └───────────────────────────────────────┘

From Stopped or Idle:  Start can be clicked to begin a new run.
```

| State              | Description                                                                  |
| ------------------ | ---------------------------------------------------------------------------- |
| `Idle`             | Initial state; no run has been started yet                                    |
| `WaitingForKindle` | Waiting for the Kindle window to become the foreground window                 |
| `Running`          | Worker thread is actively downloading                                         |
| `Paused`           | Worker thread is blocked; nothing is sent                                     |
| `Stopped`          | Run has ended (naturally, by Stop/F9, or on reaching the stop title)          |

---

## Thread Model

```
Main thread (UI)                Worker thread                    Hook thread
────────────────                ─────────────                    ───────────
DialogBox()                     WorkerProcNewApp()  or           SetWindowsHookEx(WH_KEYBOARD_LL)
  MainDlgProc                   WorkerProcClassic()              GetMessage loop
    Start  -> StartRun            own COM apartment                F9 -> RequestStop()
    Pause  -> SetPaused           kuia::Session
    Stop   -> RequestStop         PostMessage -> UI
    Exit   -> RequestStop
              JoinWorker
```

Synchronisation is by two manual-reset events, exactly as before:

| Handle          | Meaning                                                              |
| --------------- | -------------------------------------------------------------------- |
| `g_hStopEvent`  | Signalled = stop as soon as possible                                  |
| `g_hPauseEvent` | Signalled = running; reset = paused (worker blocks, timer frozen)     |

`WaitInterruptible(ms)` is the single chokepoint: it honours pause by blocking, honours stop by
returning `true`, and is used for every delay in both engines so no wait is ever uninterruptible.

The worker never touches controls directly. It posts messages instead:

| Message            | Payload                             | Handled by                    |
| ------------------ | ----------------------------------- | ----------------------------- |
| `WM_APP_SET_STATE` | `WPARAM` = new `AppState`           | `UpdateUiState`               |
| `WM_APP_LOG`       | `LPARAM` = `new std::wstring*`      | `AddLogLine` (then `delete`)  |
| `WM_APP_PROGRESS`  | none; counters live behind a mutex  | `RefreshProgress`             |

`DrainPendingLogs` frees any log strings still queued when the dialog closes.

## Run log

A run against a 30,000 book library takes hours, so the list box on the dialog is only the headline.
Every run also opens a file:

```
%LOCALAPPDATA%\KindleDownloader\logs\run-<date>-<time>.log
```

`LogUi` writes to both; `LogDetail` writes only to the file. The file carries the things needed to
diagnose a run after it has finished: per-book button rectangles, the click point, **what UIA
actually reported was under the cursor**, and for every scroll the distance moved, the target, the
notch count and the calibrated pixels-per-notch. It is UTF-8 with a BOM and is flushed after every
line, so a run that is killed still leaves a complete tail.

Both real bugs in the traversal logic — the row-pitch outlier and the premature hit-test — were
invisible on screen and obvious in this file, which is the argument for keeping it verbose.

---

## New-app worker loop

One press per reading of the list — not one screenful per reading. The extra snapshot costs a few
hundred milliseconds against a multi-second delay, and it removes a whole class of bug, because the
library re-sorts itself as downloads complete and a rectangle a few seconds old can point at a
different book.

```
Initialize COM + UI Automation
Attach       -> find a Microsoft.UI.Windowing.Window owned by Kindle.exe,
                confirm the library list is present
loop:
    honour pause / stop
    wait until the Kindle window is foreground        (never click otherwise)
    snapshot the realised rows (sorted by y) + the viewport rectangle

    scan the visible rows top to bottom and pick the next book:
        matches the stop title       -> log and finish
        no Download button           -> count once as already downloaded
        ASIN already attempted       -> pass over (a second press cancels the transfer)
        button not wholly on screen  -> defer to the next screen, which overlaps
        otherwise                    -> this is the target

    if a target was found:
        press it
        spend the configured delay watching for its Download button to vanish
            gone    -> "Downloaded"
            still there -> "Downloading", left to finish in the background
        loop (re-read the list)

    otherwise, nothing left on this screen:
        scroll down by viewport - 2 rows
        if the list did not move, retry up to 5 times with a pause
        only then conclude the library has ended
```

Two counts deserve care. A row that is only half on screen is **deferred**, not skipped — it comes
round on the next screen, and reporting it as a skip is what made the skip counter look alarming.
A genuine click failure is retried up to three times before it is ever called a skip.

Two safety properties are worth calling out, because they are what stop this from clicking wildly
around the desktop:

- **Re-resolve, then hit-test.** `ClickDownload` looks the button up again by ASIN, checks it is
  inside the viewport, moves the pointer, and calls `ElementFromPoint` to confirm that the button is
  genuinely the topmost thing under the cursor. Anything else — a moved row, another window on top,
  a dialog that stole focus — is reported as a typed `ClickResult` instead of being clicked.
- **Never press the same ASIN twice.** A second press on a Download button cancels the transfer the
  first one started, so attempted ASINs are remembered for the run. (Rows that failed for a
  transient reason are removed from that set so a later pass can retry them.)

The pointer is returned to where the user left it after every press.

---

## Stopping at a title

`kuia::MatchesStopTitle` compares case-insensitively and with runs of whitespace collapsed.

The app lists authors surname-first ("Hunt, Samantha") while people naturally type them
forename-first, so a whole-string comparison of *"The Unwritten Book: An Investigation by Samantha
Hunt"* would never match. The matcher therefore tries, in order:

1. the text as a substring of the clean title,
2. the text as a substring of `"<title> by <author>"`,
3. if the text contains `" by "`, just the part before it against the title.

When a row matches, the run stops **without downloading that book** — it is a boundary, not the last
item to fetch.

---

## Function Reference

### `Kindle Downloader.cpp`

| Function                          | Role                                                                       |
| --------------------------------- | -------------------------------------------------------------------------- |
| `wWinMain`                        | Sets per-monitor DPI awareness, starts the hook thread, shows the dialog    |
| `MainDlgProc`                     | Dialog messages, button commands, worker-posted updates                     |
| `UpdateUiState(hDlg, state)`      | Status text and which controls are enabled for a state                      |
| `SyncStopTitleAvailability(hDlg)` | Stop-at-title only applies to the new engine; greys it out otherwise        |
| `StartRun(hDlg, cfg)`             | Resets counters, creates the events, launches the right worker              |
| `RequestStop` / `SetPaused`       | Signal the events                                                           |
| `JoinWorker`                      | Joins the thread and closes the handles so a new run can start              |
| `WaitInterruptible(ms)`           | The one pausable, stoppable delay used everywhere                           |
| `WaitUntilKindleActive(...)`      | Blocks until Kindle is foreground, narrating the wait once                  |
| `WorkerProcNewApp(hDlg, cfg)`     | The UI Automation loop described above                                      |
| `WorkerProcClassic(hDlg, cfg)`    | The original `Enter` / `Up` keystroke loop                                  |
| `SendKey(vk)`                     | One synthetic key press (classic engine)                                    |
| `KeyboardProc` / `HookThreadProc` | Global F9 hotkey, routed to the same `RequestStop` as the button            |

### `KindleUia.h/.cpp` — `kuia::Session`

| Member                    | Role                                                                          |
| ------------------------- | ----------------------------------------------------------------------------- |
| `Initialize(err)`         | COM apartment, `IUIAutomation`, and the cached-property request                |
| `Attach(err)`             | Finds the Kindle window (class **and** owning process) and its library list    |
| `EnsureForeground()`      | Un-minimises and activates Kindle, then verifies it really is foreground       |
| `SnapshotItems(out, err)` | One cached bulk read of every realised row — title, author, ASIN, rectangles   |
| `GetViewport(out)`        | The scroll viewport, clipped to the window                                     |
| `HasDownloadButton(asin)` | Still needs downloading? The completion signal a press is verified against      |
| `ClickDownload(asin, &i)` | Re-resolve, bounds-check, hover, patiently hit-test, press; typed `ClickResult` |
| `ScrollDown(&info)`       | Down by viewport − 2 rows via the wheel, self-calibrating; reports what moved   |

A `Session` owns per-thread COM state and must be created, used and destroyed on the worker thread.

`SnapshotItems` uses `FindAllBuildCache` with a subtree cache request, so a whole page of rows costs
one cross-process call rather than several hundred property fetches — this matters a great deal on
a 35,000-book library.

---

## Resource Layout (`IDD_MAINDLG`)

300 × 215 dialog units.

| Row    | Controls                                                                     |
| ------ | ---------------------------------------------------------------------------- |
| y=4    | "Kindle app" group box: `IDC_MODE_NEW`, `IDC_MODE_CLASSIC`                    |
| y=44   | "Delay (seconds)": `IDC_DELAY_EDIT` + `IDC_DELAY_SPIN` (range 1–60, default 3)|
| y=62   | `IDC_STOP_TITLE_CHECK` and `IDC_STOP_TITLE_EDIT`                              |
| y=95   | `IDC_STATUS_LABEL`, `IDC_COUNTS_LABEL`, `IDC_CURRENT_LABEL`                   |
| y=130  | `IDC_LOG` list box (activity log, capped at 500 lines)                        |
| y=194  | `IDC_SELECT` (Start), `IDC_PAUSE`, `IDC_STOP`, `IDC_EXIT_BTN`                 |

The `.rc` is UTF-16LE with **no BOM**; keep it that way when editing.

---

## Building

```
build.bat            :: release
build.bat debug      :: debug
```

Output lands in `build\release\KindleDownloader.exe`. The UI Automation engine adds `ole32.lib`,
`oleaut32.lib` and `uiautomationcore.lib` to the link line.

Per-monitor-v2 DPI awareness is set at startup and is **not optional** — UI Automation reports
physical pixels, so without it every rectangle is scaled and every click misses.
