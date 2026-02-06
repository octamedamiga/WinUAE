// WinUAE includes
#include "sysconfig.h"
#include "sysdeps.h"

// Local includes
#include "od-win32/sounddep/audio_resampler.h"

// Standard includes
#include <cstring>
#include <cmath>

AudioResampler::AudioResampler()
    : initialized(false)
    , inputRate(0.0)
    , outputRate(0)
    , channels(0)
    , position(0.0)
    , lastFrame(nullptr)
{
}

AudioResampler::~AudioResampler() {
    Shutdown();
}

bool AudioResampler::Initialize(double inRate, int outRate, int numChannels) {
    if (inRate <= 0.0 || outRate <= 0 || numChannels <= 0) {
        return false;
    }
    
    inputRate = inRate;
    outputRate = outRate;
    channels = numChannels;
    position = 0.0;
    
    // Allocate last frame buffer для interpolation
    lastFrame = new uae_s16[channels];
    if (!lastFrame) return false;
    
    memset(lastFrame, 0, channels * sizeof(uae_s16));
    
    initialized = true;
    return true;
}

void AudioResampler::Shutdown() {
    if (lastFrame) {
        delete[] lastFrame;
        lastFrame = nullptr;
    }
    
    initialized = false;
    position = 0.0;
}

int AudioResampler::Process(
    const uae_s16* input,
    int inputFrames,
    float* output,
    int outputCapacity)
{
    if (!initialized || !input || !output || inputFrames == 0 || outputCapacity == 0) {
        return 0;
    }
    
    // Resampling ratio
    double ratio = inputRate / (double)outputRate;
    
    int outputFrames = 0;
    
    while (outputFrames < outputCapacity) {
        // Current position в input
        int inputIndex = (int)position;
        
        // Закончились input samples?
        if (inputIndex >= inputFrames - 1) {
            break;
        }
        
        // Fractional part для interpolation
        double frac = position - (double)inputIndex;
        
        // Linear interpolation для каждого канала
        for (int ch = 0; ch < channels; ch++) {
            uae_s16 sample0 = input[inputIndex * channels + ch];
            uae_s16 sample1 = input[(inputIndex + 1) * channels + ch];
            
            // Interpolate
            double interpolated = sample0 + (sample1 - sample0) * frac;
            
            // Convert to float [-1.0, 1.0]
            output[outputFrames * channels + ch] = (float)(interpolated / 32768.0);
        }
        
        outputFrames++;
        
        // Advance position
        position += ratio;
    }
    
    // Save last frame для следующего вызова
    if (inputFrames > 0) {
        int lastInputFrame = inputFrames - 1;
        for (int ch = 0; ch < channels; ch++) {
            lastFrame[ch] = input[lastInputFrame * channels + ch];
        }
    }
    
    // Reset position для следующего chunk
    // (position должна быть относительно current chunk)
    position -= (double)inputFrames;
    if (position < 0.0) position = 0.0;
    
    return outputFrames;
}
