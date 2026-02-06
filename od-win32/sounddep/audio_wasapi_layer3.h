#pragma once

// WinUAE includes
#include "sysconfig.h"
#include "sysdeps.h"

// Windows Audio includes
#include <Audioclient.h>
#include <Mmdeviceapi.h>

// Forward declarations
class AudioLayer2;

// WASAPI Backend Layer 3
// Connects Layer 2 ring buffer to WASAPI hardware
class AudioWASAPILayer3 {
public:
    AudioWASAPILayer3();
    ~AudioWASAPILayer3();
    
    // Initialize with WASAPI interfaces
    // device: IMMDevice from WASAPI initialization
    // audioClient: IAudioClient3 or IAudioClient1
    // renderClient: IAudioRenderClient for writing
    // sampleRate: Target sample rate (48000)
    // channels: Number of channels (2 for stereo)
    // bits: Bits per sample (16 or 32)
    // bufferFrameCount: WASAPI buffer size in frames
    bool Initialize(
        IMMDevice* device,
        IAudioClient* audioClient,
        IAudioRenderClient* renderClient,
        int sampleRate,
        int channels,
        int bits,
        UINT32 bufferFrameCount
    );
    
    void Shutdown();
    
    // Pull event handler - called when WASAPI needs data
    // layer2: Pointer to Layer 2 instance to pull from
    // Returns: true if data was written, false on error
    bool OnPullEvent(AudioLayer2* layer2);
    
    // Query
    UINT32 GetBufferFrameCount() const { return bufferFrameCount; }
    UINT32 GetAvailableFrames();
    
    // Stats
    struct Stats {
        uint64_t totalFramesWritten;
        uint64_t totalPullEvents;
        uint64_t underruns;
        double avgLatencyMs;
    };
    Stats GetStats() const { return stats; }
    
private:
    bool initialized;
    
    // WASAPI interfaces (not owned - managed by sound.cpp)
    IMMDevice* pDevice;
    IAudioClient* pAudioClient;
    IAudioRenderClient* pRenderClient;
    
    // Configuration
    int sampleRate;
    int channels;
    int bits;  // 16 or 32
    UINT32 bufferFrameCount;
    
    // Temporary buffers
    float* tempBuffer;       // Buffer для pull from Layer 2 (float)
    void* convertBuffer;     // Buffer для format conversion (int16/int32)
    int tempBufferCapacity;  // In frames
    
    // Stats
    Stats stats;
    int64_t lastStatsTime;
    
    // Format conversion helpers
    void ConvertFloatToInt16(const float* input, int16_t* output, int frameCount);
    void ConvertFloatToInt32(const float* input, int32_t* output, int frameCount);
};

// Global instance (created in sound.cpp)
extern AudioWASAPILayer3* g_audioWASAPILayer3;
