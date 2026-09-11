// KindleUia.h
// UI Automation driver for the new Kindle app from the Microsoft Store
// (AMZNKindle.AmazonKindleReadingApp - React Native for Windows on WinUI 3).
//
// The app publishes its whole library to UI Automation, so we can read exact
// book titles and press a specific book's Download button instead of firing
// blind keystrokes at the window the way the classic Kindle for PC driver does.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

struct IUIAutomation;
struct IUIAutomationElement;
struct IUIAutomationCondition;
struct IUIAutomationCacheRequest;

namespace kuia {

// One row of the library list.
struct BookItem
{
    std::wstring title;         // clean title, e.g. "Storm Prey (The Prey Series Book 20)"
    std::wstring author;        // as shown, e.g. "Sandford, John"
    std::wstring fullName;      // "<title> by <author>" (+ ", New" once downloaded)
    std::wstring asin;          // taken from download-button-<ASIN>; empty when downloaded
    bool         needsDownload = false;
    RECT         downloadRect{};  // screen coords of the Download button
    RECT         itemRect{};      // screen coords of the whole row
};

// Why a click could not be attempted. Distinguishing these lets the caller
// decide between "retry", "skip this book" and "abort the run".
enum class ClickResult
{
    Ok,             // the press was delivered
    NotForeground,  // the Kindle window is not the active window
    Occluded,       // something else is on top of the button
    OffScreen,      // the row is outside the scroll viewport (retry after scrolling)
    Gone,           // the button vanished before we could press it
    Failed          // SendInput or UIA failed
};

const wchar_t* ClickResultText(ClickResult r);

// Outcome of asking the list to move down a page.
enum class ScrollResult
{
    Moved,      // the list advanced
    NotMoved,   // the list did not budge (may just be busy - retry before believing it)
    Failed      // the scroll could not be attempted at all
};

// What one scroll actually did, for the run log.
struct ScrollInfo
{
    LONG   movedPx    = 0;
    LONG   targetPx   = 0;
    int    notchesSent = 0;
    double pxPerNotch = 0.0;
    bool   usedPattern = false;   // true if we fell back to the scroll pattern
    LONG   rowHeight  = 0;
    LONG   viewHeight = 0;
};

// Everything we learned while trying to press one Download button. Written to
// the run log so a failure can be diagnosed after the fact.
struct ClickInfo
{
    RECT         buttonRect{};
    POINT        point{};
    std::wstring hitTestId;     // what UIA says was actually under the cursor
    bool         wasForeground = false;
    bool         insideViewport = false;
};

// Owns the COM apartment work for one thread. Create, use and destroy this on
// a single thread (the automation worker) - COM interface pointers here are
// not marshalled between threads.
class Session
{
public:
    Session();
    ~Session();

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;

    // Initialise COM + UI Automation. Returns false and fills `err` on failure.
    bool Initialize(std::wstring* err);

    // Locate the running Kindle window and its library list.
    bool Attach(std::wstring* err);

    // True while the attached window still exists.
    bool IsAlive() const;

    HWND Window() const { return m_hwnd; }

    // Un-minimise and activate the Kindle window. Clicking only works when the
    // target is the foreground window, so the worker calls this before a press.
    bool EnsureForeground();

    // True when the Kindle window is currently the foreground window.
    bool IsForeground() const;

    // Read every library row currently realised in the UIA tree, in visual
    // order. The list is virtualised, so this is a window of ~90 rows around
    // the current scroll position, not the whole library.
    bool SnapshotItems(std::vector<BookItem>* out, std::wstring* err);

    // The scroll viewport in screen coords. Only rows inside it can be clicked.
    bool GetViewport(RECT* out) const;

    // True when this book still shows a Download button, i.e. it has not
    // finished downloading. Used to confirm a press actually did something.
    bool HasDownloadButton(const std::wstring& asin) const;

    // Press one book's Download button. Re-resolves the button by ASIN and
    // hit-tests the point immediately beforehand, so a moved, scrolled-away or
    // covered button is reported rather than clicked blindly.
    ClickResult ClickDownload(const std::wstring& asin, ClickInfo* info);

    // Move the library down by a bit less than one screenful, so consecutive
    // passes overlap by a couple of rows and no row is ever stepped over while
    // it is only half on screen.
    //
    // Uses the mouse wheel, which moves a small measured amount per notch, and
    // re-calibrates itself from what actually happened. The scroll pattern is
    // only a fallback: it moves exactly one viewport, leaving no overlap.
    ScrollResult ScrollDown(ScrollInfo* info);

private:
    IUIAutomationElement* FindByAutomationId(IUIAutomationElement* root,
                                             const wchar_t* id,
                                             int scope) const;
    IUIAutomationElement* FlatList() const;

    // Spin the mouse wheel over the middle of the list.
    bool WheelScroll(int notches);

    // Row pitch and viewport height, both in pixels, from a snapshot.
    bool ListMetrics(const std::vector<BookItem>& items, LONG* rowHeight, LONG* viewHeight) const;

    // How far the list moved between two snapshots, matching rows by name.
    // Returns false when no common row could be found (we moved a long way).
    bool MeasureShift(const std::vector<BookItem>& before,
                      const std::vector<BookItem>& after,
                      LONG* movedPx) const;

    bool                       m_comInit  = false;
    IUIAutomation*             m_ua       = nullptr;
    IUIAutomationElement*      m_root     = nullptr;   // the Kindle window element
    IUIAutomationCondition*    m_itemCond = nullptr;   // AutomationId == library-item-container
    IUIAutomationCacheRequest* m_cache    = nullptr;
    HWND                       m_hwnd     = nullptr;

    // Measured mouse-wheel step. Seeded with what this app does at 100% DPI and
    // corrected from observation, so a different wheel setting self-corrects.
    double                     m_pxPerNotch = 48.0;
};

// Normalise a string for tolerant title comparison: lowercase, collapse runs of
// whitespace, drop surrounding space.
std::wstring NormalizeTitle(const std::wstring& s);

// True when `item` is the book the user asked us to stop at.
//
// The user types something like "The Unwritten Book: An Investigation by
// Samantha Hunt", but the app renders the author surname-first ("Hunt,
// Samantha"), so a whole-string compare would never match. We therefore match
// on the title, splitting the user's text at " by " when it looks like a
// "<title> by <author>" phrase.
bool MatchesStopTitle(const BookItem& item, const std::wstring& stopText);

} // namespace kuia
