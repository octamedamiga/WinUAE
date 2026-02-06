// WinUAE includes
#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"

// Layer headers
#include "od-win32/sounddep/audio_wasapi_layer3.h"
#include "od-win32/sounddep/audio_layer2.h"

// Standard includes
#include <cstring>
#include <cmath>

// Global instance
AudioWASAPILayer3* g_audioWASAPILayer3 = nullptr;

AudioWASAPILayer3::AudioWASAPILayer3()
    : initialized(false)
    , pDevice(nullptr)
    , pAudioClient(nullptr)
    , pRenderClient(nullptr)
    , sampleRate(0)
    , channels(0)
    , bits(0)
    , bufferFrameCount(0)
    , tempBuffer(nullptr)
    , convertBuffer(nullptr)
    , tempBufferCapacity(0)
    , lastStatsTime(0)
{
    stats = {0, 0, 0, 0.0};
}

AudioWASAPILayer3::~AudioWASAPILayer3()
{
    Shutdown();
}

bool AudioWASAPILayer3::Initialize(
    IMMDevice* device,
    IAudioClient* audioClient,
    IAudioRenderClient* renderClient,
    int rate,
    int numChannels,
    int bitsPerSample,
    UINT32 bufferFrames)
{
    if (initialized) {
        Shutdown();
    }
    
    // Validate parameters
    if (!device || !audioClient || !renderClient) {
        write_log(_T("WASAPI Layer3: Invalid parameters\n"));
        return false;
    }
    
    if (rate <= 0 || numChannels <= 0 || bufferFrames == 0) {
        write_log(_T("WASAPI Layer3: Invalid configuration\n"));
        return false;
    }
    
    // Store references (NOT owned by us)
    pDevice = device;
    pAudioClient = audioClient;
    pRenderClient = renderClient;
    
    // Store configuration
    sampleRate = rate;
    channels = numChannels;
    bits = bitsPerSample;
    bufferFrameCount = bufferFrames;
    
    // Allocate temporary buffers
    tempBufferCapacity = bufferFrameCount * 2;  // Extra headroom
    
    // Float buffer for Layer 2 pull
    tempBuffer = new float[tempBufferCapacity * channels];
    if (!tempBuffer) {
        write_log(_T("WASAPI Layer3: Failed to allocate temp buffer\n"));
        return false;
    }
    
    // Convert buffer for format conversion
    if (bits == 16) {
        convertBuffer = new int16_t[tempBufferCapacity * channels];
    } else if (bits == 32) {
        convertBuffer = new int32_t[tempBufferCapacity * channels];
    } else {
        // Fallback to 16-bit
        bits = 16;
        convertBuffer = new int16_t[tempBufferCapacity * channels];
    }
    
    if (!convertBuffer) {
        write_log(_T("WASAPI Layer3: Failed to allocate convert buffer\n"));
        delete[] tempBuffer;
        tempBuffer = nullptr;
        return false;
    }
    
    // Clear buffers
    memset(tempBuffer, 0, tempBufferCapacity * channels * sizeof(float));
    memset(convertBuffer, 0, tempBufferCapacity * channels * (bits / 8));
    
    // Reset stats
    stats.totalFramesWritten = 0;
    stats.totalPullEvents = 0;
    stats.underruns = 0;
    stats.avgLatencyMs = 0.0;
    lastStatsTime = GetTickCount64();
    
    initialized = true;
    
    write_log(_T("WASAPI Layer3: Initialized - %d Hz, %d ch, %d bit, buffer=%u frames (%.1f ms)\n"),
             sampleRate, channels, bits, bufferFrameCount,
             (float)bufferFrameCount * 1000.0f / sampleRate);
    
    return true;
}

void AudioWASAPILayer3::Shutdown()
{
    if (!initialized) return;
    
    if (tempBuffer) {
        delete[] tempBuffer;
        tempBuffer = nullptr;
    }
    
    if (convertBuffer) {
        if (bits == 16) {
            delete[] (int16_t*)convertBuffer;
        } else {
            delete[] (int32_t*)convertBuffer;
        }
        convertBuffer = nullptr;
    }
    
    // Do NOT release WASAPI interfaces - they're managed by sound.cpp
    pDevice = nullptr;
    pAudioClient = nullptr;
    pRenderClient = nullptr;
    
    tempBufferCapacity = 0;
    initialized = false;
    
    write_log(_T("WASAPI Layer3: Shutdown complete\n"));
}

bool AudioWASAPILayer3::OnPullEvent(AudioLayer2* layer2)
{
    if (!initialized || !layer2) return false;
    
    stats.totalPullEvents++;
    
    HRESULT hr;
    BYTE* pData = nullptr;
    
    // 1. Query WASAPI buffer space
    UINT32 numFramesPadding = 0;
    hr = pAudioClient->GetCurrentPadding(&numFramesPadding);
    if (FAILED(hr)) {
        write_log(_T("WASAPI Layer3: GetCurrentPadding() failed %08X\n"), hr);
        return false;
    }
    
    // Calculate available frames
    UINT32 availFrames = bufferFrameCount - numFramesPadding;
    
    if (availFrames == 0) {
        // WASAPI buffer full, nothing to do
        return true;
    }
    
    // Clamp to our buffer capacity
    if (availFrames > tempBufferCapacity) {
        availFrames = tempBufferCapacity;
    }
    
    // 2. Pull samples from Layer 2
    int pulledFrames = layer2->PullSamples(tempBuffer, availFrames);
    
    if (pulledFrames == 0) {
        // Layer 2 buffer empty - underrun
        stats.underruns++;
        
        // Write silence to prevent audio glitches
        hr = pRenderClient->GetBuffer(availFrames, &pData);
        if (SUCCEEDED(hr)) {
            memset(pData, 0, availFrames * channels * (bits / 8));
            pRenderClient->ReleaseBuffer(availFrames, 0);
        }
        
        write_log(_T("WASAPI Layer3: Underrun! Layer2 buffer empty\n"));
        return false;
    }
    
    // 3. Format conversion
    if (bits == 16) {
        ConvertFloatToInt16(tempBuffer, (int16_t*)convertBuffer, pulledFrames);
    } else if (bits == 32) {
        ConvertFloatToInt32(tempBuffer, (int32_t*)convertBuffer, pulledFrames);
    }
    
    // 4. Get WASAPI buffer
    hr = pRenderClient->GetBuffer(pulledFrames, &pData);
    if (FAILED(hr)) {
        write_log(_T("WASAPI Layer3: GetBuffer() failed %08X\n"), hr);
        return false;
    }
    
    // 5. Copy data to WASAPI
    int bytesPerFrame = channels * (bits / 8);
    memcpy(pData, convertBuffer, pulledFrames * bytesPerFrame);
    
    // 6. Release buffer
    hr = pRenderClient->ReleaseBuffer(pulledFrames, 0);
    if (FAILED(hr)) {
        write_log(_T("WASAPI Layer3: ReleaseBuffer() failed %08X\n"), hr);
        return false;
    }
    
    // Update stats
    stats.totalFramesWritten += pulledFrames;
    
    // Log stats periodically
    int64_t now = GetTickCount64();
    if (now - lastStatsTime >= 5000) {  // Every 5 seconds
        lastStatsTime = now;
        
        // Calculate average latency
        float ringFill = layer2->GetBufferFillPercent();
        stats.avgLatencyMs = ringFill * 40.0;  // 40ms = ring buffer size
        
        write_log(_T("WASAPI Layer3: Stats - Written=%llu frames, Pulls=%llu, Underruns=%llu, Latency=%.1f ms\n"),
                 stats.totalFramesWritten, stats.totalPullEvents, 
                 stats.underruns, stats.avgLatencyMs);
    }
    
    return true;
}

UINT32 AudioWASAPILayer3::GetAvailableFrames()
{
    if (!initialized || !pAudioClient) return 0;
    
    UINT32 numFramesPadding = 0;
    HRESULT hr = pAudioClient->GetCurrentPadding(&numFramesPadding);
    if (FAILED(hr)) return 0;
    
    return bufferFrameCount - numFramesPadding;
}

void AudioWASAPILayer3::ConvertFloatToInt16(const float* input, int16_t* output, int frameCount)
{
    int sampleCount = frameCount * channels;
    
    for (int i = 0; i < sampleCount; i++) {
        // Clamp to [-1.0, 1.0]
        float sample = input[i];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        
        // Convert to int16 range
        output[i] = (int16_t)(sample * 32767.0f);
    }
}

void AudioWASAPILayer3::ConvertFloatToInt32(const float* input, int32_t* output, int frameCount)
{
    int sampleCount = frameCount * channels;
    
    for (int i = 0; i < sampleCount; i++) {
        // Clamp to [-1.0, 1.0]
        float sample = input[i];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        
        // Convert to int32 range
        output[i] = (int32_t)(sample * 2147483647.0f);
    }
}
