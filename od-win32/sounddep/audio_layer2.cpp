// WinUAE includes - ОБЯЗАТЕЛЬНО ПЕРВЫМИ!
#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "audio.h"
#include "custom.h"

// Layer 2 headers
#include "od-win32/sounddep/audio_layer2.h"
#include "od-win32/sounddep/audio_ringbuffer.h"
#include "od-win32/sounddep/audio_resampler.h"

// Standard includes
#include <cmath>
#include <cstdarg>

// Global instances
AudioLayer2* g_audioLayer2 = nullptr;
AudioDebugVars g_audioDebugVars = {0};
int g_audioLogLevel = 2;  // 0=Errors, 1=Warnings, 2=Info, 3=Verbose

// Logging implementation
void AudioLog(int level, const TCHAR* layer, const TCHAR* format, ...) {
    if (level > g_audioLogLevel) return;
    
    // Rate limiting per layer
    static int64_t lastLog[4] = {0};
    static int layerIndex = 0;
    
    // Simple layer hash для rate limiting
    if (layer) {
        layerIndex = (layer[0] + layer[1]) % 4;
    }
    
    int64_t now = GetTickCount64();
    if (now - lastLog[layerIndex] < 10) return;  // 10ms limit during debug
    lastLog[layerIndex] = now;
    
    // Format message
    TCHAR buffer[512];
    va_list args;
    va_start(args, format);
    _vsntprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    
    // Write to WinUAE log
    write_log(_T("[AUDIO:%s] %s"), layer, buffer);
}

// AudioLayer2 implementation
AudioLayer2::AudioLayer2()
    : initialized(false)
    , ringBuffer(nullptr)
    , resampler(nullptr)
    , tempBuffer(nullptr)
    , tempBufferCapacity(0)
    , lastPaulaRate(0.0)
    , lastLogTime(0)
{
    config.targetSampleRate = 0;
    config.channels = 0;
    config.ringBufferFrames = 0;
}

AudioLayer2::~AudioLayer2() {
    Shutdown();
}

bool AudioLayer2::Initialize(const AudioLayer2Config& cfg) {
    if (initialized) {
        Shutdown();
    }
    
    config = cfg;
    
    // Validate config
    if (config.targetSampleRate <= 0 || config.channels <= 0 || config.ringBufferFrames <= 0) {
        AudioLog(0, _T("Layer2"), _T("Invalid config: rate=%d, channels=%d, frames=%d\n"),
                 config.targetSampleRate, config.channels, config.ringBufferFrames);
        return false;
    }
    
    // Create ring buffer
    ringBuffer = new AudioRingBuffer<float>();
    if (!ringBuffer->Initialize(config.ringBufferFrames, config.channels)) {
        AudioLog(0, _T("Layer2"), _T("Ring buffer init failed\n"));
        delete ringBuffer;
        ringBuffer = nullptr;
        return false;
    }
    
    // Create resampler (will be initialized later when we know Paula rate)
    resampler = new AudioResampler();
    
    // Allocate temp buffer (начальный размер)
    tempBufferCapacity = 2048;
    tempBuffer = new float[tempBufferCapacity * config.channels];
    
    if (!tempBuffer) {
        AudioLog(0, _T("Layer2"), _T("Temp buffer allocation failed\n"));
        delete ringBuffer;
        delete resampler;
        ringBuffer = nullptr;
        resampler = nullptr;
        return false;
    }
    
    initialized = true;
    lastLogTime = GetTickCount64();
    
    AudioLog(2, _T("Layer2"), _T("Initialized: %d Hz, %d ch, %d frames buffer (%.1f ms)\n"),
             config.targetSampleRate, config.channels, config.ringBufferFrames,
             (float)config.ringBufferFrames * 1000.0f / config.targetSampleRate);
    
    return true;
}

void AudioLayer2::Shutdown() {
    if (!initialized) return;
    
    if (ringBuffer) {
        ringBuffer->Shutdown();
        delete ringBuffer;
        ringBuffer = nullptr;
    }
    
    if (resampler) {
        resampler->Shutdown();
        delete resampler;
        resampler = nullptr;
    }
    
    if (tempBuffer) {
        delete[] tempBuffer;
        tempBuffer = nullptr;
    }
    
    tempBufferCapacity = 0;
    initialized = false;
    
    AudioLog(2, _T("Layer2"), _T("Shutdown complete\n"));
}

void AudioLayer2::ProcessFromPaula(
    const uae_s16* samples,
    int frameCount,
    double cpuCyclesPerSample,
    double syncCyclesPerSec)
{
    static int callCount = 0;
    callCount++;
    
    if (!initialized || !samples || frameCount == 0) return;
    
    g_audioDebugVars.totalProcessCalls++;
    
    // Calculate Paula NATIVE rate (Hardware Cycle-Perfect)
    // Formula: rate = (scaled cycles per sec) / (scaled cycles per sample)
    double paulaRate = syncCyclesPerSec / cpuCyclesPerSample; 
    
    if (callCount == 1 || callCount % 200 == 0) {
        AudioLog(2, _T("Layer2"), _T("Paula Native: base=%.0f, interval=%.2f -> rate=%.2f Hz\n"),
                  syncCyclesPerSec, cpuCyclesPerSample, paulaRate);
    }
    
    if (paulaRate < 1000.0 || paulaRate > 200000.0) return;
    
    g_audioDebugVars.paulaActualRate = paulaRate;
    g_audioDebugVars.paulaFramesGenerated += frameCount;
    
    // Adaptive Resampling (Drift Correction)
    // Instead of changing Paula's speed, we subtly nudge the resampler.
    float fill = GetBufferFillPercent();
    double targetFill = 0.25f; // Aim for 25% buffer
    double driftAdjustment = 1.0;
    
    if (fill < targetFill - 0.05f) driftAdjustment = 0.9998; // Host is faster, pitch down Paula in resampler
    else if (fill > targetFill + 0.05f) driftAdjustment = 1.0002; // Host is slower, pitch up Paula in resampler
    
    if (resampler) {
        // If rate significantly changed (e.g. PAL->NTSC), reinit. Otherwise just set rate.
        if (fabs(paulaRate - lastPaulaRate) > 100.0) {
            resampler->Shutdown();
            resampler->Initialize(paulaRate * driftAdjustment, config.targetSampleRate, config.channels);
            lastPaulaRate = paulaRate;
        } else {
            resampler->SetInputRate(paulaRate * driftAdjustment);
        }
    }

    g_audioDebugVars.resamplerInputRate = (int)paulaRate;
    g_audioDebugVars.resamplerOutputRate = config.targetSampleRate;
    
    if (callCount == 1 || callCount % 200 == 0) {
        AudioLog(2, _T("Layer2"), _T("Resampler: %.2f Hz -> %d Hz (drift: %.4f)\n"),
                 paulaRate, config.targetSampleRate, driftAdjustment);
    }
    
    // Resample
    int outputFrames = (int)(frameCount * config.targetSampleRate / paulaRate) + 10;
    
    if (callCount == 1 || callCount % 100 == 0) {
        write_log(_T("DEBUG ProcessFromPaula #%d: outputFrames calculated: %d (target=%d, paula=%.2f)\n"),
                  callCount, outputFrames, config.targetSampleRate, (float)paulaRate);
    }

    if (outputFrames > tempBufferCapacity) {
        // Expand buffer
        delete[] tempBuffer;
        tempBufferCapacity = outputFrames * 2;
        tempBuffer = new float[tempBufferCapacity * config.channels];
        
        AudioLog(1, _T("Layer2"), _T("Expanded temp buffer to %d frames\n"), 
                 tempBufferCapacity);
    }
    
    int resampledFrames = 0;
    if (resampler) {
        resampledFrames = resampler->Process(
            (const uae_s16*)samples, frameCount,
            tempBuffer, outputFrames
        );

        if (callCount == 1 || callCount % 100 == 0) {
            write_log(_T("DEBUG ProcessFromPaula #%d: resampledFrames=%d (from %d input)\n"),
                      callCount, resampledFrames, frameCount);
        }
        
        // Защита от огромного числа
        if (resampledFrames > 10000) {
            write_log(_T("ERROR: resampledFrames TOO LARGE: %d, skipping per-frame Write\n"), resampledFrames);
            return;
        }
    }
    
    if (resampledFrames == 0) {
        AudioLog(1, _T("Layer2"), _T("Resampler produced 0 frames\n"));
        return;
    }
    
    // Write to ring buffer
    if (ringBuffer && !ringBuffer->Write(tempBuffer, resampledFrames)) {
        // Overrun
        auto stats = ringBuffer->GetStats();
        g_audioDebugVars.layer2Overruns = stats.overruns;
        
        AudioLog(1, _T("Layer2"), _T("Overrun! Dropped %d frames (fill: %.1f%%)\n"),
                 resampledFrames, GetBufferFillPercent() * 100.0f);
    }
    
    // Update debug vars periodically
    uae_u64 now = GetTickCount64();
    if (now - lastLogTime >= 1000) {  // Every 1 second
        lastLogTime = now;
        
        g_audioDebugVars.ringBufferFillPercent = GetBufferFillPercent();
        
        if (ringBuffer) {
            auto stats = ringBuffer->GetStats();
            g_audioDebugVars.layer2Underruns = stats.underruns;
            g_audioDebugVars.layer2Overruns = stats.overruns;
            
            // Log если есть проблемы
            if (stats.underruns > 0 || stats.overruns > 0) {
                AudioLog(1, _T("Layer2"), _T("Stats: underruns=%llu, overruns=%llu, fill=%.1f%%\n"),
                         stats.underruns, stats.overruns, 
                         GetBufferFillPercent() * 100.0f);
            }
        }
    }
}

int AudioLayer2::PullSamples(float* output, int requestedFrames) {
    if (!initialized || !output || !ringBuffer) return 0;
    
    int framesRead = ringBuffer->Read(output, requestedFrames);
    
    if (framesRead < requestedFrames) {
        // Underrun - zero fill
        int samplesRead = framesRead * config.channels;
        int samplesTotal = requestedFrames * config.channels;
        
        memset(output + samplesRead, 0, 
               (samplesTotal - samplesRead) * sizeof(float));
        
        static int underrunLogCount = 0;
        if (underrunLogCount % 100 == 0) {
            AudioLog(1, _T("Layer2"), _T("Underrun! Requested %d, got %d (count=%d)\n"),
                     requestedFrames, framesRead, underrunLogCount);
        }
        underrunLogCount++;
    }
    
    return requestedFrames;  // Always return requested (with zero-fill)
}

float AudioLayer2::GetBufferFillPercent() const {
    if (!initialized || !ringBuffer) return 0.0f;
    return ringBuffer->GetFillPercent();
}
