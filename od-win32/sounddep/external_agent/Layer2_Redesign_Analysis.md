# WinUAE Audio Layer 2 - Root Cause Analysis & New Approach

## Post-Mortem: Why Phase 6 Failed

### Test Results
- **Symptom:** Complete audio loss, only crackling audible
- **Root cause:** Burst detection logic failed - no bursts were actually detected
- **Why:** The assumption about "bursts" was fundamentally wrong

### The False Assumption

**We assumed:** Paula sends audio in discrete 20ms bursts during vsync_handle()

**Reality:** Paula generates samples **continuously**, not in bursts. Let me explain the actual architecture:

## How Paula Audio REALLY Works

### 1. The Sample Generation Loop (update_audio)

```cpp
void update_audio(void) {
    n_cycles = get_cycles() - last_cycles;
    
    while (n_cycles > 0) {
        // Find next event (channel DMA, sample output, etc.)
        best_evtime = min(channel_evtimes, next_sample_evtime);
        
        // Advance time
        next_sample_evtime -= best_evtime;
        
        // When next_sample_evtime reaches 0:
        if (rounded == best_evtime) {
            next_sample_evtime += scaled_sample_evtime;  // Schedule next sample
            (*sample_handler)();  // Output ONE sample
        }
    }
}
```

**Key insight:** `sample_handler()` is called **once per output sample**, not once per frame!

### 2. The Sample Handler (sample16_handler)

```cpp
void sample16_handler(void) {
    // Mix 4 Paula channels
    data0 = audio_channel[0].data.current_sample;
    data1 = audio_channel[1].data.current_sample;
    data2 = audio_channel[2].data.current_sample;
    data3 = audio_channel[3].data.current_sample;
    
    data = data0 + data1 + data2 + data3;
    
    // Apply filter
    do_filter(&data, 0);
    
    // Write to output buffer
    set_sound_buffers();
    PUT_SOUND_WORD_MONO(data);  // Writes ONE 16-bit sample
    check_sound_buffers();      // Calls finish_sound_buffer() if buffer full
}
```

### 3. The Output Buffer (paula_sndbuffer)

```cpp
static void check_sound_buffers(void) {
    // Check if buffer is full
    if ((uae_u8*)paula_sndbufpt - (uae_u8*)paula_sndbuffer >= paula_sndbufsize) {
        finish_sound_buffer();  // Send to Layer 2!
    }
}
```

**This is the REAL batch mechanism:**
- Samples are generated **one at a time** by `sample_handler()`
- They accumulate in `paula_sndbuffer`
- When buffer is full (size = `paula_sndbufsize`), `finish_sound_buffer()` is called
- `finish_sound_buffer()` sends the entire buffer to Layer 2 (via sound.cpp)

## The Actual "Burst" Mechanism

### What We Thought:
```
vsync_handle() → Generate 960 samples at once → Send to Layer 2 → Wait 20ms → Repeat
```

### What Actually Happens:
```
Continuous loop (running at emulation speed):
  ├─ Generate 1 sample (every scaled_sample_evtime cycles)
  ├─ Write to paula_sndbuffer
  ├─ Is buffer full?
  │   ├─ No: Continue
  │   └─ Yes: Call finish_sound_buffer()
  │            └─ Send ENTIRE buffer to Layer 2 (this is the "burst"!)
  │            └─ Reset buffer pointer
  └─ Repeat
```

**The "burst" we observed is NOT from vsync - it's from the buffer filling up!**

The buffer size is determined by:
```cpp
paula_sndbufsize = sound_desired_freq * sound_latency / 1000
```

Typical values:
- `sound_desired_freq` = 48000 Hz
- `sound_latency` = 20 ms (or whatever is configured)
- `paula_sndbufsize` = 48000 * 20 / 1000 = 960 frames

**That's why we see 960-frame "bursts" every 20ms - it's the buffer size, not vsync!**

## Why Phase 6 Failed

### The Burst Detection Logic

```cpp
const int BURST_THRESHOLD = 100;
const int QUIET_THRESHOLD = 240;  // 5ms @ 48kHz

bool isBurst = (frameCount > BURST_THRESHOLD && 
                burstTracker.quietFrameCount > QUIET_THRESHOLD);
```

**Problem:** There is NO quiet period! Samples arrive continuously from `finish_sound_buffer()`.

**What actually happens:**
1. `finish_sound_buffer()` sends 960 frames
2. Immediately starts filling buffer again
3. 20ms later, sends another 960 frames
4. No gap, no quiet period between calls

**Result:**
- `burstTracker.quietFrameCount` never reaches 240
- Bursts are never detected
- `estimatedPaulaRate` stays at 0.0
- Resampler fails to initialize or uses garbage rate
- Audio is silent or crackling

## The Real Problem: Interface Design

### Current Interface (finish_sound_buffer)

```cpp
// In sound.cpp (called from audio.cpp)
void finish_sound_buffer(void) {
    int frames = (paula_sndbufpt - paula_sndbuffer) / bytes_per_frame;
    
    // Send to Layer 2
    g_audioLayer2->ProcessFromPaula(
        paula_sndbuffer,
        frames,
        cpu_cycles_per_sample,  // Problem: Not accurate!
        syncbase              // Problem: Not the actual rate!
    );
}
```

**Issues:**
1. **cpu_cycles_per_sample is approximate** - it's calculated from configuration, not measured
2. **No timing information** - Layer 2 doesn't know WHEN samples were generated
3. **No rate information** - Paula's actual output rate is lost

### What Layer 2 Needs to Know

1. **Exact sample rate:** How fast is Paula generating samples?
2. **Timing precision:** When were these samples generated (for latency calculation)?
3. **Buffer semantics:** Is this a complete batch or partial?

### What Layer 2 Currently Gets

1. **Sample count:** "Here are N frames"
2. **Approximate cycles per sample:** Derived from config (unreliable)
3. **Sync base:** A constant, not actual measured rate

**Gap:** No actual rate measurement, no timing precision!

---

## New Approach: Rate Measurement at Source

### Core Idea

**Don't try to guess the rate from batches. Measure it where it's generated.**

### Implementation Options

## Option 1: Expose scaled_sample_evtime

**Simplest solution - use what's already there:**

```cpp
// In audio.cpp - expose the variable
extern float scaled_sample_evtime;  // Cycles per output sample

// In sound.cpp finish_sound_buffer()
void finish_sound_buffer(void) {
    int frames = get_buffer_frames();
    
    // Calculate actual rate from scaled_sample_evtime
    double cycles_per_sec = syncbase;  // e.g., 3546895 for PAL
    double samples_per_sec = cycles_per_sec / scaled_sample_evtime;
    
    g_audioLayer2->ProcessFromPaula(
        paula_sndbuffer,
        frames,
        samples_per_sec,  // ACTUAL measured rate!
        get_microseconds()  // Timestamp
    );
}
```

**Pros:**
- Minimal code change
- Uses existing, accurate variable
- No new measurement logic needed

**Cons:**
- Still batched delivery (960 frames at once)
- Resampler still processes in bursts
- Ring buffer still has sawtooth (though much smaller)

## Option 2: Timestamp Each Batch

**Track generation time precisely:**

```cpp
// In audio.cpp - add timestamps
static uint64_t last_finish_time = 0;
static int samples_since_last_finish = 0;

void check_sound_buffers(void) {
    if (buffer_full) {
        uint64_t now = get_microseconds();
        
        finish_sound_buffer_ex(
            paula_sndbuffer,
            frames,
            scaled_sample_evtime,
            last_finish_time,  // When did we start this batch?
            now                // When did we finish it?
        );
        
        last_finish_time = now;
    }
}
```

**In Layer 2:**
```cpp
void ProcessFromPaula(
    const int16_t* samples,
    int frameCount,
    double cycles_per_sample,
    uint64_t start_time,
    uint64_t end_time)
{
    // Calculate ACTUAL rate from time delta
    double duration_sec = (end_time - start_time) / 1000000.0;
    double actual_rate = frameCount / duration_sec;
    
    // Use this rate for resampler
    resampler->SetInputRate(actual_rate);
    
    // Resample and write
    resample_and_write(samples, frameCount);
}
```

**Pros:**
- Precise rate measurement
- Accounts for emulation speed variations
- Can detect drift/stutter

**Cons:**
- More invasive (changes finish_sound_buffer signature)
- Timestamp precision limited by OS (1μs typical)
- Still has batching artifacts

## Option 3: Ring Buffer Direct Write (BEST)

**Bypass the batch mechanism entirely:**

### Architecture

```
Paula Channels → sample_handler() → DIRECT → Layer 2 Ring Buffer
                     (per sample)             (lock-free SPSC)
```

**Remove the intermediate paula_sndbuffer entirely!**

### Implementation

```cpp
// In audio.cpp - replace PUT_SOUND_WORD macro
#define PUT_SOUND_WORD_LEFT(s) put_sound_sample_left(s)
#define PUT_SOUND_WORD_RIGHT(s) put_sound_sample_right(s)

static int16_t sample_accumulator[2] = {0, 0};  // L, R

void put_sound_sample_left(int16_t sample) {
    sample_accumulator[0] = sample;
}

void put_sound_sample_right(int16_t sample) {
    sample_accumulator[1] = sample;
    
    // Both channels ready - send to Layer 2
    if (g_audioLayer2) {
        g_audioLayer2->PushSample(
            sample_accumulator[0],
            sample_accumulator[1],
            scaled_sample_evtime,  // Current rate
            get_cycles()           // Timestamp
        );
    }
}
```

**In Layer 2:**
```cpp
class AudioLayer2 {
private:
    AudioRingBuffer<int16_t>* inputBuffer;   // Paula samples (native rate)
    AudioRingBuffer<float>* outputBuffer;    // Resampled (48kHz)
    
    // Resampler runs on separate thread (or WASAPI callback)
    void ResamplerThread() {
        while (running) {
            // Pull samples from inputBuffer
            int available = inputBuffer->GetAvailableRead();
            if (available > 0) {
                int16_t* samples = inputBuffer->GetReadPointer();
                
                // Resample
                float* output = outputBuffer->GetWritePointer();
                int resampled = resampler->Process(samples, available, output);
                
                inputBuffer->AdvanceRead(available);
                outputBuffer->AdvanceWrite(resampled);
            }
            
            // Sleep if no work
            if (available == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }
    
public:
    void PushSample(int16_t left, int16_t right, float cycles_per_sample, uint64_t timestamp) {
        int16_t stereo[2] = {left, right};
        
        // Write directly to input ring buffer (lock-free)
        if (!inputBuffer->Write(stereo, 1)) {
            // Input buffer full (shouldn't happen if sized correctly)
            g_audioDebugVars.layer2Overruns++;
        }
        
        // Update rate measurement
        UpdateRateMeasurement(cycles_per_sample, timestamp);
    }
    
    void UpdateRateMeasurement(float cycles_per_sample, uint64_t timestamp) {
        // Calculate rate from cycles
        double rate = syncbase / cycles_per_sample;
        
        // Smooth with EMA
        const double alpha = 0.001;  // Very slow (1000-sample time constant)
        if (estimatedRate == 0.0) {
            estimatedRate = rate;
        } else {
            estimatedRate = alpha * rate + (1.0 - alpha) * estimatedRate;
        }
        
        // Update resampler
        resampler->SetInputRate(estimatedRate);
    }
};
```

**Pros:**
- ✅ Continuous delivery (no batches!)
- ✅ Sawtooth eliminated (samples arrive one at a time)
- ✅ Precise rate measurement (updated per sample)
- ✅ Lower latency (no buffer accumulation delay)
- ✅ Clean architecture (true producer-consumer pattern)

**Cons:**
- More invasive change (replaces paula_sndbuffer mechanism)
- Need to handle stereo interleaving carefully
- Requires thread-safe ring buffer (already have this!)

---

## Recommended Solution: Option 3 (Direct Write)

### Why This is Best

1. **Eliminates the root cause:** No more batching = no more sawtooth
2. **Proper rate measurement:** Uses actual `scaled_sample_evtime` from source
3. **Scalable:** Works for any Paula rate, any output rate
4. **Professional:** Standard audio architecture (producer → ring buffer → consumer)

### Implementation Plan

#### Phase 1: Create Direct Path (2-3 hours)

1. Add `PushSample()` method to Layer 2
2. Replace `PUT_SOUND_WORD` macros to call Layer 2 directly
3. Keep `finish_sound_buffer()` as fallback (disabled with #ifdef)

#### Phase 2: Rate Measurement (1 hour)

1. Pass `scaled_sample_evtime` with each sample
2. Implement EMA filter for rate smoothing
3. Update resampler dynamically

#### Phase 3: Optimize (2 hours)

1. Batch small amounts (8-16 samples) to reduce function call overhead
2. Use SIMD for resampling if beneficial
3. Profile and tune ring buffer sizes

### Expected Results

- ✅ **Zero sawtooth:** Samples arrive continuously, not in bursts
- ✅ **Accurate rate:** Measured from source, not guessed
- ✅ **Low latency:** ~5ms (limited only by WASAPI period)
- ✅ **Stable:** No emergency mode needed (buffer naturally stable)
- ✅ **Simple:** No complex control theory, just direct data flow

---

## Alternative: Hybrid Approach (Option 1 + Small Fix)

If Option 3 is too invasive, we can salvage the current architecture:

### Quick Fix for Phase 6

```cpp
void AudioLayer2::ProcessFromPaula(
    const int16_t* samples,
    int frameCount,
    double cpuCyclesPerSample,
    double syncCyclesPerSec)
{
    // Calculate ACTUAL Paula rate (don't guess from buffers!)
    double paulaRate = syncCyclesPerSec / cpuCyclesPerSample;
    
    // Initialize resampler if needed
    if (!resampler->IsInitialized()) {
        resampler->Initialize(paulaRate, config.targetSampleRate, config.channels);
    }
    
    // Check if rate changed significantly (mode switch, etc.)
    if (abs(paulaRate - lastPaulaRate) > 100.0) {
        resampler->SetInputRate(paulaRate);
        lastPaulaRate = paulaRate;
    }
    
    // Simple buffer-level emergency correction (keep from Phase 6)
    float fill = ringBuffer->GetFillPercent();
    double rateAdjustment = 0.0;
    
    if (fill < 0.20f) {
        // Starving - speed up slightly
        rateAdjustment = -200.0 * (fill - 0.20f);  // Up to +40 Hz boost
    } else if (fill > 0.80f) {
        // Overflowing - slow down slightly
        rateAdjustment = -200.0 * (fill - 0.80f);  // Up to -40 Hz reduction
    }
    
    // Apply adjustment (clamped)
    double effectiveRate = paulaRate + rateAdjustment;
    effectiveRate = std::clamp(effectiveRate, paulaRate * 0.98, paulaRate * 1.02);
    
    // Update resampler
    resampler->SetInputRate(effectiveRate);
    
    // Resample and write
    int resampled = resampler->Process(samples, frameCount, tempBuffer, tempBufferCapacity);
    ringBuffer->Write(tempBuffer, resampled);
}
```

**This works because:**
- Uses correct rate from source (`syncCyclesPerSec / cpuCyclesPerSample`)
- Emergency correction is simple and bounded (±2%)
- No burst detection, no complex state machines
- Sawtooth still exists but is small (±20ms in 200ms buffer = ±10%)

**Trade-offs:**
- Still has batching artifacts (though reduced)
- Emergency mode may trigger occasionally
- Not as clean as Option 3

---

## Conclusion

**Root cause:** We misunderstood Paula's architecture. It generates samples continuously, not in bursts. The "bursts" we see are artifacts of buffer accumulation in `paula_sndbuffer`.

**Best solution:** Option 3 (Direct Write) - bypass the intermediate buffer entirely and write samples directly to Layer 2's ring buffer.

**Quick fix:** Use actual `scaled_sample_evtime` for rate measurement instead of trying to detect non-existent "bursts".

**Next steps:**
1. Decide on approach (recommend Option 3 for long-term stability)
2. Implement and test
3. Measure results (underruns, latency, audio quality)

The good news: **The architecture is salvageable.** We just need to measure rate at the source instead of trying to reverse-engineer it from batch timing.
