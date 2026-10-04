#include "core/timer_service.hpp"
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

namespace EngineCore::Core {

    TimerService::~TimerService() {
        RestoreDefaultTimer();
    }

    bool TimerService::EnableHighPrecisionTimer(UINT resolutionMs) {
        if (m_isPinned) {
            return true;
        }

        if (timeBeginPeriod(resolutionMs) == TIMERR_NOERROR) {
            m_targetResolution = resolutionMs;
            m_isPinned = true;
            return true;
        }

        return false;
    }

    void TimerService::RestoreDefaultTimer() {
        if (m_isPinned) {
            timeEndPeriod(m_targetResolution);
            m_isPinned = false;
        }
    }

} // namespace EngineCore::Core