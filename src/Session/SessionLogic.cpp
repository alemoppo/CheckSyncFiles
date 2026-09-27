#include "Session/SessionLogic.h"

#include <atomic>
#include <chrono>
#include <cstdio>

#include "Hashing/Sha256.h"

namespace bv {
namespace session {

bool FingerprintStale(uint64_t savedSize, uint64_t savedMtime, uint64_t curSize,
                      uint64_t curMtime) {
    return hashing::HashMetadataChanged(savedSize, savedMtime, curSize, curMtime);
}

bool NeedsReverify(const JournalEntry& saved, uint64_t curSizeA, uint64_t curMtimeA,
                   uint64_t curSizeB, uint64_t curMtimeB) {
    return FingerprintStale(saved.sizeA, saved.mtimeA, curSizeA, curMtimeA) ||
           FingerprintStale(saved.sizeB, saved.mtimeB, curSizeB, curMtimeB);
}

namespace {

const char* ScanModeName(ScanMode m) {
    switch (m) {
        case ScanMode::Presence: return "presence";
        case ScanMode::Size: return "size";
        case ScanMode::Content: return "content";
    }
    return "unknown";
}

const char* PatternName(PartialPattern p) {
    switch (p) {
        case PartialPattern::Edges: return "edges";
        case PartialPattern::Center: return "center";
        case PartialPattern::Random: return "random";
    }
    return "unknown";
}

} // namespace

CompatResult CheckResumeCompatible(const ScanSettings& saved, const ScanSettings& current) {
    if (saved.mode != current.mode) {
        return {false, std::string("scan mode differs (session=") +
                           ScanModeName(saved.mode) +
                           ", current=" + ScanModeName(current.mode) + ")"};
    }
    if (saved.caseSensitive != current.caseSensitive) {
        return {false, "case sensitivity differs"};
    }
    if (saved.backend != current.backend) {
        return {false, std::string("enumeration backend differs (session=") + saved.backend +
                           ", current=" + current.backend + ")"};
    }
    if (saved.verify.percentEffective != current.verify.percentEffective) {
        char buf[128];
        snprintf(buf, sizeof(buf), "effective verify percent differs (session=%d, current=%d)",
                 saved.verify.percentEffective, current.verify.percentEffective);
        return {false, buf};
    }
    if (saved.verify.percentEffective < 100 &&
        saved.verify.pattern != current.verify.pattern) {
        return {false, std::string("verify pattern differs (session=") +
                           PatternName(saved.verify.pattern) +
                           ", current=" + PatternName(current.verify.pattern) + ")"};
    }
    // NOTE: hashThreads is deliberately ignored (performance only).
    return {true, "compatible"};
}

std::string GenerateSessionId() {
    static std::atomic<uint64_t> counter{0};
    const uint64_t nanos = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    char buf[48];
    snprintf(buf, sizeof(buf), "%llx-%llx", static_cast<unsigned long long>(nanos),
             static_cast<unsigned long long>(n));
    return buf;
}

uint64_t NowUnixSeconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

bool ShouldCheckpoint(size_t rowsSinceCheckpoint, uint64_t rowsEvery, uint64_t nowSecs,
                      uint64_t lastCheckpointSecs, uint64_t secsEvery) {
    if (rowsEvery > 0 && rowsSinceCheckpoint >= rowsEvery) return true;
    if (secsEvery > 0 && nowSecs >= lastCheckpointSecs &&
        nowSecs - lastCheckpointSecs >= secsEvery)
        return true;
    return false;
}

const char* SessionStateName(SessionState s) {
    switch (s) {
        case SessionState::InProgress: return "in_progress";
        case SessionState::Checkpointed: return "checkpointed";
        case SessionState::Interrupted: return "interrupted";
        case SessionState::Completed: return "completed";
    }
    return "unknown";
}

} // namespace session
} // namespace bv
