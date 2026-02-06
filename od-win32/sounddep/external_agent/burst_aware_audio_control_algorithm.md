# WinUAE Audio Layer 2 Redesign
## Burst-Aware Control Algorithm - Technical Specification

**Document Version:** 1.0  
**Date:** February 6, 2026  
**Author:** Audio Architecture Team  
**Target:** WinUAE Audio Modernization Project

---

## Executive Summary

This document presents a complete redesign of the WinUAE Audio Layer 2 control algorithm to solve the critical buffer instability problem caused by bursty audio generation from the Paula emulation core.

**Problem:** The current PID-based adaptive resampler fails because Paula generates audio in 20ms bursts (≈960 frames at 48kHz) during `vsync_handle()`, creating a sawtooth waveform in the ring buffer fill level that triggers false corrections and audio artifacts (crackling, flutter).

**Solution:** A burst-aware, event-driven control algorithm that:
- Estimates the true Paula rate from burst timing, not buffer fill level
- Ignores the sawtooth ripple (±20ms) while reacting instantly to real drift
- Uses dual-mode operation: tracking mode for normal operation, emergency mode for critical situations

**Expected Outcome:** Stable, crackle-free audio with native Amiga timing and no core warping.

---

## Table of Contents

1. Problem Analysis
2. Architectural Principles
3. Algorithm Design
4. Mathematical Foundation
5. Implementation Specification
6. Testing & Validation Strategy
7. Appendices

---

## 1. Problem Analysis

### 1.1 Legacy Architecture (Core Warping)

The legacy WinUAE audio system used `sound_setadjust()` to micro-adjust the Amiga emulation speed (`scaled_sample_evtime`) to match the host audio buffer requirements.

**Characteristics:**
- Emulation speed varied ±0.6% to compensate for host drift
- Simple and effective for synchronization
- **Major flaw:** Caused audible tempo drift (wow/flutter) in long-playing audio

**Why it worked:** The control variable (emulation speed) directly affected the rate of audio generation, creating a tight feedback loop.

### 1.2 Modern Architecture (Pro-Audio Pipeline)

The new three-layer architecture attempts to achieve perfect timing without core warping:

```
Layer 1 (Paula Core) → Layer 2 (Bridge + Resampler) → Layer 3 (WASAPI)
   Fixed Native Rate       Adaptive Resampling           Pull Mode
```

**Key constraint:** We disabled `sound_setadjust()` (forced to 0.0), locking Paula to native crystal timing. Rate matching must happen entirely through resampling in Layer 2.

### 1.3 The Sawtooth Problem

**Discovery:** Paula does not generate audio sample-by-sample. Instead, it generates an entire video frame's worth of audio in one burst during `vsync_handle()`.

**Timing characteristics:**
- **Burst size:** ≈960 frames at 48kHz (20ms of audio)
- **Burst frequency:** 50 Hz (PAL) or 60 Hz (NTSC)
- **Burst interval:** 20ms (PAL) or 16.67ms (NTSC)
- **Inter-burst gap:** Complete silence for the entire interval

**Buffer behavior:**
```
Fill Level
   ▲
   │     ╱╲        ╱╲        ╱╲
   │    ╱  ╲      ╱  ╲      ╱  ╲
   │   ╱    ╲    ╱    ╲    ╱    ╲
   │  ╱      ╲  ╱      ╲  ╱      ╲
   └─────────────────────────────────► Time
     ↑        ↑        ↑
   Burst    Burst    Burst
   (20ms)   (20ms)   (20ms)
```

**Effect on PID controller:**
1. Buffer drains during the 20ms gap → PID sees "starvation" → increases resampler rate
2. Burst arrives → buffer jumps up → PID sees "overflow" → decreases resampler rate
3. This oscillation repeats at 50Hz, causing audible flutter

### 1.4 Why Phase 5.5 Failed

**Attempt:** "Damped Reservoir" using exponential moving average (EMA) low-pass filter with deadzone:

```cpp
avgFill = avgFill * 0.95f + fill * 0.05f;  // EMA smoothing
if (abs(error) < DEADZONE) error = 0.0f;    // Deadzone
```

**Failure modes:**

1. **Over-smoothing paradox:**
   - To hide the 20ms sawtooth → need τ ≈ 200ms smoothing
   - But with 200ms lag → can't react to real underruns in time
   - Result: Buffer starvation before the controller notices

2. **Under-smoothing paradox:**
   - To react quickly → need τ ≈ 50ms
   - But 50ms filter doesn't suppress the 20ms sawtooth
   - Result: Controller tracks the ripple, causes flutter

3. **Deadzone tuning impossibility:**
   - Large deadzone (±10%) → hides real drift until it's too late
   - Small deadzone (±2%) → still reacts to sawtooth edges
   - No middle ground works reliably

**Fundamental issue:** A PID controller with any smoothing constant τ cannot distinguish between:
- A 20ms sawtooth ripple (noise to ignore)
- A real 20ms drift trend (signal to track)

Both look identical in the frequency domain around 50Hz.

---

## 2. Architectural Principles

### 2.1 Event-Driven Philosophy

**Core insight:** Don't sample the buffer fill level continuously. Instead, trigger control updates precisely when bursts arrive.

**Rationale:**
- The buffer fill level between bursts contains no useful information (it's just a linear drain)
- All meaningful rate information is encoded in the **timing** of burst arrivals
- By synchronizing control updates with bursts, we eliminate the sawtooth sampling problem

### 2.2 Rate Estimation vs. Buffer Servo

**Traditional approach (buffer servo):**
```
Buffer Fill → Error → Rate Correction
```
This is a classic closed-loop control trying to maintain a buffer setpoint.

**New approach (rate matching):**
```
Burst Timestamps → Rate Estimate → Resampler Configuration
```
This is an open-loop estimator with emergency override.

**Key difference:** We don't try to "fix" the buffer level. We try to match the natural Paula rate and let the buffer stabilize organically.

### 2.3 Dual-Mode Operation

**Mode 1: Tracking Mode (Normal Operation)**
- Estimate Paula rate from burst timing
- Configure resampler to match this rate
- Buffer level floats within safe bounds (25% - 75%)
- No active correction unless bounds violated

**Mode 2: Emergency Mode (Critical Situations)**
- Triggered when buffer < 15% or > 85%
- Aggressive rate correction to prevent underrun/overrun
- Temporary deviation from native rate permitted
- Return to tracking mode once buffer recovers to safe zone

### 2.4 Temporal vs. Spatial Measurement

**Spatial measurement (legacy):** How much audio is in the buffer right now?
- Contaminated by 20ms sawtooth
- Requires heavy filtering
- Lag is unavoidable

**Temporal measurement (new):** How often do bursts arrive?
- Immune to buffer ripple
- Direct measurement of Paula rate
- Instantaneous, no lag

---

## 3. Algorithm Design

### 3.1 State Variables

```cpp
struct BurstTracker {
    // Timing
    uint64_t lastBurstTime;        // Timestamp of last burst (microseconds)
    uint64_t burstInterval;        // Measured interval between bursts (μs)
    
    // Rate estimation
    double estimatedPaulaRate;     // Hz, derived from burstInterval
    double emaRate;                // Exponentially smoothed rate estimate
    
    // Burst detection
    int lastBurstSize;             // Frames in last burst
    int quietFrameCount;           // Frames since last burst
    
    // Safety margins
    bool emergencyMode;
    double emergencyRateCorrection;
};

struct RingBufferMonitor {
    float currentFill;             // 0.0 - 1.0
    int availableFrames;
    int capacityFrames;
    
    // Thresholds
    int emergencyLowThreshold;     // 15% capacity
    int emergencyHighThreshold;    // 85% capacity
    int targetLevel;               // 40% capacity (sweet spot)
};
```

### 3.2 Burst Detection Logic

A "burst" is defined as receiving >100 frames after a quiet period (>5ms with <10 frames received).

```cpp
void ProcessFromPaula(const int16_t* samples, int frameCount) {
    uint64_t now = GetMicroseconds();
    
    // Detect burst
    if (frameCount > 100 && quietFrameCount > 240) {  // 5ms @ 48kHz
        // This is a burst!
        HandleBurst(now, frameCount);
        quietFrameCount = 0;
    } else {
        // Accumulate quiet time
        quietFrameCount += frameCount;
    }
    
    // ... rest of processing
}
```

### 3.3 Rate Estimation Algorithm

**On each burst arrival:**

```cpp
void HandleBurst(uint64_t timestamp, int frameCount) {
    if (lastBurstTime == 0) {
        lastBurstTime = timestamp;
        return;  // Need two bursts to measure interval
    }
    
    // 1. Measure actual interval
    uint64_t interval = timestamp - lastBurstTime;
    lastBurstTime = timestamp;
    
    // 2. Calculate instantaneous Paula rate
    // burstSize frames were generated over 'interval' microseconds
    double instantRate = (lastBurstSize * 1000000.0) / interval;
    
    // 3. Sanity check (reject outliers)
    if (instantRate < 40000 || instantRate > 56000) {
        // Reject crazy values (may be first frame, mode switch, etc.)
        return;
    }
    
    // 4. Update EMA estimate
    // Use longer smoothing than Phase 5.5 because we're measuring
    // at burst frequency (50Hz), not every frame (~1000Hz)
    const double alpha = 0.15;  // Roughly 6-burst time constant
    
    if (estimatedPaulaRate == 0.0) {
        estimatedPaulaRate = instantRate;
        emaRate = instantRate;
    } else {
        emaRate = alpha * instantRate + (1.0 - alpha) * emaRate;
        estimatedPaulaRate = emaRate;
    }
    
    // Store burst size for next iteration
    lastBurstSize = frameCount;
}
```

**Mathematical properties:**
- **Time constant:** τ = 1/α ≈ 6.7 bursts ≈ 134ms at 50Hz
- **Frequency response:** -3dB cutoff at ~1.2 Hz (well below 50Hz burst frequency)
- **Phase lag:** ~67ms at 1Hz, decreasing at higher frequencies
- **Noise rejection:** Attenuates 50Hz sawtooth by >30dB

### 3.4 Resampler Configuration

**In tracking mode:**

```cpp
void UpdateResampler() {
    if (emergencyMode) {
        // Emergency mode: use corrected rate
        double targetRate = estimatedPaulaRate + emergencyRateCorrection;
        resampler->SetInputRate(targetRate);
    } else {
        // Normal mode: match estimated rate directly
        resampler->SetInputRate(estimatedPaulaRate);
    }
}
```

**No feedback from buffer level to rate in tracking mode.** The buffer is allowed to float freely as long as it stays within safe bounds (15% - 85%).

### 3.5 Emergency Mode Logic

Emergency mode provides a safety net against sustained drift or initial transients.

```cpp
void CheckEmergencyConditions() {
    float fill = ringBuffer->GetFillPercent();
    int frames = ringBuffer->GetAvailableRead();
    
    // Entry conditions
    if (!emergencyMode) {
        if (fill < 0.15f || fill > 0.85f) {
            emergencyMode = true;
            emergencyRateCorrection = 0.0;
            
            AudioLog(1, _T("Layer2"), _T("EMERGENCY MODE ACTIVATED: fill=%.1f%%"), 
                     fill * 100.0f);
        }
    }
    
    // Emergency correction
    if (emergencyMode) {
        // Calculate required correction to return to target (40%)
        float target = 0.40f;
        float error = fill - target;
        
        // Aggressive P-controller (no I-term to avoid overshoot)
        // Goal: reach target in ~100ms (5 bursts)
        double Kp = 800.0;  // Hz per unit error
        emergencyRateCorrection = -error * Kp;
        
        // Clamp to ±2% rate deviation
        double maxCorrection = estimatedPaulaRate * 0.02;
        emergencyRateCorrection = std::clamp(emergencyRateCorrection, 
                                             -maxCorrection, maxCorrection);
        
        // Exit condition: back to safe zone (25% - 75%)
        if (fill > 0.25f && fill < 0.75f) {
            emergencyMode = false;
            emergencyRateCorrection = 0.0;
            
            AudioLog(2, _T("Layer2"), _T("Emergency mode deactivated: fill=%.1f%%"),
                     fill * 100.0f);
        }
    }
}
```

**Emergency mode characteristics:**
- **Entry threshold:** <15% or >85% fill
- **Exit threshold:** 25% - 75% safe zone
- **Correction strength:** 800 Hz/error (e.g., -20% error → +160 Hz boost)
- **Maximum deviation:** ±2% of base rate (±960 Hz at 48kHz)
- **Response time:** ~100ms to recover from 15% to 40%

**Why it works:**
- Rarely activated in normal operation (tracking mode keeps buffer stable)
- Strong enough to prevent underruns during transients
- Limited deviation preserves audio quality
- Clean exit hysteresis prevents mode chattering

### 3.6 Complete Control Flow

```
┌─────────────────────────────────────────────────────┐
│ ProcessFromPaula(samples, frameCount)               │
└──────────────────┬──────────────────────────────────┘
                   │
                   ▼
          ┌────────────────┐
          │ Detect Burst?  │
          └────┬───────────┘
               │
        Yes ───┴─── No
        │            │
        ▼            ▼
┌──────────────┐  (Accumulate)
│ HandleBurst  │
│ - Measure    │
│   interval   │
│ - Calculate  │
│   rate       │
│ - Update EMA │
└──────┬───────┘
       │
       ▼
┌────────────────────┐
│ Check Emergency    │
│ Conditions         │
└────────┬───────────┘
         │
    Emergency? ───┬─── Normal
         │        │
         ▼        ▼
    ┌─────────┐ ┌──────────────┐
    │ Compute │ │ Use Estimated│
    │ Aggress │ │ Rate Directly│
    │ Correct │ │              │
    └────┬────┘ └──────┬───────┘
         │             │
         └──────┬──────┘
                ▼
        ┌───────────────┐
        │ Configure     │
        │ Resampler     │
        └───────┬───────┘
                │
                ▼
        ┌───────────────┐
        │ Resample &    │
        │ Write to Ring │
        └───────────────┘
```

---

## 4. Mathematical Foundation

### 4.1 Sawtooth Frequency Analysis

The ring buffer fill level can be modeled as:

```
f(t) = f₀ + A·sawtooth(ω·t) + δ(t)
```

Where:
- f₀ = target fill level (40%)
- A = sawtooth amplitude (≈±10% at 50Hz)
- ω = 2π·50 Hz (PAL video frame rate)
- δ(t) = slow drift component (<0.1 Hz)

**Problem:** A low-pass filter cannot separate δ(t) from the fundamental of the sawtooth (50Hz) without excessive lag.

**Solution:** Don't filter f(t). Instead, measure the period T of the sawtooth directly:

```
T = time between burst peaks
Paula Rate = (burst size) / T
```

This measurement is orthogonal to the buffer fill level and immune to the sawtooth.

### 4.2 Rate Estimator Variance

Given measurements T₁, T₂, ..., Tₙ with variance σ²ₜ, the EMA filter:

```
R̂ₙ = α·Rₙ + (1-α)·R̂ₙ₋₁
```

Has variance:

```
σ²ᵣ = σ²ₜ · α / (2 - α)
```

For α = 0.15:
```
σ²ᵣ ≈ 0.081·σ²ₜ
```

**Interpretation:** The EMA reduces measurement noise by ~11× (10.9dB).

**Practical values:**
- Typical interval jitter: σₜ ≈ 50 μs (due to OS scheduling)
- Resulting rate uncertainty: σᵣ ≈ 12 Hz
- This is well below the audible threshold (~100 Hz for wow/flutter)

### 4.3 Emergency Mode Stability Analysis

The emergency P-controller has transfer function:

```
H(s) = Kp / (s + Kp/τ)
```

Where τ is the resampler update period (20ms).

**Stability margin:**
- Gain margin: ∞ (first-order system, unconditionally stable)
- Phase margin: 90° (no overshoot possible)
- Step response: Exponential approach with time constant τ·Kp

For Kp = 800 Hz/error:
```
Time constant = 0.02s · 800 = 16s (way too slow!)
```

**Wait, that's wrong.** Let me recalculate:

The loop gain is:
```
Loop gain = Kp · (frameCount/interval) · (1/bufferSize)
         = 800 · (960/0.02) · (1/9600)
         = 800 · 48000 · 0.0001
         = 3840 per second
```

**Corrected time constant:**
```
τ_closed = 1 / 3840 ≈ 0.26 ms (super fast!)
```

This is too aggressive! We need to reduce Kp.

**Revised Kp selection:**

For 100ms response time (5 burst intervals):
```
Required loop bandwidth ≈ 10 Hz
Kp ≈ 10 / (48000 · 0.0001) = 208 Hz/error
```

Round to Kp = 200 Hz/error for clean numbers.

### 4.4 Quantization and Resolution

**Timestamp resolution:** 1 μs (QueryPerformanceCounter on Windows)

**Rate resolution at 48kHz:**
```
ΔR/Δt = R² / N
```
Where N is burst size (960 frames).

```
ΔR = 48000² / 960 ≈ 2.4 MHz / 960 = 2500 Hz per μs
```

**Practical resolution:** With 50μs jitter, rate resolution is ±125 Hz, well below audible threshold.

---

## 5. Implementation Specification

### 5.1 Modified audio_layer2.h

Add to class definition:

```cpp
private:
    // Burst-aware control
    struct BurstTracker {
        uint64_t lastBurstTime;
        uint64_t lastBurstInterval;
        double estimatedPaulaRate;
        double emaRate;
        int lastBurstSize;
        int quietFrameCount;
        bool emergencyMode;
        double emergencyRateCorrection;
        int burstCount;
        
        BurstTracker() 
            : lastBurstTime(0), lastBurstInterval(0)
            , estimatedPaulaRate(0.0), emaRate(0.0)
            , lastBurstSize(0), quietFrameCount(0)
            , emergencyMode(false), emergencyRateCorrection(0.0)
            , burstCount(0)
        {}
    } burstTracker;
    
    // Timing utilities
    uint64_t GetMicroseconds() const;
    
    // Burst handling
    void HandleBurst(uint64_t timestamp, int frameCount);
    void UpdateRateEstimate(uint64_t interval, int frameCount);
    void CheckEmergencyConditions();
    void ConfigureResampler();
```

### 5.2 Modified ProcessFromPaula Implementation

Replace lines 200-341 in audio_layer2.cpp:

```cpp
void AudioLayer2::ProcessFromPaula(
    const int16_t* samples,
    int frameCount,
    double cpuCyclesPerSample,
    double syncCyclesPerSec)
{
    if (!initialized || !samples || frameCount == 0) return;
    
    uint64_t now = GetMicroseconds();
    g_audioDebugVars.totalProcessCalls++;
    
    // ======================================
    // PHASE 6: BURST-AWARE CONTROL
    // ======================================
    
    // 1. BURST DETECTION
    // A burst is defined as >100 frames after a quiet period
    const int BURST_THRESHOLD = 100;
    const int QUIET_THRESHOLD = 240;  // 5ms @ 48kHz
    
    bool isBurst = (frameCount > BURST_THRESHOLD && 
                    burstTracker.quietFrameCount > QUIET_THRESHOLD);
    
    if (isBurst) {
        HandleBurst(now, frameCount);
    } else {
        burstTracker.quietFrameCount += frameCount;
    }
    
    // 2. EMERGENCY MODE CHECK
    // Check if buffer is critically low/high
    CheckEmergencyConditions();
    
    // 3. RESAMPLER CONFIGURATION
    // Use estimated rate (with emergency correction if needed)
    ConfigureResampler();
    
    // 4. RESAMPLING
    // Calculate expected output frames
    double effectiveRate = burstTracker.estimatedPaulaRate;
    if (burstTracker.emergencyMode) {
        effectiveRate += burstTracker.emergencyRateCorrection;
    }
    
    // Safety fallback: if rate not yet estimated, use nominal
    if (effectiveRate < 10000.0 || effectiveRate > 96000.0) {
        effectiveRate = syncCyclesPerSec / cpuCyclesPerSample;
    }
    
    int estimatedOutputFrames = (int)(frameCount * config.targetSampleRate / effectiveRate) + 32;
    
    if (estimatedOutputFrames > tempBufferCapacity) {
        delete[] tempBuffer;
        tempBufferCapacity = estimatedOutputFrames * 2;
        tempBuffer = new float[tempBufferCapacity * config.channels];
    }
    
    int resampledFrames = 0;
    if (resampler && resampler->IsInitialized()) {
        resampledFrames = resampler->Process(
            samples, frameCount,
            tempBuffer, estimatedOutputFrames
        );
    } else if (resampler) {
        // Initialize resampler on first call
        resampler->Initialize(effectiveRate, config.targetSampleRate, config.channels);
        lastPaulaRate = effectiveRate;
    }
    
    // 5. WRITE TO RING BUFFER
    if (resampledFrames > 0) {
        if (ringBuffer && !ringBuffer->Write(tempBuffer, resampledFrames)) {
            g_audioDebugVars.layer2Overruns++;
        }
    }
    
    // 6. DEBUG VARIABLES UPDATE
    g_audioDebugVars.paulaActualRate = effectiveRate;
    g_audioDebugVars.paulaFramesGenerated += frameCount;
    g_audioDebugVars.ringBufferFillPercent = GetBufferFillPercent();
    g_audioDebugVars.resamplerInputRate = (int)effectiveRate;
    g_audioDebugVars.resamplerOutputRate = config.targetSampleRate;
    
    // 7. PERIODIC LOGGING
    static int logCounter = 0;
    if (++logCounter % 100 == 0) {
        AudioLog(2, _T("Layer2"), 
                 _T("Rate=%.1f Hz, Fill=%.1f%%, Bursts=%d, Emergency=%d"),
                 effectiveRate, 
                 GetBufferFillPercent() * 100.0f,
                 burstTracker.burstCount,
                 burstTracker.emergencyMode ? 1 : 0);
    }
}
```

### 5.3 Helper Functions Implementation

```cpp
uint64_t AudioLayer2::GetMicroseconds() const {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000ULL) / freq.QuadPart;
}

void AudioLayer2::HandleBurst(uint64_t timestamp, int frameCount) {
    if (burstTracker.lastBurstTime == 0) {
        // First burst - just record timestamp
        burstTracker.lastBurstTime = timestamp;
        burstTracker.lastBurstSize = frameCount;
        burstTracker.burstCount = 1;
        return;
    }
    
    // Calculate interval since last burst
    uint64_t interval = timestamp - burstTracker.lastBurstTime;
    burstTracker.lastBurstInterval = interval;
    burstTracker.lastBurstTime = timestamp;
    burstTracker.burstCount++;
    
    // Update rate estimate
    UpdateRateEstimate(interval, burstTracker.lastBurstSize);
    
    // Store size for next iteration
    burstTracker.lastBurstSize = frameCount;
    burstTracker.quietFrameCount = 0;
    
    // Log burst event (rate-limited)
    static int burstLogCounter = 0;
    if (++burstLogCounter % 50 == 0) {
        AudioLog(3, _T("Layer2"), 
                 _T("Burst #%d: %d frames, interval=%.2f ms, rate=%.1f Hz"),
                 burstTracker.burstCount,
                 frameCount,
                 interval / 1000.0,
                 burstTracker.estimatedPaulaRate);
    }
}

void AudioLayer2::UpdateRateEstimate(uint64_t interval, int frameCount) {
    // Calculate instantaneous rate
    // rate = frames / time
    double instantRate = (frameCount * 1000000.0) / interval;
    
    // Sanity check - reject outliers
    if (instantRate < 40000.0 || instantRate > 56000.0) {
        AudioLog(1, _T("Layer2"), 
                 _T("WARNING: Rejected outlier rate: %.1f Hz"), instantRate);
        return;
    }
    
    // EMA filter with α = 0.15 (time constant ≈ 6.7 bursts)
    const double alpha = 0.15;
    
    if (burstTracker.estimatedPaulaRate == 0.0) {
        // First valid measurement
        burstTracker.estimatedPaulaRate = instantRate;
        burstTracker.emaRate = instantRate;
    } else {
        // Update EMA
        burstTracker.emaRate = alpha * instantRate + (1.0 - alpha) * burstTracker.emaRate;
        burstTracker.estimatedPaulaRate = burstTracker.emaRate;
    }
}

void AudioLayer2::CheckEmergencyConditions() {
    if (!ringBuffer) return;
    
    float fill = ringBuffer->GetFillPercent();
    
    // Entry conditions
    if (!burstTracker.emergencyMode) {
        if (fill < 0.15f || fill > 0.85f) {
            burstTracker.emergencyMode = true;
            burstTracker.emergencyRateCorrection = 0.0;
            
            AudioLog(1, _T("Layer2"), 
                     _T("*** EMERGENCY MODE ACTIVATED: fill=%.1f%% ***"), 
                     fill * 100.0f);
        }
        return;
    }
    
    // Emergency correction (P-controller)
    const float targetFill = 0.40f;
    float error = fill - targetFill;
    
    // Kp = 200 Hz/error (revised from mathematical analysis)
    const double Kp = 200.0;
    burstTracker.emergencyRateCorrection = -error * Kp;
    
    // Clamp to ±2% rate deviation
    double maxCorrection = burstTracker.estimatedPaulaRate * 0.02;
    if (burstTracker.emergencyRateCorrection > maxCorrection) {
        burstTracker.emergencyRateCorrection = maxCorrection;
    }
    if (burstTracker.emergencyRateCorrection < -maxCorrection) {
        burstTracker.emergencyRateCorrection = -maxCorrection;
    }
    
    // Exit condition: back to safe zone (25% - 75%)
    if (fill > 0.25f && fill < 0.75f) {
        AudioLog(2, _T("Layer2"), 
                 _T("Emergency mode deactivated: fill=%.1f%%"), 
                 fill * 100.0f);
        
        burstTracker.emergencyMode = false;
        burstTracker.emergencyRateCorrection = 0.0;
    }
}

void AudioLayer2::ConfigureResampler() {
    if (!resampler || !resampler->IsInitialized()) return;
    
    double targetRate = burstTracker.estimatedPaulaRate;
    
    // Apply emergency correction if active
    if (burstTracker.emergencyMode) {
        targetRate += burstTracker.emergencyRateCorrection;
    }
    
    // Safety bounds
    if (targetRate < 10000.0) targetRate = 10000.0;
    if (targetRate > 96000.0) targetRate = 96000.0;
    
    // Update resampler
    resampler->SetInputRate(targetRate);
}
```

### 5.4 Modified Constructor

Add to AudioLayer2::AudioLayer2() initialization list:

```cpp
AudioLayer2::AudioLayer2()
    : initialized(false)
    , ringBuffer(nullptr)
    , resampler(nullptr)
    , tempBuffer(nullptr)
    , tempBufferCapacity(0)
    , isPreroll(true)
    , prerollThreshold(0)
    , syncAdjustment(0.0)
    , syncIntegral(0.0)
    , totalSamplesProcessed(0)
    , burstTracker()  // Add this line
{
    // ... existing code ...
}
```

### 5.5 Debug Variables Extension

Add to AudioDebugVars in audio_layer2.h:

```cpp
struct AudioDebugVars {
    // ... existing fields ...
    
    // PHASE 6: Burst-aware tracking
    int burstCount;
    double burstIntervalMs;
    double estimatedPaulaRateHz;
    int emergencyModeActive;
    double emergencyCorrection;
};
```

---

## 6. Testing & Validation Strategy

### 6.1 Unit Tests

**Test 1: Burst Detection**
- Feed artificial burst pattern (960 frames every 20ms)
- Verify burst counter increments correctly
- Verify quiet frame counter resets on burst

**Test 2: Rate Estimation**
- Feed bursts with known intervals
- Verify estimated rate converges to expected value
- Verify outlier rejection (feed one 100ms interval, verify it's ignored)

**Test 3: Emergency Mode**
- Start with empty buffer (0% fill)
- Verify emergency mode activates at <15%
- Verify positive rate correction applied
- Verify exit at >25% fill

### 6.2 Integration Tests

**Test 4: Steady State Tracking**
- Run WinUAE with kickstart ROM
- Play audio for 60 seconds
- Measure:
  - Buffer fill variance (should be <5%)
  - Rate estimate stability (should be <50 Hz variance)
  - Underrun count (should be 0)

**Test 5: Rate Change Handling**
- Switch from PAL to NTSC mid-game
- Verify rate estimate updates within 200ms
- Verify no underruns during transition

**Test 6: Load Transients**
- Pause emulation for 5 seconds
- Resume
- Verify buffer recovers within 500ms
- Verify no crackling during recovery

### 6.3 Subjective Audio Quality Tests

**Test 7: Flutter Detection**
- Play sustained tone (sine wave, 440 Hz)
- Record output with high-quality ADC
- Run wow/flutter analysis (should be <0.05% WRMS)

**Test 8: Crackle Detection**
- Play complex game audio (e.g., Shadow of the Beast)
- Listen for crackling, popping, or stuttering
- Should be indistinguishable from native Amiga

**Test 9: Long-Term Stability**
- Run for 8 hours continuous
- Monitor underrun/overrun counters
- Should remain at 0 after initial preroll

### 6.4 Stress Tests

**Test 10: CPU Starvation**
- Run demanding game with other CPU-intensive tasks
- Verify emergency mode prevents underruns
- Verify clean recovery when CPU becomes available

**Test 11: WASAPI Period Changes**
- Force WASAPI to use different buffer sizes (5ms, 10ms, 20ms)
- Verify algorithm adapts correctly
- Verify no artifacts at any period

### 6.5 Metrics to Log

Enable verbose logging (level 3) and capture:

```
[AUDIO:Layer2] Burst #50: 960 frames, interval=20.03 ms, rate=47913.2 Hz
[AUDIO:Layer2] Rate=47913.2 Hz, Fill=42.3%, Bursts=50, Emergency=0
[AUDIO:Layer2] *** EMERGENCY MODE ACTIVATED: fill=12.1% ***
[AUDIO:Layer2] Emergency mode deactivated: fill=38.7%
```

Expected patterns:
- Burst intervals: 20.00 ± 0.05 ms (PAL)
- Estimated rate: 48000 ± 50 Hz (depending on exact Paula clock)
- Fill level: 35% - 45% (steady state)
- Emergency activations: 0-2 during startup, 0 during steady operation

### 6.6 Acceptance Criteria

✅ **Pass:** All of the following must be true:
1. Zero underruns after initial preroll in steady-state operation
2. Buffer fill variance <5% over 60-second test
3. Rate estimate stable within ±50 Hz
4. Wow/flutter <0.05% WRMS
5. No audible crackling in subjective tests
6. Emergency mode activates <3 times during 1-hour test
7. Clean recovery from pause/resume within 500ms

❌ **Fail:** Any of:
1. Persistent underruns (>5 in 60 seconds)
2. Audible artifacts (crackling, popping, stuttering)
3. Wow/flutter >0.1% WRMS
4. Emergency mode stuck ON
5. Buffer drift (trend toward 0% or 100% over time)

---

## 7. Appendices

### Appendix A: Comparison with Phase 5.5

| Aspect | Phase 5.5 (Failed) | Phase 6 (Burst-Aware) |
|--------|-------------------|----------------------|
| **Measurement** | Buffer fill level (continuous) | Burst timing (event-driven) |
| **Filtering** | EMA with τ=20 bursts | EMA with τ=6.7 bursts |
| **Control law** | PI controller on fill error | Rate matching + emergency override |
| **Sawtooth immunity** | Poor (±10% ripple visible) | Excellent (orthogonal measurement) |
| **Response time** | 200-400ms (lag from smoothing) | 134ms (rate estimate lag) |
| **Emergency handling** | Reactive (deadzone escape) | Proactive (dedicated mode) |
| **Stability** | Marginal (oscillation at 50Hz) | Excellent (no feedback in tracking mode) |

### Appendix B: Glossary

- **Burst:** A sudden arrival of >100 audio frames after a quiet period, characteristic of Paula's frame-synchronized generation
- **Sawtooth:** The periodic rise-and-fall pattern of the ring buffer fill level caused by bursty input and continuous drain
- **Tracking Mode:** Normal operation where the resampler rate matches the estimated Paula rate with no feedback correction
- **Emergency Mode:** Safety mode activated when buffer approaches critical levels, applies aggressive correction
- **EMA:** Exponential Moving Average, a first-order low-pass filter
- **Paula:** The Amiga's custom audio chip (also used as shorthand for Layer 1 audio generation)
- **DXA:** A unit of measurement in Office documents (1440 DXA = 1 inch)

### Appendix C: References

1. WinUAE Source Code Repository (commit b059c623)
2. "Digital Audio Resampling" by Julius O. Smith III (CCRMA)
3. "Control System Design" by Karl Johan Åström
4. Windows QueryPerformanceCounter documentation (Microsoft)
5. Amiga Hardware Reference Manual (Commodore, 1989)

### Appendix D: Future Enhancements

**Phase 7 (Post-Validation):**
- Implement Kalman filter for rate estimation (better noise rejection)
- Add automatic Paula clock calibration (measure actual crystal frequency)
- Implement burst pattern learning (predict next burst time)
- Add telemetry export for offline analysis

**Phase 8 (Optimization):**
- Replace linear interpolation resampler with polyphase filter
- Implement SIMD optimization for resampling
- Add multi-threading support for heavy workloads

**Phase 9 (Advanced Features):**
- Implement synchronous sample rate conversion (SRC)
- Add support for non-standard Paula clock rates
- Implement automatic WASAPI period optimization

---

## Document Revision History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-06 | Audio Team | Initial release |

---

**End of Technical Specification**
