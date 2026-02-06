#pragma once

// WinUAE includes - ОБЯЗАТЕЛЬНО ПЕРВЫМИ!
#include "sysconfig.h"
#include "sysdeps.h"

// Standard C++ includes
#include <atomic>
#include <cstdint>

// Forward declarations
template<typename T> class AudioRingBuffer;
class AudioResampler;

// Debug variables - глобальные для Watch Window
struct AudioDebugVars {
    // Layer 1 (Paula)
    double paulaActualRate;
    uae_u64 paulaFramesGenerated;
    
    // Layer 2
    float ringBufferFillPercent;
    uae_u64 layer2Underruns;
    uae_u64 layer2Overruns;
    int resamplerInputRate;
    int resamplerOutputRate;
    
    // Layer 3 (WASAPI - будет добавлено в Phase 2)
    int wasapiPeriodFrames;
    uae_u64 wasapiCallbackCount;
    
    // Timing
    double avgProcessTimeUs;
    double maxProcessTimeUs;
    
    // General
    uae_u64 totalProcessCalls;
};

extern AudioDebugVars g_audioDebugVars;

// Configuration для Layer 2
struct AudioLayer2Config {
    int targetSampleRate;   // 48000 для Phase 1
    int channels;           // 2 для stereo
    int ringBufferFrames;   // Размер ring buffer в frames (например, 1920 = 40ms @ 48kHz)
};

// Main Layer 2 class
class AudioLayer2 {
public:
    AudioLayer2();
    ~AudioLayer2();
    
    // Initialization
    bool Initialize(const AudioLayer2Config& config);
    void Shutdown();
    
    // Processing (вызывается из Paula thread)
    // samples: uae_s16 stereo interleaved
    // frameCount: количество frames (не samples!)
    // cpuCyclesPerSample: для вычисления actual Paula rate
    void ProcessFromPaula(
        const uae_s16* samples,
        int frameCount,
        double cpuCyclesPerSample,
        double syncCyclesPerSec
    );
    
    // Pull samples для Layer 3 (WASAPI callback)
    // output: float буфер
    // requestedFrames: сколько frames запросил WASAPI
    // Возвращает: количество frames (всегда = requestedFrames, с zero-fill если underrun)
    int PullSamples(float* output, int requestedFrames);
    
    // Query
    float GetBufferFillPercent() const;
    bool IsInitialized() const { return initialized; }
    
private:
    bool initialized;
    AudioLayer2Config config;
    
    // Components (forward declared выше)
    AudioRingBuffer<float>* ringBuffer;
    AudioResampler* resampler;
    
    // Temporary buffer для resampler output
    float* tempBuffer;
    int tempBufferCapacity;
    
    // State
    double lastPaulaRate;
    int64_t lastLogTime;
};

// Global instance (создается в sound.cpp)
extern AudioLayer2* g_audioLayer2;

// Logging helper
void AudioLog(int level, const TCHAR* layer, const TCHAR* format, ...);

// Global log level (0=Errors, 1=Warnings, 2=Info, 3=Verbose)
extern int g_audioLogLevel;
