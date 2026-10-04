// =============================================================================
// EngineCore :: Collaborative PSO / Shader Cache Cloud Network
// -----------------------------------------------------------------------------
// Lets users with matching GPU hardware + driver version share pre-compiled
// PSO/shader bytecode to eliminate redundant first-run shader compilation
// stutter.
//
// TRUST MODEL (read this before wiring this into ImmutableCoreVault):
//   A raw hash (SHA-256/BLAKE3) only proves a blob was not corrupted in
//   transit; it does NOT prove the blob is safe, because anyone can hash
//   their own tampered bytecode and publish that hash alongside it. Since
//   entries accepted here are written into a vault that the purging system
//   is deliberately prevented from cleaning up, this module treats the
//   network as untrusted-by-default and enforces:
//
//     1. Candidates are only ever SUBMITTED (never trusted directly) via
//        SubmitCandidateAsync(). A submission is a proposal for your backend
//        to vet, not something a client ever consumes.
//     2. Entries are only ever CONSUMED via FetchVerifiedAsync(), and every
//        entry is verified against an ECDSA P-256 signature before its
//        callback fires. The public key used for verification is supplied
//        by the calling application (pinned at build/config time) and is
//        never taken from the network itself.
//     3. Anything that fails signature verification, exceeds configured
//        size limits, or fails structural sanity checks is dropped before
//        it ever touches disk.
//
//   This intentionally implements a MODERATED collaborative cache (clients
//   propose, a trusted backend vets and signs, clients verify before use)
//   rather than a trustless peer-to-peer swarm. A truly trustless P2P model
//   cannot establish authenticity without an equivalent trust mechanism
//   (web of trust, reputation, blockchain, etc.), which is out of scope
//   here and would meaningfully weaken the integrity guarantees this
//   subsystem is supposed to provide.
//
// Networking: performed via WinHTTP (the supported successor to WinINet)
// with all HTTP calls issued synchronously from a single dedicated
// background std::jthread that drains a task queue - callers on the render
// thread only ever enqueue work and receive results via callback, so frame
// pacing is never affected by network latency.
// =============================================================================
#pragma once

#include <windows.h>

#include <winhttp.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "shader/vault_purging.hpp" // Reuses Digest256 / hashing utilities.

namespace EngineCore::Network
{

using Dsv::Digest256;

// -----------------------------------------------------------------------------
// Deterministic identity of the local GPU + driver combination. Two machines
// with the same HardwareIdentity are assumed to produce byte-identical PSO
// bytecode for the same shader source, which is what makes cache sharing
// between them valid.
// -----------------------------------------------------------------------------
struct HardwareIdentity
{
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::wstring driverVersion; // e.g. "31.0.15.3623"

    // Derives a deterministic identity hash from the fields above. Two
    // HardwareIdentity values with identical fields always produce an
    // identical hash, regardless of machine.
    [[nodiscard]] Digest256 ComputeHash() const;

    // Queries the local default adapter via DXGI to populate this
    // structure. Throws std::system_error on unrecoverable DXGI failure.
    [[nodiscard]] static HardwareIdentity QueryLocal();
};

// -----------------------------------------------------------------------------
// A verified, ready-to-use PSO cache payload. Only ever constructed
// internally after signature verification succeeds - there is no public
// constructor that lets calling code fabricate an already-"verified" entry.
// -----------------------------------------------------------------------------
class VerifiedPsoPayload
{
public:
    [[nodiscard]] const Digest256& HardwareHash() const noexcept { return hardwareHash_; }
    [[nodiscard]] const Digest256& PsoHash() const noexcept { return psoHash_; }
    [[nodiscard]] std::span<const std::uint8_t> Bytecode() const noexcept { return bytecode_; }
    [[nodiscard]] const std::string& PublisherKeyId() const noexcept { return publisherKeyId_; }

private:
    friend class PsoCloudClient;
    VerifiedPsoPayload(Digest256 hardwareHash, Digest256 psoHash, std::vector<std::uint8_t> bytecode,
                        std::string publisherKeyId)
        : hardwareHash_(hardwareHash), psoHash_(psoHash), bytecode_(std::move(bytecode)),
          publisherKeyId_(std::move(publisherKeyId))
    {
    }

    Digest256 hardwareHash_{};
    Digest256 psoHash_{};
    std::vector<std::uint8_t> bytecode_;
    std::string publisherKeyId_;
};

// -----------------------------------------------------------------------------
// RAII wrapper around a CNG (BCrypt) ECDSA P-256 public key used solely to
// verify signatures on entries fetched from the network. Constructed from a
// raw BCRYPT_ECCPUBLIC_BLOB supplied by the application - this key is
// expected to be pinned in application config/build, NOT retrieved from the
// same network channel it is meant to authenticate.
// -----------------------------------------------------------------------------
class TrustedPublisherKey
{
public:
    // `keyId` is an opaque label (e.g. "prod-2026-01") allowing multiple
    // pinned keys to coexist for rotation; it is echoed back on
    // VerifiedPsoPayload::PublisherKeyId() so callers can audit which key
    // vetted a given entry.
    TrustedPublisherKey(std::string keyId, std::span<const std::uint8_t> eccPublicKeyBlob);
    ~TrustedPublisherKey();

    TrustedPublisherKey(const TrustedPublisherKey&) = delete;
    TrustedPublisherKey& operator=(const TrustedPublisherKey&) = delete;
    TrustedPublisherKey(TrustedPublisherKey&&) noexcept;
    TrustedPublisherKey& operator=(TrustedPublisherKey&&) noexcept;

    [[nodiscard]] const std::string& KeyId() const noexcept { return keyId_; }

    // Verifies `signature` (raw r||s, 64 bytes for P-256) over SHA-256(data).
    [[nodiscard]] bool Verify(std::span<const std::uint8_t> data, std::span<const std::uint8_t> signature) const;

private:
    std::string keyId_;
    void* algHandle_ = nullptr; // BCRYPT_ALG_HANDLE
    void* keyHandle_ = nullptr; // BCRYPT_KEY_HANDLE
};

// -----------------------------------------------------------------------------
// Network endpoint + trust configuration for a PsoCloudClient instance.
// -----------------------------------------------------------------------------
struct CloudEndpointConfig
{
    std::wstring hostName; // e.g. L"pso-cache.example-cdn.com"
    std::uint16_t port = INTERNET_DEFAULT_HTTPS_PORT;
    bool useTls = true;
    std::wstring userAgent = L"EngineCore-PsoCloudClient/1.0";

    // Hard ceiling on a single bytecode blob's size, enforced before any
    // buffer is allocated for it. Prevents a misbehaving or malicious
    // endpoint from forcing unbounded memory allocation.
    std::size_t maxBytecodeBytes = 8u * 1024u * 1024u; // 8 MiB

    // Request timeouts, forwarded directly to WinHttpSetTimeouts.
    int resolveTimeoutMs = 5000;
    int connectTimeoutMs = 5000;
    int sendTimeoutMs = 10000;
    int receiveTimeoutMs = 15000;
};

// -----------------------------------------------------------------------------
// Result of a fetch operation: verified entries ready for vault commit, plus
// counters for entries that were rejected (never exposed as payloads).
// -----------------------------------------------------------------------------
struct FetchOutcome
{
    std::vector<VerifiedPsoPayload> verified;
    std::size_t rejectedSignature = 0; // Signature verification failed.
    std::size_t rejectedOversize = 0; // Exceeded maxBytecodeBytes.
    std::size_t rejectedMalformed = 0; // Failed structural/wire-format parsing.
};

struct SubmitOutcome
{
    bool accepted = false; // True only if the backend acknowledged receipt.
    std::string backendMessage; // Optional human-readable status from the backend.
};

using FetchCallback = std::function<void(FetchOutcome)>;
using SubmitCallback = std::function<void(SubmitOutcome)>;

// -----------------------------------------------------------------------------
// Commits a VerifiedPsoPayload (i.e. one that has already passed signature
// verification in FetchVerifiedAsync) into the on-disk vault directory and
// applies a protective DACL: DENY delete/write to Everyone, ALLOW read-only.
// This is what keeps VaultManager::Purge() (see dsv/vault_manager.hpp) from
// ever being able to remove a backend-vetted entry - its DeleteFileW call
// will fail with ERROR_ACCESS_DENIED and the entry will surface in
// PurgeResult::failed rather than being silently deleted.
//
// Only ever call this with a VerifiedPsoPayload that came out of
// FetchVerifiedAsync's callback; the type system enforces this since
// VerifiedPsoPayload has no public constructor.
// -----------------------------------------------------------------------------
[[nodiscard]] bool CommitVerifiedPsoToVault(const VerifiedPsoPayload& payload,
                                             const std::filesystem::path& vaultDirectory);

// -----------------------------------------------------------------------------
// PsoCloudClient: owns a single dedicated background thread that serializes
// all network I/O for this subsystem. All public entry points are
// non-blocking with respect to the calling thread; results are delivered
// via callback, invoked on the background thread (callers dispatching to a
// render-thread queue should hop threads inside their callback as needed).
// -----------------------------------------------------------------------------
class PsoCloudClient
{
public:
    PsoCloudClient(CloudEndpointConfig config, std::vector<TrustedPublisherKey> trustedKeys);
    ~PsoCloudClient();

    PsoCloudClient(const PsoCloudClient&) = delete;
    PsoCloudClient& operator=(const PsoCloudClient&) = delete;

    void Start();
    void Stop();

    // Requests already-published, backend-signed PSO entries matching
    // `identity` for the given set of PSO hashes (i.e. "do you have a
    // vetted cache entry for any of these shader permutations, compiled for
    // my exact hardware+driver combination?"). Non-blocking; `onComplete`
    // fires on the background thread once the round-trip and all local
    // signature verification is done.
    void FetchVerifiedAsync(HardwareIdentity identity, std::vector<Digest256> wantedPsoHashes,
                             FetchCallback onComplete);

    // Submits a locally-compiled PSO blob as an unverified CANDIDATE for the
    // backend to vet. This never results in a locally-trusted entry; the
    // only path to a VerifiedPsoPayload is a subsequent FetchVerifiedAsync
    // call after the backend has (at its own discretion, out of band)
    // reviewed and signed the candidate.
    void SubmitCandidateAsync(HardwareIdentity identity, Digest256 psoHash, std::vector<std::uint8_t> bytecode,
                               SubmitCallback onComplete);

private:
    struct Task
    {
        std::function<void()> work;
    };

    void WorkerLoop(std::stop_token token);
    void Enqueue(Task task);

    FetchOutcome DoFetch(const HardwareIdentity& identity, const std::vector<Digest256>& wantedPsoHashes);
    SubmitOutcome DoSubmit(const HardwareIdentity& identity, const Digest256& psoHash,
                            const std::vector<std::uint8_t>& bytecode);

    [[nodiscard]] const TrustedPublisherKey* FindKey(std::string_view keyId) const;

    CloudEndpointConfig config_;
    std::vector<TrustedPublisherKey> trustedKeys_;

    HINTERNET session_ = nullptr; // WinHTTP session handle, opened once in Start().

    std::jthread worker_;
    std::mutex queueMutex_;
    std::condition_variable_any queueCv_;
    std::deque<Task> queue_;
};

} // namespace EngineCore::Network