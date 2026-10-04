// =============================================================================
// EngineCore :: Local Deterministic Shader Vault (DSV) Implementation
// =============================================================================
#include "dsv/vault_manager.hpp"
#include <windows.h>
#include <wincrypt.h>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace EngineCore::Dsv
{

std::string ToHex(const Digest256& digest)
{
    std::ostringstream oss;
    for (std::uint8_t byte : digest)
    {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::optional<Digest256> FromHex(std::string_view hex)
{
    if (hex.length() != 64) return std::nullopt;

    Digest256 digest{};
    for (std::size_t i = 0; i < 32; ++i)
    {
        std::string byteString(hex.substr(i * 2, 2));
        char* endPtr = nullptr;
        unsigned long byteVal = std::strtoul(byteString.c_str(), &endPtr, 16);
        if (endPtr != byteString.c_str() + 2) return std::nullopt;
        digest[i] = static_cast<std::uint8_t>(byteVal);
    }
    return digest;
}

std::optional<Digest256> HashFile(const std::filesystem::path& path)
{
    // Open file using Win32 API with full share mode to prevent contention with renderer
    HANDLE hFile = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) return std::nullopt;

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    Digest256 digest{};
    bool success = false;

    if (CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
    {
        if (CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
        {
            BYTE buffer[8192];
            DWORD bytesRead = 0;
            bool readFailed = false;

            while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
            {
                if (!CryptHashData(hHash, buffer, bytesRead, 0))
                {
                    readFailed = true;
                    break;
                }
            }

            if (!readFailed)
            {
                DWORD hashSize = 32;
                if (CryptGetHashParam(hHash, HP_HASHVAL, digest.data(), &hashSize, 0))
                {
                    success = true;
                }
            }
            CryptDestroyHash(hHash);
        }
        CryptReleaseContext(hProv, 0);
    }

    CloseHandle(hFile);
    return success ? std::make_optional(digest) : std::nullopt;
}

// -----------------------------------------------------------------------------
// VaultManager Implementation
// -----------------------------------------------------------------------------
VaultManager::VaultManager(std::filesystem::path cacheRoot)
    : cacheRoot_(std::move(cacheRoot))
{
}

std::future<std::vector<CacheScanEntry>> VaultManager::ScanAsync(ScanOptions options) const
{
    return std::async(std::launch::async, [this, opts = std::move(options)]() {
        return ScanSync(opts);
    });
}

std::vector<CacheScanEntry> VaultManager::ScanSync(const ScanOptions& options) const
{
    std::vector<CacheScanEntry> entries;

    if (!std::filesystem::exists(cacheRoot_) || !std::filesystem::is_directory(cacheRoot_))
    {
        return entries;
    }

    for (const auto& dirEntry : std::filesystem::recursive_directory_iterator(cacheRoot_))
    {
        if (!dirEntry.is_regular_file()) continue;

        const auto& path = dirEntry.path();
        if (path.extension() != options.cacheExtension) continue;

        CacheScanEntry entry;
        entry.path = path;
        entry.sizeBytes = std::filesystem::file_size(path);

        if (entry.sizeBytes == 0)
        {
            entry.status = CacheEntryStatus::ZeroByte;
            entries.push_back(entry);
            continue;
        }

        entry.computedDigest = HashFile(path);
        if (!entry.computedDigest.has_value())
        {
            entry.status = CacheEntryStatus::Unreadable;
            entries.push_back(entry);
            continue;
        }

        std::filesystem::path manifestPath = path;
        manifestPath.replace_extension(options.manifestExtension);

        auto manifest = LoadManifest(manifestPath);
        if (!manifest.has_value())
        {
            if (options.requireManifest)
            {
                entry.status = CacheEntryStatus::Unreadable;
            }
            else
            {
                entry.status = CacheEntryStatus::Valid;
            }
            entries.push_back(entry);
            continue;
        }

        if (manifest->expectedDigest != *entry.computedDigest)
        {
            entry.status = CacheEntryStatus::HashMismatch;
        }
        else if (options.currentDriver.has_value() && !(manifest->compiledAgainst == *options.currentDriver))
        {
            entry.status = CacheEntryStatus::DriverMismatch;
        }
        else
        {
            entry.status = CacheEntryStatus::Valid;
        }

        entries.push_back(entry);
    }

    return entries;
}

PurgeResult VaultManager::Purge(const std::vector<CacheScanEntry>& entries, const PurgeOptions& options) const
{
    PurgeResult result;

    for (const auto& entry : entries)
    {
        if (!entry.IsPurgeCandidate()) continue;

        bool success = false;
        bool locked = false;

        for (std::uint32_t attempt = 0; attempt < options.maxRetryAttempts; ++attempt)
        {
            std::error_code ec;
            if (options.quarantineInsteadOfDelete)
            {
                std::filesystem::create_directories(options.quarantineDirectory, ec);
                auto destPath = options.quarantineDirectory / entry.path.filename();
                std::filesystem::rename(entry.path, destPath, ec);
            }
            else
            {
                std::filesystem::remove(entry.path, ec);
            }

            if (!ec)
            {
                success = true;
                break;
            }

            if (ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_LOCK_VIOLATION)
            {
                locked = true;
                Sleep(10); // Transient lock delay retry
            }
            else
            {
                break;
            }
        }

        if (success)
        {
            result.purged.push_back(entry.path);
        }
        else if (locked)
        {
            result.skippedLocked.push_back(entry.path);
        }
        else
        {
            result.failed.push_back(entry.path);
        }
    }

    return result;
}

std::future<VaultManager::ScanAndPurgeResult> VaultManager::ScanAndPurgeAsync(
    ScanOptions scanOptions, PurgeOptions purgeOptions) const
{
    return std::async(std::launch::async, [this, sOpts = std::move(scanOptions), pOpts = std::move(purgeOptions)]() {
        ScanAndPurgeResult res;
        res.scan = ScanSync(sOpts);
        res.purge = Purge(res.scan, pOpts);
        return res;
    });
}

std::optional<CacheManifestEntry> VaultManager::LoadManifest(const std::filesystem::path& manifestPath)
{
    if (!std::filesystem::exists(manifestPath)) return std::nullopt;

    std::ifstream inFile(manifestPath, std::ios::binary);
    if (!inFile.is_open()) return std::nullopt;

    CacheManifestEntry entry;
    if (!inFile.read(reinterpret_cast<char*>(&entry.expectedDigest), sizeof(Digest256)))
    {
        return std::nullopt;
    }

    return entry;
}

} // namespace EngineCore::Dsv