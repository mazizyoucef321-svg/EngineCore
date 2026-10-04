// =============================================================================
// EngineCore :: Local Deterministic Shader Vault (DSV) & Purging Manager
// -----------------------------------------------------------------------------
// Provides asynchronous integrity scanning and surgical purging of
// pre-compiled PSO / shader-cache blobs (e.g. D3D12 PSO caches, DXVK/vkd3d
// caches, engine-specific shader permutation caches).
//
// A cache entry is flagged for purge when it is:
//   - zero-byte / truncated,
//   - fails a fast cryptographic integrity check against its stored digest
//     (i.e. the file was corrupted after being written), or
//   - was compiled against a GPU driver fingerprint that no longer matches
//     the currently installed driver (driver-mismatched, therefore stale).
//
// All scanning is read-only and all deletion is performed defensively
// (shared, non-exclusive handles; explicit retry-on-share-violation) so the
// renderer's own concurrent cache access is never blocked or corrupted.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace EngineCore::Dsv
{

// -----------------------------------------------------------------------------
// A 256-bit digest, produced by the vault's fast hashing backend.
// If EngineCore is built with BLAKE3 available (ENGINECORE_HAS_BLAKE3
// defined), BLAKE3-256 is used; otherwise a self-contained SHA-256
// implementation is used as a dependency-free fallback. Both produce a
// 32-byte digest, so the public type is identical either way.
// -----------------------------------------------------------------------------
using Digest256 = std::array<std::uint8_t, 32>;

[[nodiscard]] std::string ToHex(const Digest256& digest);
[[nodiscard]] std::optional<Digest256> FromHex(std::string_view hex);

// Computes the digest of a file's contents. Opens the file with
// FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE so it never
// contends with a renderer that may still be writing or reading the cache.
// Returns std::nullopt if the file could not be opened or read.
[[nodiscard]] std::optional<Digest256> HashFile(const std::filesystem::path& path);

// -----------------------------------------------------------------------------
// Fingerprint identifying the GPU driver a shader cache entry was compiled
// against. Used to detect stale, driver-mismatched cache entries after a
// driver update.
// -----------------------------------------------------------------------------
struct DriverFingerprint
{
    std::wstring adapterDescription; // e.g. "NVIDIA GeForce RTX 4080"
    std::wstring driverVersion; // e.g. "31.0.15.3623"
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;

    [[nodiscard]] bool operator==(const DriverFingerprint&) const noexcept = default;
};

// -----------------------------------------------------------------------------
// Result of scanning a single cache file.
// -----------------------------------------------------------------------------
enum class CacheEntryStatus : std::uint8_t
{
    Valid,
    ZeroByte,
    HashMismatch, // Stored manifest digest does not match recomputed digest.
    DriverMismatch, // Manifest driver fingerprint differs from the current one.
    Unreadable, // I/O error while scanning; treated conservatively (not purged).
};

struct CacheScanEntry
{
    std::filesystem::path path;
    std::uint64_t sizeBytes = 0;
    CacheEntryStatus status = CacheEntryStatus::Valid;
    std::optional<Digest256> computedDigest;

    [[nodiscard]] bool IsPurgeCandidate() const noexcept
    {
        return status == CacheEntryStatus::ZeroByte || status == CacheEntryStatus::HashMismatch ||
               status == CacheEntryStatus::DriverMismatch;
    }
};

// -----------------------------------------------------------------------------
// Sidecar manifest describing the expected digest and compiling driver for
// one cache file. Stored as "<cache-file-name>.manifest" alongside the
// binary blob it describes. Entries with no manifest are hash-checked
// against nothing (only zero-byte / unreadable checks apply) unless
// `requireManifest` is set in ScanOptions.
// -----------------------------------------------------------------------------
struct CacheManifestEntry
{
    Digest256 expectedDigest{};
    DriverFingerprint compiledAgainst;
};

struct ScanOptions
{
    // Current driver fingerprint, typically obtained via DXGI adapter
    // enumeration at startup. Entries whose manifest fingerprint differs
    // are flagged DriverMismatch.
    std::optional<DriverFingerprint> currentDriver;

    // If true, any cache file lacking a sidecar manifest is treated as
    // Unreadable (conservative: never silently trusted, never auto-purged).
    bool requireManifest = false;

    // File extension (including the dot) identifying shader cache blobs,
    // e.g. L".psocache". Non-matching files in the directory are ignored.
    std::wstring cacheExtension = L".psocache";

    // File extension used for sidecar manifest files.
    std::wstring manifestExtension = L".manifest";
};

struct PurgeOptions
{
    // If true, files are moved to a quarantine sub-directory instead of
    // being permanently deleted, allowing recovery/inspection.
    bool quarantineInsteadOfDelete = false;
    std::filesystem::path quarantineDirectory; // Used only if the above is true.

    // Number of retry attempts if a file is transiently share-violated by
    // another process (e.g. the renderer mid-write) before giving up on it.
    std::uint32_t maxRetryAttempts = 3;
};

struct PurgeResult
{
    std::vector<std::filesystem::path> purged;
    std::vector<std::filesystem::path> skippedLocked; // Still in use after retries; left untouched.
    std::vector<std::filesystem::path> failed; // Unexpected I/O error.
};

// -----------------------------------------------------------------------------
// VaultManager: orchestrates asynchronous scanning and purging of a shader
// cache directory tree.
// -----------------------------------------------------------------------------
class VaultManager
{
public:
    explicit VaultManager(std::filesystem::path cacheRoot);

    // Launches a non-blocking scan of the cache directory. Internally uses
    // std::async(std::launch::async) so the caller's thread (e.g. the render
    // thread) is never blocked; the returned future can be polled or waited
    // on from a background/maintenance thread.
    [[nodiscard]] std::future<std::vector<CacheScanEntry>> ScanAsync(ScanOptions options) const;

    // Synchronous variant, useful for tooling / offline maintenance passes.
    [[nodiscard]] std::vector<CacheScanEntry> ScanSync(const ScanOptions& options) const;

    // Purges every entry in `entries` for which IsPurgeCandidate() is true.
    // Safe to call from a background maintenance thread while the renderer
    // continues to read/write other, unrelated cache files: only files
    // explicitly listed as purge candidates are ever touched, and deletion
    // uses share-mode-tolerant handles with bounded retry rather than
    // failing the whole pass on a single transient lock.
    PurgeResult Purge(const std::vector<CacheScanEntry>& entries, const PurgeOptions& options) const;

    // Convenience: scan then purge in one asynchronous operation, returning
    // both results together once complete.
    struct ScanAndPurgeResult
    {
        std::vector<CacheScanEntry> scan;
        PurgeResult purge;
    };
    [[nodiscard]] std::future<ScanAndPurgeResult> ScanAndPurgeAsync(
        ScanOptions scanOptions, PurgeOptions purgeOptions) const;

    [[nodiscard]] const std::filesystem::path& CacheRoot() const noexcept { return cacheRoot_; }

private:
    std::filesystem::path cacheRoot_;

    [[nodiscard]] static std::optional<CacheManifestEntry> LoadManifest(
        const std::filesystem::path& manifestPath);
};

} // namespace EngineCore::Dsv