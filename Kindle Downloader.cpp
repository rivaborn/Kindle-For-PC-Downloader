// Kindle Downloader.cpp
// Win32 app that automates Kindle for PC library downloads.
// Shows a single control dialog; automation runs on a worker thread.

#include "framework.h"
#include "Kindle Downloader.h"
#include <commctrl.h>
#include <thread>
#include <atomic>

#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define WM_APP_SET_STATE    (WM_APP + 1)   // worker -> UI: WPARAM = new AppState
#define MAX_BOOKS           800            // max iterations per run
#define KINDLE_STABLE_POLLS 3             // polls foreground != dialog before starting

// ---------------------------------------------------------------------------
// State machine
// ---------------------------------------------------------------------------
enum AppState { Idle, WaitingForKindle, Running, Paused, Stopped };

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HINSTANCE            g_hInst       = NULL;
static HWND                 g_hDlg        = NULL;
static AppState             g_state       = Idle;

// Worker thread + sync primitives
static std::thread          g_workerThread;
static HANDLE               g_hStopEvent  = NULL;  // manual-reset; SET = stop
static HANDLE               g_hPauseEvent = NULL;  // manual-reset; SET = running, RESET = paused

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
// Entry point — just show the dialog; no main window needed
// ---------------------------------------------------------------------------
int APIENTRY wWinMain(_In_     HINSTANCE hInstance,
                      _In_opt_ HINSTANCE /*hPrevInstance*/,
                      _In_     LPWSTR    /*lpCmdLine*/,
                      _In_     int       /*nCmdShow*/)
{
    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_UPDOWN_CLASS };
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

    return 0;
}

// ---------------------------------------------------------------------------
// Key sending helper
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

        // PauseEvent RESET means paused — block until resume or stop.
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

// ---------------------------------------------------------------------------
// WaitForKindle
// Polls until a window other than hDlg becomes the foreground for
// KINDLE_STABLE_POLLS consecutive checks (≈ 600 ms).
// Returns true  if stop was requested.
// Returns false if a non-dialog foreground was detected (Kindle ready).
// ---------------------------------------------------------------------------
static bool WaitForKindle(HWND hDlg)
{
    int stable = 0;
    while (WaitForSingleObject(g_hStopEvent, 0) != WAIT_OBJECT_0)
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

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
static void WorkerProc(HWND hDlg, int delaySec)
{
    // Phase 1: wait for the user to click into Kindle.
    if (WaitForKindle(hDlg))
    {
        PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Stopped, 0);
        return;
    }

    PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Running, 0);

    for (int i = 0; i < MAX_BOOKS; i++)
    {
        // Honour pause / stop before each iteration.
        if (WaitInterruptible(0)) break;

        SendKey(VK_RETURN);                                          // trigger download

        if (WaitInterruptible((DWORD)delaySec * 1000u)) break;      // configured delay

        SendKey(VK_UP);                                              // move to next book

        if (WaitInterruptible(100u)) break;                          // brief debounce
    }

    PostMessage(hDlg, WM_APP_SET_STATE, (WPARAM)Stopped, 0);
}

// ---------------------------------------------------------------------------
// Run control helpers
// ---------------------------------------------------------------------------
static void StartRun(HWND hDlg, int delaySec)
{
    g_hStopEvent  = CreateEvent(NULL, TRUE, FALSE, NULL);  // not signalled
    g_hPauseEvent = CreateEvent(NULL, TRUE, TRUE,  NULL);  // signalled = running
    g_workerThread = std::thread(WorkerProc, hDlg, delaySec);
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

// ---------------------------------------------------------------------------
// UI state update  (always called on UI thread)
// ---------------------------------------------------------------------------
static void UpdateUiState(HWND hDlg, AppState state)
{
    g_state = state;

    const wchar_t* status;
    switch (state)
    {
    case WaitingForKindle: status = L"Status: Waiting for Kindle\u2026"; break;
    case Running:          status = L"Status: Running";                   break;
    case Paused:           status = L"Status: Paused";                    break;
    case Stopped:          status = L"Status: Stopped";                   break;
    default:               status = L"Status: Idle";                      break;
    }
    SetDlgItemText(hDlg, IDC_STATUS_LABEL, status);

    bool canSelect = (state == Idle || state == Stopped);
    bool isActive  = (state == Running || state == Paused);
    bool canStop   = (isActive || state == WaitingForKindle);

    EnableWindow(GetDlgItem(hDlg, IDC_SELECT),     canSelect ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_PAUSE),      isActive  ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_STOP),       canStop   ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_DELAY_EDIT), canSelect ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_DELAY_SPIN), canSelect ? TRUE : FALSE);

    SetDlgItemText(hDlg, IDC_PAUSE,
                   state == Paused ? L"Resume" : L"Pause");
}

// ---------------------------------------------------------------------------
// Dialog procedure
// ---------------------------------------------------------------------------
INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    switch (message)
    {
    case WM_INITDIALOG:
        g_hDlg = hDlg;
        // Set spin range and default position (buddy edit is set via UDM_SETBUDDY).
        {
            HWND hSpin = GetDlgItem(hDlg, IDC_DELAY_SPIN);
            HWND hEdit = GetDlgItem(hDlg, IDC_DELAY_EDIT);
            SendMessage(hSpin, UDM_SETBUDDY,   (WPARAM)hEdit, 0);
            SendMessage(hSpin, UDM_SETRANGE32, (WPARAM)1,     (LPARAM)60);
            SendMessage(hSpin, UDM_SETPOS32,   0,             (LPARAM)3);
        }
        UpdateUiState(hDlg, Idle);
        return TRUE;

    case WM_APP_SET_STATE:
    {
        AppState newState = (AppState)wParam;
        if (newState == Stopped) JoinWorker();
        UpdateUiState(hDlg, newState);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SELECT:
        {
            BOOL ok = FALSE;
            int delay = (int)GetDlgItemInt(hDlg, IDC_DELAY_EDIT, &ok, FALSE);
            if (!ok || delay < 1) delay = 3;
            if (delay > 60)       delay = 60;
            UpdateUiState(hDlg, WaitingForKindle);
            StartRun(hDlg, delay);
            break;
        }
        case IDC_PAUSE:
            if (g_state == Running)
            {
                SetPaused(true);
                UpdateUiState(hDlg, Paused);
            }
            else if (g_state == Paused)
            {
                SetPaused(false);
                UpdateUiState(hDlg, Running);
            }
            break;

        case IDC_STOP:
            RequestStop();
            // Worker will PostMessage(WM_APP_SET_STATE, Stopped) when done.
            break;

        case IDC_EXIT_BTN:
        case IDCANCEL:
            RequestStop();
            JoinWorker();
            EndDialog(hDlg, 0);
            break;
        }
        return TRUE;

    case WM_CLOSE:
        RequestStop();
        JoinWorker();
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
