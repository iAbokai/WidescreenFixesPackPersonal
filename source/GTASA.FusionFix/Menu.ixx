module;

#include <stdafx.h>
#include "common.h"
#include "FixSettings.hpp"
#include <mmsystem.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#pragma comment(lib, "winmm.lib") // needed for timeBeginPeriod()/timeEndPeriod()

export module Menu;

import Skeleton;
import Options;
import Timer;

export enum eMenuScreen : int8_t
{
    SCREEN_GO_BACK = -2,
    SCREEN_NONE = -1,
    SCREEN_STATS = 0,
    SCREEN_NOP = 0,
    SCREEN_START_GAME,                     //  1  New Game, Load Game, Delete Game
    SCREEN_BRIEF,                          //  2
    SCREEN_AUDIO_SETTINGS,                 //  3
    SCREEN_DISPLAY_SETTINGS,               //  4
    SCREEN_MAP,                            //  5
    SCREEN_NEW_GAME_ASK,                   //  6  Are you sure you want to start a new game? All current game progress will be lost. Proceed?
    SCREEN_SELECT_GAME,                    //  7  Please select which new game you wish to start:
    SCREEN_MISSION_PACK_LOADING_ASK,       //  8  Are you sure you want to load a San Andreas Mission Pack? All current game progress will be lost. Proceed?
    SCREEN_LOAD_GAME,                      //  9  Select save file to load:
    SCREEN_DELETE_GAME,                    // 10  Select save file to delete:
    SCREEN_LOAD_GAME_ASK,                  // 11  All unsaved progress in your current game will be lost. Proceed with loading?
    SCREEN_DELETE_GAME_ASK,                // 12  Are you sure you wish to delete this save file?
    SCREEN_LOAD_FIRST_SAVE,                // 13
    SCREEN_DELETE_FINISHED,                // 14
    SCREEN_DELETE_SUCCESSFUL,              // 15  Delete Successful. Select OK to continue.
    SCREEN_GAME_SAVE,                      // 16  Select file you wish to save to:
    SCREEN_SAVE_WRITE_ASK,                 // 17  Are you sure you wish to save?
    SCREEN_SAVE_DONE_1,                    // 18
    SCREEN_SAVE_DONE_2,                    // 19  Save Successful. Select OK to continue.
    SCREEN_GAME_SAVED,                     // 20  OK
    SCREEN_GAME_LOADED,                    // 21  OK
    SCREEN_GAME_WARNING_DONT_SAVE,         // 22  Warning! One or more cheats have been activated. This may affect your save game. It is recommended that you do not save this game.
    SCREEN_ASK_DISPLAY_DEFAULT_SETS,       // 23  Are you sure you want to reset your current settings to default?
    SCREEN_ASK_AUDIO_DEFAULT_SETS,         // 24  Are you sure you want to reset your current settings to default?
    SCREEN_CONTROLS_RESET,                 // 25  Are you sure you want to reset your current settings to default?
    SCREEN_USER_TRACKS_OPTIONS,            // 26
    SCREEN_DISPLAY_ADVANCED,               // 27  DRAW DISTANCE, ...
    SCREEN_LANGUAGE,                       // 28  English, ...
    SCREEN_SAVE_GAME_DONE,                 // 29  O.K
    SCREEN_SAVE_GAME_FAILED,               // 30  Save Unsuccessful., O.K.
    SCREEN_SAVE_WRITE_FAILED,              // 31  Save Unsuccessful.
    SCREEN_SAVE_FAILED_FILE_ERROR,         // 32  Load Unsuccessful. File Corrupted, Please delete.
    SCREEN_OPTIONS,                        // 33  Controller Setup, Audio Setup ...
    SCREEN_MAIN_MENU,                      // 34  Start Game, Options, Quit Game
    SCREEN_QUIT_GAME_ASK,                  // 35  Are you sure you want to quit? All progress since the last save game will be lost. Proceed?
    SCREEN_CONTROLLER_SETUP,               // 36  CONFIGURATION, Redefine Controls ...
    SCREEN_REDEFINE_CONTROLS,              // 37  Foot Controls, Vehicle Controls
    SCREEN_CONTROLS_DEFINITION,            // 38  ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    SCREEN_MOUSE_SETTINGS,                 // 39
    SCREEN_JOYPAD_SETTINGS,                // 40
    SCREEN_PAUSE_MENU,                     // 41  Resume, START NEW GAME ...
    SCREEN_INITIAL,                        // 42  SCREEN_QUIT_GAME_2
    SCREEN_EMPTY,                          // 43

    SCREEN_COUNT,                          // Screen count
};

export enum CutsceneBordersMode : int8_t
{
    Off = 0,
    Letterbox = 1,
    Pillarbox = 2,
    Both = 3
};

export enum FrameLimiterMode : int8_t
{
    eOff, e30, e40, e50, e60, e75, e100, e120, e144, e165, e200, e240, e90, e180
};

// The Frame Limiter row's steps and their frame rates
static constexpr std::pair<int8_t, int> FrameLimits[] = { { FrameLimiterMode::eOff, 0 }, { FrameLimiterMode::e30, 30 },
    { FrameLimiterMode::e40, 40 }, { FrameLimiterMode::e50, 50 }, { FrameLimiterMode::e60, 60 }, { FrameLimiterMode::e75, 75 },
    { FrameLimiterMode::e90, 90 }, { FrameLimiterMode::e100, 100 }, { FrameLimiterMode::e120, 120 }, { FrameLimiterMode::e144, 144 },
    { FrameLimiterMode::e165, 165 }, { FrameLimiterMode::e180, 180 }, { FrameLimiterMode::e200, 200 }, { FrameLimiterMode::e240, 240 } };

static int FrameLimitOfStep(int8_t step)
{
    for (auto& [mode, fps] : FrameLimits)
    {
        if (mode == step)
            return fps;
    }
    return 0;
}

// The step of a frame rate: the nearest one (MAIN.FPSLimit may hold any rate)
static int8_t StepOfFrameLimit(int fps)
{
    if (fps <= 0)
        return FrameLimiterMode::eOff;
    int8_t step = FrameLimiterMode::e30;
    for (auto& [mode, rate] : FrameLimits)
    {
        if (rate && std::abs(rate - fps) < std::abs(FrameLimitOfStep(step) - fps))
            step = mode;
    }
    return step;
}

static int8_t NextFrameLimiter(int8_t current, int direction)
{
    static constexpr std::array<int8_t, 14> modes{ FrameLimiterMode::eOff, FrameLimiterMode::e30,
        FrameLimiterMode::e40, FrameLimiterMode::e50, FrameLimiterMode::e60, FrameLimiterMode::e75,
        FrameLimiterMode::e90, FrameLimiterMode::e100, FrameLimiterMode::e120, FrameLimiterMode::e144,
        FrameLimiterMode::e165, FrameLimiterMode::e180, FrameLimiterMode::e200, FrameLimiterMode::e240 };
    const auto it = std::find(modes.begin(), modes.end(), current);
    if (it == modes.end())
        return FrameLimiterMode::eOff;
    const auto index = static_cast<int>(it - modes.begin());
    return modes[(index + static_cast<int>(modes.size()) + direction) % static_cast<int>(modes.size())];
}

class CSprite2d
{
public:
    RwTexture* m_pTexture;
};

enum eRadarMode : int32_t
{
    RADAR_MODE_MAPS_AND_BLIPS = 0,
    RADAR_MODE_BLIPS_ONLY = 1,
    RADAR_MODE_OFF = 2,

    RADAR_MODE_COUNT
};

export class CMenuManager
{
    enum
    {
        MPACK_COUNT = 25,
        SPRITE_COUNT = 25,
    };

public:
    int8_t      m_nStatsScrollDirection;
    float     m_fStatsScrollSpeed;
    uint8_t     m_nSelectedRow;
    int32_t     m_CurrentGallerySelected;
    int32_t     m_CurrentMaxGalleryImages;
    int32_t     m_CurrentMaxRealPhotos;
    int32_t     m_CurrentSelectedRealPhoto;
    uint32_t    m_ReloadImageTime;
    bool      m_PrefsUseVibration;
    bool      m_bHudOn;
    eRadarMode m_nRadarMode;
    int32_t     field_28;
    int32_t     m_nTargetBlipIndex;
    int8_t      m_nSysMenu;
    bool      m_DisplayControllerOnFoot;
    bool      m_bDontDrawFrontEnd;
    bool      m_bActivateMenuNextFrame;
    bool      m_bMenuAccessWidescreen;
    bool      field_35;
    int32_t  m_KeyPressedCode;
    int32_t     m_PrefsBrightness;
    float     m_fDrawDistance;

    bool      m_bShowSubtitles;
    union
    {
        struct
        {
            bool m_ShowLocationsBlips;
            bool m_ShowContactsBlips;
            bool m_ShowMissionBlips;
            bool m_ShowOtherBlips;
            bool m_ShowGangAreaBlips;
        };
        bool m_abPrefsMapBlips[5];
    };
    bool      m_bMapLegend;
    int8_t      m_bWidescreenOn;
    int8_t      m_bPrefsFrameLimiter;
    bool      m_bRadioAutoSelect;
    bool      m_PrefsAudioOutputMode;
    int8_t      m_nSfxVolume;
    int8_t      m_nRadioVolume;
    bool      m_bRadioEq;

    int8_t  m_nRadioStation;
    bool      m_RecheckNumPhotos;
    int32_t     m_nCurrentScreenItem;
    bool      m_bQuitGameNoDVD;

    bool      m_bDrawingMap;
    bool      m_bStreamingDisabled;
    bool      m_bAllStreamingStuffLoaded;

    bool      m_bMenuActive;
    bool      m_bStartGameLoading;
    int8_t      m_nGameState;
    bool      m_bIsSaveDone;
    bool      m_bLoadingData;
    float     m_fMapZoom;
    CVector2D m_vMapOrigin;
    CVector2D m_vMousePos;
    bool      m_bMapLoaded;

    int32_t     m_nTitleLanguage;
    int32_t     m_nTextLanguage;
    uint8_t m_nPrefsLanguage;
    uint8_t m_nPreviousLanguage;
    int32_t     m_SystemLanguage;
    bool      m_LoadedLanguage;
    int32_t     m_ListSelection;
    int32_t     m_RenderScreenOnce;
    uint8_t* m_GalleryImgBuffer;
    RwRaster* m_GpJpgTex;
    bool      m_StartUpFrontEndRequestedForPads;
    int32_t     m_ScreenXOffset;
    int32_t     m_ScreenYOffset;
    uint32_t    m_UserTrackIndex;
    int8_t m_RadioMode;

    bool      m_bInvertPadX1;
    bool      m_bInvertPadY1;
    bool      m_bInvertPadX2;
    bool      m_bInvertPadY2;
    bool      m_bSwapPadAxis1;
    bool      m_bSwapPadAxis2;

    uint8_t   m_RedefiningControls;
    bool      m_DisplayTheMouse;
    int32_t     m_nMousePosX;
    int32_t     m_nMousePosY;
    bool      m_bPrefsMipMapping;
    bool      m_bTracksAutoScan;
    int32_t     m_nPrefsAntialiasing;
    int32_t     m_nDisplayAntialiasing;
    int8_t m_ControlMethod;
    int32_t     m_nPrefsVideoMode;
    int32_t     m_nDisplayVideoMode;
    int32_t     m_nCurrentRwSubsystem;

    int32_t     m_nMousePosWinX;
    int32_t     m_nMousePosWinY;

    bool      m_bSavePhotos;
    bool      m_bMainMenuSwitch;
    uint8_t     m_nPlayerNumber;
    bool      m_bLanguageChanged;
    int32_t     field_EC;
    int32_t* m_pPressedKey;
    bool      m_isPreInitialised;
    ///...

    eMenuScreen GetCurrentScreen() const
    {
        // The sprites following the partial layout end at the current screen.
        return *reinterpret_cast<const eMenuScreen*>(reinterpret_cast<const uint8_t*>(this) + 0x15D);
    }
};

export GameRef<CMenuManager> FrontendMenuManager([]() -> CMenuManager*
{
    auto pattern = hook::pattern("B9 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? 6A 01 E8 ? ? ? ? 83 C4 04 E8 ? ? ? ? C6 46 ? ? A1 ? ? ? ? 83 C5 14 47 83 C3 2C");
    return *pattern.get_first<CMenuManager*>(1);
});

// The gameplay frame rate limit (MAIN.FPSLimit, 0: none)
static int FrameLimit()
{
    static auto& fps = FixSettings().Int("MAIN.FPSLimit");
    return std::max(0, fps);
}

// The menus' frame rate limit (MAIN.FPSLimitMenus, 0: none, -1: Sync, the gameplay limit)
static int MenuFrameLimit()
{
    static auto& fps = FixSettings().Int("MAIN.FPSLimitMenus");
    return fps < 0 ? FrameLimit() : fps;
}

namespace CText
{
    void* TheText = nullptr;
    const char* (__fastcall* Get)(void*, void*, const char*) = nullptr;

    std::string BordersLabel, BordersValue, LimiterValue;

    const char* GetCutsceneBordersLabel()
    {
        BordersLabel = FixOptions::GameText("cutsceneborders");
        return BordersLabel.c_str();
    }

    const char* GetCutsceneBordersText()
    {
        const char* key = nullptr;
        switch (FrontendMenuManager->m_bWidescreenOn)
        {
            case CutsceneBordersMode::Off:
                return Get(TheText, 0, "FEM_OFF");
            case CutsceneBordersMode::Letterbox:
                key = "cutsceneborders.letterbox";
                break;
            case CutsceneBordersMode::Pillarbox:
                key = "cutsceneborders.pillarbox";
                break;
            case CutsceneBordersMode::Both:
                key = "cutsceneborders.both";
                break;
            default:
                return Get(TheText, 0, "FEM_ON");
        };
        BordersValue = FixOptions::GameText(key, true);
        return BordersValue.c_str();
    }

    const char* GetFrameLimiterText()
    {
        int fps = FrameLimit();
        if (!fps)
            return Get(TheText, 0, "FEM_OFF");
        LimiterValue = std::to_string(fps) + " FPS";
        return LimiterValue.c_str();
    }
}

// Cutscene Borders and the Frame Limiter are the game's rows of Display Setup and Advanced, saved in gta_sa.set, and
// the fix's MAIN.CutsceneBorders and MAIN.FPSLimit (FusionFixHub, presets): the .cfg decides at the start, and a change
// on either side is written to the other
namespace NativeRows
{
    int8_t SyncedBorders = -1;
    int8_t SyncedLimiter = -1;

    void ApplyBorders()
    {
        static auto& borders = FixSettings().Int("MAIN.CutsceneBorders");
        FrontendMenuManager->m_bWidescreenOn = int8_t(std::clamp(borders, 0, int(CutsceneBordersMode::Both)));
        SyncedBorders = FrontendMenuManager->m_bWidescreenOn;
    }

    void ApplyLimiter()
    {
        FrontendMenuManager->m_bPrefsFrameLimiter = StepOfFrameLimit(FrameLimit());
        SyncedLimiter = FrontendMenuManager->m_bPrefsFrameLimiter;
    }

    // The game's rows changed them (or its Restore Defaults)
    void Update()
    {
        if (FrontendMenuManager->m_bWidescreenOn != SyncedBorders)
        {
            SyncedBorders = FrontendMenuManager->m_bWidescreenOn;
            FixSettings().Set("MAIN.CutsceneBorders", std::to_string(SyncedBorders));
        }
        if (FrontendMenuManager->m_bPrefsFrameLimiter != SyncedLimiter)
        {
            SyncedLimiter = FrontendMenuManager->m_bPrefsFrameLimiter;
            FixSettings().Set("MAIN.FPSLimit", std::to_string(FrameLimitOfStep(SyncedLimiter)));
        }
    }
}

// "Sync" for the menus' limit (MAIN.FPSLimitMenus -1) is the gameplay limit, which only exists while the Frame Limiter
// is on: it becomes Off when the Frame Limiter is turned off, and an Off of the menus' limit becomes Sync again when
// the Frame Limiter is turned on again (the Frame Limiter Menu row of Options.ixx also leaves out Sync while there is
// no limit)
namespace MenuLimit
{
    bool GameLimited = true;

    void Update()
    {
        static auto& menus = FixSettings().Int("MAIN.FPSLimitMenus");
        const bool limited = FrameLimit() > 0;
        if (limited && !GameLimited && menus == 0)
            FixSettings().Set("MAIN.FPSLimitMenus", std::string("-1"));
        else if (!limited && menus < 0)
            FixSettings().Set("MAIN.FPSLimitMenus", std::string("0"));
        GameLimited = limited;
    }
}

enum RsEvent
{
    rsCAMERASIZE = 0,
    rsCOMMANDLINE = 1,
    rsFILELOAD = 2,
    rsINITDEBUG = 3,
    rsINPUTDEVICEATTACH = 4,
    rsLEFTBUTTONDOWN = 5,
    rsLEFTBUTTONUP = 6,
    rsMOUSEMOVE = 7,
    rsMOUSEWHEELMOVE = 8,
    rsPLUGINATTACH = 9,
    rsREGISTERIMAGELOADER = 10,

    rsRIGHTBUTTONDOWN = 11,
    rsRIGHTBUTTONUP = 12,
    rsMIDDLEBUTTONDOWN = 13,
    rsMIDDLEBUTTONUP = 14,
    rsMOUSEWHEELMOVEDUP = 15,
    rsMOUSEWHEELMOVEDDOWN = 16,
    rsFIRST_XBUTTONUP = 17,
    rsSECOND_XBUTTONUP = 18,
    rsFIRST_XBUTTONDOWN = 19,
    rsSECOND_XBUTTONDOWN = 20,

    rsRWINITIALIZE = 21,
    rsRWTERMINATE = 22,
    rsSELECTDEVICE = 23,
    rsINITIALIZE = 24,
    rsTERMINATE = 25,
    rsIDLE = 26,
    rsFRONTENDIDLE = 27,
    rsKEYDOWN = 28,
    rsKEYUP = 29,
    rsQUITAPP = 30,

    rsPADBUTTONDOWN = 31,
    rsPADBUTTONUP = 32,
    rsPADANALOGUELEFT = 33,
    rsPADANALOGUELEFTRESET = 34,
    rsPADANALOGUERIGHT = 35,
    rsPADANALOGUERIGHTRESET = 36,
    rsPREINITCOMMANDLINE = 37,
    rsACTIVATE = 38,
    rsSETMEMORYFUNCS = 39,
};

class FrameLimiter
{
public:
    enum FPSLimitMode
    {
        FPS_NONE,
        FPS_REALTIME,
        FPS_ACCURATE
    };

    FPSLimitMode mFPSLimitMode = FPS_NONE;

private:
    double TIME_Frequency = 0.0;
    double TIME_Ticks = 0.0;
    double TIME_Frametime = 0.0;
    float fFPSLimit = 0.0f;
    bool bFpsLimitWasUpdated = false;

public:
    void Init(FPSLimitMode mode, float fps_limit)
    {
        bFpsLimitWasUpdated = true;
        mFPSLimitMode = mode;
        fFPSLimit = fps_limit;

        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);

        static constexpr auto TICKS_PER_FRAME = 1;
        auto TICKS_PER_SECOND = (TICKS_PER_FRAME * fFPSLimit);

        if (mFPSLimitMode == FPS_ACCURATE)
        {
            TIME_Frametime = 1000.0 / (double)fFPSLimit;
            TIME_Frequency = (double)frequency.QuadPart / 1000.0;
        }
        else
        {
            TIME_Frequency = (double)frequency.QuadPart / (double)TICKS_PER_SECOND;
        }

        Ticks();
    }

    DWORD Sync_RT()
    {
        if (bFpsLimitWasUpdated)
        {
            bFpsLimitWasUpdated = false;
            return 1;
        }

        DWORD lastTicks, currentTicks;
        LARGE_INTEGER counter;

        QueryPerformanceCounter(&counter);
        lastTicks = (DWORD)TIME_Ticks;
        TIME_Ticks = (double)counter.QuadPart / TIME_Frequency;
        currentTicks = (DWORD)TIME_Ticks;

        return (currentTicks > lastTicks) ? currentTicks - lastTicks : 0;
    }

    DWORD Sync_SLP()
    {
        if (bFpsLimitWasUpdated)
        {
            bFpsLimitWasUpdated = false;
            return 1;
        }

        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        double millis_current = (double)counter.QuadPart / TIME_Frequency;
        double millis_delta = millis_current - TIME_Ticks;

        if (TIME_Frametime <= millis_delta)
        {
            TIME_Ticks = millis_current;
            return 1;
        }
        else if (TIME_Frametime - millis_delta > 2.0)
            Sleep(1);
        else
            Sleep(0);

        return 0;
    }

    void Sync()
    {
        if (fFPSLimit <= 0.0f)
            return;

        if (mFPSLimitMode == FPS_REALTIME)
            while (!Sync_RT());
        else if (mFPSLimitMode == FPS_ACCURATE)
            while (!Sync_SLP());
    }

private:
    void Ticks()
    {
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        TIME_Ticks = (double)counter.QuadPart / TIME_Frequency;
    }
};

static float GetFrameLimiterValue()
{
    return float(FrameLimit());
}

// Keep menu pacing independent of the gameplay limiter's timestamps.
static FrameLimiter menuFpsLimiter;

static void LimitMenuFrame()
{
    static bool initialized = false;
    static int limit = 0;
    const int wanted = MenuFrameLimit();
    if (wanted <= 0)
    {
        limit = 0;
        return;
    }

    // Started again when the limit changed, or after the menus ran without one
    if (!initialized || wanted != limit)
    {
        if (!initialized)
            timeBeginPeriod(1);
        menuFpsLimiter.Init(FrameLimiter::FPS_ACCURATE, static_cast<float>(wanted));
        initialized = true;
        limit = wanted;
    }
    menuFpsLimiter.Sync();
}

// Temporary diagnostics for the stutter of the dancing minigames at a 30 FPS limit: FusionFix_Frames.log in the game's folder
// has the frames that came much later or earlier than the limit asks for (how long the game worked, how long the limiter
// waited, the time step the game used) and a summary of every ten seconds of play. The menus and the frame after them are left out.
namespace FrameLog
{
    FILE* Out = nullptr;
    double Frequency = 0.0;
    double PreviousLeft = 0.0, WindowStart = 0.0, Average = 0.0;
    double Longest = 0.0, Shortest = 0.0, WorkTotal = 0.0, WorkLongest = 0.0;
    uint32_t Frames = 0, Slow = 0, Fast = 0, Lines = 0;

    double Now()
    {
        if (Frequency == 0.0)
        {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            Frequency = static_cast<double>(frequency.QuadPart) / 1000.0;
        }
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) / Frequency;
    }

    void Write(const char* format, ...)
    {
        if (!Out)
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (auto slash = strrchr(path, '\\'))
                slash[1] = 0;
            strcat_s(path, "FusionFix_Frames.log");
            fopen_s(&Out, path, "a");
            if (Out && ftell(Out) > 512 * 1024)
            {
                fclose(Out);
                fopen_s(&Out, path, "w");
            }
            if (!Out)
                return;
            fprintf(Out, "---- session (FusionFix frames log) ----\n");
        }
        char text[512];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        fprintf(Out, "[%10lu] %s\n", GetTickCount(), text);
        fflush(Out);
    }

    // The menus, loading and pauses: the next frame has nothing to be compared with
    void Restart()
    {
        PreviousLeft = 0.0;
    }

    void Summary(double now, int limit)
    {
        if (Frames)
            Write("10 s: %u frames, average %.2f ms (%.1f fps), shortest %.1f, longest %.1f | work average %.1f, longest %.1f | %u late, %u early (limit %d fps)",
                Frames, (now - WindowStart) / Frames, Frames * 1000.0 / (now - WindowStart), Shortest, Longest, WorkTotal / Frames, WorkLongest, Slow, Fast, limit);
        Frames = Slow = Fast = 0;
        Longest = WorkTotal = WorkLongest = 0.0;
        Shortest = 1.0e9;
        WindowStart = now;
    }

    // arrived: the frame reached the limiter, left: the limiter let it go
    void Frame(double arrived, double left, int limit)
    {
        const double previous = PreviousLeft;
        PreviousLeft = left;
        if (previous <= 0.0)
        {
            Summary(left, limit);
            return;
        }

        const double interval = left - previous;
        const double work = arrived - previous;
        const double target = limit > 0 ? 1000.0 / limit : Average;
        Average = Average <= 0.0 ? interval : Average + (interval - Average) * 0.05;
        ++Frames;
        WorkTotal += work;
        WorkLongest = std::max(WorkLongest, work);
        Longest = std::max(Longest, interval);
        Shortest = Shortest > 0.0 ? std::min(Shortest, interval) : interval;

        const bool late = target > 0.0 && interval > target * 1.25;
        const bool early = target > 0.0 && interval < target * 0.75;
        Slow += late;
        Fast += early;
        if ((late || early) && Lines < 400)
        {
            ++Lines;
            float step = 0.0f;
            try
            {
                if (CTimer::ms_fTimeStep.is_initialized())
                    step = CTimer::ms_fTimeStep;
            }
            catch (...) {}
            Write("%s frame: %.1f ms (limit %.1f) = work %.1f + wait %.1f | game's step of the frame before %.3f (%.1f ms)",
                late ? "late" : "early", interval, target, work, left - arrived, step, step * 20.0f);
            if (Lines == 400)
                Write("(the following frames are only counted)");
        }
        if (left - WindowStart >= 10000.0)
            Summary(left, limit);
    }
}

injector::hook_back<bool(__cdecl*)(RsEvent event, void* data)> hbRsEventHandler;
bool __cdecl RsEventHandler(RsEvent event, void* data)
{
    if (FrontendMenuManager->m_bMenuActive)
    {
        FrameLog::Restart();
        return hbRsEventHandler.fun(event, data);
    }

    static FrameLimiter fpsLimiter;
    static constexpr auto mode = FrameLimiter::FPS_ACCURATE;
    static bool bTimerResolutionSet = false;
    static float fLastFpsLimit = -1.0f;

    float fFpsLimit = GetFrameLimiterValue();

    if (fFpsLimit != fLastFpsLimit)
    {
        fLastFpsLimit = fFpsLimit;

        if (!bTimerResolutionSet)
        {
            timeBeginPeriod(1);
            bTimerResolutionSet = true;
        }

        RsGlobal->maxFPS = static_cast<int32_t>(fFpsLimit);
        fpsLimiter.Init(mode, fFpsLimit);
        FrameLog::Restart();
    }

    const double arrived = FrameLog::Now();
    fpsLimiter.Sync();
    FrameLog::Frame(arrived, FrameLog::Now(), static_cast<int>(fFpsLimit));

    return hbRsEventHandler.fun(event, data);
}

static SafetyHookInline DrawMenuHook;

static void __fastcall DrawMenuWithIndicators(uint8_t* manager, void*)
{
    FixOptions::BeginMenuDraw(manager);
    DrawMenuHook.unsafe_thiscall<void>(manager);
    FixOptions::DrawScrollIndicators();
}

class Menu
{
public:
    Menu()
    {
        WFP::onInitEvent() += []()
        {
            // Full screen used the lowest refresh rate of 60 Hz or more for the resolution, which holds the frame rate
            // at 60 wherever the picture is synced to the display: take the highest one instead
            auto pattern = hook::pattern("BD FF FF FF 7F 75 ? BB 16 00 00 00");
            injector::WriteMemory<uint32_t>(pattern.get_first(1), 0, true);
            pattern = hook::pattern("3B C5 73 ? 83 F8 3C");
            injector::WriteMemory<uint8_t>(pattern.get_first(2), 0x76, true); // jnb -> jbe
            pattern = hook::pattern("81 FD FF FF FF 7F 75 ? 5F");
            injector::WriteMemory<uint32_t>(pattern.get_first(2), 0, true);
        };

        WFP::onGameInitEvent() += []()
        {
            // The game has read gta_sa.set: the .cfg's values replace its two
            NativeRows::ApplyBorders();
            NativeRows::ApplyLimiter();
            OnSettingChange("MAIN.CutsceneBorders", NativeRows::ApplyBorders);
            OnSettingChange("MAIN.FPSLimit", NativeRows::ApplyLimiter);
            WFP::onGameProcessEvent() += NativeRows::Update;

            // The menus' limit follows the Frame Limiter with Sync; the game's Restore Defaults put the fix's limit on the
            // game's row too (its own default is 30)
            MenuLimit::GameLimited = FrameLimit() > 0;
            WFP::onGameProcessEvent() += MenuLimit::Update;
            FixOptions::OnLimiterRestored(NativeRows::ApplyLimiter);

            auto pattern = hook::pattern("B9 ? ? ? ? ? ? ? E8 ? ? ? ? 50 E8 ? ? ? ? 83 C4 ? 8B 46");
            CText::TheText = *pattern.get_first<void*>(1);
            CText::Get = (decltype(CText::Get))injector::GetBranchDestination(pattern.get_first(8)).as_int();

            pattern = hook::pattern("8B 4C 24 ? 8B 54 24 ? 53 51 52 E8");
            static auto GetMenuOptionText = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                auto ptr = (char*)(regs.ebx);
                if (ptr)
                {
                    if (std::string_view(ptr) == CText::Get(CText::TheText, 0, "FED_WIS"))
                    {
                        *(char**)&regs.ebx = const_cast<char*>(CText::GetCutsceneBordersLabel());
                    }
                }
            });

            static int cachedAction = -1;
            pattern = hook::pattern("83 C0 E8 83 F8 29");
            static auto GetCurrentAction = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                cachedAction = regs.eax;
            });

            pattern = hook::pattern("85 DB BF C0 01 00 00");
            static auto GetMenuOnOffText = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                // The values of the fix's own rows (Options.ixx)
                if (auto text = FixOptions::RightText(cachedAction))
                {
                    regs.esi = reinterpret_cast<uintptr_t>(text);
                    return;
                }

                auto ptr = (char*)(regs.esi);
                if (ptr)
                {
                    if (cachedAction == 0x18)
                    {
                        if (std::string_view(ptr) == CText::Get(CText::TheText, 0, "FEM_ON") || std::string_view(ptr) == CText::Get(CText::TheText, 0, "FEM_OFF"))
                        {
                            *(char**)&regs.esi = const_cast<char*>(CText::GetFrameLimiterText());
                        }
                    }
                    else if (cachedAction == 0x1A)
                    {
                        if (std::string_view(ptr) == CText::Get(CText::TheText, 0, "FEM_ON") || std::string_view(ptr) == CText::Get(CText::TheText, 0, "FEM_OFF"))
                        {
                            *(char**)&regs.esi = const_cast<char*>(CText::GetCutsceneBordersText());
                        }
                    }
                }
            });

            pattern = hook::pattern("8A 46 ? 84 C0 0F 94 C2 8B CE");
            injector::MakeNOP(pattern.get_first(), 13, true);
            static auto ProcessOnOffMenuOptionsHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                regs.ecx = regs.esi;
                auto& pref = FrontendMenuManager->m_bWidescreenOn;

                auto menuDirection = 1;
                if (*(int8_t*)(regs.esp + 0x8) == -1)
                    menuDirection = -1;
                pref += menuDirection;

                if (pref > CutsceneBordersMode::Both)
                    pref = CutsceneBordersMode::Off;
                if (pref < CutsceneBordersMode::Off)
                    pref = CutsceneBordersMode::Both;
            });

            pattern = hook::pattern("8A 46 ? 84 C0 0F 94 C0 8B CE 88 46 ? E8 ? ? ? ? 5F 5E 8A C3");
            injector::MakeNOP(pattern.get_first(), 13, true);
            static auto ProcessOnOffMenuOptionsHook2 = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext& regs)
            {
                regs.ecx = regs.esi;
                auto& pref = FrontendMenuManager->m_bPrefsFrameLimiter;

                auto menuDirection = 1;
                if (*(int8_t*)(regs.esp + 0x8) == -1)
                    menuDirection = -1;
                pref = NextFrameLimiter(pref, menuDirection);
            });

            pattern = hook::pattern("75 ? 80 3D ? ? ? ? ? 74 ? B9");
            injector::MakeNOP(pattern.get_first(), 2, true);

            pattern = hook::pattern("7A ? 57 6A ? E8");
            injector::MakeNOP(pattern.get_first(), 2, true);

            pattern = hook::pattern("E8 ? ? ? ? 83 C4 ? 39 1D ? ? ? ? 75 ? 89 3D");
            hbRsEventHandler.fun = injector::MakeCALL(pattern.get_first(0), RsEventHandler).get();

            pattern = hook::pattern("E8 ? ? ? ? 83 C4 ? E8 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? 8B CE");
            static auto DrawFrontEndHook = safetyhook::create_mid(pattern.get_first(), [](SafetyHookContext&)
            {
                LimitMenuFrame();
            });
            DrawMenuHook = safetyhook::create_inline(pattern.get_first<uint8_t>(-19), DrawMenuWithIndicators);
        };
    }
} Menu;
