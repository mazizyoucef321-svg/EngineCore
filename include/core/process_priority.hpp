#pragma once
#include <windows.h>
#include <system_error>

namespace EngineCore::Core {

enum class ProcessPriorityLevel {
    Normal = NORMAL_PRIORITY_CLASS,
    AboveNormal = ABOVE_NORMAL_PRIORITY_CLASS,
    High = HIGH_PRIORITY_CLASS
};

class ProcessPriorityManager {
public:
    // رفع أولوية العملية الحالية أو اللعبة المستهدفة عبر الـ PID
    static std::error_code SetProcessPriority(HANDLE processHandle, ProcessPriorityLevel level) noexcept;
    static std::error_code SetProcessPriorityByPid(DWORD processId, ProcessPriorityLevel level) noexcept;
};

} // namespace EngineCore::Core