// Kindle Downloader.cpp
// Win32 app that automates bulk downloads of a Kindle library.
//
// Two engines, chosen by the radio buttons on the dialog:
//
//   New Kindle app (Microsoft Store)
//       Drives the app through UI Automation. It reads the real title of every
//       row, presses that row's own Download button, confirms the download
//       actually started, and can stop when it reaches a title you name.
//       See KindleUia.h/.cpp.
//
//   Classic Kindle for PC
//       The original behaviour: send Enter to download the selected book and
//       Up to move to the one above, relying on the list being sorted by
//       Recent. Kept for people still on the old desktop app.
//
// Automation always runs on a worker thread; the dialog stays responsive.
// Every run also writes a detailed log file - see FileLog below.

#include "framework.h"
#include "Kindle Downloader.h"
#include "KindleUia.h"

#include <commctrl.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>

#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define WM_APP_SET_STATE    (WM_APP + 1)   // worker -> UI: WPARAM = new AppState
#define WM_APP_LOG          (WM_APP + 2)   // worker -> UI: LPARAM = new std::wstring*
#define WM_APP_PROGRESS     (WM_APP + 3)   // worker -> UI: re-read the shared counters

#define MAX_BOOKS           800            // classic mode: max keystroke iterations
#define KINDLE_STABLE_POLLS 3              // classic mode: polls before starting
#define MAX_LOG_LINES       500

// A single scroll that does not move the list means very little - the React
// Native list is often just busy. Only believe the library has ended after this
// many consecutive failures, each with a pause in between.
#define END_OF_LIBRARY_TRIES 5
#define SCROLL_RETRY_WAIT_MS 1500

// How often to re-check whether a download has finished.
#define DOWNLOAD_POLL_MS     250
// How many times a transient click failure (row moved, button covered) may be
// retried before the book is reported as skipped.
#define CLICK_RETRY_LIMIT    3

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum AppState { Idle, WaitingForKindle, Running, Paused, Stopped };

struct RunConfig
{
    bool         useNewApp   = true;
    int          delaySec    = 3;
    bool         stopAtTitle = false;
    std::wstring stopTitle;
};

// ---------------------------------------------------------------------------
// Run log file
//
// Long runs are the norm here - a 30,000 book library is many hours - so the
// on-screen list is only the headline. Everything, including per-click
// coordinates, hit-test results and scroll measurements, goes to a file.
// ---------------------------------------------------------------------------
class FileLog
{
public:
    // Creates %LOCALAPPDATA%\KindleDownloader\logs\run-<date>-<time>.log
    bool Open(std::wstring* pathOut)
    {
        Close();

        wchar_t base[MAX_PATH] = {};
        if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH)) return false;

        std::wstring dir = base;
        dir += L"\\KindleDownloader";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t leaf[64];
        swprintf_s(leaf, L"\\run-%04u%02u%02u-%02u%02u%02u.log",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        m_path = dir + leaf;
        m_handle = CreateFileW(m_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) return false;

        // UTF-8 BOM so Notepad and friends read it correctly.
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        WriteFile(m_handle, bom, 3, &written, nullptr);

        if (pathOut) *pathOut = m_path;
        return true;
    }

    void Line(const std::wstring& text)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_handle == INVALID_HANDLE_VALUE) return;

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t stamp[32];
        swprintf_s(stamp, L"%02u:%02u:%02u.%03u  ",
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::wstring line = stamp + text + L"\r\n";

        const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                              nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) return;

        std::string utf8((size_t)bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                            &utf8[0], bytes, nullptr, nullptr);

        DWORD written = 0;
        WriteFile(m_handle, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        FlushFileBuffers(m_handle);   // a long run may be killed; never lose the tail
    }

    void Close()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    const std::wstring& Path() const { return m_path; }

private:
    HANDLE       m_handle = INVALID_HANDLE_VALUE;
    std::wstring m_path;
    std::mutex   m_mutex;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE            g_hInst  = NULL;
static HWND                 g_hDlg   = NULL;
static AppState             g_state  = Idle;
static FileLog              g_log;

// Worker thread + sync primitives
static std::thread          g_workerThread;
static HANDLE               g_hStopEvent  = NULL;  // manual-reset; SET = stop
static HANDLE               g_hPauseEvent = NULL;  // manual-reset; SET = running, RESET = paused

// Progress shared with the UI thread.
static std::mutex           g_progressMutex;
static int                  g_confirmed  = 0;   // download verified to have finished
static int                  g_started    = 0;   // press delivered (includes confirmed)
static int                  g_alreadyHad = 0;
static int                  g_skipped    = 0;   // genuine failures
static std::wstring         g_currentBook;

// F9 keyboard hook thread
static std::atomic<DWORD>   g_hookThreadId{ 0 };
static HANDLE               g_hookReadyEvent = NULL;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void         RequestStop();
static void         JoinWorker();
static void         UpdateUiState(HWND hDlg, AppState state);
INT_PTR CALLBACK    MainDlgProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    KeyboardProc(int, WPARAM, LPARAM);
static void         HookThreadProc();

// ---------------------------------------------------------------------------
// Worker -> UI helpers
// ---------------------------------------------------------------------------

// Shown on screen *and* written to the log file.
static void LogUi(HWND hDlg, const std::wstring& text)
{
    g_log.Line(text);
    PostMessage(hDlg, WM_APP_LOG, 0, (LPARAM)new std::wstring(text));
}

// Detail that would drown the on-screen list; log file only.
static void LogDetail(const std::wstring& text)
{
    g_log.Line(L"    " + text);
}

static void SetCurrentBook(HWND hDlg, const std::wstring& title)
{
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        g_currentBook = title;
    }
    PostMessage(hDlg, WM_APP_PROGRESS, 0, 0);
}

static void ResetProgress()
{
    std::lock_guard<std::mutex> lock(g_progressMutex);
    g_confirmed  = 0;
    g_started    = 0;
    g_alreadyHad = 0;
    g_skipped    = 0;
    g_currentBook.clear();
}

static std::wstring RectText(const RECT& r)
{
    return L"(" + std::to_wstring(r.left)  + L"," + std::to_wstring(r.top) + L")-("
                + std::to_wstring(r.right) + L"," + std::to_wstring(r.bottom) + L")";
}

// ---------------------------------------------------------------------------
// Entry point - just show the dialog; no main window needed
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(_In_     HINSTANCE hInstance,
                      _In_opt_ HINSTANCE /*hPrevInstance*/,
                      _In_     LPWSTR    /*lpCmdLine*/,
                      _In_     int       /*nCmdShow*/)
{
    // UI Automation reports physical pixels. Without this the rectangles we
    // click would be scaled and every press would land in the wrong place.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    // Start the F9 keyboard hook on its own thread (needs a message pump).
    g_hookReadyEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    std::thread hookThread(HookThreadProc);
    WaitForSingleObject(g_hookReadyEvent, 2000);
    CloseHandle(g_hookReadyEvent);
    g_hookReadyEvent = NULL;

    // Run the app as a modal dialog.
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAINDLG), NULL, MainDlgProc);

    // Shut down the hook thread cleanly.
    DWORD tid = g_hookThreadId.load();
    if (tid) PostThreadMessage(tid, WM_QUIT, 0, 0);
    hookThread.join();

    g_log.Close();
    return 0;
}

// ---------------------------------------------------------------------------
// Key sending helper (classic mode)
// ---------------------------------------------------------------------------
static void SendKey(WORD vk)
{
    INPUT inp[2] = {};
    inp[0].type       = INPUT_KEYBOARD;
    inp[0].ki.wVk     = vk;
    inp[1].type       = INPUT_KEYBOARD;
    inp[1].ki.wVk     = vk;
    inp[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inp, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// WaitInterruptible
// Waits up to `ms` milliseconds.  Blocks while paused (timer frozen).
// Returns true  if the stop event fired (caller should exit).
// Returns false if the time elapsed normally.
// ---------------------------------------------------------------------------
static bool WaitInterruptible(DWORD ms)
{
    HANDLE h[2] = { g_hStopEvent, g_hPauseEvent };
    DWORD remaining = ms;

    while (true)
    {
        // Check stop first.
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0)
            return true;

        // PauseEvent RESET means paused - block until resume or stop.
        if (WaitForSingleObject(g_hPauseEvent, 0) == WAIT_TIMEOUT)
        {
            DWORD r = WaitForMultipleObjects(2, h, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) return true;  // stop
            continue;                              // resumed; re-check
        }

        if (remaining == 0) return false;

        DWORD slice = (remaining < 50u) ? remaining : 50u;
        ULONGLONG t0 = GetTickCount64();
        Sleep(slice);
        ULONGLONG elapsed = GetTickCount64() - t0;
        remaining = (elapsed >= remaining) ? 0u : remaining - (DWORD)elapsed;
    }
}

static bool StopRequested()
{
    return WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0;
}

// ===========================================================================
// Engine 1 - the new Kindle app, driven through UI Automation
// ===========================================================================

// Bring Kindle forward, waiting (interruptibly) if the user is busy elsewhere.
// Returns false if stop was requested while waiting.
static bool WaitUntilKindleActive(HWND hDlg, kuia::Session& session, bool& announced)
{
    while (!StopRequested())
    {
        if (session.EnsureForeground())
        {
            if (announced)
            {
                LogUi(hDlg, L"Kindle is active again - continuing.");
                announced = false;
                PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Running, 0);
            }
            return true;
        }

        if (!announced)
        {
            LogUi(hDlg, L"Waiting for the Kindle window to become active...");
            announced = true;
            PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)WaitingForKindle, 0);
        }
        if (WaitInterruptible(500)) return false;
    }
    return false;
}

// Is this Download button wholly inside the scroll viewport?
//
// A button that is only half on screen is clipped and cannot be pressed - but
// that is not a failure, it just has not come round yet. Keeping this test the
// same as the one ClickDownload applies means we never report a book as
// "skipped" when all that happened is that it was still half off the screen.
static bool ButtonFullyVisible(const RECT& button, const RECT& viewport)
{
    const LONG margin = 4;
    return button.left   >= viewport.left   + margin
        && button.top    >= viewport.top    + margin
        && button.right  <= viewport.right  - margin
        && button.bottom <= viewport.bottom - margin;
}

// Wait for a book's Download button to disappear, which is how this app says
// the download finished.
//   1  = confirmed finished
//   0  = still downloading when the budget ran out
//  -1  = stop requested
static int WaitForDownloadToClear(kuia::Session& session, const std::wstring& asin, DWORD budgetMs)
{
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < budgetMs)
    {
        if (!session.HasDownloadButton(asin)) return 1;
        if (WaitInterruptible(DOWNLOAD_POLL_MS)) return -1;
    }
    return session.HasDownloadButton(asin) ? 0 : 1;
}

// Books whose download had not finished when we moved on usually finish a few
// seconds later. Sweep them without blocking, purely so the counters catch up.
//
// We deliberately do NOT wait for them: Kindle completes downloads in the
// background whether or not their row is still on screen (verified - books
// pressed in one pass were confirmed complete several passes later), so
// blocking here would cost minutes per screen on a large library.
static void SweepPending(HWND hDlg, kuia::Session& session,
                         std::unordered_set<std::wstring>& pending)
{
    if (pending.empty()) return;

    size_t cleared = 0;
    for (auto it = pending.begin(); it != pending.end(); )
    {
        if (!session.HasDownloadButton(*it))
        {
            {
                std::lock_guard<std::mutex> lock(g_progressMutex);
                g_confirmed++;
            }
            cleared++;
            it = pending.erase(it);
        }
        else ++it;
    }

    if (cleared)
    {
        LogDetail(std::to_wstring(cleared) + L" download(s) finished; "
                  + std::to_wstring(pending.size()) + L" still running");
        PostMessage(hDlg, WM_APP_PROGRESS, 0, 0);
    }
}

static void WorkerProcNewApp(HWND hDlg, RunConfig cfg)
{
    kuia::Session session;
    std::wstring err;

    g_log.Line(L"=== Run started: new Kindle app (UI Automation) ===");
    g_log.Line(L"delay=" + std::to_wstring(cfg.delaySec) + L"s  stop-at-title="
               + (cfg.stopAtTitle ? (L"\"" + cfg.stopTitle + L"\"") : std::wstring(L"(off)")));

    if (!session.Initialize(&err) || !session.Attach(&err))
    {
        LogUi(hDlg, L"Cannot start: " + err);
        PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Stopped, 0);
        return;
    }

    LogUi(hDlg, L"Attached to the Kindle app.");
    if (cfg.stopAtTitle)
        LogUi(hDlg, L"Will stop when it reaches: " + cfg.stopTitle);

    PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Running, 0);

    // Never press the same book's Download button twice - a second press
    // cancels the transfer that the first one started.
    std::unordered_set<std::wstring>        attempted;
    std::unordered_set<std::wstring>        pending;   // pressed, not yet confirmed done
    std::unordered_set<std::wstring>        counted;   // rows already tallied as "already had"
    std::unordered_map<std::wstring, int>   retries;   // transient click failures per book
    bool waitingAnnounced = false;
    int  noScrollStreak   = 0;
    bool finished         = false;
    int  screen           = 0;

    while (!StopRequested() && !finished)
    {
        if (WaitInterruptible(0)) break;          // honours pause

        if (!session.IsAlive())
        {
            LogUi(hDlg, L"The Kindle window closed - stopping.");
            break;
        }

        if (!WaitUntilKindleActive(hDlg, session, waitingAnnounced)) break;

        std::vector<kuia::BookItem> items;
        if (!session.SnapshotItems(&items, &err))
        {
            LogUi(hDlg, L"Could not read the library list: " + err);
            break;
        }

        RECT viewport{};
        const bool haveViewport = session.GetViewport(&viewport);

        // Choose the next book, scanning the visible rows from the top down.
        //
        // We deliberately re-read the list before every press instead of
        // working through a whole screenful from one reading: this library
        // re-sorts itself as downloads complete, so a rectangle more than a few
        // seconds old can point at a different book entirely.
        const kuia::BookItem* target = nullptr;
        int deferred = 0;

        for (const kuia::BookItem& item : items)
        {
            if (haveViewport)
            {
                const LONG centreY = (item.itemRect.top + item.itemRect.bottom) / 2;
                if (centreY < viewport.top || centreY > viewport.bottom) continue;
            }

            if (cfg.stopAtTitle && kuia::MatchesStopTitle(item, cfg.stopTitle))
            {
                LogUi(hDlg, L"Reached the stop title: " + item.title);
                LogUi(hDlg, L"Stopping without downloading it.");
                finished = true;
                break;
            }

            if (!item.needsDownload)
            {
                if (counted.insert(item.title).second)
                {
                    std::lock_guard<std::mutex> lock(g_progressMutex);
                    g_alreadyHad++;
                }
                continue;
            }

            if (item.asin.empty() || attempted.count(item.asin)) continue;

            // Half-visible rows wait for the next screen, which overlaps this
            // one by a couple of rows. That is not a failure, so it is not a
            // "skip" - it was reporting these as skipped that made the counter
            // look alarming.
            if (haveViewport && !ButtonFullyVisible(item.downloadRect, viewport))
            {
                deferred++;
                continue;
            }

            target = &item;
            break;
        }

        if (finished || StopRequested()) break;

        if (target)
        {
            const std::wstring asin  = target->asin;
            const std::wstring title = target->title;

            attempted.insert(asin);
            SetCurrentBook(hDlg, title);

            if (!WaitUntilKindleActive(hDlg, session, waitingAnnounced)) break;

            kuia::ClickInfo ci;
            const kuia::ClickResult r = session.ClickDownload(asin, &ci);

            LogDetail(L"click " + asin + L" \"" + title + L"\" button=" + RectText(ci.buttonRect)
                      + L" point=(" + std::to_wstring(ci.point.x) + L"," + std::to_wstring(ci.point.y)
                      + L") hit=\"" + ci.hitTestId + L"\" -> " + kuia::ClickResultText(r));

            if (r == kuia::ClickResult::Ok)
            {
                {
                    std::lock_guard<std::mutex> lock(g_progressMutex);
                    g_started++;
                }
                retries.erase(asin);

                // The delay doubles as a verification budget: rather than
                // sleeping blind, spend it watching for the Download button to
                // vanish, which is this app's "finished" signal. Books that
                // need longer keep downloading in the background.
                const int done = WaitForDownloadToClear(session, asin,
                                                        (DWORD)cfg.delaySec * 1000u);
                if (done < 0) break;

                if (done == 1)
                {
                    {
                        std::lock_guard<std::mutex> lock(g_progressMutex);
                        g_confirmed++;
                    }
                    LogUi(hDlg, L"Downloaded: " + title);
                }
                else
                {
                    pending.insert(asin);
                    LogUi(hDlg, L"Downloading: " + title);
                }
            }
            else if (r == kuia::ClickResult::Gone)
            {
                if (counted.insert(title).second)
                {
                    std::lock_guard<std::mutex> lock(g_progressMutex);
                    g_alreadyHad++;
                }
            }
            else
            {
                // Transient: the row moved, or something covered it. Allow a
                // few retries before giving up and calling it skipped, but cap
                // them so a genuinely stuck book cannot spin the loop forever.
                const int n = ++retries[asin];
                if (n <= CLICK_RETRY_LIMIT)
                {
                    attempted.erase(asin);
                    LogDetail(L"will retry \"" + title + L"\" (attempt " + std::to_wstring(n)
                              + L" of " + std::to_wstring(CLICK_RETRY_LIMIT) + L")");
                }
                else
                {
                    {
                        std::lock_guard<std::mutex> lock(g_progressMutex);
                        g_skipped++;
                    }
                    LogUi(hDlg, L"Skipped '" + title + L"' - " + kuia::ClickResultText(r));
                }
            }

            PostMessage(hDlg, WM_APP_PROGRESS, 0, 0);
            SweepPending(hDlg, session, pending);
            continue;                       // re-read the list for the next book
        }

        // Nothing left to press on this screen - move down.
        SweepPending(hDlg, session, pending);

        screen++;
        LogDetail(L"screen " + std::to_wstring(screen) + L" done: " + std::to_wstring(items.size())
                  + L" rows realised, viewport " + (haveViewport ? RectText(viewport) : L"(unknown)")
                  + (deferred ? (L", " + std::to_wstring(deferred) + L" row(s) wait for the next screen")
                              : std::wstring()));

        // Advance. A single unmoved scroll means nothing - the list is often
        // just busy - so retry before concluding the library has ended.
        kuia::ScrollInfo si;
        const kuia::ScrollResult sr = session.ScrollDown(&si);

        LogDetail(L"scroll: moved=" + std::to_wstring(si.movedPx) + L"px target="
                  + std::to_wstring(si.targetPx) + L"px notches=" + std::to_wstring(si.notchesSent)
                  + L" px/notch=" + std::to_wstring((int)(si.pxPerNotch + 0.5))
                  + L" row=" + std::to_wstring(si.rowHeight)
                  + L" view=" + std::to_wstring(si.viewHeight)
                  + (si.usedPattern ? L" (scroll pattern)" : L" (wheel)")
                  + L" -> " + (sr == kuia::ScrollResult::Moved   ? L"moved"
                             : sr == kuia::ScrollResult::NotMoved ? L"did not move"
                                                                  : L"failed"));

        if (sr == kuia::ScrollResult::Moved)
        {
            noScrollStreak = 0;
            continue;
        }

        noScrollStreak++;
        LogDetail(L"list did not advance (" + std::to_wstring(noScrollStreak) + L" of "
                  + std::to_wstring(END_OF_LIBRARY_TRIES) + L" before assuming end of library)");

        if (noScrollStreak >= END_OF_LIBRARY_TRIES)
        {
            LogUi(hDlg, L"Reached the end of the library.");
            break;
        }

        if (WaitInterruptible(SCROLL_RETRY_WAIT_MS)) break;
    }

    SetCurrentBook(hDlg, L"");
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        LogUi(hDlg, L"Finished. " + std::to_wstring(g_confirmed) + L" downloaded, "
                  + std::to_wstring(g_started - g_confirmed) + L" still finishing, "
                  + std::to_wstring(g_skipped) + L" skipped.");
    }
    g_log.Line(L"=== Run ended ===");
    PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Stopped, 0);
}

// ===========================================================================
// Engine 2 - classic Kindle for PC, driven by keystrokes
// ===========================================================================

// Polls until a window other than hDlg becomes the foreground for
// KINDLE_STABLE_POLLS consecutive checks.
// Returns true if stop was requested, false once a non-dialog foreground is up.
static bool WaitForKindle(HWND hDlg)
{
    int stable = 0;
    while (!StopRequested())
    {
        HWND fg = GetForegroundWindow();
        if (fg && fg != hDlg)
        {
            if (++stable >= KINDLE_STABLE_POLLS) return false;  // detected
        }
        else
        {
            stable = 0;
        }
        Sleep(200);
    }
    return true;  // stopped
}

static void WorkerProcClassic(HWND hDlg, RunConfig cfg)
{
    g_log.Line(L"=== Run started: classic Kindle for PC (keystrokes) ===");
    g_log.Line(L"delay=" + std::to_wstring(cfg.delaySec) + L"s");

    LogUi(hDlg, L"Click the Kindle for PC window and select a book in the list.");

    if (WaitForKindle(hDlg))
    {
        g_log.Line(L"=== Run ended (stopped before starting) ===");
        PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Stopped, 0);
        return;
    }

    LogUi(hDlg, L"Kindle window active - starting.");
    PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Running, 0);

    for (int i = 0; i < MAX_BOOKS; i++)
    {
        // Honour pause / stop before each iteration.
        if (WaitInterruptible(0)) break;

        SendKey(VK_RETURN);                                     // trigger download

        if (WaitInterruptible((DWORD)cfg.delaySec * 1000u)) break;

        SendKey(VK_UP);                                         // move to next book

        {
            std::lock_guard<std::mutex> lock(g_progressMutex);
            g_started++;
        }
        PostMessage(hDlg, WM_APP_PROGRESS, 0, 0);

        if (WaitInterruptible(100u)) break;                      // brief debounce
    }

    g_log.Line(L"=== Run ended ===");
    PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Stopped, 0);
}

// ---------------------------------------------------------------------------
// Run control helpers
// ---------------------------------------------------------------------------
static void StartRun(HWND hDlg, const RunConfig& cfg)
{
    ResetProgress();
    g_hStopEvent  = CreateEvent(NULL, TRUE, FALSE, NULL);  // not signalled
    g_hPauseEvent = CreateEvent(NULL, TRUE, TRUE,  NULL);  // signalled = running
    g_workerThread = std::thread(cfg.useNewApp ? WorkerProcNewApp : WorkerProcClassic,
                                 hDlg, cfg);
}

static void RequestStop()
{
    if (g_hStopEvent)  SetEvent(g_hStopEvent);
    if (g_hPauseEvent) SetEvent(g_hPauseEvent);  // unblock any pause wait
}

static void SetPaused(bool paused)
{
    if (!g_hPauseEvent) return;
    if (paused) ResetEvent(g_hPauseEvent);
    else        SetEvent(g_hPauseEvent);
}

static void JoinWorker()
{
    if (g_workerThread.joinable()) g_workerThread.join();
    if (g_hStopEvent)  { CloseHandle(g_hStopEvent);  g_hStopEvent  = NULL; }
    if (g_hPauseEvent) { CloseHandle(g_hPauseEvent); g_hPauseEvent = NULL; }
}

// Free any log messages the worker posted but the dialog never processed.
static void DrainPendingLogs(HWND hDlg)
{
    MSG msg;
    while (PeekMessage(&msg, hDlg, WM_APP_LOG, WM_APP_LOG, PM_REMOVE))
        delete reinterpret_cast<std::wstring*>(msg.lParam);
}

// ---------------------------------------------------------------------------
// UI helpers  (always called on the UI thread)
// ---------------------------------------------------------------------------
static void AddLogLine(HWND hDlg, const std::wstring& text)
{
    HWND hList = GetDlgItem(hDlg, IDC_LOG);
    if (!hList) return;

    int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    while (count >= MAX_LOG_LINES)
    {
        SendMessage(hList, LB_DELETESTRING, 0, 0);
        count--;
    }
    int index = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)text.c_str());
    if (index >= 0) SendMessage(hList, LB_SETTOPINDEX, index, 0);
}

static void RefreshProgress(HWND hDlg)
{
    int confirmed, started, alreadyHad, skipped;
    std::wstring current;
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        confirmed  = g_confirmed;
        started    = g_started;
        alreadyHad = g_alreadyHad;
        skipped    = g_skipped;
        current    = g_currentBook;
    }

    std::wstring counts = L"Downloaded: " + std::to_wstring(confirmed)
                        + L"    In progress: " + std::to_wstring(started - confirmed)
                        + L"    Already had: " + std::to_wstring(alreadyHad)
                        + L"    Skipped: " + std::to_wstring(skipped);
    SetDlgItemText(hDlg, IDC_COUNTS_LABEL, counts.c_str());

    std::wstring cur = current.empty() ? std::wstring(L"") : (L"Current: " + current);
    SetDlgItemText(hDlg, IDC_CURRENT_LABEL, cur.c_str());
}

static void UpdateUiState(HWND hDlg, AppState state)
{
    g_state = state;

    const wchar_t* status;
    switch (state)
    {
    case WaitingForKindle: status = L"Status: Waiting for Kindle…"; break;
    case Running:          status = L"Status: Running";                  break;
    case Paused:           status = L"Status: Paused";                   break;
    case Stopped:          status = L"Status: Stopped";                  break;
    default:               status = L"Status: Idle";                     break;
    }
    SetDlgItemText(hDlg, IDC_STATUS_LABEL, status);

    const bool canSelect = (state == Idle || state == Stopped);
    const bool isActive  = (state == Running || state == Paused);
    const bool canStop   = (isActive || state == WaitingForKindle);

    EnableWindow(GetDlgItem(hDlg, IDC_SELECT),           canSelect ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_PAUSE),            canStop   ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_STOP),             canStop   ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_DELAY_EDIT),       canSelect ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_DELAY_SPIN),       canSelect ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_MODE_NEW),         canSelect ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_MODE_CLASSIC),     canSelect ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_STOP_TITLE_CHECK), canSelect ? TRUE : FALSE);

    const bool stopTitleOn = IsDlgButtonChecked(hDlg, IDC_STOP_TITLE_CHECK) == BST_CHECKED;
    const bool newMode     = IsDlgButtonChecked(hDlg, IDC_MODE_NEW) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_STOP_TITLE_EDIT),
                 (canSelect && stopTitleOn && newMode) ? TRUE : FALSE);

    SetDlgItemText(hDlg, IDC_PAUSE, state == Paused ? L"Resume" : L"&Pause");
}

// Stopping at a title needs the real titles, which only the new engine reads.
static void SyncStopTitleAvailability(HWND hDlg)
{
    const bool newMode = IsDlgButtonChecked(hDlg, IDC_MODE_NEW) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_STOP_TITLE_CHECK), newMode ? TRUE : FALSE);

    const bool stopTitleOn = IsDlgButtonChecked(hDlg, IDC_STOP_TITLE_CHECK) == BST_CHECKED;
    EnableWindow(GetDlgItem(hDlg, IDC_STOP_TITLE_EDIT),
                 (newMode && stopTitleOn) ? TRUE : FALSE);
}

static std::wstring GetDlgText(HWND hDlg, int id)
{
    HWND h = GetDlgItem(hDlg, id);
    if (!h) return std::wstring();
    int len = GetWindowTextLength(h);
    if (len <= 0) return std::wstring();

    std::wstring s((size_t)len, L'\0');
    GetWindowText(h, &s[0], len + 1);
    return s;
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        g_hDlg = hDlg;
        {
            HWND hSpin = GetDlgItem(hDlg, IDC_DELAY_SPIN);
            HWND hEdit = GetDlgItem(hDlg, IDC_DELAY_EDIT);
            SendMessage(hSpin, UDM_SETBUDDY,   (WPARAM)hEdit, 0);
            SendMessage(hSpin, UDM_SETRANGE32, (WPARAM)1,     (LPARAM)60);
            SendMessage(hSpin, UDM_SETPOS32,   0,             (LPARAM)3);
        }
        CheckRadioButton(hDlg, IDC_MODE_NEW, IDC_MODE_CLASSIC, IDC_MODE_NEW);
        SyncStopTitleAvailability(hDlg);
        UpdateUiState(hDlg, Idle);
        RefreshProgress(hDlg);
        AddLogLine(hDlg, L"Ready. Open the Kindle app at your Library, then press Start.");
        return TRUE;

    case WM_APP_SET_STATE:
    {
        AppState newState = (AppState)wParam;
        if (newState == Stopped) JoinWorker();
        UpdateUiState(hDlg, newState);
        RefreshProgress(hDlg);
        return TRUE;
    }

    case WM_APP_LOG:
    {
        std::wstring* text = reinterpret_cast<std::wstring*>(lParam);
        if (text)
        {
            AddLogLine(hDlg, *text);
            delete text;
        }
        return TRUE;
    }

    case WM_APP_PROGRESS:
        RefreshProgress(hDlg);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_MODE_NEW:
        case IDC_MODE_CLASSIC:
        case IDC_STOP_TITLE_CHECK:
            SyncStopTitleAvailability(hDlg);
            return TRUE;

        case IDC_SELECT:
        {
            RunConfig cfg;
            cfg.useNewApp = IsDlgButtonChecked(hDlg, IDC_MODE_NEW) == BST_CHECKED;

            BOOL ok = FALSE;
            int delay = (int)GetDlgItemInt(hDlg, IDC_DELAY_EDIT, &ok, FALSE);
            if (!ok || delay < 1) delay = 3;
            if (delay > 60)       delay = 60;
            cfg.delaySec = delay;

            cfg.stopAtTitle = cfg.useNewApp
                           && IsDlgButtonChecked(hDlg, IDC_STOP_TITLE_CHECK) == BST_CHECKED;
            if (cfg.stopAtTitle)
            {
                cfg.stopTitle = GetDlgText(hDlg, IDC_STOP_TITLE_EDIT);
                if (cfg.stopTitle.empty())
                {
                    AddLogLine(hDlg, L"Enter the title to stop at, or clear the check box.");
                    SetFocus(GetDlgItem(hDlg, IDC_STOP_TITLE_EDIT));
                    return TRUE;
                }
            }

            SendMessage(GetDlgItem(hDlg, IDC_LOG), LB_RESETCONTENT, 0, 0);

            std::wstring logPath;
            if (g_log.Open(&logPath)) AddLogLine(hDlg, L"Log file: " + logPath);
            else                      AddLogLine(hDlg, L"(Could not create a log file.)");

            UpdateUiState(hDlg, WaitingForKindle);
            RefreshProgress(hDlg);
            StartRun(hDlg, cfg);
            return TRUE;
        }

        case IDC_PAUSE:
            if (g_state == Paused)
            {
                SetPaused(false);
                g_log.Line(L"-- resumed --");
                UpdateUiState(hDlg, Running);
            }
            else
            {
                SetPaused(true);
                g_log.Line(L"-- paused --");
                UpdateUiState(hDlg, Paused);
            }
            return TRUE;

        case IDC_STOP:
            g_log.Line(L"-- stop requested --");
            RequestStop();
            // Worker will PostMessage(WM_APP_SET_STATE, Stopped) when done.
            return TRUE;

        case IDC_EXIT_BTN:
        case IDCANCEL:
            RequestStop();
            JoinWorker();
            DrainPendingLogs(hDlg);
            g_log.Close();
            EndDialog(hDlg, 0);
            return TRUE;
        }
        return FALSE;

    case WM_CLOSE:
        RequestStop();
        JoinWorker();
        DrainPendingLogs(hDlg);
        g_log.Close();
        EndDialog(hDlg, 0);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// F9 keyboard hook (low-level, runs on its own thread)
// ---------------------------------------------------------------------------
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN)
    {
        if (reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam)->vkCode == VK_F9)
            RequestStop();
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

static void HookThreadProc()
{
    g_hookThreadId.store(GetCurrentThreadId());

    // Prime the message queue before signalling ready.
    MSG dummy;
    PeekMessage(&dummy, NULL, 0, 0, PM_NOREMOVE);
    SetEvent(g_hookReadyEvent);

    HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hook) UnhookWindowsHookEx(hook);
}
