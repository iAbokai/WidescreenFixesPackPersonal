module;

#include <stdafx.h>
#include "common.h"
#include "FixSettings.hpp"
#include "Shared/RWGTA/FixMenu.hpp"
#include "Shared/RWGTA/RootPages.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

export module Options;

import Skeleton;

namespace MenuText
{
    template<typename Measure>
    std::string FitLabel(std::string label, float room, Measure&& measure)
    {
        if (!std::isfinite(room) || room <= 0.0f)
            return {};
        if (measure(label) <= room)
            return label;
        const std::string suffix{ "..." };
        if (measure(suffix) > room)
            return {};
        while (!label.empty())
        {
            label.pop_back();
            if (measure(label + suffix) <= room)
                return label + suffix;
        }
        return suffix;
    }
}

// The fix's settings in the game's frontend: "Plugins" at the end of the Options screen shows the list of plugins (the
// fix first, then the other plugins with ASI Settings, see Shared/RWGTA/FixMenu.hpp), and each page its settings with
// their values like Display Setup. Console gamma is a row of Display Setup (after Brightness), the menus' frame rate
// limit and the field of view rows of its Advanced screen (after the game's Frame Limiter and Widescreen). SMAA is a
// choice of the game's own Anti Aliasing row there (Off, SMAA, MSAA 2x, 4x, 8x). The game's Restore Defaults of those
// screens resets them too. MAIN.ConsoleGammaMenu, MAIN.FOVMenu and MAIN.CutsceneBordersMenu (.cfg) leave those rows
// out. In the main menu and in the pause menu, which share the frontend.
//
// San Andreas' screen table (MenuPages) has no screen the PC game leaves unused, so the Options screen shows the fix's
// rows while they are open: its rows and title are saved, replaced, and put back when the fix's pages are left (Back,
// Escape, or any other way off the screen). Nothing else of the frontend changes, and only one row is added to the
// Options screen, before its Back row; when another mod changed that row, the fix leaves the menu alone and the
// settings stay in the .cfg and FusionFixHub.
//
// The rows are answered through call sites, each calling the previous target (another mod's hook or the game): the
// row's and the screen's text (CText::Get), CMenuManager::UserInput (to fill the rows), CMenuManager::ProcessUserInput
// (Escape on the fix's pages goes back to their list) and CMenuManager::ProcessMenuOptions (accept, and left/right on
// rows with values). The value at the right of a row comes through Menu.ixx's hook at the end of the right text's
// switch in CMenuManager::DrawStandardMenus (RightText).
//
// A changed setting that applies at the next start puts the game's own label line (a first row of action 1, drawn
// at 60, 97 like the save screens' questions) above the page's rows, and the selected row's description, also of the
// fix's rows on Display Setup and Advanced, is the game's helper text at the bottom right (DisplayHelperText).

namespace
{
#pragma pack(push, 1)
    struct CMenuEntry
    {
        uint8_t action;
        char name[8];
        uint8_t type;           // 11: a button, 12: a row with a value (left and right change it)
        int8_t targetMenu;
        uint8_t pad1;
        int16_t x;
        int16_t y;
        uint8_t align;          // 1: left, 2: right, else centered
        uint8_t pad2;
    };

    struct CMenuScreen
    {
        char name[8];
        int8_t previousPage;
        int8_t startingButton;
        CMenuEntry entries[12];
    };
#pragma pack(pop)
    static_assert(sizeof(CMenuEntry) == 18 && sizeof(CMenuScreen) == 226);

    constexpr int RowCount = 12;
    constexpr int PageRows = 9;             // settings visible at once; one more slot includes Back
    constexpr int RootRows = 9;             // links of the list; Back follows
    constexpr int ScreenOptions = 33;
    constexpr int ScreenDisplay = 4;
    constexpr int ScreenAdvanced = 27;
    constexpr int16_t LineHeight = 30;      // the game's distance between rows placed one after another
    constexpr uint8_t ActionLabel = 1;      // the line of text above the rows (the first entry only)
    constexpr uint8_t ActionBase = 0xC0;    // the fix's rows: ActionBase + row; far from the game's ids (up to 0x41)
    constexpr uint8_t ActionOpen = 0xD0;
    constexpr uint8_t ActionJoined = 0xE0;  // the fix's rows on the game's screens: ActionJoined + index of Joined
    constexpr uint8_t ActionAntiAliasing = 0x2C;    // the game's Anti Aliasing row (FED_AAS)
    constexpr uint8_t ActionFrameLimiter = 0x18;    // the game's Frame Limiter row (FEM_FRM)
    constexpr uint8_t TypeButton = 11, TypeValue = 12;
    constexpr uint8_t AlignLeft = 1, AlignCenter = 3;

    // CMenuManager
    constexpr size_t SelectedItem = 0x54;
    constexpr size_t Language = 0x84;
    constexpr size_t MainMenu = 0xE9;
    constexpr size_t CurrentPage = 0x15D;
    constexpr size_t HelperMessage = 0x1B18;    // the game's own notice in the helper text (0: none)
    constexpr size_t HelperTimer = 0x1AEC;      // its time left (300 when it is set)
    constexpr int32_t HelperApply = 1;          // "Apply new setting" (FET_APP), shown until the choice is applied
    constexpr int32_t HelperRestored = 3;       // "Original setting restored" (FET_RSO), shown for a while
    constexpr size_t AntiAliasingActive = 0xC8; // the MSAA level in use (1: off) and the one chosen in the menu
    constexpr size_t AntiAliasingPref = 0xCC;

    CMenuScreen* aScreens = nullptr;
    uint8_t* Manager = nullptr;
    int(__cdecl* MaxMultiSampling)() = nullptr;     // RenderWare's highest MSAA level of the graphics card (none: not found)
    bool PendingSmaa = false;                       // SMAA chosen on the Anti Aliasing row; applied with Enter, like MSAA
    bool bReady = false;

    enum class Mode { None, Root, Page };
    Mode State = Mode::None;
    CMenuScreen SavedOptions{};
    int OpenRow = 0;
    int CurrentShownPage = 0;
    int RootRowCount = 0;

    RWGTAMenu::Model Model;
    RWGTAMenu::WheelInput Wheel;
    RWGTAMenu::ValueAreas ValueAreas;
    void* Audio = nullptr;
    void(__fastcall* NavigationSound)(void*, void*, int, float, float) = nullptr;
    void(__cdecl* DrawRect)(const float*, const uint8_t*) = nullptr;
    RWGTAMenu::RootPages Root;
    std::vector<const RWGTAMenu::Row*> ShownRows;
    bool LabelShown = false;                // the page starts with the restart line
    GameRef<float> RightJustifyWrap;        // CFont's left edge of right-aligned text
    float(__fastcall* StretchX)(uint8_t*, void*, float) = nullptr;

    // The fix's settings on the game's own screens, each after the game's row of that key. A row with a .cfg setting
    // ("shown", on by default, applies at the next start) is left out when that is off: the setting then stays in the
    // .cfg and FusionFixHub only. Anti Aliasing is not here: it is the game's row, which also chooses SMAA (see
    // ChangeAntiAliasing). A row "inPlace" has no line of its own: it takes turns with the game's row of that key in
    // the same line (see ShowMenuLimiter)
    struct JoinedSetting
    {
        const char* id;
        int screen;
        const char* after;
        const char* shown;
        bool inPlace;
    };
    constexpr JoinedSetting Joined[] =
    {
        { "GRAPHICS.ConsoleGamma", ScreenDisplay, "FED_BRI", "MAIN.ConsoleGammaMenu", false },
        { "MAIN.FPSLimitMenus", ScreenAdvanced, "FEM_FRM", nullptr, true },
        { "MAIN.FOVFactor", ScreenAdvanced, "FED_WIS", "MAIN.FOVMenu", false },
    };
    bool bJoined = false;                   // the rows are on the game's screens
    bool JoinedWanted[std::size(Joined)]{}; // the .cfg shows each of them

    // The Frame Limiter (the gameplay limit, the game's row) and the Frame Limiter Menu (the menus' limit, the fix's row)
    // share one line of the Advanced screen: it shows the Frame Limiter, Enter on it shows the Frame Limiter Menu in its
    // place (and Enter on that the Frame Limiter again), and leaving the screen puts the Frame Limiter back
    int LimiterLine = -1;                   // the screen's entry of the line (-1: the rows are not shared)
    int LimiterJoined = -1;                 // the index in Joined of the Frame Limiter Menu
    CMenuEntry GameLimiter{}, MenuLimiter{};
    bool MenuLimiterShown = false;
    void(*LimiterRestored)() = nullptr;     // Menu.ixx: the game's Restore Defaults put back its own limit

    void ShowMenuLimiter(bool menus)
    {
        if (LimiterLine < 0 || !aScreens)
            return;
        aScreens[ScreenAdvanced].entries[LimiterLine] = menus ? MenuLimiter : GameLimiter;
        MenuLimiterShown = menus;
    }

    // The settings on the game's screens: their rows are the ones on the screen, the others are the .cfg's alone
    std::set<std::string> JoinedIds(int screen = -1)
    {
        std::set<std::string> ids;
        for (size_t i = 0; i < std::size(Joined); ++i)
        {
            if (bJoined && JoinedWanted[i] && (screen < 0 || Joined[i].screen == screen))
                ids.insert(Joined[i].id);
        }
        return ids;
    }

    // Not on the fix's pages either: the rows on the game's screens, and the ones the .cfg turned off
    std::set<std::string> JoinedHidden()
    {
        std::set<std::string> ids;
        for (size_t i = 0; i < std::size(Joined); ++i)
        {
            if (bJoined || !JoinedWanted[i])
                ids.insert(Joined[i].id);
        }
        return ids;
    }
    std::map<std::string, std::string, std::less<>> Texts;  // the rows' and titles' text by key, in the game's font

    int Screen(uint8_t* manager) { return *reinterpret_cast<int8_t*>(manager + CurrentPage); }
    int32_t& Option(uint8_t* manager) { return *reinterpret_cast<int32_t*>(manager + SelectedItem); }
    bool InGame(uint8_t* manager) { return !*(manager + MainMenu); }

    // The language of the fix's texts and of the other plugins' pages: MAIN.TextLanguage (cfg and editors only, for
    // translations of the game that replaced one of its languages), or the game's language
    std::string LanguageTag()
    {
        auto& chosen = FixSettings().String("MAIN.TextLanguage");
        if (!chosen.empty() && chosen != "auto")
            return chosen;
        static const char* tags[] = { "en", "fr", "de", "it", "es" };
        int language = Manager ? *reinterpret_cast<int8_t*>(Manager + Language) : 0;
        return language >= 0 && language < int(std::size(tags)) ? tags[language] : "en";
    }

    // San Andreas' font is 8 bits with the same table as III and Vice City
    std::string InGameFont(const std::string& utf8)
    {
        std::string text;
        for (auto c : RWGTAMenu::ToGameFont(utf8, false))
            text += char(c < 0x100 ? c : '?');
        return text;
    }

    void SetText(const std::string& key, const std::string& utf8)
    {
        Texts[key] = InGameFont(utf8);
    }

    void SetEntry(CMenuEntry& entry, uint8_t action, const char* name, uint8_t type, int16_t x, int16_t y, uint8_t align)
    {
        entry = {};
        entry.action = action;
        strncpy_s(entry.name, name, _TRUNCATE);
        entry.type = type;
        entry.targetMenu = ScreenOptions;
        entry.x = x;
        entry.y = y;
        entry.align = align;
    }

    void FillRoot()
    {
        auto& screen = aScreens[ScreenOptions];
        for (auto& entry : screen.entries)
            entry = {};
        strncpy_s(screen.name, "WFPT0", _TRUNCATE);
        LabelShown = false;
        auto& pages = Model.Pages();
        RootRowCount = int(Root.Rows());
        int16_t y = 130;
        for (int i = 0; i < RootRowCount; ++i, y += 30)
        {
            char key[8];
            sprintf_s(key, "WFPL%02d", i);
            int page = Root.Page(size_t(i));
            SetText(key, page >= 0 ? pages[page].title : Model.Text(page == RWGTAMenu::RootPages::Previous ? "menu.previous" : "menu.next"));
            SetEntry(screen.entries[i], ActionBase + i, key, TypeButton, 320, y, AlignCenter);
        }
        SetEntry(screen.entries[RootRowCount], ActionBase + RootRowCount, "FEDS_TB", TypeButton, 320, y, AlignCenter);
    }

    // The entry of the page's first row: after the restart line when it is shown
    int FirstRowEntry()
    {
        return LabelShown ? 1 : 0;
    }

    void FillPage(uint8_t* manager)
    {
        auto& screen = aScreens[ScreenOptions];
        for (auto& entry : screen.entries)
            entry = {};
        strncpy_s(screen.name, "WFPT1", _TRUNCATE);
        ShownRows = Model.WindowRows(size_t(CurrentShownPage));
        // A changed setting that applies at the next start: the game's own label line above the rows says so
        bool label = Model.RestartRequired();
        if (label != LabelShown)
        {
            Option(manager) += label ? 1 : -1;
            LabelShown = label;
        }
        int first = FirstRowEntry();
        if (label)
        {
            SetText("WFPWARN", Model.Text("menu.restartwarning"));
            SetEntry(screen.entries[0], ActionLabel, "WFPWARN", 0, 0, 0, 0);
        }
        // A little closer than the game's rows, so that Back stays above the helper text; below the label too
        int count = int(ShownRows.size());
        int16_t y = label ? 136 : 127;
        int16_t step = label ? 27 : 28;
        for (int i = 0; i < count; ++i, y += step)
        {
            char key[8];
            sprintf_s(key, "WFPR%02d", i);
            if (!ShownRows[i])
            {
                SetEntry(screen.entries[first + i], ActionBase + i, "FEDS_TB", TypeButton, 320, y + 5, AlignCenter);
                continue;
            }
            SetText(key, Model.Label(*ShownRows[i]));
            SetText(std::string(key) + "V", Model.Value(*ShownRows[i]));
            auto type = ShownRows[i]->kind == RWGTAMenu::RowKind::Setting || ShownRows[i]->kind == RWGTAMenu::RowKind::Preset ? TypeValue : TypeButton;
            SetEntry(screen.entries[first + i], ActionBase + i, key, type, 57, y, AlignLeft);
        }
        Option(manager) = std::clamp(Option(manager), first, std::max(first, first + count - 1));
        if (size_t(CurrentShownPage) < Model.Pages().size())
            SetText("WFPT1", Model.Pages()[CurrentShownPage].title);
    }

    // Settings that are the game's own rows (Advanced's Widescreen, Frame Limiter and Anti Aliasing, Menu.ixx): not on
    // the fix's pages
    constexpr const char* NativeRows[] = { "MAIN.CutsceneBorders", "MAIN.FPSLimit" };

    void OpenModel()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OpenModel), &self);
        auto hidden = JoinedHidden();
        hidden.insert(std::begin(NativeRows), std::end(NativeRows));
        if (MaxMultiSampling)
            hidden.insert("GRAPHICS.AntiAliasing");
        Model.OpenPlugins(self, LanguageTag(), PageRows, hidden, true);
    }

    // The model's row of a setting on the game's screens
    const RWGTAMenu::Row* JoinedRow(int index)
    {
        if (index < 0 || index >= int(std::size(Joined)))
            return nullptr;
        for (auto& row : Model.JoinedRows())
        {
            if (row.setting && row.setting->id == Joined[index].id)
                return &row;
        }
        return nullptr;
    }

    void FillJoined()
    {
        for (int i = 0; i < int(std::size(Joined)); ++i)
        {
            char key[8];
            sprintf_s(key, "WFPJ%02d", i);
            auto row = JoinedRow(i);
            SetText(key, row ? Model.Label(*row) : std::string{});
            SetText(std::string(key) + "V", row ? Model.Value(*row) : std::string{});
        }
    }

    bool JoinedScreen(int screen) { return bJoined && (screen == ScreenDisplay || screen == ScreenAdvanced); }

    void Open(uint8_t* manager)
    {
        OpenModel();
        SetText("WFPT0", Model.Text("menu.plugins"));
        Root.Reset(Model.Pages().size(), RootRows);
        SavedOptions = aScreens[ScreenOptions];
        State = Mode::Root;
        FillRoot();
        Option(manager) = 0;
    }

    void Close()
    {
        if (State == Mode::None)
            return;
        aScreens[ScreenOptions] = SavedOptions;
        State = Mode::None;
        Model.Close();
        ShownRows.clear();
        LabelShown = false;
    }

    void ShowPage(uint8_t* manager, int page)
    {
        CurrentShownPage = page;
        Model.PageShown(size_t(page));
        State = Mode::Page;
        LabelShown = false;
        Option(manager) = 0;
        FillPage(manager);
    }

    void Back(uint8_t* manager)
    {
        if (State == Mode::Page)
        {
            State = Mode::Root;
            FillRoot();
            Option(manager) = int(Root.Selection(size_t(CurrentShownPage)));
        }
        else if (State == Mode::Root)
        {
            Close();
            Option(manager) = OpenRow;
        }
    }

    void Update(uint8_t* manager)
    {
        Manager = manager;
        if (MenuLimiterShown && Screen(manager) != ScreenAdvanced)
            ShowMenuLimiter(false);
        if (State == Mode::None)
        {
            int screen = Screen(manager);
            if (screen == ScreenOptions)
                SetText("WFPOPT", RWGTAMenu::PluginText(FixSettings(), LanguageTag(), "menu.plugins"));
            if (JoinedScreen(screen))
            {
                if (!Model.IsOpen())
                    OpenModel();
                FillJoined();
            }
            else if (Model.IsOpen())
            {
                Model.Close();
            }
            return;
        }
        if (Screen(manager) != ScreenOptions)
        {
            Close();
            return;
        }
        if (State == Mode::Page)
            FillPage(manager);
    }

    void* UserInputPrevious = nullptr;
    void __fastcall UserInput(uint8_t* manager, void* edx)
    {
        if (bReady)
            Update(manager);
        bool overValue = ValueAreas.Contains(
            float(*reinterpret_cast<int32_t*>(manager + 0xE0)), float(*reinterpret_cast<int32_t*>(manager + 0xE4)));
        int wheel{ Wheel.Begin(bReady && State == Mode::Page && Screen(manager) == ScreenOptions && !overValue) };
        if (wheel)
        {
            Model.Scroll(size_t(CurrentShownPage), wheel);
            FillPage(manager);
        }
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(UserInputPrevious)(manager, edx);
        Wheel.End();
        if (bReady)
            Update(manager);
    }

    // Escape on the fix's pages: back to their list, or to the Options screen
    void* ProcessUserInputPrevious = nullptr;
    void __fastcall ProcessUserInput(uint8_t* manager, void* edx, int32_t down, int32_t up, int32_t enter, int32_t exit, int32_t wheel)
    {
        if (bReady && State != Mode::None && Screen(manager) == ScreenOptions && uint8_t(exit & 0xFF))
        {
            Back(manager);
            exit = 0;
        }
        if (bReady && State == Mode::Page && Screen(manager) == ScreenOptions && (uint8_t(down) || uint8_t(up)))
        {
            if (NavigationSound)
                NavigationSound(Audio, nullptr, 3, 0.0f, 1.0f);
            int first = FirstRowEntry();
            Option(manager) = first + Model.Move(size_t(CurrentShownPage), Option(manager) - first, uint8_t(down) ? 1 : -1);
            FillPage(manager);
            down = up = 0;
        }
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int32_t, int32_t, int32_t, int32_t, int32_t)>(ProcessUserInputPrevious)(manager, edx, down, up, enter, exit, wheel);
    }

    // The Anti Aliasing row of the Advanced screen is the selected one
    bool AntiAliasingSelected(uint8_t* manager)
    {
        const int option = Option(manager);
        return Screen(manager) == ScreenAdvanced && option >= 0 && option < RowCount &&
            aScreens[ScreenAdvanced].entries[option].action == ActionAntiAliasing;
    }

    // Off or SMAA was chosen on the Anti Aliasing row and is not applied yet
    bool AntiAliasingPending()
    {
        return PendingSmaa != (FixSettings().Int("GRAPHICS.AntiAliasing") == 1);
    }

    // The game's Anti Aliasing row is its MSAA level (1: off, else the number of samples), chosen with left and right
    // and applied with Enter (the game's steps stop at 4), and SMAA (GRAPHICS.AntiAliasing) is one more choice that
    // works the same way: Off, SMAA, MSAA 2x, 4x and 8x, each MSAA level when the graphics card supports it. SMAA is not
    // drawn on top of MSAA, so choosing it also chooses no MSAA level. Returns false for Enter (the game applies its
    // MSAA level after this has applied SMAA) and true when the row was changed
    bool ChangeAntiAliasing(uint8_t* manager, int dir)
    {
        if (!MaxMultiSampling)
            return false;
        auto& pref = *reinterpret_cast<int32_t*>(manager + AntiAliasingPref);
        if (!dir)
        {
            PendingSmaa = PendingSmaa && pref <= 1;
            FixSettings().Set("GRAPHICS.AntiAliasing", std::string(PendingSmaa ? "1" : "0"));
            return false;
        }

        std::vector<int> levels;
        for (int level : { 2, 4, 8 })
        {
            if (level <= int(MaxMultiSampling()))
                levels.push_back(level);
        }
        const int count = 2 + int(levels.size());

        // Where the row is: 0 Off, 1 SMAA, then the levels; a level that is none of ours (the game's own 3x, from a
        // saved setting) counts as the last one below it, and the next step leaves it in the direction asked
        int index = 0;
        bool exact = true;
        if (pref <= 1)
        {
            index = PendingSmaa ? 1 : 0;
        }
        else
        {
            index = 1;
            for (int i = 0; i < int(levels.size()); ++i)
            {
                if (levels[i] <= pref)
                {
                    index = 2 + i;
                    exact = levels[i] == pref;
                }
            }
        }
        index += dir > 0 ? 1 : (exact ? -1 : 0);
        index = (index + count) % count;

        pref = index >= 2 ? levels[index - 2] : 1;
        PendingSmaa = index == 1;
        return true;
    }

    // CMenuManager::ProcessMenuOptions(direction, &exit, enter); true when the row was the fix's
    bool __cdecl ProcessOptions(uint8_t* manager, int32_t direction, int32_t enter)
    {
        Manager = manager;
        if (!bReady)
            return 0;
        int option = Option(manager);
        if (option < 0 || option >= RowCount)
            return 0;
        int dir = uint8_t(enter & 0xFF) ? 0 : int(int8_t(direction & 0xFF));
        int screen = Screen(manager);
        if (State == Mode::None && screen == ScreenAdvanced && aScreens[screen].entries[option].action == ActionAntiAliasing)
            return ChangeAntiAliasing(manager, dir);
        // Enter on the line of the two frame limiters shows the other one
        if (State == Mode::None && dir == 0 && screen == ScreenAdvanced && option == LimiterLine && JoinedScreen(screen))
        {
            ShowMenuLimiter(!MenuLimiterShown);
            FillJoined();
            return true;
        }
        if (State == Mode::None && JoinedScreen(screen))
        {
            const int index = aScreens[screen].entries[option].action - ActionJoined;
            auto joined = JoinedRow(index);
            if (!joined)
                return false;
            const int step = dir > 0 ? 1 : dir < 0 ? -1 : 0;
            Model.Change(*joined, step, InGame(manager));
            // "Sync" is the gameplay limit: there is none to follow while the Frame Limiter is off, so it is skipped
            if (index == LimiterJoined && step && FixSettings().Int("MAIN.FPSLimitMenus") < 0 && FixSettings().Int("MAIN.FPSLimit") <= 0)
                Model.Change(*joined, step, InGame(manager));
            FillJoined();
            return true;
        }
        if (screen != ScreenOptions)
            return false;
        int action = aScreens[ScreenOptions].entries[option].action;
        if (State == Mode::None)
        {
            if (action != ActionOpen)
                return false;
            if (dir == 0)
                Open(manager);
            return true;
        }
        int row = action - ActionBase;
        if (row < 0 || row >= RowCount)
            return false;
        if (State == Mode::Root)
        {
            if (dir != 0)
                return true;
            if (row < RootRowCount)
            {
                int page = Root.Page(size_t(row));
                if (Root.Move(page))
                {
                    FillRoot();
                    Option(manager) = 0;
                }
                else if (page >= 0)
                {
                    ShowPage(manager, page);
                }
            }
            else
            {
                Back(manager);
            }
            return true;
        }
        if (row >= int(ShownRows.size()) || !ShownRows[row])
        {
            if (dir == 0)
                Back(manager);
            return true;
        }
        Model.Change(*ShownRows[row], dir > 0 ? 1 : dir < 0 ? -1 : 0, InGame(manager));
        FillPage(manager);
        return true;
    }

    void* OptionsAcceptPrevious = nullptr;
    void* OptionsChangePrevious = nullptr;

    // thiscall (direction, &exit, enter), callee cleans 12 bytes: called as fastcall with three stack arguments, the
    // previous target gets ecx, edx and the arguments as they were
    using OptionsFunction = uint32_t(__fastcall*)(uint8_t*, void*, uint32_t, uint32_t, uint32_t);

    uint32_t __fastcall OptionsAccept(uint8_t* manager, void* edx, uint32_t direction, uint32_t exit, uint32_t enter)
    {
        if (ProcessOptions(manager, int32_t(direction), int32_t(enter)))
            return 1;
        return reinterpret_cast<OptionsFunction>(OptionsAcceptPrevious)(manager, edx, direction, exit, enter);
    }

    uint32_t __fastcall OptionsChange(uint8_t* manager, void* edx, uint32_t direction, uint32_t exit, uint32_t enter)
    {
        if (ProcessOptions(manager, int32_t(direction), int32_t(enter)))
            return 1;
        return reinterpret_cast<OptionsFunction>(OptionsChangePrevious)(manager, edx, direction, exit, enter);
    }

    const char* Answer(const char* key)
    {
        if (!bReady || !key || strncmp(key, "WFP", 3) != 0)
            return nullptr;
        auto it = Texts.find(std::string_view(key, strnlen(key, 8)));
        return it != Texts.end() ? it->second.c_str() : nullptr;
    }

    // CText::Get at the rows, the title, the label line and the helper text
    void* RowTextPrevious = nullptr;
    void* TitleTextPrevious = nullptr;
    void* LabelTextPrevious = nullptr;
    void* HelperKeyPrevious = nullptr;
    void* TheText = nullptr;                // CText's object

    // The game's own text of a key, as the helper text gets it
    const char* GameKeyText(const char* key)
    {
        if (!TheText || !HelperKeyPrevious)
            return nullptr;
        return reinterpret_cast<const char* (__fastcall*)(void*, void*, const char*)>(HelperKeyPrevious)(TheText, nullptr, key);
    }

    template<void*& Previous>
    const char* __fastcall AnsweredText(void* text, void* edx, const char* key)
    {
        if (auto answer = Answer(key))
            return answer;
        return reinterpret_cast<const char* (__fastcall*)(void*, void*, const char*)>(Previous)(text, edx, key);
    }

    // Keep labels within the space left by the right-aligned value, at the native font scale.
    using PrintStringFn = void(__cdecl*)(float, float, const char*);
    PrintStringFn TitlePrintPrevious = nullptr;
    void(__cdecl* RenderFontBuffer)() = nullptr;

    void __cdecl TitlePrint(float x, float y, const char* text)
    {
        TitlePrintPrevious(x, y, text);
        // Keep the title's font state before the setting labels are measured and drawn.
        if (bReady && State != Mode::None && RenderFontBuffer)
            RenderFontBuffer();
    }

    PrintStringFn LabelPrintPrevious = nullptr;
    PrintStringFn ValuePrintPrevious = nullptr;
    float(__cdecl* GetStringWidth)(const char*, bool, bool) = nullptr;
    GameRef<float> FontScaleX;
    float ValueRightEdge = 0.0f;

    void __cdecl ValuePrint(float x, float y, const char* text)
    {
        if (bReady && (State == Mode::Page || (Manager && JoinedScreen(Screen(Manager)))))
        {
            ValueRightEdge = x;
            if (State == Mode::Page && text && *text && GetStringWidth)
                ValueAreas.Add(x, y, GetStringWidth(text, true, false), 30.0f * SCREEN_HEIGHT / 448.0f, 8.0f * SCREEN_HEIGHT / 480.0f);
        }
        ValuePrintPrevious(x, y, text);
    }

    void __cdecl LabelPrint(float x, float y, const char* text)
    {
        bool joined = State == Mode::None && Manager && JoinedScreen(Screen(Manager));
        if (!bReady || (State != Mode::Page && !joined) || ValueRightEdge <= x || !text)
            return LabelPrintPrevious(x, y, text);
        const char* value = nullptr;
        size_t count = joined ? std::size(Joined) : ShownRows.size();
        for (size_t i = 0; i < count && !value; ++i)
        {
            char key[8];
            sprintf_s(key, joined ? "WFPJ%02d" : "WFPR%02d", int(i));
            auto label = Texts.find(std::string_view(key));
            if (label != Texts.end() && label->second.c_str() == text)
            {
                auto it = Texts.find(std::string(key) + "V");
                value = it != Texts.end() ? it->second.c_str() : "";
            }
        }
        if (!value)
            return LabelPrintPrevious(x, y, text);
        float room = ValueRightEdge - x - GetStringWidth(value, true, false) - GetStringWidth("WW", true, false);
        auto fitted = MenuText::FitLabel(text, room, [](const std::string& label)
        {
            return GetStringWidth(label.c_str(), true, false);
        });
        LabelPrintPrevious(x, y, fitted.c_str());
    }

    // The selected row of the fix: on its pages, or on Display Setup and Advanced while the game shows no notice of
    // its own there
    const RWGTAMenu::Row* SelectedRow(uint8_t* manager)
    {
        int option = Option(manager);
        if (option < 0 || option >= RowCount)
            return nullptr;
        int screen = Screen(manager);
        if (State == Mode::Page && screen == ScreenOptions)
        {
            int row = aScreens[ScreenOptions].entries[option].action - ActionBase;
            return row >= 0 && row < int(ShownRows.size()) ? ShownRows[row] : nullptr;
        }
        if (State == Mode::None && JoinedScreen(screen) && Model.IsOpen() && !*reinterpret_cast<int32_t*>(manager + HelperMessage))
            return JoinedRow(aScreens[screen].entries[option].action - ActionJoined);
        return nullptr;
    }

    // CMenuManager::DisplayHelperText at the end of DrawStandardMenus
    using HelperFn = void(__fastcall*)(uint8_t*, void*, const char*);
    void* HelperTextPrevious = nullptr;

    // The line of the two frame limiters is selected, with no notice of the game's own
    bool LimiterLineSelected(uint8_t* manager)
    {
        return LimiterLine >= 0 && State == Mode::None && Screen(manager) == ScreenAdvanced && Option(manager) == LimiterLine &&
            !*reinterpret_cast<int32_t*>(manager + HelperMessage);
    }

    // WFPHELP while it is the notice of the line of the two frame limiters (the pad's buttons replace its keys)
    const char* LimiterHelpText = nullptr;

    // The selected row's description as the game's helper text: right-aligned at the bottom, wrapped inside the menu. The
    // line of the two frame limiters has the game's notice of adjusting a value and, under it, that the other limiter is
    // one press away
    bool RowHelp(uint8_t* manager, void* edx)
    {
        LimiterHelpText = nullptr;
        if (!bReady)
            return false;
        if (LimiterLineSelected(manager))
        {
            std::string text;
            if (auto adjust = GameKeyText("FET_MIG"))
                text = std::string(adjust) + "~n~";
            Texts["WFPHELP"] = text + InGameFont(RWGTAMenu::PluginText(FixSettings(), LanguageTag(), "menufps.switch.keys"));
            LimiterHelpText = Texts["WFPHELP"].c_str();
        }
        else
        {
            auto row = SelectedRow(manager);
            if (!row)
                return false;
            SetText("WFPHELP", Model.Help(*row));
        }
        if (Texts["WFPHELP"].empty())
            return false;
        float wrap = RightJustifyWrap.is_initialized() ? *RightJustifyWrap : 0.0f;
        if (RightJustifyWrap.is_initialized() && StretchX)
            *RightJustifyWrap = StretchX(manager, nullptr, 40.0f);
        reinterpret_cast<HelperFn>(HelperTextPrevious)(manager, edx, "WFPHELP");
        if (RightJustifyWrap.is_initialized())
            *RightJustifyWrap = wrap;
        return true;
    }

    // The game's notice "Apply new setting" is for an MSAA level that was chosen and not applied. Off and SMAA are chosen
    // the same way, but are no level of the game: its notice is set for them here, and it takes it back by itself (as for
    // a level) once the choice is the one in use, and shows "Original setting restored" when the choice is dropped (see
    // BeginMenuDraw)
    void AntiAliasingNotice(uint8_t* manager)
    {
        if (bReady && MaxMultiSampling && State == Mode::None && manager && AntiAliasingSelected(manager) && AntiAliasingPending())
            *reinterpret_cast<int32_t*>(manager + HelperMessage) = HelperApply;
    }

    // The game's general notice (no key) gives way to the description of the fix's selected row
    void __fastcall HelperText(uint8_t* manager, void* edx, const char* key)
    {
        if (!key)
            AntiAliasingNotice(manager);
        if (!key && RowHelp(manager, edx))
            return;
        reinterpret_cast<HelperFn>(HelperTextPrevious)(manager, edx, key);
    }

    bool AddOptionsEntry()
    {
        auto& options = aScreens[ScreenOptions];
        if (strncmp(options.name, "FET_OPT", 8) != 0)
            return false;
        int back = -1;
        for (int i = 0; i < RowCount; ++i)
        {
            if (!options.entries[i].action)
                break;
            if (strncmp(options.entries[i].name, "WFPOPT", 8) == 0)
                return false;
            if (strncmp(options.entries[i].name, "FEDS_TB", 8) == 0)
                back = i;
        }
        if (back < 1 || back + 1 >= RowCount || options.entries[back + 1].action)
            return false;
        options.entries[back + 1] = options.entries[back];
        options.entries[back] = options.entries[back - 1];
        options.entries[back].action = ActionOpen;
        strncpy_s(options.entries[back].name, "WFPOPT", _TRUNCATE);
        options.entries[back].type = TypeButton;
        options.entries[back].targetMenu = ScreenOptions;
        options.entries[back].x = options.entries[back].y = 0;
        OpenRow = back;
        return true;
    }

    int RowsOf(const CMenuScreen& page)
    {
        int count = 0;
        while (count < RowCount && page.entries[count].action)
            ++count;
        return count;
    }

    int EntryNamed(const CMenuScreen& page, const char* name)
    {
        int count = RowsOf(page);
        for (int i = 0; i < count; ++i)
        {
            if (strncmp(page.entries[i].name, name, 8) == 0)
                return i;
        }
        return -1;
    }

    // The fix's rows on Display Setup and its Advanced screen, each after its game row, as rows with a value placed
    // one line below the previous; the rows the game places at a fixed height below them move down by as much. Left
    // alone unless every anchor is there and each screen keeps room for them. The Cutscene Borders row (the game's
    // Widescreen, Menu.ixx) is taken out when MAIN.CutsceneBordersMenu is off: the rows below it move up.
    bool AddJoinedEntries()
    {
        for (size_t i = 0; i < std::size(Joined); ++i)
        {
            JoinedWanted[i] = !Joined[i].shown || FixSettings().Bool(Joined[i].shown);
        }
        for (int screen : { ScreenDisplay, ScreenAdvanced })
        {
            auto& page = aScreens[screen];
            if (strncmp(page.name, "FEH_DIS", 8) != 0)
                return false;
            int added = 0;
            for (size_t i = 0; i < std::size(Joined); ++i)
            {
                if (Joined[i].screen != screen || !JoinedWanted[i])
                    continue;
                if (EntryNamed(page, Joined[i].after) < 0)
                    return false;
                if (!Joined[i].inPlace)
                    ++added;
            }
            if (RowsOf(page) + added > RowCount)
                return false;
        }
        bool any = false;
        for (int i = 0; i < int(std::size(Joined)); ++i)
        {
            if (!JoinedWanted[i] || Joined[i].inPlace)
                continue;
            auto& page = aScreens[Joined[i].screen];
            int count = RowsOf(page);
            int at = EntryNamed(page, Joined[i].after) + 1;
            for (int k = count; k > at; --k)
            {
                page.entries[k] = page.entries[k - 1];
                if (page.entries[k].y)
                    page.entries[k].y += LineHeight;
            }
            char key[8];
            sprintf_s(key, "WFPJ%02d", i);
            SetEntry(page.entries[at], ActionJoined + i, key, TypeValue, 0, 0, page.entries[at - 1].align);
            page.entries[at].targetMenu = int8_t(Joined[i].screen);
            any = true;
        }
        // After the rows that follow it (the field of view's) were placed
        if (!FixSettings().Bool("MAIN.CutsceneBordersMenu"))
        {
            auto& page = aScreens[ScreenAdvanced];
            int at = EntryNamed(page, "FED_WIS");
            if (at >= 0 && page.entries[at].action == 0x1A && !page.entries[at].y)
            {
                int count = RowsOf(page);
                for (int k = at; k + 1 < count; ++k)
                {
                    page.entries[k] = page.entries[k + 1];
                    if (page.entries[k].y)
                        page.entries[k].y -= LineHeight;
                }
                page.entries[count - 1] = {};
            }
        }
        // The rows that share the line of a game row, once the lines have found their places
        for (int i = 0; i < int(std::size(Joined)); ++i)
        {
            if (!JoinedWanted[i] || !Joined[i].inPlace)
                continue;
            auto& page = aScreens[Joined[i].screen];
            int line = EntryNamed(page, Joined[i].after);
            if (line < 0 || page.entries[line].action != ActionFrameLimiter || Joined[i].screen != ScreenAdvanced)
                continue;
            char key[8];
            sprintf_s(key, "WFPJ%02d", i);
            GameLimiter = page.entries[line];
            SetEntry(MenuLimiter, ActionJoined + i, key, TypeValue, GameLimiter.x, GameLimiter.y, GameLimiter.align);
            MenuLimiter.targetMenu = int8_t(Joined[i].screen);
            LimiterLine = line;
            LimiterJoined = i;
            any = true;
        }
        return any;
    }

    // CMenuManager::SetDefaultPreferences at the game's Restore Defaults (after its question): the fix's rows of that
    // screen too. Display Setup and Advanced share the game's defaults, which put back its own Frame Limiter (30 FPS):
    // the fix's limit replaces it
    void* SetDefaultsPrevious = nullptr;
    void __fastcall SetDefaults(uint8_t* manager, void* edx, int8_t screen)
    {
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int8_t)>(SetDefaultsPrevious)(manager, edx, screen);
        auto ids = JoinedIds(screen);
        if (screen == ScreenAdvanced && bReady && MaxMultiSampling)
            ids.insert("GRAPHICS.AntiAliasing");
        if (screen == ScreenDisplay || screen == ScreenAdvanced)
        {
            RWGTAMenu::RestoreDefaults(FixSettings(), { "MAIN.FPSLimit" });
            if (LimiterRestored)
                LimiterRestored();
        }
        if (!ids.empty())
        {
            RWGTAMenu::RestoreDefaults(FixSettings(), ids);
            if (Model.IsOpen())
                FillJoined();
        }
    }
}

export namespace FixOptions
{
    // From Menu.ixx's hook at the end of CMenuManager::DrawStandardMenus' right text switch
    const char* RightText(int action)
    {
        // The game's Anti Aliasing row: the MSAA level named by its samples, or SMAA when none is chosen (see
        // ChangeAntiAliasing); Off is the game's own text
        if (bReady && State == Mode::None && action == ActionAntiAliasing && Manager && MaxMultiSampling)
        {
            const int pref = *reinterpret_cast<int32_t*>(Manager + AntiAliasingPref);
            if (pref > 1)
            {
                static char text[16];
                sprintf_s(text, "MSAA %dx", pref);
                return text;
            }
            if (PendingSmaa)
                return "SMAA";
        }
        if (bReady && State == Mode::None && action >= ActionJoined && action < ActionJoined + int(std::size(Joined)))
        {
            char key[8];
            sprintf_s(key, "WFPJ%02dV", action - ActionJoined);
            auto it = Texts.find(std::string_view(key));
            return it != Texts.end() && !it->second.empty() ? it->second.c_str() : nullptr;
        }
        if (!bReady || State != Mode::Page)
            return nullptr;
        int row = action - ActionBase;
        if (row < 0 || row >= int(ShownRows.size()) || !ShownRows[row])
            return nullptr;
        char key[8];
        sprintf_s(key, "WFPR%02dV", row);
        auto it = Texts.find(std::string_view(key));
        return it != Texts.end() && !it->second.empty() ? it->second.c_str() : nullptr;
    }
}

export namespace FixOptions
{
    // Menu.ixx: called after the game's Restore Defaults of Display Setup and Advanced, once the fix's Frame Rate Limit
    // is back to its default, to put it on the game's Frame Limiter row
    void OnLimiterRestored(void(*callback)())
    {
        LimiterRestored = callback;
    }

    // A text of the fix in the game's font, in the language of the menus
    std::string GameText(const char* key, bool upper = false)
    {
        std::string text;
        for (auto c : RWGTAMenu::ToGameFont(RWGTAMenu::PluginText(FixSettings(), LanguageTag(), key), upper))
            text += char(c < 0x100 ? c : '?');
        return text;
    }

    // ButtonIcons.ixx: the notice of the line of the two frame limiters with the pad's buttons ({A}...), when that is the
    // text about to be drawn (empty: it is not)
    std::string PadHelpOf(const char* text)
    {
        if (!text || text != LimiterHelpText)
            return {};
        return GameText("padhelp.adjust") + "~n~" + GameText("menufps.switch.pad");
    }

    void BeginMenuDraw(uint8_t* manager)
    {
        ValueAreas.bounds.clear();

        // The line of the two frame limiters shows the game's again once the screen is left
        if (bReady && MenuLimiterShown && manager && Screen(manager) != ScreenAdvanced)
            ShowMenuLimiter(false);

        // The game drops an MSAA level that was chosen on the Anti Aliasing row but not applied once the cursor leaves
        // the row (or the screen), and the row shows the level in use again, with its notice "Original setting
        // restored" on Display Setup and Advanced. Off and SMAA are the same choice made by this fix: they are dropped at
        // the same moment, back to what the setting says, with the same notice
        if (bReady && MaxMultiSampling && State == Mode::None && manager && !AntiAliasingSelected(manager) && AntiAliasingPending())
        {
            PendingSmaa = FixSettings().Int("GRAPHICS.AntiAliasing") == 1;
            const int screen = Screen(manager);
            if (screen == ScreenDisplay || screen == ScreenAdvanced)
            {
                *reinterpret_cast<int32_t*>(manager + HelperMessage) = HelperRestored;
                *reinterpret_cast<int32_t*>(manager + HelperTimer) = 300;
            }
        }
    }

    void DrawScrollIndicators()
    {
        if (!bReady || !DrawRect || State != Mode::Page)
            return;
        const uint8_t colour[] = { 225, 225, 225, 255 };
        // Between the label line and the first row when the label is shown
        float top = LabelShown ? 132.0f : 113.0f;
        RWGTAMenu::DrawIndicators(Model.HiddenAbove(size_t(CurrentShownPage)), Model.HiddenBelow(size_t(CurrentShownPage)),
            12.0f, top, 430.0f, SCREEN_WIDTH, SCREEN_HEIGHT / 480.0f, colour, DrawRect);
    }
}

// Diagnostic log for the freeze after applying an MSAA level on Anti Aliasing (FusionFix_MSAA.log in the game's folder):
// the game applies the level with a Reset of the Direct3D device (RwD3D9ChangeMultiSamplingLevels) and, when that fails,
// with a second Reset of the old settings, after which a device that stays lost shows a frozen picture. The log has the
// parameters and the result of every Reset, the state of the device and the window around the change, and the device's
// state every half second for ten seconds after it. Nothing is written unless a level is applied or the device is reset.
namespace MsaaLog
{
    FILE* Out = nullptr;
    uint32_t* GameLevel = nullptr;      // the MSAA level in use
    uint32_t* GameMaxLevel = nullptr;   // the highest level the device offers
    uint32_t* GameFlags = nullptr;      // bit 0: exclusive (full screen)
    SafetyHookInline ChangeHook, VideoModeHook, ResetHook;
    SafetyHookMid CheckSite, ReleasedSite, BuiltSite, Reset1Site, Reset1Result, Reset2Site, Reset2Result;
    IDirect3DDevice9** GameDevice = nullptr;        // the game's variable with its device
    D3DPRESENT_PARAMETERS* GameParams = nullptr;    // the parameters it resets the device with
    void* HookedEntry = nullptr;
    DWORD WatchUntil = 0, NextWatch = 0;

    void Write(const char* format, ...)
    {
        if (!Out)
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (auto slash = strrchr(path, '\\'))
                slash[1] = 0;
            strcat_s(path, "FusionFix_MSAA.log");
            fopen_s(&Out, path, "a");
            if (Out && ftell(Out) > 512 * 1024)
            {
                fclose(Out);
                fopen_s(&Out, path, "w");
            }
            if (!Out)
                return;
            fprintf(Out, "---- session (FusionFix MSAA log) ----\n");
        }
        char text[1536];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        fprintf(Out, "[%10lu] %s\n", GetTickCount(), text);
        fflush(Out);
    }

    const char* Hr(HRESULT hr)
    {
        switch (hr)
        {
        case D3D_OK: return "OK";
        case D3DERR_DEVICELOST: return "DEVICELOST";
        case D3DERR_DEVICENOTRESET: return "DEVICENOTRESET";
        case D3DERR_INVALIDCALL: return "INVALIDCALL";
        case D3DERR_OUTOFVIDEOMEMORY: return "OUTOFVIDEOMEMORY";
        case D3DERR_DRIVERINTERNALERROR: return "DRIVERINTERNALERROR";
        case D3DERR_NOTAVAILABLE: return "NOTAVAILABLE";
        case D3DERR_WASSTILLDRAWING: return "WASSTILLDRAWING";
        case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
        default: return "?";
        }
    }

    IDirect3DDevice9* Device()
    {
        try
        {
            return pD3D9Device.is_initialized() ? pD3D9Device.get() : nullptr;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    void WriteParams(const char* what, const D3DPRESENT_PARAMETERS& pp)
    {
        Write("%s: windowed=%d %ux%u fmt=%d backbuffers=%u msaa=%d quality=%lu swap=%d flags=0x%lX depth=%d/%d refresh=%u interval=0x%X window=%p",
            what, pp.Windowed, pp.BackBufferWidth, pp.BackBufferHeight, int(pp.BackBufferFormat), pp.BackBufferCount,
            int(pp.MultiSampleType), pp.MultiSampleQuality, int(pp.SwapEffect), pp.Flags, pp.EnableAutoDepthStencil,
            int(pp.AutoDepthStencilFormat), pp.FullScreen_RefreshRateInHz, pp.PresentationInterval, pp.hDeviceWindow);
    }

    void Dump(const char* when)
    {
        auto device = Device();
        if (!device)
        {
            Write("%s: no device", when);
            return;
        }
        const HRESULT coop = device->TestCooperativeLevel();
        Write("%s: cooperative level %s (0x%08lX) | game: msaa=%lu max=%lu flags=0x%lX", when, Hr(coop), static_cast<unsigned long>(coop),
            GameLevel ? *GameLevel : 0ul, GameMaxLevel ? *GameMaxLevel : 0ul, GameFlags ? *GameFlags : 0ul);
        IDirect3DSwapChain9* chain = nullptr;
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain)
        {
            if (SUCCEEDED(chain->GetPresentParameters(&pp)))
                WriteParams("  swap chain", pp);
            chain->Release();
        }
        D3DDEVICE_CREATION_PARAMETERS creation{};
        if (SUCCEEDED(device->GetCreationParameters(&creation)) && creation.hFocusWindow)
        {
            RECT rect{};
            GetWindowRect(creation.hFocusWindow, &rect);
            Write("  window %p: style=0x%08lX exstyle=0x%08lX rect=%ld,%ld,%ld,%ld iconic=%d foreground=%d", creation.hFocusWindow,
                static_cast<unsigned long>(GetWindowLongW(creation.hFocusWindow, GWL_STYLE)),
                static_cast<unsigned long>(GetWindowLongW(creation.hFocusWindow, GWL_EXSTYLE)),
                rect.left, rect.top, rect.right, rect.bottom, IsIconic(creation.hFocusWindow),
                GetForegroundWindow() == creation.hFocusWindow);
        }
    }

    HRESULT __stdcall ResetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp)
    {
        if (pp)
            WriteParams("Reset with", *pp);
        const HRESULT hr = ResetHook.unsafe_stdcall<HRESULT>(device, pp);
        Write("Reset -> %s (0x%08lX)", Hr(hr), static_cast<unsigned long>(hr));
        return hr;
    }

    void HookReset()
    {
        auto device = Device();
        if (ResetHook || !device)
            return;
        auto table = *reinterpret_cast<void***>(device);
        HookedEntry = table[16];
        ResetHook = safetyhook::create_inline(table[16], ResetDetour);
        Write("hooked IDirect3DDevice9::Reset at %p (%s) in %s", table[16], ResetHook ? "ok" : "FAILED", ModuleOf(table[16]).c_str());
    }

    // The game's own calls of Reset in the function that changes the level (the first with the new settings, the second
    // with the old ones when the first failed), whatever the device's table says
    void LogResetCall(const char* which)
    {
        void* device = GameDevice ? *GameDevice : nullptr;
        void** table = device ? *reinterpret_cast<void***>(device) : nullptr;
        Write("%s: Reset of the game's device %p (table %p, Reset entry %p) | the fix's device %p, hooked entry %p", which, device, table,
            table ? table[16] : nullptr, Device(), HookedEntry);
        if (GameParams)
            WriteParams("  parameters", *GameParams);
    }

    bool Is(const uint8_t* function, size_t at, std::initializer_list<uint8_t> bytes)
    {
        return std::equal(bytes.begin(), bytes.end(), function + at);
    }

    // The code of the function as it is in memory now, to compare with the game's file (something else may have changed it)
    void DumpCode(const uint8_t* function)
    {
        Write("the function that changes the level is at %p", function);
        for (size_t at = 0; at < 0x210; at += 32)
        {
            char line[32 * 3 + 1]{};
            for (size_t i = 0; i < 32; ++i)
                snprintf(line + i * 3, 4, "%02X ", function[at + i]);
            Write("code +0x%03zX: %s", at, line);
        }
    }

    void Trace(SafetyHookMid& hook, const uint8_t* function, size_t at, std::initializer_list<uint8_t> expected, const char* name, void (*destination)(SafetyHookContext&))
    {
        if (!Is(function, at, expected))
        {
            Write("trace '%s' at +0x%zX: the code there is not the game's, not installed", name, at);
            return;
        }
        hook = safetyhook::create_mid(const_cast<uint8_t*>(function) + at, destination);
        Write("trace '%s' at +0x%zX: %s", name, at, hook ? "installed" : "NOT installed");
    }

    std::string ModuleOf(void* address)
    {
        char path[MAX_PATH]{};
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(address), &module))
            GetModuleFileNameA(module, path, MAX_PATH);
        return path;
    }

    int __cdecl ChangeDetour(int level)
    {
        Write("==== ChangeMultiSamplingLevels(%d)", level);
        HookReset();
        Dump("before");
        const int result = ChangeHook.unsafe_ccall<int>(level);
        Write("ChangeMultiSamplingLevels returned %d%s", result, result ? "" : " (failed: the device may be left lost)");
        Dump("after");
        NextWatch = GetTickCount();
        WatchUntil = NextWatch + 10000;
        return result;
    }

    int __cdecl VideoModeDetour(int mode)
    {
        Write("RsSetVideoMode(%d)", mode);
        const int result = VideoModeHook.unsafe_ccall<int>(mode);
        Write("RsSetVideoMode returned %d", result);
        Dump("after video mode");
        return result;
    }

    void Watch()
    {
        if (!WatchUntil)
            return;
        const DWORD now = GetTickCount();
        if (int(now - WatchUntil) > 0)
        {
            WatchUntil = 0;
            Write("watch ended");
            return;
        }
        if (int(now - NextWatch) < 0)
            return;
        NextWatch = now + 500;
        Dump("watch");
    }

    void Install()
    {
        auto change = hook::pattern("A1 ? ? ? ? 83 EC 38 85 C0 53 55 56 57 0F 84");
        if (!change.empty())
        {
            auto function = change.get_first<uint8_t>();
            auto global = [&](int offset, uint8_t second) -> uint32_t*
            {
                return function[offset] == 0x8B && function[offset + 1] == second ? *reinterpret_cast<uint32_t**>(function + offset + 2) : nullptr;
            };
            GameLevel = global(0x14, 0x15);
            GameMaxLevel = global(0x3C, 0x0D);
            GameFlags = global(0x64, 0x35);
            ChangeHook = safetyhook::create_inline(function, ChangeDetour);

            // Inside the function (offsets of the game's 1.0 code, each checked against the code that is really there)
            DumpCode(function);
            if (function[0xBF] == 0xA1)
                GameDevice = *reinterpret_cast<IDirect3DDevice9***>(function + 0xC0);
            if (function[0x31] == 0xBE)
                GameParams = *reinterpret_cast<D3DPRESENT_PARAMETERS**>(function + 0x32);
            Trace(CheckSite, function, 0x91, { 0x85, 0xC0, 0x7D }, "CheckDeviceMultiSampleType", [](SafetyHookContext& regs)
            {
                const HRESULT hr = static_cast<HRESULT>(regs.eax);
                Write("CheckDeviceMultiSampleType: level %lu, windowed argument %d -> %s (0x%08lX)", GameLevel ? *GameLevel : 0ul,
                    static_cast<int>(regs.esi), Hr(hr), static_cast<unsigned long>(hr));
            });
            Trace(ReleasedSite, function, 0xBF, { 0xA1 }, "after the release", [](SafetyHookContext&)
            {
                Write("the game's resources are released, it goes on to build the parameters");
            });
            Trace(BuiltSite, function, 0xE1, { 0xA1 }, "parameters built", [](SafetyHookContext&)
            {
                if (GameParams)
                    WriteParams("parameters built by the game", *GameParams);
            });
            Trace(Reset1Site, function, 0xF1, { 0xFF, 0x51, 0x40, 0x85, 0xC0, 0x0F, 0x8C }, "Reset 1", [](SafetyHookContext&) { LogResetCall("Reset 1 (the new level)"); });
            Trace(Reset1Result, function, 0xF6, { 0x0F, 0x8C }, "Reset 1 result", [](SafetyHookContext& regs)
            {
                Write("Reset 1 -> %s (0x%08lX)", Hr(static_cast<HRESULT>(regs.eax)), static_cast<unsigned long>(regs.eax));
            });
            Trace(Reset2Site, function, 0x1FE, { 0xFF, 0x52, 0x40, 0x85, 0xC0, 0x7D }, "Reset 2", [](SafetyHookContext&) { LogResetCall("Reset 2 (the old settings, after Reset 1 failed)"); });
            Trace(Reset2Result, function, 0x203, { 0x7D }, "Reset 2 result", [](SafetyHookContext& regs)
            {
                Write("Reset 2 -> %s (0x%08lX)", Hr(static_cast<HRESULT>(regs.eax)), static_cast<unsigned long>(regs.eax));
            });
        }
        auto videoMode = hook::pattern("8B 44 24 04 50 E8 ? ? ? ? 83 C4 04 E8 ? ? ? ? A1 ? ? ? ? 8B 48 60");
        if (!videoMode.empty())
            VideoModeHook = safetyhook::create_inline(videoMode.get_first<uint8_t>(), VideoModeDetour);
        WFP::onBeforeReset() += []()
        {
            // The game repeats it every frame while the device is lost
            static unsigned count = 0;
            if (++count <= 5 || count % 1000 == 0)
                Write("the fix releases its Direct3D resources for a Reset (#%u)", count);
        };
        WFP::onGameProcessEvent() += Watch;
        HookReset();
    }
}

class OptionsInit
{
public:
    OptionsInit()
    {
        WFP::onGameInitEvent() += []()
        {
            auto screens = hook::pattern("0F BE 86 ? ? ? ? 8B 4E 54 69 C0 E2 00 00 00 8D 14 C9 0F B6 94 50");
            auto rowText = hook::pattern("57 E8 ? ? ? ? 8B D8 89 5C 24");
            auto titleText = hook::pattern("81 C2 ? ? ? ? 52 B9 ? ? ? ? E8 ? ? ? ? 8B 4C 24");
            auto userInput = hook::pattern("8A 46 5B 50 E8 ? ? ? ? 8B CE E8");
            auto processUserInput = hook::pattern("50 51 52 8B CE E8 ? ? ? ? 5E 83 C4 18 C3");
            auto accept = hook::pattern("8D 54 24 ? 52 6A 00 8B CE E8 ? ? ? ? 8A 44 24");
            auto change = hook::pattern("8D 44 24 ? 50 53 8B CE E8 ? ? ? ? 53 8B CE E8");
            auto labelPrint = hook::pattern("53 51 52 E8 ? ? ? ? 83 C4 0C");
            auto valuePrint = hook::pattern("56 50 D8 E1 51 D9 1C 24 DD D8 E8");
            auto setScale = hook::pattern("52 50 E8 ? ? ? ? 6A 02");
            auto stringWidth = hook::pattern("53 56 8B B4 24 A4 01 00 00 56 C7 44 24");

            aScreens = reinterpret_cast<CMenuScreen*>(*screens.get_first<uint8_t*>(23) - offsetof(CMenuScreen, entries));

            // MAIN.PluginsMenu (.cfg, applies at the next start): off leaves the Options screen without its Plugins row;
            // the rows on Display Setup and Advanced stay
            const bool pluginsRow = FixSettings().Bool("MAIN.PluginsMenu");
            if (pluginsRow && !AddOptionsEntry())
                return;

            // The fix's rows on Display Setup and Advanced, reset by the game's Restore Defaults there
            auto restore = hook::pattern("33 D2 8A 90 ? ? ? ? 8B CE 52 E8 ? ? ? ? 0F BE 86 ? ? ? ? 69 C0 E2 00 00 00");
            if (AddJoinedEntries())
            {
                bJoined = true;
                SetDefaultsPrevious = injector::MakeCALL(restore.get_first(11), SetDefaults, true).get();
            }

            // The game's Anti Aliasing row also chooses SMAA, which needs RenderWare's highest MSAA level of the graphics
            // card (the function reading it); an executable where it is not found keeps the game's own row
            auto multisampling = hook::pattern("A1 2C 24 8E 00 C3");
            if (!multisampling.empty())
                MaxMultiSampling = reinterpret_cast<decltype(MaxMultiSampling)>(multisampling.get_first());
            // The choice shown follows the setting until the row is changed in the menu (and again when the setting is
            // changed by a preset or an editor)
            PendingSmaa = FixSettings().Int("GRAPHICS.AntiAliasing") == 1;
            OnSettingChange("GRAPHICS.AntiAliasing", []() { PendingSmaa = FixSettings().Int("GRAPHICS.AntiAliasing") == 1; });

            RowTextPrevious = injector::MakeCALL(rowText.get_first(1), AnsweredText<RowTextPrevious>, true).get();
            TitleTextPrevious = injector::MakeCALL(titleText.get_first(12), AnsweredText<TitleTextPrevious>, true).get();
            auto renderFontBuffer = hook::pattern("A1 ? ? ? ? 83 EC 18 3D ? ? ? ? 0F 84 ? ? ? ? 0F BF 0D");
            RenderFontBuffer = reinterpret_cast<decltype(RenderFontBuffer)>(renderFontBuffer.get_first());
            // Menu mods that hide the page title replace its print call with NOPs; it stays hidden then.
            if (*titleText.get_first<uint8_t>(28) == 0xE8)
                TitlePrintPrevious = reinterpret_cast<PrintStringFn>(injector::MakeCALL(titleText.get_first(28), TitlePrint, true).as_int());
            UserInputPrevious = injector::MakeCALL(userInput.get_first(11), UserInput, true).get();
            ProcessUserInputPrevious = injector::MakeCALL(processUserInput.get_first(5), ProcessUserInput, true).get();
            OptionsAcceptPrevious = injector::MakeCALL(accept.get_first(9), OptionsAccept, true).get();
            OptionsChangePrevious = injector::MakeCALL(change.get_first(8), OptionsChange, true).get();
            auto scale = injector::GetBranchDestination(setScale.get_first(2)).get<uint8_t>();
            const auto scaleAt = reinterpret_cast<uintptr_t>(scale + 8);
            hook::range_pattern(scaleAt, scaleAt + 5, "A3 ? ? ? ?").get_first();
            {
                FontScaleX.SetAddress(*reinterpret_cast<float**>(scale + 9));
                GetStringWidth = reinterpret_cast<decltype(GetStringWidth)>(stringWidth.get_first<uint8_t>(-6));
                LabelPrintPrevious = reinterpret_cast<PrintStringFn>(injector::MakeCALL(labelPrint.get_first(3), LabelPrint, true).as_int());
                ValuePrintPrevious = reinterpret_cast<PrintStringFn>(injector::MakeCALL(valuePrint.get_first(10), ValuePrint, true).as_int());
            }
            auto wheelButtons = hook::pattern("A0 ? ? ? ? 83 C4 04 84 C0 74 4D B0 01 C2 04 00 6A 00 E8");
            Wheel.buttons = *wheelButtons.get_first<uint8_t*>(1);
            // CSprite2d::DrawRect, at the end of the user tracks scan's bar in CMenuManager::DrawBackground
            auto rectangle = hook::pattern("50 8D 54 24 ? 52 E8 ? ? ? ? 83 C4 08 E8 ? ? ? ? 8B CE E8");
            DrawRect = reinterpret_cast<decltype(DrawRect)>(injector::GetBranchDestination(rectangle.get_first(6)).as_int());

            // The restart line: CText::Get of the label's key in DrawStandardMenus (before y = 97.0)
            auto labelText = hook::pattern("E8 ? ? ? ? 39 35 ? ? ? ? 75 ? C7 44 24 ? 00 00 C2 42");
            LabelTextPrevious = injector::MakeCALL(labelText.get_first(), AnsweredText<LabelTextPrevious>, true).get();

            // The helper text: DrawStandardMenus' switch of the screens with one (movsx eax, [ebp+15Dh]; ... push 0;
            // mov ecx, ebp; call DisplayHelperText), the CText::Get of a key given to DisplayHelperText, its x
            // (StretchX(610.0)), and CFont's right-justify wrap (SetRightJustifyWrap(10.0) after the label line)
            auto helper = hook::pattern("0F BE 85 5D 01 00 00 83 F8 28 77 ? 0F B6 90 ? ? ? ? FF 24 95 ? ? ? ? 6A 00 8B CD E8");
            auto helperKey = hook::pattern("85 C0 74 ? 50 B9 ? ? ? ? E8 ? ? ? ? 68 FF 00 00 00");
            auto helperX = hook::pattern("68 00 80 18 44 E8 ? ? ? ? 51 D9 1C 24");
            auto wrap = hook::pattern("68 00 00 20 41 E8 ? ? ? ? 83 C4 08 80 BD 5D 01 00 00 26");
            HelperKeyPrevious = injector::MakeCALL(helperKey.get_first(10), AnsweredText<HelperKeyPrevious>, true).get();
            TheText = *helperKey.get_first<void*>(6);
            HelperTextPrevious = injector::MakeCALL(helper.get_first(30), HelperText, true).get();
            // The Options screen has no helper text of its own: the fix's pages get theirs before the switch
            static auto PageHelpHook = safetyhook::create_mid(helper.get_first(), [](SafetyHookContext& regs)
            {
                auto manager = reinterpret_cast<uint8_t*>(regs.ebp);
                if (bReady && State == Mode::Page && Screen(manager) == ScreenOptions)
                    RowHelp(manager, nullptr);
            });
            StretchX = reinterpret_cast<decltype(StretchX)>(injector::GetBranchDestination(helperX.get_first(5)).as_int());
            auto setWrap = injector::GetBranchDestination(wrap.get_first(5)).get<uint8_t>();
            if (!memcmp(setWrap, "\x8B\x44\x24\x04\xA3", 5))
                RightJustifyWrap.SetAddress(*reinterpret_cast<float**>(setWrap + 5));

            auto navigationSound = hook::pattern("68 00 00 80 3F 6A 00 6A 03 B9 ? ? ? ? E8 ? ? ? ? 0F BE 96");
            Audio = *navigationSound.get_first<void*>(10);
            NavigationSound = reinterpret_cast<decltype(NavigationSound)>(injector::GetBranchDestination(navigationSound.get_first(14)).as_int());

            auto oldWheelButtons = hook::pattern("A0 ? ? ? ? 84 C0 74 0C A0 ? ? ? ? 84 C0 75 03 B0 01 C3");
            for (size_t i = 0; i < oldWheelButtons.size(); ++i)
            {
                auto match = oldWheelButtons.get(i);
                if (*match.get<uint8_t*>(1) == Wheel.buttons)
                    Wheel.oldButtons = *match.get<uint8_t*>(10);
            }

            MsaaLog::Install();
            bReady = true;
        };
    }
} OptionsInit;
