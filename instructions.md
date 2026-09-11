# Kindle Downloader — Instructions

Automates bulk downloading of your Kindle library.

The app supports both Kindle desktop apps, chosen with the radio buttons at the top:

| Mode                                 | Use it for                                                      |
| ------------------------------------ | ---------------------------------------------------------------- |
| **New Kindle app (Microsoft Store)** | The current Kindle app from the Microsoft Store *(default)*      |
| **Classic Kindle for PC**            | The older desktop app (Kindle for PC 1.x)                        |

---

## New Kindle app (Microsoft Store)

This mode reads your library directly, so it knows the real title of every book and presses each
book's own **Download** button. There is no guessing and no blind keystrokes.

### Prerequisites

- The Kindle app is running and showing your **Library** (not a book, not the store).
- **List view** is recommended.
- For a long run, sort by **Title** or **Author** rather than Recent. Under the Recent sort the
  library re-orders itself as books finish downloading, which can shuffle rows between passes.

### Quick start

1. Launch **Kindle Downloader**. It opens on the control dialog.
2. Leave the mode on **New Kindle app (Microsoft Store)**.
3. Set the **Delay** (1–60 seconds, default 3) — the pause after starting one download before
   moving to the next. Raise it on a slow connection.
4. Optionally tick **Stop when this title is reached** and type a title (see below).
5. Click **Start**.

The app brings the Kindle window to the front and begins. For each book on screen it presses
Download, waits the delay, and moves on; when it runs out of visible rows it scrolls down a page
and continues until the library ends, you press Stop, or it reaches your stop title.

The on-screen log shows each title as it goes, and the counters mean:

| Counter         | Meaning                                                                              |
| --------------- | ------------------------------------------------------------------------------------ |
| **Downloaded**  | Confirmed finished — the book's Download button is gone.                              |
| **In progress** | Pressed and downloading, but not finished yet. Kindle completes these in the background. |
| **Already had** | Rows that were already downloaded before this run.                                    |
| **Skipped**     | Genuinely could not be pressed after several retries. Should normally be 0.           |

"In progress" is normal and not a problem. The app waits out your **Delay** watching for each
download to finish; anything slower than that is left to Kindle, which finishes it whether or not
the row is still on screen. Raise the Delay if you would rather see more of them confirmed.

### The log file

Every run writes a detailed log to:

```
%LOCALAPPDATA%\KindleDownloader\logs\run-<date>-<time>.log
```

The full path is shown as the first line of the on-screen log when a run starts. It records every
title, the exact button coordinates, what was actually under the cursor when each press was made,
every scroll with the distance moved, and the reason for anything skipped. It is flushed after every
line, so it survives even if the app is killed mid-run. This is the thing to look at if a run does
not do what you expect — and the right thing to attach to a bug report.

### Stopping at a particular book

Tick the check box and enter a title, for example:

```
The Unwritten Book: An Investigation by Samantha Hunt
```

Matching ignores case and extra spaces. You can type just the title, or the whole
"title by author" phrase — the app lists authors surname-first ("Hunt, Samantha"), so it compares
the title part separately rather than demanding an exact match. A distinctive fragment
(`The Unwritten Book`) works just as well.

When the run reaches that book it **stops without downloading it** — the title marks where to stop,
not the last book to fetch.

### While it runs

The Kindle window has to stay in front, because the only way to press a button in the new app is a
real mouse click. If you switch to another application the run does not click blindly — it pauses
itself, logs *"Waiting for the Kindle window to become active…"*, and picks up again when Kindle is
back in front. Every press is also hit-tested first, so a covered or moved button is skipped and
reported rather than clicked.

Your mouse pointer is put back where you left it after each press.

---

## Classic Kindle for PC

The original behaviour, unchanged.

### Prerequisites

- **Kindle for PC** open in **List View**
- Sorted by **Recent**, so downloading a book moves it to the top and shifts the list down
- Scrolled to the **bottom** of the library

### Quick start

1. Select **Classic Kindle for PC (keystrokes)**.
2. Set the **Delay**.
3. Click **Start** — the status becomes "Waiting for Kindle…".
4. Click the Kindle for PC window and select a book (single-click to highlight; do not open it).
5. Automation begins: **Enter → wait Delay → Up Arrow → repeat**, up to 800 books per run.

Stopping at a title is not available in this mode — the classic app does not expose its titles, so
the check box is greyed out.

---

## Controls

| Control                            | What it does                                                                       |
| ---------------------------------- | ----------------------------------------------------------------------------------- |
| **Kindle app**                     | Chooses the automation engine. Locked while a run is active.                        |
| **Delay (seconds)**                | Pause between starting one download and moving on. 1–60, default 3.                 |
| **Stop when this title is reached**| New-app mode only. Stops the run when it reaches the named book.                    |
| **Start**                          | Begins a run (enabled when Idle or Stopped).                                        |
| **Pause / Resume**                 | Freezes the run without losing your place; press again to continue.                 |
| **Stop**                           | Ends the run at the next safe point. You can then start a fresh run.                |
| **Exit**                           | Stops any run and closes the app.                                                   |
| **F9** (global hotkey)             | Same as Stop — works even when the Kindle window has focus.                         |

## Status labels

| Status                | Meaning                                                            |
| --------------------- | ------------------------------------------------------------------ |
| **Idle**              | Just launched; no run started.                                      |
| **Waiting for Kindle…**| Waiting for the Kindle window to come to the front.                |
| **Running**           | Actively downloading.                                               |
| **Paused**            | Frozen; nothing is being sent.                                      |
| **Stopped**           | Run ended (Stop, F9, stop title reached, or end of library).        |

---

## Troubleshooting

**"Cannot start: The Kindle app is not running."**
Start the Kindle app first. The downloader looks for a window belonging to `Kindle.exe`.

**"Cannot start: The library list was not found."**
The Kindle app is showing something other than your Library — a book, or the store. Go back to the
Library and press Start again.

**Rows skipped as "the Download button is covered by another window"**
Something really is overlapping the Kindle window (a notification, another app). Each book is
retried a few times first, so a brief popup will not cost you a book. Move the obstruction and let
the run continue.

**It says "Reached the end of the library" far too early**
It should not — the run only concludes this after five consecutive scrolls that move nothing, with a
pause between each. If you still see it on a large library, check the `scroll:` lines in the log
file: they record how far each scroll moved and whether the wheel or the scroll pattern was used.

**The run keeps saying it is waiting for Kindle**
Another application keeps taking focus. This is the safety guard working as intended — nothing is
clicked while Kindle is not in front. Leave Kindle in the foreground for the duration of the run.

**A book takes longer to download than the delay allows**
Downloads continue in the background, so this is usually harmless — the app starts the next one
without waiting for the previous to finish. If your connection is struggling, raise the Delay.

**Some books never download**
A small number of titles cannot be downloaded through the desktop app at all. Download those from
the Amazon website directly.

**Classic mode: race conditions (list jumps, or a book opens)**
Kindle for PC sometimes re-sorts at the same moment a keystroke arrives. Press Stop (or F9), scroll
back to the bottom, select a book, and start again. This class of problem is why the new-app mode
presses named buttons instead of sending keystrokes.
