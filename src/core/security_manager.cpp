// =============================================================================
// EngineCore :: System Security & Privilege Manager (Implementation)
// =============================================================================
#include "core/security_manager.hpp"
#include <shlobj.h>

namespace EngineCore::Security
{

bool SecurityManager::IsRunAsAdmin() noexcept
{
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

bool SecurityManager::RequestPrivilege(std::wstring_view privilegeName) noexcept
{
    HANDLE tokenHandle = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tokenHandle))
    {
        return false;
    }

    TOKEN_PRIVILEGES tp{};
    LUID luid{};

    if (!LookupPrivilegeValueW(nullptr, privilegeName.data(), &luid))
    {
        CloseHandle(tokenHandle);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    const BOOL result = AdjustTokenPrivileges(tokenHandle, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
    const DWORD error = GetLastError();
    CloseHandle(tokenHandle);

    return (result && error == ERROR_SUCCESS);
}

std::filesystem::path SecurityManager::GetSecuredAppDataPath(std::wstring_view subfolder)
{
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &path)))
    {
        std::filesystem::path safePath(path);
        CoTaskMemFree(path);
        safePath /= L"EngineCore";
        safePath /= subfolder;
        std::filesystem::create_directories(safePath);
        return safePath;
    }
    return std::filesystem::current_path() / "Cache";
}

} // namespace EngineCore::Security