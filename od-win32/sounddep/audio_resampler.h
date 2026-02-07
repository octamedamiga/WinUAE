#pragma once

// WinUAE includes
#include "sysconfig.h"
#include "sysdeps.h"

// Standard includes
#include <cstdint>

// Linear interpolation resampler
// Простой и эффективный для Phase 1
class AudioResampler {
public:
    AudioResampler();
    ~AudioResampler();
    
    // Initialize
    // inputRate: Paula rate (например, 48011.234 Hz)
    // outputRate: Target rate (например, 48000 Hz)
    // channels: 2 для stereo
    bool Initialize(double inputRate, int outputRate, int channels);
    void Shutdown();

    bool IsInitialized() const { return initialized; }
    
    // Update input rate dynamically (for drift correction)
    void SetInputRate(double newInputRate) { inputRate = newInputRate; }
    
    // Process
    // input: int16_t samples (interleaved)
    // inputFrames: количество frames
    // output: float samples (interleaved)
    // outputCapacity: максимум frames в output buffer
    // Возвращает: количество записанных frames в output
    int Process(
        const int16_t* input,
        int inputFrames,
        float* output,
        int outputCapacity
    );
    
private:
    bool initialized;
    double inputRate;
    int outputRate;
    int channels;
    
    // Resampling state
    double position;  // Fractional position в input
    int16_t* lastFrame;  // Последний frame для interpolation
};
