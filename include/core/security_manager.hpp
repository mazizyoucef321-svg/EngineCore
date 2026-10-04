// =============================================================================
// EngineCore :: System Security & Privilege Manager (Header)
// =============================================================================
#pragma once

#include <windows.h>
#include <filesystem>
#include <string_view>

namespace EngineCore::Security
{

class SecurityManager
{
public:
    // Checks if the process currently holds Administrator rights
    [[nodiscard]] static bool IsRunAsAdmin() noexcept;

    // Requests a specific system privilege token from Windows OS
    [[nodiscard]] static bool RequestPrivilege(std::wstring_view privilegeName) noexcept;

    // Returns a safe, secured working directory for cache/logs (%LOCALAPPDATA%)
    [[nodiscard]] static std::filesystem::path GetSecuredAppDataPath(std::wstring_view subfolder);
};

} // namespace EngineCore::Security