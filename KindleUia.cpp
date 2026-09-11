// KindleUia.cpp - see KindleUia.h for the rationale.
//
// Notes on the quirks of the new Kindle app, all verified against
// AMZNKindle.AmazonKindleReadingApp 1.0.23620.0:
//
//  * The Download buttons expose neither InvokePattern nor a working
//    LegacyIAccessible::DoDefaultAction - the latter returns S_OK and does
//    nothing. React Native for Windows publishes the accessibility shell but
//    never wires it to its touch handler, so a synthesized mouse click is the
//    only thing that presses a button.
//  * That click needs a hover first: move the pointer, let React Native see
//    pointer-enter, and only then press. Pressing immediately after the move is
//    silently ignored.
//  * ScrollPattern works, but VerticalScrollPercent is stuck at 0, so list
//    movement has to be detected by comparing which rows are realised. The
//    pattern also intermittently does nothing while the list is busy, hence the
//    mouse-wheel fallback in ScrollDown.

#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <uiautomation.h>

#include "KindleUia.h"

#include <algorithm>
#include <cwctype>

namespace kuia {

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------
namespace {

// React Native needs to process pointer-enter before it accepts a press.
constexpr DWORD kHoverSettleMs   = 150;
// How long the button stays held down.
constexpr DWORD kPressHoldMs     = 90;
// Pause after releasing, before we move the pointer back.
constexpr DWORD kAfterReleaseMs  = 60;
// A button closer than this to the viewport edge is treated as off-screen; it
// may be clipped even though its rectangle looks valid.
constexpr LONG  kViewportMarginPx = 4;
// How long to let the list settle after asking it to scroll.
constexpr DWORD kScrollSettleMs   = 700;
// ElementFromPoint returns nothing while the list is re-rendering, so the
// hit-test is retried rather than believed first time. 12 x 100ms covers the
// pause after a download completes or the list scrolls; measured worst case was
// a little over half a second.
constexpr int   kHitTestAttempts  = 12;
constexpr DWORD kHitTestPollMs    = 100;

const wchar_t* const kWindowClass    = L"Microsoft.UI.Windowing.Window";
const wchar_t* const kFlatListId     = L"library-items-flatlist";
const wchar_t* const kItemId         = L"library-item-container";
const wchar_t* const kDownloadPrefix = L"download-button-";

std::wstring FromBstr(BSTR b)
{
    if (!b) return std::wstring();
    return std::wstring(b, SysStringLen(b));
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix)
{
    size_t n = wcslen(prefix);
    return s.size() >= n && wcsncmp(s.c_str(), prefix, n) == 0;
}

// Move the pointer in absolute virtual-desktop coordinates.
void MovePointer(int x, int y)
{
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 1 || vh <= 1) return;

    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dx      = (LONG)(((double)(x - vx) * 65535.0) / (vw - 1));
    in.mi.dy      = (LONG)(((double)(y - vy) * 65535.0) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof(INPUT));
}

bool PressButton(DWORD flag)
{
    INPUT in{};
    in.type       = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool RectContains(const RECT& outer, const RECT& inner, LONG margin)
{
    return inner.left   >= outer.left   + margin
        && inner.top    >= outer.top    + margin
        && inner.right  <= outer.right  - margin
        && inner.bottom <= outer.bottom - margin;
}

} // namespace

const wchar_t* ClickResultText(ClickResult r)
{
    switch (r)
    {
    case ClickResult::Ok:            return L"ok";
    case ClickResult::NotForeground: return L"Kindle is not the active window";
    case ClickResult::Occluded:      return L"the Download button is covered by another window";
    case ClickResult::OffScreen:     return L"the row is not fully on screen yet";
    case ClickResult::Gone:          return L"the Download button disappeared";
    default:                         return L"the click could not be sent";
    }
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------
Session::Session() = default;

Session::~Session()
{
    if (m_cache)    m_cache->Release();
    if (m_itemCond) m_itemCond->Release();
    if (m_root)     m_root->Release();
    if (m_ua)       m_ua->Release();
    if (m_comInit)  CoUninitialize();
}

bool Session::Initialize(std::wstring* err)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        if (err) *err = L"COM could not be started.";
        return false;
    }
    m_comInit = SUCCEEDED(hr);

    hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(IUIAutomation), (void**)&m_ua);
    if (FAILED(hr) || !m_ua)
    {
        if (err) *err = L"UI Automation is unavailable on this system.";
        return false;
    }

    // Condition reused for every library-row lookup.
    VARIANT v; v.vt = VT_BSTR; v.bstrVal = SysAllocString(kItemId);
    hr = m_ua->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &m_itemCond);
    VariantClear(&v);
    if (FAILED(hr) || !m_itemCond)
    {
        if (err) *err = L"UI Automation could not be initialised.";
        return false;
    }

    // Pull every property we need for a whole page of rows in one cross-process
    // call. Walking ~90 rows property-by-property is far too slow otherwise.
    if (FAILED(m_ua->CreateCacheRequest(&m_cache)) || !m_cache)
    {
        if (err) *err = L"UI Automation could not be initialised.";
        return false;
    }
    m_cache->AddProperty(UIA_NamePropertyId);
    m_cache->AddProperty(UIA_AutomationIdPropertyId);
    m_cache->AddProperty(UIA_BoundingRectanglePropertyId);
    m_cache->AddProperty(UIA_ControlTypePropertyId);
    m_cache->put_TreeScope(TreeScope_Subtree);
    m_cache->put_AutomationElementMode(AutomationElementMode_Full);

    return true;
}

bool Session::Attach(std::wstring* err)
{
    if (m_root) { m_root->Release(); m_root = nullptr; }
    m_hwnd = nullptr;

    // Match the window class, but only accept one that belongs to Kindle.exe -
    // other Windows App SDK apps share this class.
    HWND hw = nullptr;
    while ((hw = FindWindowExW(nullptr, hw, kWindowClass, nullptr)) != nullptr)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hw, &pid);
        if (!pid) continue;

        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hp) continue;

        wchar_t path[MAX_PATH] = {};
        DWORD   len = MAX_PATH;
        bool    isKindle = false;
        if (QueryFullProcessImageNameW(hp, 0, path, &len))
        {
            const wchar_t* slash = wcsrchr(path, L'\\');
            const wchar_t* leaf  = slash ? slash + 1 : path;
            isKindle = (_wcsicmp(leaf, L"Kindle.exe") == 0);
        }
        CloseHandle(hp);

        if (isKindle) { m_hwnd = hw; break; }
    }

    if (!m_hwnd)
    {
        if (err) *err = L"The Kindle app is not running. Start it, open your Library, then try again.";
        return false;
    }

    if (FAILED(m_ua->ElementFromHandle(m_hwnd, &m_root)) || !m_root)
    {
        if (err) *err = L"The Kindle window could not be inspected.";
        return false;
    }

    IUIAutomationElement* flat = FlatList();
    if (!flat)
    {
        if (err) *err = L"The library list was not found. Switch the Kindle app to your Library "
                        L"(not a book or the store) and try again.";
        return false;
    }
    flat->Release();
    return true;
}

bool Session::IsAlive() const
{
    return m_hwnd && IsWindow(m_hwnd);
}

IUIAutomationElement* Session::FindByAutomationId(IUIAutomationElement* root,
                                                  const wchar_t* id,
                                                  int scope) const
{
    if (!root || !m_ua) return nullptr;

    VARIANT v; v.vt = VT_BSTR; v.bstrVal = SysAllocString(id);
    IUIAutomationCondition* cond = nullptr;
    HRESULT hr = m_ua->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &cond);
    VariantClear(&v);
    if (FAILED(hr) || !cond) return nullptr;

    IUIAutomationElement* found = nullptr;
    root->FindFirst((TreeScope)scope, cond, &found);
    cond->Release();
    return found;
}

IUIAutomationElement* Session::FlatList() const
{
    return FindByAutomationId(m_root, kFlatListId, TreeScope_Descendants);
}

bool Session::IsForeground() const
{
    return m_hwnd && GetForegroundWindow() == m_hwnd;
}

bool Session::EnsureForeground()
{
    if (!IsAlive()) return false;
    if (IsForeground()) return true;

    if (IsIconic(m_hwnd))
        ShowWindow(m_hwnd, SW_RESTORE);

    SetForegroundWindow(m_hwnd);

    // SetForegroundWindow is advisory; give the shell a moment then re-check.
    for (int i = 0; i < 10; i++)
    {
        if (IsForeground()) return true;
        Sleep(60);
    }
    return IsForeground();
}

bool Session::GetViewport(RECT* out) const
{
    if (!out) return false;

    IUIAutomationElement* flat = FlatList();
    if (!flat) return false;

    RECT r{};
    HRESULT hr = flat->get_CurrentBoundingRectangle(&r);
    flat->Release();
    if (FAILED(hr)) return false;

    // Clip to the window: the list element reports its full content height,
    // which extends past the bottom of the window.
    RECT win{};
    if (GetWindowRect(m_hwnd, &win))
    {
        r.left   = (std::max)(r.left,   win.left);
        r.top    = (std::max)(r.top,    win.top);
        r.right  = (std::min)(r.right,  win.right);
        r.bottom = (std::min)(r.bottom, win.bottom);
    }
    *out = r;
    return r.right > r.left && r.bottom > r.top;
}

bool Session::HasDownloadButton(const std::wstring& asin) const
{
    std::wstring id = kDownloadPrefix + asin;
    IUIAutomationElement* btn = FindByAutomationId(m_root, id.c_str(), TreeScope_Descendants);
    if (!btn) return false;
    btn->Release();
    return true;
}

bool Session::SnapshotItems(std::vector<BookItem>* out, std::wstring* err)
{
    if (!out) return false;
    out->clear();

    IUIAutomationElement* flat = FlatList();
    if (!flat)
    {
        if (err) *err = L"The library list is no longer visible.";
        return false;
    }

    IUIAutomationElementArray* rows = nullptr;
    HRESULT hr = flat->FindAllBuildCache(TreeScope_Children, m_itemCond, m_cache, &rows);
    flat->Release();
    if (FAILED(hr) || !rows)
    {
        if (err) *err = L"The library list could not be read.";
        return false;
    }

    int count = 0;
    rows->get_Length(&count);

    for (int i = 0; i < count; i++)
    {
        IUIAutomationElement* row = nullptr;
        if (FAILED(rows->GetElement(i, &row)) || !row) continue;

        BookItem item;
        BSTR name = nullptr;
        if (SUCCEEDED(row->get_CachedName(&name)))
        {
            item.fullName = FromBstr(name);
            SysFreeString(name);
        }
        row->get_CachedBoundingRectangle(&item.itemRect);

        // Walk the cached subtree for the title/author text and the Download
        // button. Order within the info container is: title, author, [Download], More.
        int textSeen = 0;
        struct Walker
        {
            static void Visit(IUIAutomationElement* e, BookItem& it, int& textSeen)
            {
                BSTR aid = nullptr;
                std::wstring id;
                if (SUCCEEDED(e->get_CachedAutomationId(&aid)))
                {
                    id = FromBstr(aid);
                    SysFreeString(aid);
                }

                CONTROLTYPEID ct = 0;
                e->get_CachedControlType(&ct);

                if (StartsWith(id, kDownloadPrefix))
                {
                    it.needsDownload = true;
                    it.asin = id.substr(wcslen(kDownloadPrefix));
                    e->get_CachedBoundingRectangle(&it.downloadRect);
                }
                else if (ct == UIA_TextControlTypeId && textSeen < 2)
                {
                    BSTR t = nullptr;
                    if (SUCCEEDED(e->get_CachedName(&t)))
                    {
                        std::wstring s = FromBstr(t);
                        SysFreeString(t);
                        if (!s.empty())
                        {
                            if (textSeen == 0)      it.title  = s;
                            else                    it.author = s;
                            textSeen++;
                        }
                    }
                }

                IUIAutomationElementArray* kids = nullptr;
                if (SUCCEEDED(e->GetCachedChildren(&kids)) && kids)
                {
                    int n = 0;
                    kids->get_Length(&n);
                    for (int k = 0; k < n; k++)
                    {
                        IUIAutomationElement* child = nullptr;
                        if (SUCCEEDED(kids->GetElement(k, &child)) && child)
                        {
                            Visit(child, it, textSeen);
                            child->Release();
                        }
                    }
                    kids->Release();
                }
            }
        };
        Walker::Visit(row, item, textSeen);

        // Fall back to the row's accessible name if the text nodes were empty.
        if (item.title.empty()) item.title = item.fullName;

        out->push_back(std::move(item));
        row->Release();
    }

    rows->Release();

    // Hand back strict visual order. The tree is usually already in that order,
    // but the list parks recycled rows out of sequence, and every caller below
    // reasons about "the next row down the screen".
    std::sort(out->begin(), out->end(),
              [](const BookItem& a, const BookItem& b)
              { return a.itemRect.top < b.itemRect.top; });

    return true;
}

ClickResult Session::ClickDownload(const std::wstring& asin, ClickInfo* info)
{
    if (info) *info = ClickInfo{};

    if (!IsAlive()) return ClickResult::Failed;

    const bool fg = IsForeground();
    if (info) info->wasForeground = fg;
    if (!fg) return ClickResult::NotForeground;

    // Re-resolve the button now: the list may have moved since the snapshot.
    std::wstring id = kDownloadPrefix + asin;
    IUIAutomationElement* btn = FindByAutomationId(m_root, id.c_str(), TreeScope_Descendants);
    if (!btn) return ClickResult::Gone;

    RECT r{};
    HRESULT hr = btn->get_CurrentBoundingRectangle(&r);
    btn->Release();
    if (FAILED(hr) || (r.right - r.left) <= 0 || (r.bottom - r.top) <= 0)
        return ClickResult::Gone;

    if (info) info->buttonRect = r;

    RECT viewport{};
    const bool inside = !GetViewport(&viewport) || RectContains(viewport, r, kViewportMarginPx);
    if (info) info->insideViewport = inside;
    if (!inside) return ClickResult::OffScreen;

    const int cx = (r.left + r.right) / 2;
    const int cy = (r.top + r.bottom) / 2;
    if (info) { info->point.x = cx; info->point.y = cy; }

    POINT saved{};
    const bool haveSaved = GetCursorPos(&saved) != FALSE;

    MovePointer(cx, cy);
    Sleep(kHoverSettleMs);   // React Native must see pointer-enter first

    // Confirm the button really is the topmost thing at that point. This is
    // what stops us clicking into another application if focus moved or a
    // dialog appeared between the snapshot and now.
    //
    // ElementFromPoint has to be given time. While the React Native list is
    // busy - re-rendering after the previous download, or just after a scroll -
    // it returns nothing at all for a point that is perfectly clickable a
    // moment later. Treat "nothing there" as "not ready yet" and keep asking;
    // only a different, real element means something is genuinely on top.
    POINT p{ cx, cy };
    ClickResult guard = ClickResult::Occluded;

    for (int attempt = 0; attempt < kHitTestAttempts; attempt++)
    {
        IUIAutomationElement* atPoint = nullptr;
        if (SUCCEEDED(m_ua->ElementFromPoint(p, &atPoint)) && atPoint)
        {
            BSTR aid = nullptr;
            std::wstring hit;
            if (SUCCEEDED(atPoint->get_CurrentAutomationId(&aid)))
            {
                hit = FromBstr(aid);
                SysFreeString(aid);
            }
            atPoint->Release();

            if (info) info->hitTestId = hit;

            if (hit == id)
            {
                guard = ClickResult::Ok;
                break;
            }
            if (!hit.empty())
            {
                // Something else is really there - no amount of waiting helps.
                guard = ClickResult::Occluded;
                break;
            }
            // Empty id: an anonymous element of the app's own, keep trying.
        }
        else if (info)
        {
            info->hitTestId = L"(nothing yet)";
        }

        Sleep(kHitTestPollMs);
    }

    if (guard != ClickResult::Ok)
    {
        if (haveSaved) SetCursorPos(saved.x, saved.y);
        return guard;
    }

    bool sent = PressButton(MOUSEEVENTF_LEFTDOWN);
    Sleep(kPressHoldMs);
    sent = PressButton(MOUSEEVENTF_LEFTUP) && sent;
    Sleep(kAfterReleaseMs);

    if (haveSaved) SetCursorPos(saved.x, saved.y);

    return sent ? ClickResult::Ok : ClickResult::Failed;
}

bool Session::ListMetrics(const std::vector<BookItem>& items,
                          LONG* rowHeight, LONG* viewHeight) const
{
    if (items.size() < 2 || !rowHeight || !viewHeight) return false;

    // Row pitch: the smallest positive gap between neighbouring rows.
    //
    // Not simply items[1].top - items[0].top: the list keeps a few recycled
    // rows parked far above the others, so the first gap can be ten rows wide.
    // Taking the minimum ignores those strays and lands on the true pitch.
    LONG pitch = 0;
    for (size_t i = 1; i < items.size(); i++)
    {
        const LONG d = items[i].itemRect.top - items[i - 1].itemRect.top;
        if (d > 0 && (pitch == 0 || d < pitch)) pitch = d;
    }
    if (pitch <= 0) return false;
    *rowHeight = pitch;

    RECT vp{};
    if (!GetViewport(&vp)) return false;
    *viewHeight = vp.bottom - vp.top;
    return *viewHeight > 0;
}

bool Session::MeasureShift(const std::vector<BookItem>& before,
                           const std::vector<BookItem>& after,
                           LONG* movedPx) const
{
    if (!movedPx) return false;

    // Find any row present in both snapshots and compare where it sits. Using a
    // named row rather than "the first realised row" keeps this honest when the
    // virtualisation window slides.
    for (const BookItem& b : before)
    {
        if (b.fullName.empty()) continue;
        for (const BookItem& a : after)
        {
            if (a.fullName == b.fullName)
            {
                *movedPx = b.itemRect.top - a.itemRect.top;   // positive = moved down
                return true;
            }
        }
    }
    return false;
}

bool Session::WheelScroll(int notches)
{
    RECT vp{};
    if (!GetViewport(&vp)) return false;

    const int cx = (vp.left + vp.right) / 2;
    const int cy = (vp.top + vp.bottom) / 2;

    POINT saved{};
    const bool haveSaved = GetCursorPos(&saved) != FALSE;

    MovePointer(cx, cy);
    Sleep(60);

    INPUT in{};
    in.type         = INPUT_MOUSE;
    in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = (DWORD)(-WHEEL_DELTA * notches);   // negative scrolls down
    const bool sent = SendInput(1, &in, sizeof(INPUT)) == 1;

    Sleep(60);
    if (haveSaved) SetCursorPos(saved.x, saved.y);
    return sent;
}

ScrollResult Session::ScrollDown(ScrollInfo* info)
{
    if (info) *info = ScrollInfo{};

    std::vector<BookItem> before;
    if (!SnapshotItems(&before, nullptr) || before.empty())
        return ScrollResult::Failed;

    LONG rowH = 0, viewH = 0;
    const bool haveMetrics = ListMetrics(before, &rowH, &viewH);

    // Deliberately short of a full screen: the scroll pattern moves exactly one
    // viewport, which lands a half-visible bottom row half-visible at the top
    // where it can be stepped over. Leaving two rows of overlap removes that
    // whole class of bug.
    LONG targetPx = haveMetrics ? (viewH - 2 * rowH) : 0;
    if (targetPx < (haveMetrics ? rowH : 1))
        targetPx = haveMetrics ? rowH : 200;

    if (info)
    {
        info->targetPx   = targetPx;
        info->rowHeight  = rowH;
        info->viewHeight = viewH;
    }

    // Preferred route: the wheel, which moves a small measured amount per notch.
    if (IsForeground())
    {
        int notches = (int)((targetPx / (m_pxPerNotch > 1.0 ? m_pxPerNotch : 48.0)) + 0.5);
        if (notches < 1) notches = 1;

        if (WheelScroll(notches))
        {
            Sleep(kScrollSettleMs);

            std::vector<BookItem> after;
            if (SnapshotItems(&after, nullptr) && !after.empty())
            {
                LONG moved = 0;
                const bool measured = MeasureShift(before, after, &moved);

                if (info)
                {
                    info->notchesSent = notches;
                    info->movedPx     = measured ? moved : 0;
                }

                if (measured && moved > 0)
                {
                    // Re-calibrate from what actually happened, so a different
                    // wheel setting or DPI corrects itself after one scroll.
                    const double observed = (double)moved / notches;
                    if (observed > 1.0 && observed < 1000.0)
                        m_pxPerNotch = (m_pxPerNotch * 0.5) + (observed * 0.5);
                    if (info) info->pxPerNotch = m_pxPerNotch;
                    return ScrollResult::Moved;
                }

                // No common row at all means we travelled a long way, not that
                // we stood still.
                if (!measured && after.front().fullName != before.front().fullName)
                    return ScrollResult::Moved;
            }
        }
    }

    // Fallback: the scroll pattern. Less desirable because of the zero overlap,
    // but it works when the window is not foreground or the wheel was ignored.
    IUIAutomationElement* flat = FlatList();
    if (!flat) return ScrollResult::Failed;

    bool asked = false;
    IUIAutomationScrollPattern* scroll = nullptr;
    if (SUCCEEDED(flat->GetCurrentPatternAs(UIA_ScrollPatternId,
                                            __uuidof(IUIAutomationScrollPattern),
                                            (void**)&scroll)) && scroll)
    {
        asked = SUCCEEDED(scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement));
        scroll->Release();
    }
    flat->Release();
    if (!asked) return ScrollResult::Failed;

    if (info) info->usedPattern = true;
    Sleep(kScrollSettleMs);

    std::vector<BookItem> after;
    if (!SnapshotItems(&after, nullptr) || after.empty()) return ScrollResult::Failed;

    LONG moved = 0;
    if (MeasureShift(before, after, &moved))
    {
        if (info) info->movedPx = moved;
        return moved > 0 ? ScrollResult::Moved : ScrollResult::NotMoved;
    }
    return (after.front().fullName != before.front().fullName)
         ? ScrollResult::Moved : ScrollResult::NotMoved;
}

// ---------------------------------------------------------------------------
// Title matching
// ---------------------------------------------------------------------------
std::wstring NormalizeTitle(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());

    bool pendingSpace = false;
    for (wchar_t c : s)
    {
        if (iswspace(c)) { pendingSpace = !out.empty(); continue; }
        if (pendingSpace) { out.push_back(L' '); pendingSpace = false; }
        out.push_back((wchar_t)towlower(c));
    }
    return out;
}

bool MatchesStopTitle(const BookItem& item, const std::wstring& stopText)
{
    const std::wstring needle = NormalizeTitle(stopText);
    if (needle.empty()) return false;

    const std::wstring title = NormalizeTitle(item.title);
    const std::wstring full  = NormalizeTitle(item.fullName);

    // Straight containment first - covers "The Unwritten Book: An Investigation".
    if (!title.empty() && title.find(needle) != std::wstring::npos) return true;
    if (!full.empty()  && full.find(needle)  != std::wstring::npos) return true;

    // The user usually pastes "<title> by <author>", but the app lists the
    // author surname-first, so compare the two halves separately.
    const size_t by = needle.rfind(L" by ");
    if (by != std::wstring::npos && by > 0)
    {
        const std::wstring titlePart = needle.substr(0, by);
        if (!titlePart.empty() && !title.empty() &&
            title.find(titlePart) != std::wstring::npos)
        {
            return true;
        }
    }
    return false;
}

} // namespace kuia
