#pragma once

#include <Windows.h>
#include <commctrl.h>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#pragma comment(lib, "comctl32.lib")

namespace QuestBookMouse {

inline HWND WindowRoot(HWND window) {
    return window ? GetAncestor(window, GA_ROOT) : nullptr;
}

inline bool HasFocus(HWND game) {
    return game && WindowRoot(GetForegroundWindow()) == WindowRoot(game);
}

inline bool ClientBounds(HWND window, RECT& bounds) {
    RECT client{};
    if (!GetClientRect(window, &client)) return false;
    POINT topLeft{client.left, client.top}, bottomRight{client.right, client.bottom};
    if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return false;
    bounds = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    return bounds.right > bounds.left && bounds.bottom > bounds.top;
}

class MouseEvents {
public:
    struct State {
        std::optional<POINT> leftClick, rightClick, wheelPosition;
        int wheel = 0;
    };

private:
    static inline thread_local MouseEvents* activeListener = nullptr;
    static inline MouseEvents* activeWindow = nullptr;
    HWND window = nullptr;
    HHOOK windowHook = nullptr;
    std::thread listener;
    DWORD listenerId = 0;
    mutable std::mutex inputMutex;
    bool lostFocus = false;
    bool lowLevelLeft = false, lowLevelRight = false, lowLevelWheel = false;
    State pending;
    int wheelDelta = 0;

    static bool IsMouseAction(UINT message) {
        return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MOUSEWHEEL;
    }

    void Record(UINT message, POINT at, int delta, bool lowLevel) {
        switch (message) {
        case WM_LBUTTONDOWN:
            if (lowLevel || !lowLevelLeft) pending.leftClick = at;
            lowLevelLeft |= lowLevel;
            break;
        case WM_RBUTTONDOWN:
            if (lowLevel || !lowLevelRight) pending.rightClick = at;
            lowLevelRight |= lowLevel;
            break;
        case WM_MOUSEWHEEL:
            if (lowLevel || !lowLevelWheel) {
                pending.wheelPosition = at;
                wheelDelta += delta;
            }
            lowLevelWheel |= lowLevel;
            break;
        }
    }

    static LRESULT CALLBACK Observe(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION && activeListener && HasFocus(activeListener->window)
            && IsMouseAction(static_cast<UINT>(message))) {
            auto& mouse = *activeListener;
            const auto& input = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
            std::lock_guard lock(mouse.inputMutex);
            mouse.Record(static_cast<UINT>(message), input.pt,
                         static_cast<short>(HIWORD(input.mouseData)), true);
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    static LRESULT CALLBACK ObserveWindow(int code, WPARAM sent, LPARAM data) {
        if (code == HC_ACTION && activeWindow) {
            const auto& message = *reinterpret_cast<const CWPSTRUCT*>(data);
            auto& mouse = *activeWindow;
            if (message.hwnd == mouse.window) {
                if ((message.message == WM_ACTIVATEAPP && !message.wParam)
                    || (message.message == WM_ACTIVATE && LOWORD(message.wParam) == WA_INACTIVE)) {
                    std::lock_guard lock(mouse.inputMutex);
                    mouse.lostFocus = true;
                    mouse.ClearPending();
                } else if (IsMouseAction(message.message) && HasFocus(mouse.window)) {
                    // Some wrappers deliver mouse input directly to the window.
                    POINT at{static_cast<short>(LOWORD(message.lParam)),
                             static_cast<short>(HIWORD(message.lParam))};
                    if (message.message != WM_MOUSEWHEEL && !ClientToScreen(message.hwnd, &at)) {
                        return CallNextHookEx(nullptr, code, sent, data);
                    }
                    std::lock_guard lock(mouse.inputMutex);
                    mouse.Record(message.message, at, GET_WHEEL_DELTA_WPARAM(message.wParam), false);
                }
            }
        }
        return CallNextHookEx(nullptr, code, sent, data);
    }

    void Listen(std::promise<bool> ready, HMODULE module) {
        MSG message{};
        // Create the thread's queue before Start returns. The game thread can now
        // pace its frames without delaying Windows' low-level mouse callback.
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        listenerId = GetCurrentThreadId();
        activeListener = this;
        const auto hook = SetWindowsHookExW(WH_MOUSE_LL, Observe, module, 0);
        ready.set_value(hook != nullptr);
        if (!hook) return;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            DispatchMessageW(&message);
        }
        UnhookWindowsHookEx(hook);
    }

    void ClearPending() {
        pending = {};
        wheelDelta = 0;
        lowLevelLeft = lowLevelRight = lowLevelWheel = false;
    }

public:
    MouseEvents() = default;
    MouseEvents(const MouseEvents&) = delete;
    MouseEvents& operator=(const MouseEvents&) = delete;
    ~MouseEvents() { Stop(); }

    bool Start(HWND game) {
        if (!game || activeWindow || listener.joinable()) return false;
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&Observe), &module)) return false;
        window = game;
        std::promise<bool> ready;
        auto installed = ready.get_future();
        try {
            listener = std::thread(&MouseEvents::Listen, this, std::move(ready), module);
        } catch (const std::system_error&) {
            return false;
        }
        if (!installed.get()) listener.join();
        if (GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()) {
            windowHook = SetWindowsHookExW(WH_CALLWNDPROC, ObserveWindow, nullptr, GetCurrentThreadId());
        }
        if (!listener.joinable() && !windowHook) return false;
        activeWindow = this;
        return true;
    }

    void Stop() {
        if (activeWindow == this) activeWindow = nullptr;
        if (windowHook) UnhookWindowsHookEx(std::exchange(windowHook, nullptr));
        if (listener.joinable()) {
            PostThreadMessageW(listenerId, WM_QUIT, 0, 0);
            // A later hook in the chain may belong to this thread.
            const HANDLE worker = listener.native_handle();
            while (MsgWaitForMultipleObjects(1, &worker, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
                MSG message{};
                PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            }
            listener.join();
        }
        Discard();
        lostFocus = false;
    }

    void Discard() {
        std::lock_guard lock(inputMutex);
        ClearPending();
    }

    bool TakeFocusLoss() {
        std::lock_guard lock(inputMutex);
        return std::exchange(lostFocus, false);
    }

    State Take() {
        std::lock_guard lock(inputMutex);
        pending.wheel = wheelDelta / WHEEL_DELTA;
        wheelDelta -= pending.wheel * WHEEL_DELTA;
        return std::exchange(pending, State{});
    }
};

class QuestPointer {
    HWND cursorWindow = nullptr;
    HCURSOR cursor = nullptr;
    int showCalls = 0;
    std::optional<RECT> originalClip, ownedClip;

    static bool SameRect(const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }

    static HCURSOR LoadQuestCursor() {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&LoadQuestCursor), &module)) {
            if (auto resource = LoadCursorW(module, L"G2NR_QUESTBOOK_CURSOR")) return resource;
        }
        return LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    }

    static LRESULT CALLBACK KeepQuestCursor(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                              UINT_PTR id, DWORD_PTR context) {
        auto& pointer = *reinterpret_cast<QuestPointer*>(context);
        if (message == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && HasFocus(window)) {
            SetCursor(pointer.cursor);
            return TRUE;
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(window, KeepQuestCursor, id);
            pointer.cursorWindow = nullptr;
        }
        return DefSubclassProc(window, message, wp, lp);
    }

public:
    QuestPointer() = default;
    QuestPointer(const QuestPointer&) = delete;
    QuestPointer& operator=(const QuestPointer&) = delete;
    ~QuestPointer() { Leave(); }

    void Enter(HWND window, const RECT& client, bool center) {
        RECT previous{};
        if (GetClipCursor(&previous) && !SameRect(client, previous) && ClipCursor(&client)) {
            originalClip = previous;
            ownedClip = client;
        }
        cursor = LoadQuestCursor();
        if (cursor && GetWindowThreadProcessId(window, nullptr) == GetCurrentThreadId()
            && SetWindowSubclass(window, KeepQuestCursor, 0, reinterpret_cast<DWORD_PTR>(this))) {
            cursorWindow = window;
        }
        do {
            ++showCalls;
        } while (ShowCursor(TRUE) < 0);
        if (cursor) SetCursor(cursor);
        if (center) SetCursorPos(client.left + (client.right - client.left) / 2,
                                client.top + (client.bottom - client.top) / 2);
    }

    void Leave() {
        if (cursorWindow) RemoveWindowSubclass(std::exchange(cursorWindow, nullptr), KeepQuestCursor, 0);
        RECT current{};
        if (originalClip && ownedClip && GetClipCursor(&current) && SameRect(*ownedClip, current)) {
            ClipCursor(&*originalClip);
        }
        originalClip.reset();
        ownedClip.reset();
        while (showCalls > 0) {
            ShowCursor(FALSE);
            --showCalls;
        }
    }
};

} // namespace QuestBookMouse
