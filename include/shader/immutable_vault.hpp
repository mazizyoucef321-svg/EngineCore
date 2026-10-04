// =============================================================================
// EngineCore :: Immutable Core Vault & Deterministic PSO Protection
// =============================================================================
#pragma once

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace EngineCore::Shader
{

// -----------------------------------------------------------------------------
// Security Manager for Low-Level DACL File Lock Primitives
// -----------------------------------------------------------------------------
class VaultSecurityManager
{
public:
    // Configures Windows DACL security descriptors to explicitly DENY deletion to OS/Drivers
    [[nodiscard]] static bool ApplyImmutableDaclLock(const std::filesystem::path& filePath) noexcept
    {
        if (!std::filesystem::exists(filePath))
        {
            return false;
        }

        // Grant system read/execute but remove DELETE & WRITE permissions
        PACL pOldDacl = nullptr;
        PACL pNewDacl = nullptr;
        PSECURITY_DESCRIPTOR pSD = nullptr;

        const wchar_t* pathStr = filePath.c_str();

        if (GetNamedSecurityInfoW(pathStr, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, &pOldDacl, nullptr, &pSD) != ERROR_SUCCESS)
        {
            return false;
        }

        EXPLICIT_ACCESS_W ea{};
        wchar_t everyoneGroup[] = L"EVERYONE";
        BuildExplicitAccessWithNameW(&ea, everyoneGroup, FILE_GENERIC_READ | FILE_EXECUTE, SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);

        if (SetEntriesInAclW(1, &ea, pOldDacl, &pNewDacl) == ERROR_SUCCESS)
        {
            SetNamedSecurityInfoW(const_cast<PWSTR>(pathStr), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, pNewDacl, nullptr);
            LocalFree(pNewDacl);
        }

        if (pSD) LocalFree(pSD);

        // Also enforce Windows READONLY file attribute flag
        DWORD attributes = GetFileAttributesW(pathStr);
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            SetFileAttributesW(pathStr, attributes | FILE_ATTRIBUTE_READONLY);
        }

        return true;
    }

    // Safely unlocks the file ONLY when explicit internal cache maintenance is triggered
    [[nodiscard]] static bool TemporarilyUnlockDacl(const std::filesystem::path& filePath) noexcept
    {
        if (!std::filesystem::exists(filePath)) return false;

        const wchar_t* pathStr = filePath.c_str();

        DWORD attributes = GetFileAttributesW(pathStr);
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            SetFileAttributesW(pathStr, attributes & ~FILE_ATTRIBUTE_READONLY);
            return true;
        }
        return false;
    }
};

// -----------------------------------------------------------------------------
// Immutable Core Vault Class
// -----------------------------------------------------------------------------
class ImmutableCoreVault
{
public:
    ImmutableCoreVault();
    ~ImmutableCoreVault();

    // Securely registers a compiled PSO cache file and locks it against OS eviction
    bool RegisterAndLockPSO(const std::string& psoName, const std::vector<std::uint8_t>& psoData);

    // Verifies integrity and retrieves cached PSO bytecode
    [[nodiscard]] bool ReadLockedPSO(const std::string& psoName, std::vector<std::uint8_t>& outData) noexcept;

    [[nodiscard]] std::size_t GetLockedPSOCount() const noexcept {
        return lockedPsoCount_.load(std::memory_order_relaxed);
    }

private:
    std::filesystem::path vaultDirectory_;
    std::atomic<std::size_t> lockedPsoCount_{0};
};

} // namespace EngineCore::Shader