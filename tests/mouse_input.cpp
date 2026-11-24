#include "../src/mouse_win32.hpp"

#include <cstdio>
#include <cstdlib>

using namespace QuestBookMouse;

namespace {
constexpr wchar_t WindowClass[] = L"QuestBookMouseTestWindow";
unsigned cursorMessages = 0;

LRESULT CALLBACK GameWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        ++cursorMessages;
        SetCursor(nullptr);
        return TRUE;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void Check(bool condition, const char* description) {
    if (condition) return;
    std::fprintf(stderr, "%s (Win32 error %lu)\n", description, GetLastError());
    std::exit(1);
}

void Pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void SendMouse(DWORD flags, DWORD data = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = data;
    Check(SendInput(1, &input, sizeof(input)) == 1, "Send mouse input");
}

MouseEvents::State WaitForAction(MouseEvents& mouse, bool (*condition)(const MouseEvents::State&)) {
    const auto deadline = GetTickCount64() + 2000;
    do {
        const auto state = mouse.Take();
        if (condition(state)) return state;
        Sleep(5);
    } while (GetTickCount64() < deadline);
    Check(false, "Receive input without pumping the game window");
    return {};
}
}

int main() {
    const auto instance = GetModuleHandleW(nullptr);
    const auto bronze = LoadCursorW(instance, L"G2NR_QUESTBOOK_CURSOR");
    Check(bronze != nullptr, "Load the custom cursor resource");
    ICONINFO icon{};
    Check(GetIconInfo(bronze, &icon) && !icon.fIcon && icon.xHotspot == 0 && icon.yHotspot == 0,
          "Click at the tip of the cursor");
    if (icon.hbmColor) DeleteObject(icon.hbmColor);
    if (icon.hbmMask) DeleteObject(icon.hbmMask);

    WNDCLASSW wc{};
    wc.lpfnWndProc = GameWindow;
    wc.hInstance = instance;
    wc.lpszClassName = WindowClass;
    Check(RegisterClassW(&wc) != 0, "Register the test window");
    const HWND game = CreateWindowExW(WS_EX_TOPMOST, WindowClass, L"QuestBook mouse test",
                                      WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 500, 350,
                                      nullptr, nullptr, instance, nullptr);
    Check(game != nullptr, "Create the test window");
    SetForegroundWindow(game);
    Pump();
    Check(HasFocus(game), "Focus the test window");
    RECT client{}, originalClip{};
    Check(ClientBounds(game, client) && GetClipCursor(&originalClip), "Read cursor bounds");

    QuestPointer pointer;
    pointer.Enter(game, client, true);
    const unsigned previousMessages = cursorMessages;
    SendMessageW(game, WM_SETCURSOR, reinterpret_cast<WPARAM>(game), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    Check(cursorMessages == previousMessages && GetCursor() == bronze, "Keep the custom system cursor");

    MouseEvents mouse;
    Check(mouse.Start(game), "Start mouse capture");
    const POINT directPoint{client.left + 20, client.top + 35};
    SendMessageW(game, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 35));
    SendMessageW(game, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(40, 50));
    SendMessageW(game, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2),
                 MAKELPARAM(directPoint.x, directPoint.y));
    const auto direct = mouse.Take();
    Check(direct.leftClick && direct.leftClick->x == directPoint.x && direct.leftClick->y == directPoint.y,
          "Convert left click from client to desktop coordinates");
    Check(direct.rightClick && direct.rightClick->x == client.left + 40 && direct.rightClick->y == client.top + 50,
          "Capture right click separately");
    Check(direct.wheel == 0, "Accumulate a partial wheel step");
    SendMessageW(game, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA / 2),
                 MAKELPARAM(directPoint.x, directPoint.y));
    const auto wheel = mouse.Take();
    Check(wheel.wheel == 1 && wheel.wheelPosition && wheel.wheelPosition->x == directPoint.x,
          "Use the desktop position of the completed wheel step");

    const POINT first{client.left + 100, client.top + 100};
    const POINT second{client.left + 150, client.top + 150};
    Check(SetCursorPos(first.x, first.y) != 0, "Position the click");
    SendMouse(MOUSEEVENTF_LEFTDOWN);
    SendMouse(MOUSEEVENTF_LEFTUP);
    Check(SetCursorPos(second.x, second.y) != 0, "Move before processing input");
    const auto clicked = WaitForAction(mouse, [](const auto& state) { return state.leftClick.has_value(); });
    Check(clicked.leftClick->x == first.x && clicked.leftClick->y == first.y, "Use the point where the click occurred");
    SendMouse(MOUSEEVENTF_RIGHTDOWN);
    SendMouse(MOUSEEVENTF_RIGHTUP);
    WaitForAction(mouse, [](const auto& state) { return state.rightClick.has_value(); });
    SendMouse(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-WHEEL_DELTA));
    WaitForAction(mouse, [](const auto& state) { return state.wheel == -1; });
    Pump();
    const auto repeated = mouse.Take();
    Check(!repeated.leftClick && !repeated.rightClick && repeated.wheel == 0, "Consume mouse actions once");

    const HWND other = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"Other window",
                                       WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 300, 220,
                                       nullptr, nullptr, instance, nullptr);
    Check(other != nullptr, "Create the other window");
    SetForegroundWindow(other);
    Pump();
    Check(!HasFocus(game), "Switch focus away");
    SendMouse(MOUSEEVENTF_WHEEL, WHEEL_DELTA);
    SetForegroundWindow(game);
    Pump();
    Check(HasFocus(game) && mouse.TakeFocusLoss(), "Preserve focus loss between frames");
    Check(mouse.Take().wheel == 0, "Ignore wheel input in the other window");

    mouse.Stop();
    pointer.Leave();
    const auto beforeRestore = cursorMessages;
    SendMessageW(game, WM_SETCURSOR, reinterpret_cast<WPARAM>(game), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    Check(cursorMessages == beforeRestore + 1, "Return cursor control to the game");
    RECT restored{};
    Check(GetClipCursor(&restored) && EqualRect(&restored, &originalClip), "Restore the original cursor clip");
    DestroyWindow(other);
    DestroyWindow(game);
}
