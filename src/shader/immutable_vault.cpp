// =============================================================================
// EngineCore :: Immutable Core Vault Implementation
// =============================================================================
#include "shader/immutable_vault.hpp"
#include "core/security_manager.hpp"
#include <fstream>

namespace EngineCore::Shader
{

ImmutableCoreVault::ImmutableCoreVault()
    : vaultDirectory_(Security::SecurityManager::GetSecuredAppDataPath(L"ImmutablePSO"))
{
}

ImmutableCoreVault::~ImmutableCoreVault()
{
}

bool ImmutableCoreVault::RegisterAndLockPSO(const std::string& psoName, const std::vector<std::uint8_t>& psoData)
{
    std::filesystem::path targetFile = vaultDirectory_ / (psoName + ".bin");

    // Unlock file if it already existed to allow clean atomic write
    VaultSecurityManager::TemporarilyUnlockDacl(targetFile);

    std::ofstream outFile(targetFile, std::ios::binary | std::ios::trunc);
    if (!outFile.is_open())
    {
        return false;
    }

    outFile.write(reinterpret_cast<const char*>(psoData.data()), psoData.size());
    outFile.close();

    // Enforce Immutable DACL Lock & Read-Only policy to block Driver/OS purge
    if (VaultSecurityManager::ApplyImmutableDaclLock(targetFile))
    {
        lockedPsoCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    return false;
}

bool ImmutableCoreVault::ReadLockedPSO(const std::string& psoName, std::vector<std::uint8_t>& outData) noexcept
{
    std::filesystem::path targetFile = vaultDirectory_ / (psoName + ".bin");

    if (!std::filesystem::exists(targetFile))
    {
        return false;
    }

    // std::ios::ate (At The End) -> المصححة بدلاً من atanend
    std::ifstream inFile(targetFile, std::ios::binary | std::ios::ate);
    if (!inFile.is_open())
    {
        return false;
    }

    std::streamsize fileSize = inFile.tellg();
    inFile.seekg(0, std::ios::beg);

    outData.resize(static_cast<std::size_t>(fileSize));
    if (inFile.read(reinterpret_cast<char*>(outData.data()), fileSize))
    {
        return true;
    }

    return false;
}

} // namespace EngineCore::Shader