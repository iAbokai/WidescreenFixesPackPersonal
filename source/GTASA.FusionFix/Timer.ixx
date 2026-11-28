module;

#include <stdafx.h>

export module Timer;

export namespace CTimer
{
    GameRef<unsigned int> m_snTimeInMilliseconds([]() -> unsigned int*
    {
        auto pattern = hook::pattern("A1 ? ? ? ? 2B 46 ? 85 C0 89 44 24 ? DB 44 24 ? 7D ? D8 05 ? ? ? ? DA 76");
        return *pattern.get_first<unsigned int*>(1);
    });

    GameRef<unsigned int> m_snTimeInMillisecondsPauseMode([]() -> unsigned int*
    {
        auto pattern = hook::pattern("A1 ? ? ? ? 2B C2 01 44 8E");
        return *pattern.get_first<unsigned int*>(1);
    });

    GameRef<float> ms_fTimeStep([]() -> float*
    {
        auto pattern = hook::pattern("C7 05 ? ? ? ? ? ? ? ? C7 05 ? ? ? ? ? ? ? ? C7 05 ? ? ? ? ? ? ? ? FF 15");
        return *pattern.get_first<float*>(2);
    });
}

// Switches for trying the timing changes, read from a file named FusionFix_Experiment.txt next to the game's executable
// (the file holds a number, and is read again every second, so it can be changed while the game runs). Without the file
// nothing here does anything. The number is the sum of the changes to try:
//   1: the timers that read the time step in milliseconds keep the fractions of milliseconds also at 30 fps
//   2: the game's clock (CTimer::m_snTimeInMilliseconds) keeps the fractions of milliseconds
//   4: the gameplay frame limiter lets the frames go a few milliseconds early or late (the average stays)
export namespace Experiment
{
    struct State
    {
        int value = 0;
        bool exists = false;
        ULONGLONG checked = 0;
    };

    inline State& Poll()
    {
        static State state;
        const ULONGLONG now = GetTickCount64();
        if (state.checked && now - state.checked < 1000)
            return state;
        state.checked = now;
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (auto slash = strrchr(path, '\\'))
            slash[1] = 0;
        strcat_s(path, "FusionFix_Experiment.txt");
        state.exists = false;
        state.value = 0;
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            char text[32]{};
            DWORD read = 0;
            ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
            CloseHandle(file);
            state.exists = true;
            state.value = static_cast<int>(strtol(text, nullptr, 10));
        }
        return state;
    }

    inline int Value() { return Poll().value; }
    inline bool Exists() { return Poll().exists; }

    // A line of FusionFix_Timing.log (only while the file above exists)
    inline void Log(const char* format, ...)
    {
        if (!Exists())
            return;
        static FILE* out = nullptr;
        if (!out)
        {
            char path[MAX_PATH]{};
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (auto slash = strrchr(path, '\\'))
                slash[1] = 0;
            strcat_s(path, "FusionFix_Timing.log");
            fopen_s(&out, path, "a");
            if (out && ftell(out) > 512 * 1024)
            {
                fclose(out);
                fopen_s(&out, path, "w");
            }
            if (!out)
                return;
            fprintf(out, "---- session (FusionFix timing log) ----\n");
        }
        va_list args;
        va_start(args, format);
        vfprintf(out, format, args);
        va_end(args);
        fputc('\n', out);
        fflush(out);
    }
}
