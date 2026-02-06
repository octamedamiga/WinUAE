#pragma once

// WinUAE includes
#include "sysconfig.h"
#include "sysdeps.h"

// Standard includes
#include <atomic>
#include <cstdint>
#include <cstring>

// Lock-free ring buffer для float samples
// Thread-safe для single producer / single consumer
template<typename T>
class AudioRingBuffer {
public:
    AudioRingBuffer();
    ~AudioRingBuffer();
    
    // Инициализация
    // capacityFrames ДОЛЖЕН быть степенью двойки!
    bool Initialize(uint32_t capacityFrames, int channels);
    void Shutdown();
    
    // Write (из Paula thread)
    // Возвращает true если записано, false если overflow
    bool Write(const T* data, uint32_t frameCount);
    
    // Read (из Audio thread)
    // Возвращает количество прочитанных frames (может быть < frameCount)
    uint32_t Read(T* data, uint32_t frameCount);
    
    // Query
    uint32_t GetAvailableRead() const;
    uint32_t GetAvailableWrite() const;
    float GetFillPercent() const;
    
    // Stats
    struct Stats {
        uint64_t totalWritten;
        uint64_t totalRead;
        uint64_t overruns;
        uint64_t underruns;
    };
    Stats GetStats() const { return stats; }
    void ResetStats();
    
private:
    // Cache-line aligned atomic positions
    alignas(64) std::atomic<uint32_t> writePos;
    alignas(64) std::atomic<uint32_t> readPos;
    
    T* buffer;
    uint32_t capacity;  // Frames (must be power of 2)
    uint32_t capacityMask;  // capacity - 1 (для быстрого modulo)
    int channels;
    
    Stats stats;
    
    // Helper
    uint32_t NextPowerOf2(uint32_t n);
};

// =============================================================================
// IMPLEMENTATION (header-only для template)
// =============================================================================

template<typename T>
AudioRingBuffer<T>::AudioRingBuffer()
    : writePos(0)
    , readPos(0)
    , buffer(nullptr)
    , capacity(0)
    , capacityMask(0)
    , channels(0)
{
    stats = {0, 0, 0, 0};
}

template<typename T>
AudioRingBuffer<T>::~AudioRingBuffer() {
    Shutdown();
}

template<typename T>
bool AudioRingBuffer<T>::Initialize(uint32_t capacityFrames, int numChannels) {
    // Ensure power of 2
    capacity = NextPowerOf2(capacityFrames);
    capacityMask = capacity - 1;
    channels = numChannels;
    
    // Allocate
    uint32_t totalSamples = capacity * channels;
    buffer = new T[totalSamples];
    if (!buffer) return false;
    
    // Clear
    memset(buffer, 0, totalSamples * sizeof(T));
    
    // Reset positions
    writePos.store(0, std::memory_order_relaxed);
    readPos.store(0, std::memory_order_relaxed);
    
    ResetStats();
    
    return true;
}

template<typename T>
void AudioRingBuffer<T>::Shutdown() {
    if (buffer) {
        delete[] buffer;
        buffer = nullptr;
    }
    capacity = 0;
}

template<typename T>
bool AudioRingBuffer<T>::Write(const T* data, uint32_t frameCount) {
    if (!buffer || !data || frameCount == 0) return false;
    
    uint32_t wPos = writePos.load(std::memory_order_relaxed);
    uint32_t rPos = readPos.load(std::memory_order_acquire);
    
    // Check available space
    uint32_t available = (rPos - wPos - 1) & capacityMask;
    
    if (frameCount > available) {
        // Overflow - drop frames
        stats.overruns++;
        return false;
    }
    
    // Write data
    uint32_t samplesPerFrame = channels;
    
    for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t frameIdx = (wPos + i) & capacityMask;
        uint32_t bufferOffset = frameIdx * samplesPerFrame;
        
        for (int ch = 0; ch < channels; ch++) {
            buffer[bufferOffset + ch] = data[i * samplesPerFrame + ch];
        }
    }
    
    // Update write position
    writePos.store((wPos + frameCount) & capacityMask, std::memory_order_release);
    
    stats.totalWritten += frameCount;
    return true;
}

template<typename T>
uint32_t AudioRingBuffer<T>::Read(T* data, uint32_t frameCount) {
    if (!buffer || !data || frameCount == 0) return 0;
    
    uint32_t wPos = writePos.load(std::memory_order_acquire);
    uint32_t rPos = readPos.load(std::memory_order_relaxed);
    
    // Check available data
    uint32_t available = (wPos - rPos) & capacityMask;
    
    if (available == 0) {
        // Underrun
        stats.underruns++;
        return 0;
    }
    
    // Read min(frameCount, available)
    uint32_t toRead = (frameCount < available) ? frameCount : available;
    uint32_t samplesPerFrame = channels;
    
    for (uint32_t i = 0; i < toRead; i++) {
        uint32_t frameIdx = (rPos + i) & capacityMask;
        uint32_t bufferOffset = frameIdx * samplesPerFrame;
        
        for (int ch = 0; ch < channels; ch++) {
            data[i * samplesPerFrame + ch] = buffer[bufferOffset + ch];
        }
    }
    
    // Update read position
    readPos.store((rPos + toRead) & capacityMask, std::memory_order_release);
    
    stats.totalRead += toRead;
    
    if (toRead < frameCount) {
        stats.underruns++;
    }
    
    return toRead;
}

template<typename T>
uint32_t AudioRingBuffer<T>::GetAvailableRead() const {
    uint32_t wPos = writePos.load(std::memory_order_acquire);
    uint32_t rPos = readPos.load(std::memory_order_relaxed);
    return (wPos - rPos) & capacityMask;
}

template<typename T>
uint32_t AudioRingBuffer<T>::GetAvailableWrite() const {
    uint32_t wPos = writePos.load(std::memory_order_relaxed);
    uint32_t rPos = readPos.load(std::memory_order_acquire);
    return (rPos - wPos - 1) & capacityMask;
}

template<typename T>
float AudioRingBuffer<T>::GetFillPercent() const {
    if (capacity == 0) return 0.0f;
    uint32_t available = GetAvailableRead();
    return (float)available / (float)capacity;
}

template<typename T>
void AudioRingBuffer<T>::ResetStats() {
    stats.totalWritten = 0;
    stats.totalRead = 0;
    stats.overruns = 0;
    stats.underruns = 0;
}

template<typename T>
uint32_t AudioRingBuffer<T>::NextPowerOf2(uint32_t n) {
    if (n == 0) return 1;
    
    // Уже степень 2?
    if ((n & (n - 1)) == 0) return n;
    
    // Find next power of 2
    uint32_t power = 1;
    while (power < n) {
        power <<= 1;
    }
    
    return power;
}
