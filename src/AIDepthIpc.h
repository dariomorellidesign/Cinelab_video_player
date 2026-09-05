#pragma once

#include <cstddef>
#include <cstdint>

namespace DmpAIDepthIpc {

static constexpr uint32_t Magic = 0x41494450u; // 'AIDP'
static constexpr uint32_t Version = 1u;
static constexpr uint32_t OutputWidth = 518u;
static constexpr uint32_t OutputHeight = 518u;
static constexpr size_t ErrorTextBytes = 1024u;
static constexpr size_t SummaryTextBytes = 1024u;

enum ChildState : uint32_t {
    Starting = 0u,
    Ready = 1u,
    Failed = 2u,
    Stopped = 3u
};

struct alignas(64) SharedHeader {
    uint32_t magic = Magic;
    uint32_t version = Version;
    uint32_t capacityWidth = 0;
    uint32_t capacityHeight = 0;
    uint64_t inputCapacityBytes = 0;
    uint64_t outputCapacityFloats = uint64_t(OutputWidth) * OutputHeight;

    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    uint64_t frameStrideBytes = 0;
    uint64_t frameBytes = 0;
    int64_t frameTimestamp100ns = 0;
    uint64_t frameSequence = 0;
    uint64_t frameGeneration = 0;

    uint64_t resultSequence = 0;
    uint64_t resultGeneration = 0;
    int64_t resultTimestamp100ns = 0;
    uint32_t resultWidth = 0;
    uint32_t resultHeight = 0;
    float percentile02 = 0.0f;
    float percentile98 = 0.0f;
    double inferenceMs = 0.0;
    double totalMs = 0.0;

    uint32_t childState = Starting;
    uint32_t reserved0 = 0;
    char errorText[ErrorTextBytes]{};
    char summaryText[SummaryTextBytes]{};
};

inline size_t Align64(size_t value) { return (value + 63u) & ~size_t(63u); }
inline size_t HeaderBytes() { return Align64(sizeof(SharedHeader)); }
inline size_t OutputBytes() { return size_t(OutputWidth) * OutputHeight * sizeof(float); }
inline uint64_t MappingBytes(uint64_t inputCapacityBytes) {
    return uint64_t(HeaderBytes()) + inputCapacityBytes + uint64_t(OutputBytes());
}
inline uint8_t* InputBytes(void* base) {
    return static_cast<uint8_t*>(base) + HeaderBytes();
}
inline const uint8_t* InputBytes(const void* base) {
    return static_cast<const uint8_t*>(base) + HeaderBytes();
}
inline float* OutputDepth(void* base, const SharedHeader& h) {
    return reinterpret_cast<float*>(static_cast<uint8_t*>(base) + HeaderBytes() + h.inputCapacityBytes);
}
inline const float* OutputDepth(const void* base, const SharedHeader& h) {
    return reinterpret_cast<const float*>(static_cast<const uint8_t*>(base) + HeaderBytes() + h.inputCapacityBytes);
}

} // namespace DmpAIDepthIpc
