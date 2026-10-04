#pragma once

#include <windows.h>

namespace EngineCore::Core {

    class TimerService {
    public:
        TimerService() = default;
        ~TimerService();

        // Enforces high-resolution system timer (0.5ms / 1.0ms)
        bool EnableHighPrecisionTimer(UINT resolutionMs = 1);

        // Restores the default system timer resolution
        void RestoreDefaultTimer();

    private:
        UINT m_targetResolution{0};
        bool m_isPinned{false};
    };

} // namespace EngineCore::Core