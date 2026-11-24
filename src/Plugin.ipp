#include "log_list.hpp"
#include "log_recency.hpp"
#include "mouse_win32.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace GOTHIC_NAMESPACE {
namespace {

using QuestBookMouse::ClientBounds;
using QuestBookMouse::HasFocus;
using QuestBookMouse::WindowRoot;

constexpr unsigned long paper = 0xFFF0DFBB;
constexpr unsigned long bright = 0xFFFFEAC4;
constexpr unsigned long bronze = 0xFFDDB67D;
constexpr unsigned long muted = 0xFFD6C6A5;
constexpr unsigned long lineInk = 0xFF655740;

struct Box {
    int x = 0, y = 0, width = 0, height = 0;
    bool Hit(POINT p) const {
        return p.x >= x && p.x - x < width && p.y >= y && p.y - y < height;
    }
    int Right() const { return x + width; }
    int Bottom() const { return y + height; }
    bool operator==(const Box&) const = default;
};

struct Topic {
    const oCLogTopic* id;
    std::string title;
    QuestBook::Activity activity;
};

enum Category { Current, Done, Failed, Notes, CategoryCount };
using Topics = std::array<std::vector<Topic>, CategoryCount>;

QuestBook::Recency recency;

std::optional<Category> GetCategory(const oCLogTopic& topic) {
    if (topic.m_enuSection == oCLogTopic::zELogSection_Notes) return Notes;
    if (topic.m_enuSection != oCLogTopic::zELogSection_Missions) return std::nullopt;
    switch (topic.m_enuStatus) {
    case oCLogTopic::zELogTopicStatus_Running: return Current;
    case oCLogTopic::zELogTopicStatus_Success: return Done;
    case oCLogTopic::zELogTopicStatus_Failure: return Failed;
    default: return std::nullopt;
    }
}

Topics ReadTopics() {
    Topics topics;
    const auto& list = oCLogManager::GetLogManager().m_lstTopics;
    for (auto* node = list.GetNextInList(); node; node = node->GetNextInList()) {
        const auto* topic = node->GetData();
        if (!topic) continue;
        if (const auto tab = GetCategory(*topic)) {
            topics[*tab].push_back({topic, topic->m_strDescription.ToChar(), recency.Get(topic)});
        }
    }
    for (auto& tab : topics) QuestBook::MostRecentFirst(tab);
    return topics;
}

std::optional<POINT> ScreenPoint(const RECT& client, std::optional<POINT> point) {
    if (!point || !screen || !PtInRect(&client, *point)
        || screen->psizex <= 0 || screen->psizey <= 0) return {};
    return POINT{
        (std::min)(screen->psizex - 1, MulDiv(point->x - client.left, screen->psizex, client.right - client.left)),
        (std::min)(screen->psizey - 1, MulDiv(point->y - client.top, screen->psizey, client.bottom - client.top))
    };
}

std::string MenuText(zCMenu& menu, const char* id) {
    for (int i = 0; i < menu.m_listItems.GetNumInList(); ++i) {
        auto* item = menu.m_listItems[i];
        if (item && !std::strcmp(item->GetName().ToChar(), id)) return item->GetText(0).ToChar();
    }
    return {};
}

std::string Russian(const wchar_t* text) {
    const int size = WideCharToMultiByte(1251, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string bytes(size, '\0');
    WideCharToMultiByte(1251, 0, text, -1, bytes.data(), size, nullptr, nullptr);
    bytes.pop_back();
    return bytes;
}

bool HasCyrillicCaption(zCMenu& menu) {
    const auto caption = MenuText(menu, "MENU_ITEM_SEL_MISSIONS_ACT");
    return std::count_if(caption.begin(), caption.end(), [](unsigned char c) { return c >= 0xC0; }) >= 4;
}

void InitSprite(zCView& view, const char* texture, int alpha = 255) {
    view.InsertBack(zSTRING(texture));
    view.SetAlphaBlendFunc(zRND_ALPHA_FUNC_BLEND);
    view.SetTransparency(alpha);
}

void Place(zCView& view, Box box) {
    view.pposx = box.x;
    view.pposy = box.y;
    view.psizex = box.width;
    view.psizey = box.height;
}

zCOLOR Ink(unsigned long rgba) {
    zCOLOR color;
    color.dword = rgba;
    return color;
}

class JournalPanel : public zCView {
    zCView solid{0, 0, 8, 8}, pointer{0, 0, 8, 8};
    Topics topics;
    std::vector<std::string> entryLines;
    std::vector<std::pair<Box, unsigned long>> marks;
    std::array<Box, CategoryCount> tabs;
    Box bounds, close, list, reading, listTrack, textTrack;
    int tab = Current, selected = 0, listTop = 0, textTop = 0;
    int listRows = 0, textRows = 0, line = 0, inset = 0;
    bool cyrillic = false, dirty = true;
    std::string calendar;

    std::string Label(const wchar_t* ru, const char* en) const {
        return cyrillic ? Russian(ru) : en;
    }

    void Normalize(std::string& text) const {
        if (!cyrillic) return;
        std::replace(text.begin(), text.end(), '\xA8', '\xC5');
        std::replace(text.begin(), text.end(), '\xB8', '\xE5');
    }

    int Width(const std::string& text) {
        return GetFont()->GetFontX(zSTRING(text.c_str()));
    }

    void Text(int x, int y, std::string text, int width, unsigned long ink = paper) {
        if (text.empty() || width <= 0) return;
        Normalize(text);
        if (Width(text) > width) {
            const int dots = Width("...");
            if (dots > width) return;
            while (!text.empty() && Width(text) + dots > width) text.pop_back();
            text += "...";
        }
        const zSTRING str(text.c_str());
        auto color = Ink(ink);
        auto shadow = Ink(0xFF0A0907);
        CreateText(anx(x - bounds.x), any(y - bounds.y), str, 0.0f, color, FALSE, TRUE);
        CreateText(anx(x + 1 - bounds.x), any(y + 1 - bounds.y), str, 0.0f, shadow, FALSE, TRUE);
    }

    void WrapLine(std::string_view source, int width, std::vector<std::string>& destination) {
        if (source.empty()) {
            destination.emplace_back();
            return;
        }
        while (!source.empty()) {
            std::size_t length = source.size();
            if (Width(std::string(source)) > width) {
                // Fit a prefix by measured width, including words wider than the pane.
                std::size_t low = 1, high = length;
                while (low < high) {
                    const auto middle = low + (high - low + 1) / 2;
                    if (Width(std::string(source.substr(0, middle))) <= width) low = middle;
                    else high = middle - 1;
                }
                length = low;
                const auto space = source.substr(0, length + 1).find_last_of(" \t");
                if (space != std::string_view::npos && space > 0) length = space;
            }
            destination.emplace_back(source.substr(0, length));
            source.remove_prefix(length);
            const auto start = source.find_first_not_of(" \t");
            if (start == std::string_view::npos) break;
            source.remove_prefix(start);
        }
    }

    std::vector<std::string> Wrap(std::string text, int width) {
        Normalize(text);
        std::vector<std::string> result;
        for (std::size_t begin = 0; begin < text.size();) {
            auto end = text.find('\n', begin);
            if (end == std::string::npos) end = text.size();
            auto paragraph = std::string_view(text).substr(begin, end - begin);
            if (!paragraph.empty() && paragraph.back() == '\r') paragraph.remove_suffix(1);
            WrapLine(paragraph, width, result);
            begin = end + 1;
        }
        return result;
    }

    void AddRule(Box rect, unsigned long ink = lineInk) {
        marks.emplace_back(rect, ink);
    }

    void SpanBar(Box bar, int count, int rows, int offset) {
        if (count <= rows || rows <= 0) return;
        const int thumb = std::clamp(bar.height * rows / count, (std::min)(12, bar.height), bar.height);
        const int y = bar.y + (bar.height - thumb) * offset / (count - rows);
        AddRule({bar.x + bar.width / 2, bar.y, 1, bar.height});
        AddRule({bar.x + bar.width / 2 - 1, y, 3, thumb}, bronze);
    }

    void ReflowText() {
        entryLines.clear();
        const auto& items = topics[tab];
        if (items.empty()) return;
        std::vector<const oCLogEntry*> entries;
        const auto& log = items[selected].id->m_lstEntries;
        for (auto* node = log.GetNextInList(); node; node = node->GetNextInList()) {
            if (const auto* entry = node->GetData()) entries.push_back(entry);
        }
        for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
            auto wrapped = Wrap((*it)->m_strDescription.ToChar(), (std::max)(1, reading.width - 12));
            if (wrapped.empty()) continue;
            if (!entryLines.empty()) entryLines.emplace_back();
            entryLines.insert(entryLines.end(), std::make_move_iterator(wrapped.begin()),
                             std::make_move_iterator(wrapped.end()));
        }
    }

    void PaintHeader() {
        const int x = bounds.x + inset;
        const int y = bounds.y + inset / 2;
        const int right = bounds.Right() - inset;
        close = {right - Width("Esc") - 12, y, Width("Esc") + 12, line};
        const auto title = Label(L"ЖУРНАЛ ЗАДАНИЙ", "QUEST JOURNAL");
        Text(x, y, title, close.x - x, bronze);
        const int available = close.x - x - Width(title) - 18;
        if (!calendar.empty() && Width(calendar) <= available) {
            Text(close.x - Width(calendar) - 12, y, calendar, available, muted);
        }
        Text(right - Width("Esc"), y, "Esc", Width("Esc"), bronze);

        const std::array<std::string, CategoryCount> titles{
            Label(L"Текущие", "Active"), Label(L"Завершенные", "Completed"),
            Label(L"Проваленные", "Failed"), Label(L"Заметки", "Notes")
        };
        const int tabY = y + line + 4;
        AddRule({x, tabY + line - 1, right - x, 1});
        for (int i = 0; i < CategoryCount; ++i) {
            const int tabLeft = x + (right - x) * i / CategoryCount;
            const int tabRight = x + (right - x) * (i + 1) / CategoryCount;
            tabs[i] = {tabLeft, tabY, tabRight - tabLeft, line};
            auto label = titles[i];
            const auto unread = std::count_if(topics[i].begin(), topics[i].end(),
                [](const Topic& topic) { return topic.activity.unread; });
            if (unread) {
                const auto withCount = label + "  " + std::to_string(unread);
                if (Width(withCount) < tabs[i].width - 8) label = withCount;
            }
            const int pad = (std::max)(4, (tabs[i].width - Width(label)) / 2);
            Text(tabLeft + pad, tabY, label, tabs[i].width - pad - 4, i == tab ? bright : bronze);
            if (i == tab) AddRule({tabLeft + 5, tabY + line - 2, tabs[i].width - 10, 2}, bronze);
        }
    }

    void Paint() {
        marks.clear();
        ClrPrintwin();
        PaintHeader();
        const int startY = tabs[0].Bottom() + inset / 2;
        const int footer = bounds.Bottom() - inset / 2 - line;
        const int x = bounds.x + inset, right = bounds.Right() - inset;
        const int split = bounds.x + bounds.width * 43 / 100;
        const auto& items = topics[tab];
        const int count = static_cast<int>(items.size());

        AddRule({split, startY, 1, footer - startY - 6});
        AddRule({x, footer - 5, right - x, 1});
        Text(x, footer, Label(L"Стрелки — выбор   PgUp/PgDn — текст", "Arrows: select   PgUp/PgDn: read"),
             right - x, muted);
        const int listRight = split - 9;
        Text(x, startY, Label(L"Записи", "Entries") + "  " + std::to_string(count), listRight - x, bronze);
        list = {x, startY + line, listRight - x, footer - startY - line - 7};
        listRows = (std::max)(0, list.height / line);
        listTrack = {list.Right() - 9, list.y, 9, listRows * line};
        const int bodyX = split + inset * 2 / 3;
        reading = {bodyX, startY, right - bodyX, footer - startY - 7};
        textRows = 0;
        textTrack = {};
        if (items.empty()) {
            Text(x, list.y, Label(L"В разделе пока нет записей", "No entries in this section"), list.width, muted);
            return;
        }
        listTop = std::clamp(listTop, 0, (std::max)(0, count - listRows));
        SpanBar(listTrack, count, listRows, listTop);
        for (int i = listTop; i < count && i < listTop + listRows; ++i) {
            const int y = list.y + (i - listTop) * line;
            if (i == selected) AddRule({x - 8, y + 2, 2, line - 5}, bronze);
            if (items[i].activity.unread) AddRule({list.Right() - 20, y + line / 2 - 1, 4, 4}, bronze);
            Text(x, y, items[i].title, list.width - 28, i == selected ? bright : paper);
        }

        const auto heading = Wrap(items[selected].title, reading.width);
        const int maxTitle = (std::min)(3, (std::max)(1, reading.height / line - 1));
        int y = startY;
        for (std::size_t i = 0; i < heading.size() && i < static_cast<std::size_t>(maxTitle); ++i) {
            Text(bodyX, y, heading[i], reading.width, bright);
            y += line;
        }
        AddRule({bodyX, y, reading.width, 1});
        y += 6;
        textRows = (std::max)(0, (reading.Bottom() - y) / line);
        const int entries = static_cast<int>(entryLines.size());
        textTop = std::clamp(textTop, 0, (std::max)(0, entries - textRows));
        textTrack = {reading.Right() - 9, y, 9, textRows * line};
        SpanBar(textTrack, entries, textRows, textTop);
        if (!entries && textRows > 0) {
            Text(bodyX, y, Label(L"Текст записи пока пуст", "No text yet"), reading.width, muted);
        }
        for (int i = textTop; i < entries && i < textTop + textRows; ++i) {
            Text(bodyX, y, entryLines[i], reading.width - 12);
            y += line;
        }
    }

    void SelectTopic(int next, bool acknowledge) {
        auto& items = topics[tab];
        if (items.empty()) return;
        next = std::clamp(next, 0, static_cast<int>(items.size()) - 1);
        if (next != selected) {
            selected = next;
            textTop = 0;
            ReflowText();
        }
        if (acknowledge && items[selected].activity.unread) {
            recency.Read(items[selected].id);
            items[selected].activity.unread = false;
        }
        if (listRows > 0) {
            listTop = std::clamp(listTop, (std::max)(0, selected - listRows + 1), selected);
        }
        dirty = true;
    }

    void SelectTab(int next) {
        if (tab == next) return;
        tab = next;
        selected = listTop = textTop = 0;
        ReflowText();
        dirty = true;
    }

    void Scroll(int& top, int delta, int count, int rows) {
        const int next = std::clamp(top + delta, 0, (std::max)(0, count - rows));
        dirty |= next != top;
        top = next;
    }

    void Jump(POINT p, Box track, int& top, int count, int rows) {
        if (count <= rows || rows <= 0 || !track.Hit(p)) return;
        const int offset = static_cast<int>(p.y - track.y);
        const int maxTop = count - rows;
        Scroll(top, MulDiv(offset, maxTop, (std::max)(1, track.height - 1)) - top, count, rows);
    }

    void DrawSolid(Box box, unsigned long color) {
        if (box.width <= 0 || box.height <= 0 || !solid.backTex) return;
        Place(solid, box);
        solid.SetColor(Ink(color));
        solid.Blit();
    }

public:
    JournalPanel() : zCView(0, 0, 8192, 8192) {
        InitSprite(*this, "G2NRQ_PANEL.TGA", 248);
        InitSprite(solid, "G2NRQ_SOLID.TGA");
        InitSprite(pointer, "G2NRQ_CURSOR.TGA");
    }

    bool Open(oCMenu_Log& menu) {
        if (!backTex || !screen->GetFont()) return false;
        SetFont(screen->GetFont());
        cyrillic = HasCyrillicCaption(menu);
        topics = ReadTopics();
        std::uint64_t newest = 0;
        const auto selectNewest = [&](int end) {
            for (int i = 0; i < end; ++i) {
                for (const auto& topic : topics[i]) {
                    if (!topic.activity.unread || topic.activity.order <= newest) continue;
                    newest = topic.activity.order;
                    tab = i;
                }
            }
        };
        selectNewest(Notes);
        if (!newest) selectNewest(CategoryCount);
        if (ogame && ogame->GetWorldTimer()) {
            int day = 0, hour = 0, minute = 0;
            ogame->GetTime(day, hour, minute);
            char time[24]{};
            std::snprintf(time, sizeof(time), "%02d:%02d", hour, minute);
            calendar = Label(L"День ", "Day ") + std::to_string(day) + "    " + time;
        }
        const auto version = MenuText(menu, "MENU_ITEM_VERSION");
        if (!version.empty() && version.find_first_of("\r\n") == std::string::npos) {
            if (!calendar.empty()) calendar += "    ";
            calendar += version;
        }
        return Update();
    }

    bool Update() {
        if (!screen || screen->psizex < 480 || screen->psizey < 300) return false;
        const int width = (std::min)(screen->psizex * 94 / 100, screen->psizey * 113 / 100);
        const int height = screen->psizey * 70 / 100;
        const Box next{(screen->psizex - width) / 2, (screen->psizey - height) / 2, width, height};
        if (next != bounds) {
            bounds = next;
            Place(*this, next);
            inset = std::clamp(height / 32, 12, 27);
            line = (std::max)(12, GetFont()->GetFontY()) + 4;
            // Paint once to get the text pane width before wrapping its contents.
            Paint();
            ReflowText();
            dirty = true;
        }
        if (std::exchange(dirty, false)) Paint();
        return true;
    }

    bool CloseAt(POINT p) const { return close.Hit(p); }

    void Click(POINT p) {
        for (int i = 0; i < CategoryCount; ++i) {
            if (tabs[i].Hit(p)) {
                SelectTab(i);
                return;
            }
        }
        const int count = static_cast<int>(topics[tab].size());
        if (count > listRows && listTrack.Hit(p)) {
            Jump(p, listTrack, listTop, count, listRows);
        } else if (list.Hit(p)) {
            const int row = static_cast<int>(p.y - list.y) / line;
            if (row < listRows && listTop + row < count) SelectTopic(listTop + row, true);
        } else {
            Jump(p, textTrack, textTop, static_cast<int>(entryLines.size()), textRows);
        }
    }

    void Wheel(POINT p, int notches) {
        const int delta = -std::clamp(notches, -8, 8) * QuestBook::WheelLines;
        if (list.Hit(p)) Scroll(listTop, delta, static_cast<int>(topics[tab].size()), listRows);
        else if (reading.Hit(p)) Scroll(textTop, delta, static_cast<int>(entryLines.size()), textRows);
    }

    bool Key(int key) {
        switch (key) {
        case KEY_LEFT: SelectTab((tab + CategoryCount - 1) % CategoryCount); break;
        case KEY_RIGHT:
        case KEY_TAB: SelectTab((tab + 1) % CategoryCount); break;
        case KEY_UP: SelectTopic(selected - 1, true); break;
        case KEY_DOWN: SelectTopic(selected + 1, true); break;
        case KEY_HOME: SelectTopic(0, true); break;
        case KEY_END: SelectTopic(static_cast<int>(topics[tab].size()) - 1, true); break;
        case KEY_RETURN:
        case KEY_NUMPADENTER: SelectTopic(selected, true); break;
        case KEY_PGUP:
        case KEY_PGDN:
            Scroll(textTop, (key == KEY_PGUP ? -1 : 1) * (std::max)(1, textRows - 1),
                   static_cast<int>(entryLines.size()), textRows);
            break;
        default: return false;
        }
        return true;
    }

    void Blit() override {
        const auto source = zrenderer->GetAlphaBlendSource();
        zrenderer->SetAlphaBlendSource(zRND_ALPHA_SOURCE_MATERIAL);
        zCView::Blit();
        for (const auto& [box, ink] : marks) DrawSolid(box, ink);
        zrenderer->SetAlphaBlendSource(source);
    }

    void DrawFallbackCursor(POINT p) {
        if (!pointer.backTex) return;
        const int size = std::clamp(screen->psizey / 42, 16, 48);
        const auto source = zrenderer->GetAlphaBlendSource();
        zrenderer->SetAlphaBlendSource(zRND_ALPHA_SOURCE_MATERIAL);
        Place(pointer, {static_cast<int>(p.x), static_cast<int>(p.y), size, size});
        pointer.Blit();
        zrenderer->SetAlphaBlendSource(source);
    }
};

void ApplyMouseAcquisition() {
    reinterpret_cast<int(__cdecl*)()>(0x004D3D30)();
}

class JournalSession {
    oCMenu_Log& menu;
    JournalPanel panel;
    HWND window = WindowRoot(hWndApp);
    QuestBookMouse::MouseEvents input;
    QuestBookMouse::QuestPointer cursor;
    RECT client{};
    bool cursorActive = false, restoreEngineMouse = false, centerOnOpen = true;
    bool listening = false, leftHeld = false, rightHeld = false;
    bool initialized = false, ready = false, closing = false;

    void RestoreMouse() {
        if (!std::exchange(cursorActive, false)) return;
        cursor.Leave();
        if (std::exchange(restoreEngineMouse, false) && zinput
            && !zinput->GetDeviceEnabled(zINPUT_MOUSE)) {
            zinput->SetDeviceEnabled(zINPUT_MOUSE, TRUE);
            ApplyMouseAcquisition();
        }
    }

    std::optional<POINT> ClickAt(std::optional<POINT> event, int button, bool& held) {
        const bool down = (GetAsyncKeyState(button) & 0x8000) != 0;
        const bool pressed = !std::exchange(held, down) && down;
        if (event) return ScreenPoint(client, event);
        POINT position{};
        if (pressed && GetCursorPos(&position)) return ScreenPoint(client, position);
        return std::nullopt;
    }

public:
    explicit JournalSession(oCMenu_Log& menu) : menu(menu) {}

    ~JournalSession() {
        RestoreMouse();
        input.Stop();
    }

    bool HandleKey(oCMenu_Log* owner, int key) {
        if (owner != &menu || !ready || zCMenu::GetActive() != &menu) return false;
        if (key == KEY_ESCAPE || (zinput && zinput->GetFirstBindedLogicalKey(key) == zLOGICKEY_SCREEN_LOG)) {
            closing = true;
            input.Discard();
            return false;
        }
        return closing || panel.Key(key);
    }

    void BeforeFrame(zCMenu* owner) {
        if (owner != &menu || zCMenu::GetActive() != &menu || !ready) return;
        if (input.TakeFocusLoss() || !HasFocus(window)) {
            RestoreMouse();
            input.Discard();
        }
        if (closing || menu.m_exitState != zCMenu::NONE || !zinput
            || !HasFocus(window) || !ClientBounds(window, client)) return;
        if (!cursorActive) {
            if (!listening) listening = input.Start(window);
            restoreEngineMouse = zinput->GetDeviceEnabled(zINPUT_MOUSE) != 0;
            if (restoreEngineMouse) {
                zinput->SetDeviceEnabled(zINPUT_MOUSE, FALSE);
                ApplyMouseAcquisition();
            }
            cursor.Enter(window, client, std::exchange(centerOnOpen, false));
            leftHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            rightHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            cursorActive = true;
        }
        const auto events = input.Take();
        const auto click = ClickAt(events.leftClick, VK_LBUTTON, leftHeld);
        if (ClickAt(events.rightClick, VK_RBUTTON, rightHeld) || (click && panel.CloseAt(*click))) {
            closing = true;
            menu.HandleEvent(KEY_ESCAPE);
            return;
        }
        if (click) panel.Click(*click);
        if (const auto wheel = ScreenPoint(client, events.wheelPosition); wheel && events.wheel) {
            panel.Wheel(*wheel, events.wheel);
        }
    }

    bool ReplaceDraw(zCView* native) {
        if (native != menu.m_pWindow) return false;
        if (ready && (closing || menu.m_exitState == zCMenu::BACK)) return true;
        if (zCMenu::GetActive() != &menu || !menu.m_screenInitDone) return false;
        if (!initialized) {
            initialized = true;
            ready = panel.Open(menu);
        }
        if (!ready || !panel.Update()) return false;
        panel.DrawItems();
        if (cursorActive && HasFocus(window)) {
            CURSORINFO systemCursor{};
            systemCursor.cbSize = sizeof(systemCursor);
            POINT position{};
            if ((!GetCursorInfo(&systemCursor) || !(systemCursor.flags & CURSOR_SHOWING) || !systemCursor.hCursor)
                && GetCursorPos(&position)) {
                if (const auto p = ScreenPoint(client, position)) panel.DrawFallbackCursor(*p);
            }
        }
        return true;
    }
};

JournalSession* activeJournal = nullptr;

void __fastcall AddEntry(oCLogTopic*, const zSTRING&);
auto AddEntryHook = Union::CreateHook(
    SIGNATURE_OF(&oCLogTopic::AddEntry), &AddEntry, Union::HookType::Hook_Detours);

void __fastcall AddEntry(oCLogTopic* topic, const zSTRING& entry) {
    const int before = topic->m_lstEntries.GetNumInList();
    AddEntryHook(topic, entry);
    recency.Record(topic, before, topic->m_lstEntries.GetNumInList());
}

void __fastcall ClearLog(oCLogManager*);
auto ClearLogHook = Union::CreateHook(
    SIGNATURE_OF(&oCLogManager::Clear), &ClearLog, Union::HookType::Hook_Detours);

void __fastcall ClearLog(oCLogManager* manager) {
    recency.Clear();
    ClearLogHook(manager);
}

void __fastcall UnarchiveLog(oCLogManager*, zCArchiver&);
auto UnarchiveLogHook = Union::CreateHook(
    SIGNATURE_OF(&oCLogManager::Unarchive), &UnarchiveLog, Union::HookType::Hook_Detours);

void __fastcall UnarchiveLog(oCLogManager* manager, zCArchiver& source) {
    recency.Clear();
    UnarchiveLogHook(manager, source);
}

void __fastcall ShowJournal(oCLogScreen*, void*);
auto ShowJournalHook = Union::CreateHook(
    SIGNATURE_OF(&oCLogScreen::Show), &ShowJournal, Union::HookType::Hook_Detours);

void __fastcall ShowJournal(oCLogScreen* self, void* edx) {
    if (!self->m_pLogMenu || !screen || activeJournal) {
        ShowJournalHook(self, edx);
        return;
    }
    JournalSession journal(*self->m_pLogMenu);
    activeJournal = &journal;
    ShowJournalHook(self, edx);
    activeJournal = nullptr;
}

void __fastcall Frame(zCMenu*, void*, int);
auto FrameHook = Union::CreateHook(
    SIGNATURE_OF(&zCMenu::HandleFrame), &Frame, Union::HookType::Hook_Detours);

void __fastcall Frame(zCMenu* self, void* edx, int render) {
    if (activeJournal) activeJournal->BeforeFrame(self);
    FrameHook(self, edx, render);
}

int __fastcall HandleKey(oCMenu_Log*, void*, int);
auto KeyHook = Union::CreateHook(
    SIGNATURE_OF(&oCMenu_Log::HandleEvent), &HandleKey, Union::HookType::Hook_Detours);

int __fastcall HandleKey(oCMenu_Log* self, void* edx, int key) {
    if (activeJournal && activeJournal->HandleKey(self, key)) return TRUE;
    return KeyHook(self, edx, key);
}

void __fastcall Draw(zCView*, void*);
auto DrawHook = Union::CreateHook(
    SIGNATURE_OF(&zCView::DrawItems), &Draw, Union::HookType::Hook_Detours);

void __fastcall Draw(zCView* self, void* edx) {
    if (!activeJournal || !activeJournal->ReplaceDraw(self)) DrawHook(self, edx);
}

} // namespace
} // namespace GOTHIC_NAMESPACE
