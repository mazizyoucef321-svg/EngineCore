#include "core/process_priority.hpp"

namespace EngineCore::Core {

std::error_code ProcessPriorityManager::SetProcessPriority(HANDLE processHandle, ProcessPriorityLevel level) noexcept {
    if (!processHandle || processHandle == INVALID_HANDLE_VALUE) {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }

    if (!SetPriorityClass(processHandle, static_cast<DWORD>(level))) {
        return std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }

    return {};
}

std::error_code ProcessPriorityManager::SetProcessPriorityByPid(DWORD processId, ProcessPriorityLevel level) noexcept {
    // فتح handle العملية بطلب صلاحية تعديل المعلومات فقط (User-Mode Safe)
    HANDLE hProcess = OpenProcess(PROCESS_SET_INFORMATION, FALSE, processId);
    if (!hProcess) {
        return std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }

    std::error_code result = SetProcessPriority(hProcess, level);
    CloseHandle(hProcess);
    return result;
}

} // namespace EngineCore::Core