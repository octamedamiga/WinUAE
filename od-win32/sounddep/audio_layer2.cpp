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
    , inputBuffer(nullptr)
    , ringBuffer(nullptr)
    , resampler(nullptr)
    , tempBuffer(nullptr)
    , tempBufferCapacity(0)
    , inputTempBuffer(nullptr)
    , inputTempCapacity(0)
    , lastPaulaRate(0.0)
    , lastLogTime(0)
    , rateMeasurement()
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
    rateMeasurement = RateMeasurement();
    
    // Validate config
    if (config.targetSampleRate <= 0 || config.channels <= 0 || config.ringBufferFrames <= 0) {
        AudioLog(0, _T("Layer2"), _T("Invalid config: rate=%d, channels=%d, frames=%d\n"),
                 config.targetSampleRate, config.channels, config.ringBufferFrames);
        return false;
    }

    // DIRECT WRITE MODE: Create input ring buffer (10ms capacity)
    int inputCapacity = config.targetSampleRate / 100;
    if (inputCapacity < 16) {
        inputCapacity = 16;
    }
    inputBuffer = new AudioRingBuffer<int16_t>();
    if (!inputBuffer->Initialize(inputCapacity, config.channels)) {
        AudioLog(0, _T("Layer2"), _T("Input ring buffer init failed\n"));
        delete inputBuffer;
        inputBuffer = nullptr;
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
        delete inputBuffer;
        inputBuffer = nullptr;
        delete ringBuffer;
        delete resampler;
        ringBuffer = nullptr;
        resampler = nullptr;
        return false;
    }

    // Temporary input buffer for direct mode
    inputTempCapacity = 128;
    inputTempBuffer = new int16_t[inputTempCapacity * config.channels];
    if (!inputTempBuffer) {
        AudioLog(0, _T("Layer2"), _T("Input temp buffer allocation failed\n"));
        delete[] tempBuffer;
        tempBuffer = nullptr;
        delete inputBuffer;
        inputBuffer = nullptr;
        delete ringBuffer;
        delete resampler;
        ringBuffer = nullptr;
        resampler = nullptr;
        return false;
    }
    
    initialized = true;
    lastLogTime = GetTickCount64();
    
    AudioLog(2, _T("Layer2"), _T("Initialized: %d Hz, %d ch, InputBuf=%d frames, OutputBuf=%d frames\n"),
             config.targetSampleRate, config.channels, inputCapacity, config.ringBufferFrames);
    
    return true;
}

void AudioLayer2::Shutdown() {
    if (!initialized) return;
    
    if (inputBuffer) {
        inputBuffer->Shutdown();
        delete inputBuffer;
        inputBuffer = nullptr;
    }

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

    if (inputTempBuffer) {
        delete[] inputTempBuffer;
        inputTempBuffer = nullptr;
    }
    
    tempBufferCapacity = 0;
    inputTempCapacity = 0;
    initialized = false;
    
    AudioLog(2, _T("Layer2"), _T("Shutdown complete\n"));
}

// ============================================================================
// DIRECT WRITE MODE IMPLEMENTATION
// ============================================================================

void AudioLayer2::PushSample(int16_t left, int16_t right, float cycles_per_sample) {
    if (!initialized || !inputBuffer) return;

    g_audioDebugVars.pushSampleCalls++;

    int16_t stereo[2] = {left, right};

    if (!inputBuffer->Write(stereo, 1)) {
        g_audioDebugVars.layer2Overruns++;

        // Drop oldest sample to make room
        int16_t dummy[2];
        inputBuffer->Read(dummy, 1);
        inputBuffer->Write(stereo, 1);
    }

    UpdateRateMeasurement(cycles_per_sample);
    ResampleInputToOutput();
}

void AudioLayer2::UpdateRateMeasurement(float cycles_per_sample) {
    extern frame_time_t syncbase;
    if (cycles_per_sample <= 0.0f) return;

    double instantRate = syncbase / cycles_per_sample;

    double minRate = config.targetSampleRate * 0.5;
    double maxRate = config.targetSampleRate * 1.5;
    if (instantRate < minRate || instantRate > maxRate) {
        static int errorCount = 0;
        if (errorCount++ < 5) {
            AudioLog(1, _T("Layer2"), _T("WARNING: Rejected outlier rate: %.1f Hz (cycles=%.2f)\n"),
                     instantRate, cycles_per_sample);
        }
        return;
    }

    const double alpha = 0.0001;

    if (rateMeasurement.currentRate == 0.0) {
        rateMeasurement.currentRate = instantRate;
        rateMeasurement.emaRate = instantRate;
        AudioLog(2, _T("Layer2"), _T("Initial rate measurement: %.2f Hz\n"), instantRate);
    } else {
        rateMeasurement.emaRate = alpha * instantRate + (1.0 - alpha) * rateMeasurement.emaRate;
        rateMeasurement.currentRate = rateMeasurement.emaRate;
    }

    rateMeasurement.sampleCount++;

    if (rateMeasurement.sampleCount % 10000 == 0) {
        uint64_t now = GetTickCount64();
        if (now - rateMeasurement.lastLogTime > 5000) {
            AudioLog(2, _T("Layer2"), _T("Rate: instant=%.2f Hz, EMA=%.2f Hz, InputBuf=%d, OutputBuf=%.1f%%\n"),
                     instantRate,
                     rateMeasurement.emaRate,
                     inputBuffer ? inputBuffer->GetAvailableRead() : 0,
                     ringBuffer ? ringBuffer->GetFillPercent() * 100.0f : 0.0f);
            rateMeasurement.lastLogTime = now;
        }
    }

    g_audioDebugVars.estimatedPaulaRateHz = rateMeasurement.currentRate;
    if (inputBuffer) g_audioDebugVars.inputBufferFrames = inputBuffer->GetAvailableRead();
    if (ringBuffer) g_audioDebugVars.outputBufferFrames = ringBuffer->GetAvailableRead();
}

void AudioLayer2::ResampleInputToOutput() {
    if (!initialized || !inputBuffer || !ringBuffer || !resampler) return;

    int available = inputBuffer->GetAvailableRead();
    if (available < 16) {
        return;
    }

    int toProcess = available;
    if (toProcess > 128) {
        toProcess = 128;
    }

    g_audioDebugVars.resampleCalls++;

    if (toProcess > inputTempCapacity) {
        delete[] inputTempBuffer;
        inputTempCapacity = toProcess * 2;
        inputTempBuffer = new int16_t[inputTempCapacity * config.channels];
    }

    int read = inputBuffer->Read(inputTempBuffer, toProcess);
    if (read <= 0) {
        return;
    }

    if (!resampler->IsInitialized()) {
        double initialRate = rateMeasurement.currentRate > 0.0
            ? rateMeasurement.currentRate
            : static_cast<double>(config.targetSampleRate);

        if (!resampler->Initialize(initialRate, config.targetSampleRate, config.channels)) {
            AudioLog(0, _T("Layer2"), _T("ERROR: Resampler initialization failed!\n"));
            return;
        }

        AudioLog(2, _T("Layer2"), _T("Resampler initialized: %.2f Hz -> %d Hz\n"),
                 initialRate, config.targetSampleRate);
    } else if (rateMeasurement.currentRate > 0.0) {
        resampler->SetInputRate(rateMeasurement.currentRate);
    }

    double inputRate = rateMeasurement.currentRate > 0.0
        ? rateMeasurement.currentRate
        : static_cast<double>(config.targetSampleRate);
    double rateRatio = static_cast<double>(config.targetSampleRate) / inputRate;
    int expectedOutput = static_cast<int>(read * rateRatio) + 32;

    if (expectedOutput > tempBufferCapacity) {
        delete[] tempBuffer;
        tempBufferCapacity = expectedOutput * 2;
        tempBuffer = new float[tempBufferCapacity * config.channels];
        AudioLog(2, _T("Layer2"), _T("Temp buffer expanded to %d frames\n"), tempBufferCapacity);
    }

    int resampled = resampler->Process(
        inputTempBuffer,
        read,
        tempBuffer,
        expectedOutput
    );

    if (resampled > 0) {
        if (!ringBuffer->Write(tempBuffer, resampled)) {
            g_audioDebugVars.layer2Overruns++;

            static int overrunLogCount = 0;
            if (overrunLogCount++ % 100 == 0) {
                AudioLog(1, _T("Layer2"), _T("WARNING: Output buffer full! Overruns=%llu\n"),
                         g_audioDebugVars.layer2Overruns);
            }
        }
    }
}

// ============================================================================
// LEGACY BATCHED MODE (kept for compatibility/fallback)
// ============================================================================

void AudioLayer2::ProcessFromPaula(
    const int16_t* samples,
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
            samples, frameCount,
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
    int64_t now = GetTickCount64();
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
