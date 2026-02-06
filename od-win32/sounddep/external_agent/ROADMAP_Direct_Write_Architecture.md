# WinUAE Audio Layer 2 - Direct Write Architecture
## Complete Roadmap & Implementation Guide

**Date:** February 6, 2026  
**Target:** Eliminate batching, achieve continuous sample delivery  
**Approach:** Option 3 (Direct Write) - Professional audio architecture

---

## Executive Summary

**Current Problem:** Phase 6 failed because we tried to detect non-existent "bursts". Paula generates samples continuously (one at a time), but they're batched by an intermediate buffer (`paula_sndbuffer`) before reaching Layer 2.

**Solution:** Remove the intermediate buffer. Write samples directly from Paula's `sample_handler()` to Layer 2's ring buffer. This eliminates the sawtooth, provides accurate rate measurement, and follows professional audio architecture patterns.

**Expected Results:**
- ✅ Zero sawtooth (continuous delivery, not batched)
- ✅ Accurate rate tracking (measured at source from `scaled_sample_evtime`)
- ✅ Lower latency (~5-10ms instead of 20-40ms)
- ✅ Simpler code (no complex control theory needed)
- ✅ Rock-solid stability (natural buffer equilibrium)

---

## Part 1: Current Architecture Analysis

### 1.1 Call Flow (Current)

```
┌─────────────────────────────────────────────────────────────────┐
│ update_audio() - Called every CPU cycle from main emulation     │
│ Located in: audio.cpp                                           │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ Event loop processes channels until next_sample_evtime == 0     │
│ Frequency: Every scaled_sample_evtime CPU cycles                │
│ Rate calculation: syncbase / scaled_sample_evtime ≈ 48011 Hz    │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ (*sample_handler)()  - Generates ONE stereo sample              │
│ Example: sample16_handler(), sample16s_handler()                │
│ Located in: audio.cpp lines 870-1410                            │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ Mix 4 Paula channels:                                           │
│   data = channel[0] + channel[1] + channel[2] + channel[3]      │
│ Apply filter: do_filter(&data, 0)                               │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ PUT_SOUND_WORD_MONO(data) or PUT_SOUND_WORD_LEFT/RIGHT          │
│ Macro definition in sound.h:                                    │
│   #define PUT_SOUND_WORD(b) do {                                │
│     *(uae_u16 *)paula_sndbufpt = b;                             │
│     paula_sndbufpt = (uae_u16 *)(((uae_u8 *)paula_sndbufpt)+2); │
│   } while (0)                                                   │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ check_sound_buffers() - Called after each sample                │
│ Located in: audio.cpp line 778                                  │
│                                                                  │
│ if ((uae_u8*)paula_sndbufpt - (uae_u8*)paula_sndbuffer          │
│     >= paula_sndbufsize) {                                      │
│   finish_sound_buffer();  ← BATCH SENT HERE!                    │
│ }                                                                │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ finish_sound_buffer() - Sound.cpp line 3060                     │
│ Sends entire paula_sndbuffer to Layer 2                         │
│                                                                  │
│ frameCount = bufsize / (2 * sizeof(uae_u16))  ← 960 frames      │
│                                                                  │
│ g_audioLayer2->ProcessFromPaula(                                │
│   (int16_t*)paula_sndbuffer,     ← 960 frames at once           │
│   frameCount,                                                   │
│   scaled_sample_evtime_orig,     ← Rate info                    │
│   sampler_evtime                                                │
│ )                                                                │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ Layer 2: ProcessFromPaula()                                     │
│ - Receives 960 frames every 20ms (BATCHED!)                     │
│ - Resamples 960 frames → ~960 frames @ 48kHz                    │
│ - Writes to ring buffer                                         │
│ - Ring buffer fill jumps up (SAWTOOTH!)                         │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 Key Variables

**In audio.cpp:**
```cpp
float scaled_sample_evtime;  // CPU cycles per output sample
                            // Example: 3546895 / 48011 ≈ 73.88 cycles/sample

float sample_evtime;         // Total cycles per sample (unused?)
float next_sample_evtime;    // Accumulator for next sample timing
```

**In sound.cpp:**
```cpp
float scaled_sample_evtime_orig;  // Original rate (before sound_setadjust)
float sampler_evtime;             // Base rate in cycles

uae_u16 paula_sndbuffer[MAX_SIZE];  // Intermediate buffer
uae_u16 *paula_sndbufpt;            // Write pointer
int paula_sndbufsize;               // Buffer size in bytes
```

**Calculation (sound.cpp line 205):**
```cpp
scaled_sample_evtime_orig = clk * CYCLE_UNIT * sound_sync_multiplier / obtainedfreq;
// Example for PAL @ 48kHz:
// = 3546895 * 256 * 1.0 / 48000
// = 18879.13 CPU cycles per sample
// But this is in CYCLE_UNIT (256), so real cycles = 18879.13 / 256 = 73.75
```

**This means:**
- Paula generates one sample every ~73.75 CPU cycles
- At 3.546895 MHz CPU clock → 3546895 / 73.75 ≈ **48,098 Hz** (actual Paula rate!)

### 1.3 Buffer Sizes

**paula_sndbufsize calculation (typical):**
```cpp
// From sound device initialization
obtainedfreq = 48000 Hz
latency = 20 ms (configurable)

paula_sndbufsize = obtainedfreq * latency / 1000 * bytes_per_frame
                 = 48000 * 20 / 1000 * 4  (2 channels × 2 bytes)
                 = 960 * 4
                 = 3840 bytes
                 = 960 frames (stereo)
```

**This confirms:** The 960-frame "bursts" are simply the buffer size, not vsync!

### 1.4 The Sawtooth Mechanism

```
Ring Buffer Fill Level Over Time:

     │
100% ├──────────────────────────────────────────
     │
 80% ┤
     │       ╱╲              ╱╲              ╱╲
 60% ┤      ╱  ╲            ╱  ╲            ╱  ╲
     │     ╱    ╲          ╱    ╲          ╱    ╲
 40% ┤────╱──────╲────────╱──────╲────────╱──────╲───
     │   ╱        ╲      ╱        ╲      ╱        ╲
 20% ┤  ╱          ╲    ╱          ╲    ╱          ╲
     │ ╱            ╲  ╱            ╲  ╱            ╲
  0% ├───────────────────────────────────────────────►
     0ms          20ms          40ms          60ms

     ↑             ↑             ↑
   Burst         Burst         Burst
   (+960)        (+960)        (+960)
```

**Problem:** PID controllers see this sawtooth as "error" and try to correct it, causing audio artifacts.

---

## Part 2: Target Architecture (Direct Write)

### 2.1 New Call Flow

```
┌─────────────────────────────────────────────────────────────────┐
│ update_audio() - Same as before                                 │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ (*sample_handler)() - Same mixing logic                         │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ PUT_SOUND_SAMPLE_DIRECT(left, right, timestamp)  ← NEW!         │
│ Replaces: PUT_SOUND_WORD macro                                  │
│                                                                  │
│ Calls directly to Layer 2:                                      │
│   g_audioLayer2->PushSample(left, right,                        │
│                             scaled_sample_evtime)               │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ Layer 2: PushSample() - NEW METHOD                              │
│ - Receives ONE stereo sample at a time                          │
│ - Writes directly to input ring buffer (lock-free SPSC)         │
│ - Updates rate measurement from scaled_sample_evtime            │
│ - No batching, no accumulation delay                            │
└────────────┬────────────────────────────────────────────────────┘
             │
             ▼
┌─────────────────────────────────────────────────────────────────┐
│ Layer 2: Resampler Thread (runs independently)                  │
│ - Pulls samples from input buffer                               │
│ - Resamples Paula rate → 48kHz                                  │
│ - Writes to output buffer                                       │
│ - WASAPI pulls from output buffer                               │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Ring Buffer Fill Level (New)

```
Ring Buffer Fill Level Over Time (Continuous Delivery):

     │
100% ├──────────────────────────────────────────
     │
 80% ┤
     │
 60% ┤
     │
 40% ┤━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ← Stable!
     │
 20% ┤
     │
  0% ├───────────────────────────────────────────►
     0ms          20ms          40ms          60ms

Small ripple (±1-2%) from quantization, but NO sawtooth!
```

### 2.3 Dual Ring Buffer Architecture

```
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   Paula      │────▶│ Input Ring   │────▶│  Resampler   │
│ (48.011 kHz) │     │ Buffer       │     │ Thread/Task  │
│              │     │ (int16_t)    │     │              │
└──────────────┘     │ ~10ms cap    │     └──────┬───────┘
                     └──────────────┘            │
                                                 ▼
┌──────────────┐     ┌──────────────┐     ┌──────────────┐
│   WASAPI     │◀────│ Output Ring  │◀────│  Resampler   │
│  (48 kHz)    │     │ Buffer       │     │  Output      │
│              │     │ (float)      │     │              │
└──────────────┘     │ ~200ms cap   │     └──────────────┘
                     └──────────────┘
```

**Key properties:**
- Input buffer: Small (480 frames = 10ms @ 48kHz), holds raw Paula samples
- Output buffer: Large (9600 frames = 200ms), holds resampled output for WASAPI
- Lock-free SPSC (Single Producer Single Consumer) - no mutex overhead
- Resampler runs independently, pulls when data available

---

## Part 3: Implementation Roadmap

### Phase 1: Preparation & Infrastructure (2-3 hours)

#### Step 1.1: Add PushSample Method to Layer 2

**File:** `od-win32/sounddep/audio_layer2.h`

**Changes:**
```cpp
class AudioLayer2 {
public:
    // ... existing methods ...
    
    // NEW: Direct sample push interface
    void PushSample(int16_t left, int16_t right, float cycles_per_sample);
    
private:
    // NEW: Input ring buffer for raw Paula samples
    AudioRingBuffer<int16_t>* inputBuffer;
    
    // NEW: Rate measurement
    struct RateMeasurement {
        double currentRate;
        double emaRate;
        uint64_t lastUpdateTime;
        int sampleCount;
    } rateMeasurement;
    
    // NEW: Helper methods
    void UpdateRateMeasurement(float cycles_per_sample);
    void ResampleInputToOutput();
};
```

#### Step 1.2: Implement PushSample

**File:** `od-win32/sounddep/audio_layer2.cpp`

```cpp
void AudioLayer2::PushSample(int16_t left, int16_t right, float cycles_per_sample) {
    if (!initialized || !inputBuffer) return;
    
    // Interleave stereo
    int16_t stereo[2] = {left, right};
    
    // Write to input buffer
    if (!inputBuffer->Write(stereo, 1)) {
        // Input buffer full (rare, means resampler is falling behind)
        g_audioDebugVars.layer2Overruns++;
        
        // Drop oldest sample and try again (better than blocking)
        int16_t dummy[2];
        inputBuffer->Read(dummy, 1);
        inputBuffer->Write(stereo, 1);
    }
    
    // Update rate measurement
    UpdateRateMeasurement(cycles_per_sample);
    
    // Trigger resampling (non-blocking)
    ResampleInputToOutput();
}

void AudioLayer2::UpdateRateMeasurement(float cycles_per_sample) {
    // Calculate instantaneous rate
    // Paula clock speed: from syncbase (e.g., 3546895 Hz for PAL)
    extern double syncbase;
    double instantRate = syncbase / cycles_per_sample;
    
    // Sanity check
    if (instantRate < 40000.0 || instantRate > 56000.0) {
        // Invalid - probably initialization or mode switch
        return;
    }
    
    // EMA filter (very slow - 10000 sample time constant)
    const double alpha = 0.0001;
    
    if (rateMeasurement.currentRate == 0.0) {
        rateMeasurement.currentRate = instantRate;
        rateMeasurement.emaRate = instantRate;
    } else {
        rateMeasurement.emaRate = alpha * instantRate + 
                                  (1.0 - alpha) * rateMeasurement.emaRate;
        rateMeasurement.currentRate = rateMeasurement.emaRate;
    }
    
    // Update debug variables
    g_audioDebugVars.estimatedPaulaRateHz = rateMeasurement.currentRate;
}

void AudioLayer2::ResampleInputToOutput() {
    // Check if we have enough input for efficient resampling
    int available = inputBuffer->GetAvailableRead();
    
    if (available < 16) {
        // Not enough data yet, wait for more to accumulate
        return;
    }
    
    // Don't process too much at once (spread CPU load)
    int toProcess = std::min(available, 128);
    
    // Allocate temp buffer for input
    int16_t* inputSamples = new int16_t[toProcess * config.channels];
    
    // Read from input buffer
    int read = inputBuffer->Read(inputSamples, toProcess);
    
    if (read > 0) {
        // Initialize resampler if needed
        if (!resampler->IsInitialized()) {
            resampler->Initialize(
                rateMeasurement.currentRate > 0 ? rateMeasurement.currentRate : 48000.0,
                config.targetSampleRate,
                config.channels
            );
        } else {
            // Update rate dynamically
            resampler->SetInputRate(rateMeasurement.currentRate);
        }
        
        // Calculate expected output size
        int expectedOutput = (int)(read * config.targetSampleRate / 
                                   rateMeasurement.currentRate) + 32;
        
        // Ensure temp buffer is large enough
        if (expectedOutput > tempBufferCapacity) {
            delete[] tempBuffer;
            tempBufferCapacity = expectedOutput * 2;
            tempBuffer = new float[tempBufferCapacity * config.channels];
        }
        
        // Resample
        int resampled = resampler->Process(
            inputSamples,
            read,
            tempBuffer,
            expectedOutput
        );
        
        // Write to output buffer
        if (resampled > 0) {
            if (!ringBuffer->Write(tempBuffer, resampled)) {
                // Output buffer full (WASAPI not pulling fast enough)
                g_audioDebugVars.layer2Overruns++;
            }
        }
    }
    
    delete[] inputSamples;
}
```

#### Step 1.3: Initialize Input Buffer

**File:** `od-win32/sounddep/audio_layer2.cpp`

**In Initialize():**
```cpp
bool AudioLayer2::Initialize(const AudioLayer2Config& cfg) {
    // ... existing code ...
    
    // Create INPUT ring buffer (small - 10ms capacity)
    int inputCapacity = config.targetSampleRate / 100;  // 480 frames @ 48kHz
    inputBuffer = new AudioRingBuffer<int16_t>();
    if (!inputBuffer->Initialize(inputCapacity, config.channels)) {
        AudioLog(0, _T("Layer2"), _T("Input ring buffer init failed\n"));
        delete inputBuffer;
        inputBuffer = nullptr;
        return false;
    }
    
    // Create OUTPUT ring buffer (large - 200ms capacity)
    int outputCapacity = config.targetSampleRate / 5;  // 9600 frames
    ringBuffer = new AudioRingBuffer<float>();
    if (!ringBuffer->Initialize(outputCapacity, config.channels)) {
        AudioLog(0, _T("Layer2"), _T("Output ring buffer init failed\n"));
        delete ringBuffer;
        ringBuffer = nullptr;
        return false;
    }
    
    // ... rest of initialization ...
    
    // Initialize rate measurement
    rateMeasurement.currentRate = 0.0;
    rateMeasurement.emaRate = 0.0;
    rateMeasurement.sampleCount = 0;
    
    initialized = true;
    return true;
}
```

### Phase 2: Modify Paula Sample Output (3-4 hours)

#### Step 2.1: Create Direct Output Interface

**File:** `audio.cpp`

**Add after existing sample handlers (line ~1410):**

```cpp
// ============================================================================
// DIRECT LAYER 2 OUTPUT (NEW ARCHITECTURE)
// ============================================================================

#ifdef USE_AUDIO_LAYER2

// Forward declaration
extern AudioLayer2* g_audioLayer2;
extern float scaled_sample_evtime;

// Global accumulator for stereo pairing
static int16_t g_sample_accumulator[2] = {0, 0};
static int g_sample_channel_count = 0;

// Direct output function (replaces PUT_SOUND_WORD macro)
static void put_sound_sample_direct(int16_t sample, int channel) {
    g_sample_accumulator[channel] = sample;
    g_sample_channel_count++;
    
    // When we have both channels, send to Layer 2
    if (g_sample_channel_count >= 2) {
        if (g_audioLayer2) {
            g_audioLayer2->PushSample(
                g_sample_accumulator[0],  // Left
                g_sample_accumulator[1],  // Right
                scaled_sample_evtime
            );
        }
        g_sample_channel_count = 0;
    }
}

// New mono sample handler (sends same sample to both channels)
void sample16_handler_direct(void) {
    int data0 = audio_channel[0].data.current_sample;
    int data1 = audio_channel[1].data.current_sample;
    int data2 = audio_channel[2].data.current_sample;
    int data3 = audio_channel[3].data.current_sample;
    int data;
    
    DO_CHANNEL_1(data0, 0);
    DO_CHANNEL_1(data1, 1);
    DO_CHANNEL_1(data2, 2);
    DO_CHANNEL_1(data3, 3);
    data0 &= audio_channel[0].data.adk_mask;
    data1 &= audio_channel[1].data.adk_mask;
    data2 &= audio_channel[2].data.adk_mask;
    data3 &= audio_channel[3].data.adk_mask;
    data0 += data1;
    data0 += data2;
    data0 += data3;
    data = SBASEVAL16(2) + data0;
    data = FINISH_DATA(data, 16, 0);
    
    do_filter(&data, 0);
    get_extra_channels_sample2(&data, NULL, 0);
    
    // Send to Layer 2 directly (mono = same sample both channels)
    put_sound_sample_direct((int16_t)data, 0);
    put_sound_sample_direct((int16_t)data, 1);
}

// New stereo sample handler
void sample16s_handler_direct(void) {
    int data0 = audio_channel[0].data.current_sample;
    int data1 = audio_channel[1].data.current_sample;
    int data2 = audio_channel[2].data.current_sample;
    int data3 = audio_channel[3].data.current_sample;
    int data_left, data_right;
    
    DO_CHANNEL_1(data0, 0);
    DO_CHANNEL_1(data1, 1);
    DO_CHANNEL_1(data2, 2);
    DO_CHANNEL_1(data3, 3);
    data0 &= audio_channel[0].data.adk_mask;
    data1 &= audio_channel[1].data.adk_mask;
    data2 &= audio_channel[2].data.adk_mask;
    data3 &= audio_channel[3].data.adk_mask;
    
    data_left = data0 + data2;
    data_right = data1 + data3;
    
    data_left = SBASEVAL16(1) + data_left;
    data_right = SBASEVAL16(1) + data_right;
    
    data_left = FINISH_DATA(data_left, 16, 0);
    data_right = FINISH_DATA(data_right, 16, 1);
    
    do_filter(&data_left, 0);
    do_filter(&data_right, 1);
    
    get_extra_channels_sample2(&data_left, &data_right, 0);
    
    // Send stereo to Layer 2
    put_sound_sample_direct((int16_t)data_left, 0);
    put_sound_sample_direct((int16_t)data_right, 1);
}

#endif // USE_AUDIO_LAYER2
```

#### Step 2.2: Hook Sample Handlers

**File:** `audio.cpp`

**Modify sample handler selection (around line 2276):**

```cpp
void update_sound_settings(void) {
    // ... existing code ...
    
#ifdef USE_AUDIO_LAYER2
    // Use direct output handlers
    if (currprefs.sound_stereo == SND_MONO) {
        sample_handler = sample16_handler_direct;
    } else {
        sample_handler = sample16s_handler_direct;
    }
    
    AudioLog(2, _T("Paula"), _T("Using DIRECT output handlers (Layer 2)\n"));
#else
    // Original code - use buffered handlers
    if (sample_handler == sample16_handler /* ... */) {
        // ... existing handler selection ...
    }
#endif
}
```

### Phase 3: Disable Old Batch Mechanism (30 minutes)

#### Step 3.1: Bypass finish_sound_buffer

**File:** `sound.cpp`

**Modify finish_sound_buffer() (line 3060):**

```cpp
void finish_sound_buffer(void) {
#ifdef USE_AUDIO_LAYER2
    // Direct write mode - this function should never be called!
    static int warnCount = 0;
    if (warnCount < 5) {
        write_log(_T("WARNING: finish_sound_buffer() called in Direct Write mode!\n"));
        write_log(_T("This indicates PUT_SOUND_WORD macros are still in use.\n"));
        warnCount++;
    }
    
    // Reset buffer pointer just in case
    paula_sndbufpt = paula_sndbuffer;
    return;
#else
    // Original batched code
    // ... keep existing implementation ...
#endif
}
```

#### Step 3.2: Disable Buffer Check

**File:** `audio.cpp`

**Modify check_sound_buffers() (line 778):**

```cpp
static void check_sound_buffers(void) {
#ifdef USE_AUDIO_LAYER2
    // Direct write mode - buffer check disabled
    // Samples go directly to Layer 2, no intermediate buffer
    return;
#else
    // Original code
    #if SOUNDSTUFF > 1
    // ... existing code ...
    #endif
    
    if ((uae_u8*)paula_sndbufpt - (uae_u8*)paula_sndbuffer >= paula_sndbufsize) {
        finish_sound_buffer();
    }
#endif
}
```

### Phase 4: Testing & Validation (4-6 hours)

#### Test 4.1: Compilation Test

```bash
# Build project
msbuild WinUAE.sln /p:Configuration=Release /p:Platform=x64

# Expected: No errors
# Watch for:
#   - Linker errors about PushSample (check audio_layer2.h declaration)
#   - Undefined references to g_audioLayer2 (check extern declaration)
#   - Template instantiation errors (check AudioRingBuffer<int16_t>)
```

#### Test 4.2: Initialization Test

**Log output to check:**
```
[AUDIO:Layer2] Initialized: 48000 Hz, 2 ch
[AUDIO:Layer2] Input buffer: 480 frames (10ms)
[AUDIO:Layer2] Output buffer: 9600 frames (200ms)
[AUDIO:Paula] Using DIRECT output handlers (Layer 2)
```

**Debug variables to watch:**
```cpp
g_audioDebugVars.estimatedPaulaRateHz  // Should converge to ~48011 Hz
inputBuffer->GetAvailableRead()         // Should stay 0-100 frames
ringBuffer->GetFillPercent()            // Should stabilize around 40%
```

#### Test 4.3: Audio Playback Test

**Procedure:**
1. Launch WinUAE
2. Load Kickstart ROM
3. Boot to Workbench
4. Play system sounds (beep, disk insert, etc.)
5. Launch a game with music (e.g., Shadow of the Beast)

**Success criteria:**
- ✅ Audio plays smoothly
- ✅ No crackling or popping
- ✅ No stuttering or gaps
- ✅ No wow/flutter (pitch variations)

**Watch for:**
- ❌ Silent audio → Check PushSample is being called
- ❌ Crackling → Check for underruns in debug vars
- ❌ High CPU usage → Check ResampleInputToOutput isn't blocking

#### Test 4.4: Stress Test

**Long-running test (8 hours):**
```cpp
// Enable verbose logging
int g_audioLogLevel = 3;

// Run overnight with demanding game
// Monitor:
g_audioDebugVars.layer2Underruns  // Should stay at 0
g_audioDebugVars.layer2Overruns   // Should be < 10 total
```

**CPU starvation test:**
```
1. Run WinUAE with demanding game
2. Launch CPU-intensive task (video encoding, compile, etc.)
3. Monitor audio quality
4. Should remain smooth (emergency mode may activate briefly, then recover)
```

#### Test 4.5: Rate Accuracy Test

**Measure actual Paula rate:**

```cpp
// In UpdateRateMeasurement, add logging every 10000 samples:
static int sampleCount = 0;
if (++sampleCount % 10000 == 0) {
    AudioLog(2, _T("Layer2"), _T("Rate measurement: instant=%.2f Hz, EMA=%.2f Hz\n"),
             instantRate, rateMeasurement.emaRate);
}
```

**Expected output:**
```
[AUDIO:Layer2] Rate measurement: instant=48011.23 Hz, EMA=48010.45 Hz
[AUDIO:Layer2] Rate measurement: instant=48012.01 Hz, EMA=48010.47 Hz
[AUDIO:Layer2] Rate measurement: instant=48009.87 Hz, EMA=48010.46 Hz
```

**Variance should be <0.1 Hz (very stable)**

---

## Part 4: Optimization (Optional - Phase 5)

### Optimization 4.1: Batch Small Amounts

**Problem:** Calling PushSample() once per sample has overhead (function call, buffer writes)

**Solution:** Accumulate 8-16 samples before calling Layer 2

```cpp
#define BATCH_SIZE 16

static int16_t g_sample_batch[BATCH_SIZE * 2];  // Stereo
static int g_batch_count = 0;

static void put_sound_sample_direct(int16_t sample, int channel) {
    g_sample_accumulator[channel] = sample;
    g_sample_channel_count++;
    
    if (g_sample_channel_count >= 2) {
        // Add to batch
        g_sample_batch[g_batch_count * 2 + 0] = g_sample_accumulator[0];
        g_sample_batch[g_batch_count * 2 + 1] = g_sample_accumulator[1];
        g_batch_count++;
        
        // Flush batch when full
        if (g_batch_count >= BATCH_SIZE) {
            if (g_audioLayer2) {
                g_audioLayer2->PushSampleBatch(
                    g_sample_batch,
                    g_batch_count,
                    scaled_sample_evtime
                );
            }
            g_batch_count = 0;
        }
        
        g_sample_channel_count = 0;
    }
}
```

**Benefit:** Reduces function call overhead by 16×, minimal latency increase (0.33ms)

### Optimization 4.2: Resampler Thread

**Problem:** ResampleInputToOutput() runs in Paula thread (emulation thread)

**Solution:** Move resampling to separate thread

```cpp
class AudioLayer2 {
private:
    std::thread resamplerThread;
    std::atomic<bool> running;
    
    void ResamplerThreadFunc() {
        while (running) {
            int available = inputBuffer->GetAvailableRead();
            
            if (available >= 64) {
                // Enough data - resample
                ResampleInputToOutput();
            } else {
                // Not enough data - sleep briefly
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    }
    
public:
    bool Initialize(const AudioLayer2Config& cfg) {
        // ... existing init ...
        
        // Start resampler thread
        running = true;
        resamplerThread = std::thread(&AudioLayer2::ResamplerThreadFunc, this);
        
        return true;
    }
    
    void Shutdown() {
        // Stop thread
        running = false;
        if (resamplerThread.joinable()) {
            resamplerThread.join();
        }
        
        // ... existing shutdown ...
    }
};
```

**Benefit:** Removes resampling overhead from emulation thread, improves CPU utilization

---

## Part 5: Debugging & Troubleshooting

### Common Issues & Solutions

#### Issue 5.1: Silent Audio

**Symptoms:**
- No sound output
- `g_audioDebugVars.estimatedPaulaRateHz` stays at 0

**Diagnosis:**
```cpp
// Add logging in PushSample:
static int pushCount = 0;
if (pushCount++ < 100) {
    AudioLog(2, _T("Layer2"), _T("PushSample called: L=%d, R=%d, cycles=%.2f\n"),
             left, right, cycles_per_sample);
}
```

**Possible causes:**
1. PushSample never called → Check sample_handler is set to direct version
2. inputBuffer write fails → Check buffer initialization
3. Resampler not initialized → Check rate measurement convergence

**Solutions:**
- Verify `sample_handler = sample16_handler_direct` in initialization
- Check `inputBuffer != nullptr` before Write
- Add fallback initialization with nominal rate (48000 Hz)

#### Issue 5.2: Crackling/Popping

**Symptoms:**
- Intermittent clicks or pops
- `layer2Underruns` counter increasing

**Diagnosis:**
```cpp
// Log buffer levels:
if (ringBuffer->GetAvailableRead() < 100) {
    AudioLog(1, _T("Layer2"), _T("LOW BUFFER: %d frames available\n"),
             ringBuffer->GetAvailableRead());
}
```

**Possible causes:**
1. Output buffer too small → Increase capacity to 400ms
2. Resampler falling behind → Check CPU usage
3. WASAPI period mismatch → Verify Reconfigure() called

**Solutions:**
- Increase output buffer: `outputCapacity = config.targetSampleRate / 2.5` (400ms)
- Add priority boost to resampler thread
- Ensure WASAPI period is set correctly in Layer 3

#### Issue 5.3: High CPU Usage

**Symptoms:**
- Emulation slows down
- 100% CPU core usage

**Diagnosis:**
```cpp
// Profile ResampleInputToOutput:
auto start = std::chrono::high_resolution_clock::now();
ResampleInputToOutput();
auto end = std::chrono::high_resolution_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

if (duration.count() > 1000) {
    AudioLog(1, _T("Layer2"), _T("SLOW RESAMPLE: %d µs\n"), duration.count());
}
```

**Possible causes:**
1. ResampleInputToOutput called too frequently → Add threshold check
2. Large batch sizes → Limit to 128 frames per call
3. Inefficient resampler → Consider SIMD optimization

**Solutions:**
- Add minimum threshold: `if (available < 16) return;`
- Limit batch: `toProcess = std::min(available, 128);`
- Implement batching (Optimization 4.1)

#### Issue 5.4: Rate Drift

**Symptoms:**
- Audio pitch slowly changes
- Buffer gradually empties or fills
- `estimatedPaulaRateHz` drifts over time

**Diagnosis:**
```cpp
// Log rate over time:
static uint64_t lastRateLog = 0;
uint64_t now = GetMicroseconds();
if (now - lastRateLog > 1000000) {  // Every second
    AudioLog(2, _T("Layer2"), _T("Rate: %.2f Hz, Fill: %.1f%%\n"),
             rateMeasurement.currentRate,
             ringBuffer->GetFillPercent() * 100.0f);
    lastRateLog = now;
}
```

**Possible causes:**
1. `scaled_sample_evtime` not accurate → Check sound_setadjust is disabled
2. EMA too slow → Increase alpha from 0.0001 to 0.001
3. Quantization errors → Use double precision for rate calculation

**Solutions:**
- Force `sound_setadjust(0.0)` to lock native timing
- Tune EMA: `alpha = 0.0005` (2000-sample time constant)
- Add emergency correction if fill < 20% or > 80%

---

## Part 6: Validation Metrics

### Success Criteria

| Metric | Target | Measurement Method |
|--------|--------|-------------------|
| **Underruns** | 0 per hour | `g_audioDebugVars.layer2Underruns` |
| **Buffer fill variance** | <2% | StdDev of `GetFillPercent()` over 60s |
| **Rate stability** | ±10 Hz | StdDev of `estimatedPaulaRateHz` |
| **Latency** | 5-15ms | Input buffer size + output fill |
| **CPU overhead** | <1% | Profile ResampleInputToOutput |
| **Wow/flutter** | <0.02% WRMS | External ADC + analysis tool |

### Performance Baselines

**Expected values (PAL @ 48kHz):**
```
estimatedPaulaRateHz: 48010 ± 10 Hz
inputBuffer fill: 10-100 frames (0.2-2ms)
outputBuffer fill: 3800-4200 frames (79-87.5ms @ 48kHz)
ResampleInputToOutput: <200 µs per call
CPU usage: +0.5% vs baseline
```

---

## Part 7: Rollback Plan

If direct write approach fails:

### Fallback Option 1: Quick Fix (Option 1 from analysis)

**Revert to batched mode but use accurate rate:**

```cpp
void AudioLayer2::ProcessFromPaula(/* ... */) {
    // Use scaled_sample_evtime directly (accurate!)
    double paulaRate = syncCyclesPerSec / cpuCyclesPerSample;
    
    // Simple emergency correction
    float fill = ringBuffer->GetFillPercent();
    double rateAdjustment = 0.0;
    
    if (fill < 0.25f) rateAdjustment = -200.0 * (fill - 0.25f);
    if (fill > 0.75f) rateAdjustment = -200.0 * (fill - 0.75f);
    
    resampler->SetInputRate(paulaRate + rateAdjustment);
    
    // Resample batch
    int resampled = resampler->Process(samples, frameCount, tempBuffer, tempBufferCapacity);
    ringBuffer->Write(tempBuffer, resampled);
}
```

**Benefits:**
- Minimal code change
- Still uses accurate rate from source
- Emergency correction prevents disasters

**Drawbacks:**
- Still has sawtooth (though reduced)
- Still batched (20ms chunks)

### Fallback Option 2: Hybrid Approach

**Keep direct write for mono, use batched for stereo:**

```cpp
#ifdef USE_AUDIO_LAYER2
    if (currprefs.sound_stereo == SND_MONO) {
        sample_handler = sample16_handler_direct;  // Direct
    } else {
        sample_handler = sample16s_handler;        // Batched
    }
#endif
```

This allows testing direct write in simpler scenario first.

---

## Part 8: Timeline & Effort Estimation

### Development Timeline (Total: 12-16 hours)

**Week 1:**
- Day 1-2: Phase 1 (Infrastructure) - 3 hours
- Day 2-3: Phase 2 (Paula modifications) - 4 hours
- Day 3: Phase 3 (Disable old code) - 1 hour
- Day 4-5: Phase 4 (Testing) - 6 hours

**Week 2:**
- Day 1-2: Bug fixes from testing - 4 hours
- Day 3: Performance profiling - 2 hours
- Day 4-5: Documentation & handoff - 2 hours

### Validation Timeline (Total: 12 hours)

- Unit tests: 2 hours
- Integration tests: 3 hours
- Stress tests: 4 hours (overnight runs)
- Performance benchmarks: 2 hours
- Audio quality assessment: 1 hour

### Total Project: 28-32 hours (3.5-4 weeks part-time)

---

## Conclusion

**Current Status:** Phase 6 failed due to false assumption about burst delivery

**Root Cause:** Paula generates continuously, not in bursts. Batching happens in intermediate buffer.

**Solution:** Direct write architecture - bypass intermediate buffer, write samples one-by-one to Layer 2

**Expected Outcome:**
- ✅ Zero sawtooth (continuous delivery)
- ✅ Accurate rate (measured at source)
- ✅ Lower latency (no accumulation delay)
- ✅ Simpler code (no complex control loops)
- ✅ Professional architecture (standard producer-consumer pattern)

**Next Step:** Begin Phase 1 - Add PushSample infrastructure to Layer 2

**Risk Mitigation:** Fallback options available if direct write encounters issues

---

**Ready to proceed with implementation?**
