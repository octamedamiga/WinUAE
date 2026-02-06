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
    
    // Update input rate dynamically (for drift correction)
    void SetInputRate(double newInputRate) { inputRate = newInputRate; }
    
    // Process
    // input: uae_s16 samples (interleaved)
    // inputFrames: количество frames
    // output: float samples (interleaved)
    // outputCapacity: максимум frames в output buffer
    // Возвращает: количество записанных frames в output
    int Process(
        const uae_s16* input,
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
    uae_s16* lastFrame;  // Последний frame для interpolation
};
