#include "log_list.hpp"
#include "mouse_win32.hpp"

#include <array>
#include <cstring>

namespace GOTHIC_NAMESPACE {
namespace {

using QuestBookMouse::ClientBounds;
using QuestBookMouse::HasFocus;
using QuestBookMouse::WindowRoot;

using CategoryLists = std::array<zCMenuItemList*, 4>;

CategoryLists Lists(oCMenu_Log& menu) {
    return {menu.listItemAct, menu.listItemOld, menu.listItemFailed, menu.listItemLog};
}

bool Hit(const zCView* view, POINT point) {
    return view && view->owner
        && point.x >= view->pposx && point.x - view->pposx < view->psizex
        && point.y >= view->pposy && point.y - view->pposy < view->psizey;
}

std::optional<POINT> ScreenPoint(const RECT& client, std::optional<POINT> point) {
    if (!point || !screen || !PtInRect(&client, *point)
        || screen->psizex <= 0 || screen->psizey <= 0) return {};
    return POINT{
        (std::min)(screen->psizex - 1, MulDiv(point->x - client.left, screen->psizex, client.right - client.left)),
        (std::min)(screen->psizey - 1, MulDiv(point->y - client.top, screen->psizey, client.bottom - client.top))
    };
}

bool Running(zCMenuItem* item) {
    return item && item->GetEnableHandleEvent() != 0;
}

zCMenuItem* ItemAt(oCMenu_Log& menu, POINT point) {
    // Later items are drawn on top. Only include the menu's interactive items.
    for (int i = menu.m_listItems.GetNumInList() - 1; i >= 0; --i) {
        auto* item = menu.m_listItems[i];
        if (Hit(item, point) && item->GetIsSelectable()) return item;
    }
    return nullptr;
}

void ActivateItem(oCMenu_Log& menu, zCMenuItem& item) {
    if (!item.owner || !item.GetIsSelectable()) return;
    const auto* items = &menu.m_listItems;
    auto* previous = menu.m_mainSel >= 0 && menu.m_mainSel < items->GetNumInList()
        ? (*items)[menu.m_mainSel] : nullptr;
    if (previous != &item) {
        if (previous) {
            previous->SetIsActive(FALSE);
            menu.HandleLeaveItem(previous);
        }
        if (!menu.SetActiveItem(&item)) return;
        menu.HandleEnterItem(&item);
    }
    // In the stock log the EFFECTS command shows the list on focus and enters
    // its nested input loop on Enter. A click leaves the category ready to use.
    if (item.GetSelActionType(0) != SEL_ACTION_EXECCOMMANDS
        || std::strncmp(item.GetSelActionString(0).ToChar(), "EFFECTS ", 8) != 0) {
        menu.HandleEvent(KEY_RETURN);
    }
}

void ScrollList(zCMenuItemList& list, int lines) {
    if (!list.m_pInnerWindow || !list.m_pInnerWindow->font) return;
    const auto position = QuestBook::ScrollList(
        {list.m_top_element, list.m_cur_element}, list.m_list.GetNumInList(),
        QuestBook::VisibleRows(list.m_pInnerWindow->FontY()), lines);
    list.m_top_element = position.top;
    list.m_cur_element = position.selected;
}

bool ClickList(zCMenuItemList& list, POINT point) {
    if (Hit(list.m_pViewArrowUp, point)) {
        ScrollList(list, -1);
        return true;
    }
    if (Hit(list.m_pViewArrowDn, point)) {
        ScrollList(list, 1);
        return true;
    }
    auto* body = list.m_pInnerWindow;
    if (!Hit(body, point) || !body->font) return false;
    const int row = QuestBook::ClickedRow(body->any(point.y - body->pposy), body->FontY(),
                                          list.m_top_element, list.m_list.GetNumInList());
    if (row >= 0 && list.m_list[row].contentViewer) {
        list.m_cur_element = row;
        list.ShowContent();
    }
    return true;
}

void ScrollText(zCMenuItemText& viewer, int notches) {
    if (viewer.m_mode != zCMenuItemText::MODE_MULTILINE
        || viewer.m_viewLines <= 0 || viewer.m_numLines <= viewer.m_viewLines) return;

    // The native handler bounds the line index and redraws the viewer.
    const int steps = std::clamp(notches, -8, 8) * QuestBook::WheelLines;
    for (int i = 0; i < (steps < 0 ? -steps : steps); ++i) {
        const int before = viewer.m_topLine;
        viewer.HandleEvent(steps > 0 ? KEY_UP : KEY_DOWN);
        if (viewer.m_topLine == before) break;
    }
}

void ApplyMouseAcquisition() {
    // Gothic II Addon 2.6 fix applies the engine mouse flag to DirectInput here.
    reinterpret_cast<int(__cdecl*)()>(0x004D3D30)();
}

class JournalMouseSession {
    oCMenu_Log& menu;
    HWND window = nullptr;
    QuestBookMouse::MouseEvents input;
    QuestBookMouse::QuestPointer cursor;
    bool listening = false;
    bool cursorActive = false;
    bool restoreEngineMouse = false;
    bool centerOnOpen = true;
    zCMenuItem* pendingItem = nullptr;

    void RestoreMouse() {
        cursor.Leave();
        cursorActive = false;
        if (std::exchange(restoreEngineMouse, false) && zinput
            && !zinput->GetDeviceEnabled(zINPUT_MOUSE)) {
            zinput->SetDeviceEnabled(zINPUT_MOUSE, TRUE);
            ApplyMouseAcquisition();
        }
    }

    bool PrepareInput(RECT& client) {
        if (input.TakeFocusLoss() || !HasFocus(window)) {
            if (cursorActive) RestoreMouse();
            pendingItem = nullptr;
            input.Discard();
        }
        if (!HasFocus(window) || !ClientBounds(window, client)) return false;
        if (!cursorActive) {
            if (!listening) listening = input.Start(window);
            if (!listening) return false;
            restoreEngineMouse = zinput->GetDeviceEnabled(zINPUT_MOUSE) != 0;
            if (restoreEngineMouse) {
                zinput->SetDeviceEnabled(zINPUT_MOUSE, FALSE);
                ApplyMouseAcquisition();
            }
            cursor.Enter(window, client, std::exchange(centerOnOpen, false));
            cursorActive = true;
        }
        return true;
    }

public:
    explicit JournalMouseSession(oCMenu_Log& menu)
        : menu(menu), window(WindowRoot(hWndApp)) {}

    ~JournalMouseSession() {
        input.Stop();
        RestoreMouse();
    }

    void Frame() {
        if (!zinput || !screen || !menu.m_screenInitDone || zCMenu::GetActive() != &menu) return;

        RECT client{};
        if (!PrepareInput(client)) return;
        const auto events = input.Take();
        const auto lists = Lists(menu);
        auto* viewer = menu.contentViewer;
        const bool reading = Running(viewer);
        zCMenuItemList* runningList = nullptr;
        for (auto* list : lists) {
            if (Running(list)) runningList = list;
        }
        // The parent menu can stay in GO while its child Run() owns input.
        if (!reading && !runningList && menu.m_exitState != zCMenu::NONE) return;

        if (pendingItem) {
            if (reading) viewer->HandleEvent(KEY_ESCAPE);
            else if (runningList) runningList->HandleEvent(KEY_ESCAPE);
            else ActivateItem(menu, *std::exchange(pendingItem, nullptr));
            return;
        }
        if (ScreenPoint(client, events.rightClick)) {
            if (reading) viewer->HandleEvent(KEY_ESCAPE);
            else if (runningList) runningList->HandleEvent(KEY_ESCAPE);
            else menu.HandleEvent(KEY_ESCAPE);
            return;
        }

        const auto click = ScreenPoint(client, events.leftClick);
        const auto wheel = ScreenPoint(client, events.wheelPosition);
        if (reading) {
            if (wheel && events.wheel && Hit(viewer, *wheel)) ScrollText(*viewer, events.wheel);
            return; // The engine opens the quest's paper viewer over the category and list.
        }
        if (auto* item = click ? ItemAt(menu, *click) : nullptr) {
            if (runningList) {
                pendingItem = item;
                runningList->HandleEvent(KEY_ESCAPE);
            } else {
                ActivateItem(menu, *item);
            }
            return;
        }

        for (auto* list : lists) {
            if (!list || !list->owner) continue;
            if (click && ClickList(*list, *click)) return;
            if (wheel && events.wheel && Hit(list, *wheel)) {
                ScrollList(*list, -std::clamp(events.wheel, -8, 8) * QuestBook::WheelLines);
                return;
            }
        }
    }

    bool Handles(const zCMenu* other) const { return &menu == other; }
};

JournalMouseSession* activeJournal = nullptr;

void __fastcall ShowJournal(oCLogScreen*, void*);
auto ShowJournalHook = Union::CreateHook(
    SIGNATURE_OF(&oCLogScreen::Show), &ShowJournal, Union::HookType::Hook_Detours);

void __fastcall ShowJournal(oCLogScreen* self, void* edx) {
    if (!self->m_pLogMenu || activeJournal) {
        ShowJournalHook(self, edx);
        return;
    }
    JournalMouseSession journal(*self->m_pLogMenu);
    activeJournal = &journal;
    ShowJournalHook(self, edx);
    activeJournal = nullptr;
}

void __fastcall HandleMenuFrame(zCMenu*, void*, int);
auto HandleMenuFrameHook = Union::CreateHook(
    SIGNATURE_OF(&zCMenu::HandleFrame), &HandleMenuFrame, Union::HookType::Hook_Detours);

void __fastcall HandleMenuFrame(zCMenu* self, void* edx, int render) {
    if (activeJournal && activeJournal->Handles(self)) activeJournal->Frame();
    HandleMenuFrameHook(self, edx, render);
}

} // namespace
} // namespace GOTHIC_NAMESPACE
