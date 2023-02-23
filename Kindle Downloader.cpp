// Kindle Downloader.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "Kindle Downloader.h"
#include <thread>

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;                                // current instance
WCHAR szTitle[MAX_LOADSTRING];                  // The title bar text
WCHAR szWindowClass[MAX_LOADSTRING];            // the main window class name
HANDLE hExit = NULL;

// Forward declarations of functions included in this code module:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    KindleWindow(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
void executeHook();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // TODO: Place code here.

    // Initialize global strings
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_KINDLEDOWNLOADER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Perform application initialization:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_KINDLEDOWNLOADER));

    MSG msg;

    // Main message loop:
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return (int) msg.wParam;
}



//
//  FUNCTION: MyRegisterClass()
//
//  PURPOSE: Registers the window class.
//
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_KINDLEDOWNLOADER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = MAKEINTRESOURCEW(IDC_KINDLEDOWNLOADER);
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

//
//   FUNCTION: InitInstance(HINSTANCE, int)
//
//   PURPOSE: Saves instance handle and creates main window
//
//   COMMENTS:
//
//        In this function, we save the instance handle in a global variable and
//        create and display the main program window.
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance; // Store instance handle in our global variable

   HWND hWnd = CreateWindowW(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd)
   {
      return FALSE;
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}

//
//  FUNCTION: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  PURPOSE: Processes messages for the main window.
//
//  WM_COMMAND  - process the application menu
//  WM_PAINT    - Paint the main window
//  WM_DESTROY  - post a quit message and return
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
        {
            int wmId = LOWORD(wParam);
            // Parse the menu selections:
            switch (wmId)
            {
            case ID_FILE_KINDLEDOWNLOADER:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_ABOUT:
                DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
                break;
            case IDM_EXIT:
                DestroyWindow(hWnd);
                break;
            default:
                return DefWindowProc(hWnd, message, wParam, lParam);
            }
        }
        break;
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            // TODO: Add any drawing code that uses hdc here...
            EndPaint(hWnd, &ps);
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    int X_LOC, Y_LOC;
    X_LOC = 0; Y_LOC = 0;
    POINT p;
    INPUT Inputs[3] = { 0 };
    int xcount = 0, Offset = 300;

    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            if (hExit) SetEvent(hExit);
            else
            {
                EndDialog(hDlg, LOWORD(wParam));
                return (INT_PTR)TRUE;
            }
        }

        if (LOWORD(wParam) == IDC_TargLoc)
        {
            hExit = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (!hExit) break;

            std::thread HookProc(executeHook);

            Sleep(3000);
            GetCursorPos(&p);

            for (xcount = 0; xcount < 1000; xcount++)
            {

                Inputs[0].type = INPUT_MOUSE;
                Inputs[0].mi.dx = p.x;
                Inputs[0].mi.dy = p.y;
                Inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE;

                Inputs[1].type = INPUT_MOUSE;
                Inputs[1].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;

                Inputs[2].type = INPUT_MOUSE;
                Inputs[2].mi.dwFlags = MOUSEEVENTF_RIGHTUP;

                SendInput(3, Inputs, sizeof(INPUT));


                if (WaitForSingleObject(hExit, 0) == WAIT_OBJECT_0) break;
                Sleep(1000);
                if (WaitForSingleObject(hExit, 0) == WAIT_OBJECT_0) break;

                SetCursorPos(p.x + 50, p.y + 100);

                Inputs[0].type = INPUT_MOUSE;
                Inputs[0].mi.dx = p.x + 50;
                Inputs[0].mi.dy = p.y + 100;
                Inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE;


                Inputs[1].type = INPUT_MOUSE;
                Inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;

                Inputs[2].type = INPUT_MOUSE;
                Inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;

                SendInput(3, Inputs, sizeof(INPUT));


                if (WaitForSingleObject(hExit, 0) == WAIT_OBJECT_0) break;
                Sleep(3000);
                if (WaitForSingleObject(hExit, 0) == WAIT_OBJECT_0) break;

                SetCursorPos(p.x, p.y);
            }
            
            HookProc.join();
            CloseHandle(hExit);

            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }

        break;
    }
    return (INT_PTR)FALSE;
}



LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        switch (wParam)
        {
        case WM_KEYDOWN:
        case WM_KEYUP:
            if (reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam)->vkCode == VK_F9)
                SetEvent(hExit);
            break;
        }
    }
    return CallNextHookEx(0, nCode, wParam, lParam);
}


void executeHook()
{
    PostThreadMessage(GetCurrentThreadId(), WM_NULL, 0, 0);

    HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, &KeyboardProc, NULL, 0);
    if (hook)
    {
        MSG msg;

        do
        {
            if (MsgWaitForMultipleObjects(1, &hExit, FALSE, INFINITE, QS_ALLINPUT) != (WAIT_OBJECT_0 + 1))
                break;

            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        } while (true);

        UnhookWindowsHookEx(hook);
    }

    SetEvent(hExit);
}