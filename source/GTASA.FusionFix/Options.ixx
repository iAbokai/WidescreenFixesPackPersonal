module;

#include <stdafx.h>
#include "common.h"
#include "FixSettings.hpp"
#include "Shared/RWGTA/FixMenu.hpp"
#include "Shared/RWGTA/RootPages.hpp"
#include <cmath>
#include <filesystem>
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
// out. In the main menu and in the pause menu, which share the frontend. The Language screen also gets the rows of the
// languages the US executable left out, when their text files are in the TEXT folder.
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
    constexpr int ScreenLanguage = 28;
    constexpr int16_t LineHeight = 30;      // the game's distance between rows placed one after another
    constexpr uint8_t ActionLabel = 1;      // the line of text above the rows (the first entry only)
    constexpr uint8_t ActionBase = 0xC0;    // the fix's rows: ActionBase + row; far from the game's ids (up to 0x41)
    constexpr uint8_t ActionOpen = 0xD0;
    constexpr uint8_t ActionJoined = 0xE0;  // the fix's rows on the game's screens: ActionJoined + index of Joined
    constexpr uint8_t ActionAntiAliasing = 0x2C;    // the game's Anti Aliasing row (FED_AAS)
    constexpr uint8_t TypeButton = 11, TypeValue = 12;
    constexpr uint8_t AlignLeft = 1, AlignCenter = 3;

    // CMenuManager
    constexpr size_t SelectedItem = 0x54;
    constexpr size_t Language = 0x84;
    constexpr size_t MainMenu = 0xE9;
    constexpr size_t CurrentPage = 0x15D;
    constexpr size_t HelperMessage = 0x1B18;    // the game's own notice in the helper text (0: none)
    constexpr size_t AntiAliasingActive = 0xC8; // the MSAA level in use (1: off) and the one chosen in the menu
    constexpr size_t AntiAliasingPref = 0xCC;

    CMenuScreen* aScreens = nullptr;
    uint8_t* Manager = nullptr;
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
    // ChangeAntiAliasing)
    struct JoinedSetting
    {
        const char* id;
        int screen;
        const char* after;
        const char* shown;
    };
    constexpr JoinedSetting Joined[] =
    {
        { "GRAPHICS.ConsoleGamma", ScreenDisplay, "FED_BRI", "MAIN.ConsoleGammaMenu" },
        { "MAIN.FPSLimitMenus", ScreenAdvanced, "FEM_FRM", nullptr },
        { "MAIN.FOVFactor", ScreenAdvanced, "FED_WIS", "MAIN.FOVMenu" },
    };
    bool bJoined = false;                   // the rows are on the game's screens
    bool JoinedWanted[std::size(Joined)]{}; // the .cfg shows each of them

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
    void SetText(const std::string& key, const std::string& utf8)
    {
        std::string text;
        for (auto c : RWGTAMenu::ToGameFont(utf8, false))
            text += char(c < 0x100 ? c : '?');
        Texts[key] = text;
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
    constexpr const char* NativeRows[] = { "MAIN.CutsceneBorders", "MAIN.FPSLimit", "GRAPHICS.AntiAliasing" };

    void OpenModel()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OpenModel), &self);
        auto hidden = JoinedHidden();
        hidden.insert(std::begin(NativeRows), std::end(NativeRows));
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

    int(__cdecl* MaxMultiSampling)() = nullptr;     // the game's call for the highest MSAA level of the graphics card

    // The game's Anti Aliasing row is its MSAA level (1: off, else the number of samples; Enter applies it, and the
    // game's own steps stop at 4), and the fix's SMAA (GRAPHICS.AntiAliasing) is one more choice: Off, SMAA, MSAA 2x,
    // 4x and 8x, each MSAA level when the graphics card supports it. SMAA is not drawn on top of MSAA, so choosing it
    // takes the MSAA level off right away (the game's own Enter on the row). Returns 0 for Enter (the game's), 1 when
    // the row changed, 2 when the game then applies it
    int ChangeAntiAliasing(uint8_t* manager, int dir)
    {
        if (!dir || !MaxMultiSampling)
            return 0;
        auto& pref = *reinterpret_cast<int32_t*>(manager + AntiAliasingPref);
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
            index = FixSettings().Int("GRAPHICS.AntiAliasing") == 1 ? 1 : 0;
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
        FixSettings().Set("GRAPHICS.AntiAliasing", std::string(index == 1 ? "1" : "0"));
        return index == 1 && *reinterpret_cast<int32_t*>(manager + AntiAliasingActive) > 1 ? 2 : 1;
    }

    // CMenuManager::ProcessMenuOptions(direction, &exit, enter); 0 when the row is not the fix's (or is the game's to
    // handle), 1 when handled here, 2 when handled and the game then takes Enter on the row
    int __cdecl ProcessOptions(uint8_t* manager, int32_t direction, int32_t enter)
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
        if (State == Mode::None && JoinedScreen(screen))
        {
            auto joined = JoinedRow(aScreens[screen].entries[option].action - ActionJoined);
            if (!joined)
                return false;
            Model.Change(*joined, dir > 0 ? 1 : dir < 0 ? -1 : 0, InGame(manager));
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
        switch (ProcessOptions(manager, int32_t(direction), int32_t(enter)))
        {
        case 1:
            return 1;
        case 2:
            // As if Enter was pressed on the row
            return reinterpret_cast<OptionsFunction>(OptionsChangePrevious)(manager, edx, 0, exit, 1);
        }
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

    // The selected row's description as the game's helper text: right-aligned at the bottom, wrapped inside the menu
    bool RowHelp(uint8_t* manager, void* edx)
    {
        auto row = bReady ? SelectedRow(manager) : nullptr;
        if (!row)
            return false;
        SetText("WFPHELP", Model.Help(*row));
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

    // The game's general notice (no key) gives way to the description of the fix's selected row
    void __fastcall HelperText(uint8_t* manager, void* edx, const char* key)
    {
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
                ++added;
            }
            if (RowsOf(page) + added > RowCount)
                return false;
        }
        bool any = false;
        for (int i = 0; i < int(std::size(Joined)); ++i)
        {
            if (!JoinedWanted[i])
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
        return any;
    }

    // The language screen of the US executable lists English and Spanish only, though the game switches to the other
    // three just as well (their handlers and file names are in the executable): their rows are added, for each of
    // them with its text file in the game's TEXT folder. Left alone when the screen is not as that executable has it
    void AddLanguageEntries()
    {
        auto& page = aScreens[ScreenLanguage];
        if (strncmp(page.name, "FEH_LAN", 8) != 0 || RowsOf(page) != 3 ||
            page.entries[0].action != 0x25 || strncmp(page.entries[0].name, "FEL_ENG", 8) != 0 ||
            page.entries[1].action != 0x29 || strncmp(page.entries[1].name, "FEL_SPA", 8) != 0 ||
            strncmp(page.entries[2].name, "FEDS_TB", 8) != 0)
            return;
        struct Language
        {
            uint8_t action;
            const char* name;
            const wchar_t* file;
        };
        const Language languages[] =
        {
            { 0x26, "FEL_FRE", L"FRENCH.GXT" },
            { 0x27, "FEL_GER", L"GERMAN.GXT" },
            { 0x28, "FEL_ITA", L"ITALIAN.GXT" },
        };
        wchar_t exe[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH))
            return;
        const auto folder = std::filesystem::path(exe).parent_path() / L"TEXT";
        int at = 1;
        for (auto& language : languages)
        {
            std::error_code error;
            if (!std::filesystem::is_regular_file(folder / language.file, error))
                continue;
            if (RowsOf(page) >= RowCount)
                break;
            for (int k = RowsOf(page); k > at; --k)
                page.entries[k] = page.entries[k - 1];
            page.entries[at] = page.entries[at + 1];
            page.entries[at].action = language.action;
            strncpy_s(page.entries[at].name, language.name, _TRUNCATE);
            ++at;
        }
    }

    // CMenuManager::SetDefaultPreferences at the game's Restore Defaults (after its question): the fix's rows of that
    // screen too
    void* SetDefaultsPrevious = nullptr;
    void __fastcall SetDefaults(uint8_t* manager, void* edx, int8_t screen)
    {
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*, int8_t)>(SetDefaultsPrevious)(manager, edx, screen);
        auto ids = JoinedIds(screen);
        if (screen == ScreenAdvanced && bReady)
            ids.insert("GRAPHICS.AntiAliasing");
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
            if (FixSettings().Int("GRAPHICS.AntiAliasing") == 1)
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
    // A text of the fix in the game's font, in the language of the menus
    std::string GameText(const char* key, bool upper = false)
    {
        std::string text;
        for (auto c : RWGTAMenu::ToGameFont(RWGTAMenu::PluginText(FixSettings(), LanguageTag(), key), upper))
            text += char(c < 0x100 ? c : '?');
        return text;
    }

    void BeginMenuDraw()
    {
        ValueAreas.bounds.clear();
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

            // French, German and Italian on the language screen, when their text files are there
            AddLanguageEntries();

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

            // The game's Anti Aliasing row also chooses SMAA: its MSAA level limit, from the row's own handler
            auto multisampling = hook::pattern("41 8B C1 83 F8 04 89 8E CC 00 00 00 7F ? E8 ? ? ? ? 39 86 CC 00 00 00");
            MaxMultiSampling = reinterpret_cast<decltype(MaxMultiSampling)>(injector::GetBranchDestination(multisampling.get_first(14)).as_int());

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

            bReady = true;
        };
    }
} OptionsInit;
