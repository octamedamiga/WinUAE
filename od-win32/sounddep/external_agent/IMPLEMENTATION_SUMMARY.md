# WinUAE Audio Direct Write - Implementation Summary

## Quick Start Guide

### What This Does

Replaces batched audio delivery (960 frames every 20ms) with continuous delivery (one sample at a time). Eliminates the sawtooth waveform in the ring buffer and provides accurate rate measurement.

### Files Included

1. **ROADMAP_Direct_Write_Architecture.md** - Complete technical documentation (75+ pages)
2. **Layer2_Redesign_Analysis.md** - Root cause analysis and solution comparison
3. **Patch files** (6 total):
   - Phase 1: Layer 2 infrastructure (2 patches)
   - Phase 2: Paula direct output (2 patches)
   - Phase 3: Disable old batch code (1 patch)

---

## Application Instructions

### Step 1: Backup Your Repository

```bash
cd /path/to/WinUAE
git checkout -b direct-write-implementation
git commit -am "Checkpoint before direct write implementation"
```

### Step 2: Apply Patches in Order

```bash
# Phase 1: Layer 2 infrastructure
git apply patch_PHASE1_001_layer2_header.patch
git apply patch_PHASE1_002_layer2_implementation.patch

# Phase 2: Paula modifications
git apply patch_PHASE2_001_paula_direct_handlers.patch
git apply patch_PHASE2_002_disable_buffer_check.patch

# Phase 3: Disable old code
git apply patch_PHASE3_001_disable_finish_sound_buffer.patch

# Verify
git status
git diff
```

### Step 3: Build

```bash
msbuild WinUAE.sln /p:Configuration=Release /p:Platform=x64
```

### Step 4: Test

```bash
# Enable verbose logging in audio_layer2.cpp:
int g_audioLogLevel = 3;  // Line ~20

# Launch WinUAE
./WinUAE.exe

# Load Kickstart and boot to Workbench
# Play system sounds, launch a game

# Monitor log output:
tail -f winuae.log | grep "AUDIO:"
```

---

## Expected Log Output (Success)

```
[AUDIO:Layer2] Initialized: 48000 Hz, 2 ch, InputBuf=480 frames, OutputBuf=9600 frames
[AUDIO:Paula] Using DIRECT STEREO handler (Layer 2)
[AUDIO:Layer2] Initial rate measurement: 48011.23 Hz
[AUDIO:Layer2] Resampler initialized: 48011.23 Hz → 48000 Hz
[AUDIO:Layer2] Rate: instant=48012.01 Hz, EMA=48010.47 Hz, InputBuf=23, OutputBuf=42.3%
```

**Good signs:**
- "Using DIRECT ... handler" message appears
- Rate measurement converges to ~48010 Hz
- Input buffer stays low (0-100 frames)
- Output buffer stabilizes around 40%

---

## Troubleshooting

### Problem: No Audio

**Check:**
```
grep "Using DIRECT" winuae.log
```

If not found → sample_handler not set correctly

**Fix:**
- Verify `patch_PHASE2_001_paula_direct_handlers.patch` applied correctly
- Check `#ifdef USE_AUDIO_LAYER2` is defined

### Problem: "finish_sound_buffer() called in DIRECT WRITE mode"

**Cause:** Paula is still using PUT_SOUND_WORD macros (batched path)

**Fix:**
- Verify `patch_PHASE2_002_disable_buffer_check.patch` applied
- Check that `check_sound_buffers()` returns early in direct write mode

### Problem: Crackling/Popping

**Check:**
```
grep "layer2Underruns\|layer2Overruns" winuae.log
```

If underruns > 0 → Output buffer too small or WASAPI pulling too fast

**Fix:**
- Increase output buffer capacity (change `/ 5` to `/ 2.5` in Initialize)
- Verify WASAPI period configuration in Layer 3

### Problem: High CPU Usage

**Check resampler performance:**

Add to `ResampleInputToOutput()`:
```cpp
auto start = std::chrono::high_resolution_clock::now();
// ... resample code ...
auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
if (duration.count() > 1000) {
    AudioLog(1, _T("Layer2"), _T("SLOW: %d µs\n"), duration.count());
}
```

If > 1000 µs → Resampler running too frequently

**Fix:**
- Increase minimum threshold (change `< 16` to `< 32`)
- Implement batching optimization (see ROADMAP Phase 5)

---

## Architecture Overview

### Before (Batched Mode)

```
Paula → sample_handler() → paula_sndbuffer → finish_sound_buffer() → Layer 2
        (one sample)         (accumulate)     (send 960 frames)

Problem: Creates sawtooth in Layer 2 ring buffer
```

### After (Direct Write Mode)

```
Paula → sample_handler_direct() → PushSample() → Layer 2 input buffer
        (one sample)                              (write 1 frame)
        
        → ResampleInputToOutput() → Layer 2 output buffer → WASAPI
          (pulls when needed)

Benefit: Continuous delivery, no sawtooth
```

### Key Variables

**Rate measurement:**
- `scaled_sample_evtime` (audio.cpp) - CPU cycles per Paula sample (~73.75 for PAL @ 48kHz)
- `syncbase` (global) - Paula clock speed (3546895 Hz for PAL)
- **Calculated rate:** `syncbase / scaled_sample_evtime` ≈ 48011 Hz

**Buffers:**
- Input buffer: 480 frames (10ms) - holds raw Paula samples (int16_t)
- Output buffer: 9600 frames (200ms) - holds resampled output (float)

---

## Success Criteria

### Functional Requirements

- ✅ Audio plays without gaps or crackling
- ✅ Zero underruns after startup (first 5 seconds)
- ✅ Rate estimate stable within ±10 Hz
- ✅ Buffer fill level stable (35-45%)

### Performance Requirements

- ✅ CPU overhead < 1% vs baseline
- ✅ ResampleInputToOutput < 500 µs per call
- ✅ Input buffer rarely exceeds 100 frames
- ✅ Output buffer variance < 5%

### Quality Requirements

- ✅ No audible artifacts (wow/flutter, clicks, pops)
- ✅ Pitch stability (no drift over time)
- ✅ Clean start/stop (no clicks when audio starts/pauses)

---

## Validation Checklist

### Basic Functionality
- [ ] WinUAE compiles without errors
- [ ] Log shows "Using DIRECT ... handler"
- [ ] Rate measurement appears in log
- [ ] Audio plays from system sounds
- [ ] Audio plays from games

### Stability
- [ ] Zero underruns after 1 minute runtime
- [ ] Rate estimate variance < 10 Hz over 1 minute
- [ ] Buffer fill stays between 35-45%
- [ ] No crashes or hangs

### Quality
- [ ] No crackling or popping
- [ ] No pitch drift (A440 stays at 440 Hz)
- [ ] Clean audio start (no click at first sound)
- [ ] Pause/resume works without artifacts

### Performance
- [ ] CPU usage similar to baseline (within 1%)
- [ ] Emulation speed stable (no slowdown)
- [ ] Long-term stability (8+ hour test)

---

## Next Steps After Validation

### If Tests Pass ✅

1. Remove old batched code (ProcessFromPaula can be simplified)
2. Implement optimizations:
   - Sample batching (16 samples per PushSample call)
   - Resampler thread (move to separate thread)
   - SIMD optimization for resampler

3. Add interpolation support:
   - Create `sample16s_handler_direct_sinc()`
   - Create `sample16s_handler_direct_anti()`

4. Documentation:
   - Update code comments
   - Create developer guide
   - Add to WinUAE wiki

### If Tests Fail ❌

1. **Fallback Plan:** Revert to Quick Fix (Option 1)
   - Use `scaled_sample_evtime` for rate in batched mode
   - Keep simple emergency correction
   - Document as interim solution

2. **Debug Strategy:**
   - Enable verbose logging (level 3)
   - Capture 60 seconds of metrics
   - Analyze rate stability, buffer behavior
   - Check for race conditions (threading issues)

3. **Hybrid Approach:**
   - Keep direct write for mono
   - Use batched for stereo
   - Isolate which mode has issues

---

## Technical Reference

### Critical Code Paths

**Sample generation:** `audio.cpp:2450` - `update_audio()` main loop

**Direct output:** `audio.cpp:1408` - `sample16s_handler_direct()`

**Rate measurement:** `audio_layer2.cpp:249` - `UpdateRateMeasurement()`

**Resampling:** `audio_layer2.cpp:307` - `ResampleInputToOutput()`

### Important Constants

```cpp
#define INPUT_BUFFER_SIZE  (48000 / 100)   // 480 frames = 10ms
#define OUTPUT_BUFFER_SIZE (48000 / 5)     // 9600 frames = 200ms
#define MIN_RESAMPLE_BATCH 16              // Minimum frames before resampling
#define MAX_RESAMPLE_BATCH 128             // Maximum frames per resample call
#define RATE_EMA_ALPHA     0.0001          // Rate smoothing (10000-sample τ)
```

### Debug Variables to Monitor

```cpp
g_audioDebugVars.estimatedPaulaRateHz   // Should be ~48010 Hz
g_audioDebugVars.inputBufferFrames      // Should be 0-100
g_audioDebugVars.outputBufferFrames     // Should be ~4000 (42%)
g_audioDebugVars.pushSampleCalls        // Increments continuously
g_audioDebugVars.resampleCalls          // Increments in bursts
g_audioDebugVars.layer2Underruns        // Should stay at 0
g_audioDebugVars.layer2Overruns         // Should be < 10 total
```

---

## FAQ

**Q: Why direct write instead of fixing the PID controller?**

A: The sawtooth is inherent to batched delivery. No amount of filtering can distinguish between the 20ms sawtooth ripple and real drift. Direct write eliminates the problem at the source.

**Q: Will this work with interpolation modes (SINC, anti-click)?**

A: Not in Phase 1. Interpolated direct handlers need to be implemented separately. Current implementation uses basic filtering only.

**Q: What about CD audio, AHI, sound boards?**

A: This only affects Paula (4-channel chip audio). CD audio, AHI, and sound boards are handled separately and remain unchanged.

**Q: Does this increase latency?**

A: No - actually reduces it! Batched mode had 20ms accumulation delay. Direct write has only the resampler delay (~0.5ms) plus output buffer (configurable, default 40ms).

**Q: Is this compatible with save states?**

A: Yes. Audio state is ephemeral and not saved. Save state compatibility is unaffected.

**Q: What if I need to rollback?**

A: Simply revert the git branch:
```bash
git checkout main  # or original branch
git branch -D direct-write-implementation
```

---

## Contact & Support

**Documentation:** See ROADMAP_Direct_Write_Architecture.md for complete details

**Issues:** Check troubleshooting section above

**Questions:** Refer to technical reference or source code comments

---

## Acknowledgments

**Architecture design:** Based on professional audio pipeline patterns (ASIO, CoreAudio, JACK)

**Analysis:** Root cause investigation of Phase 6 failure

**Implementation:** Conservative, incremental approach with fallback options

---

**Ready to implement? Start with Step 1 (Backup) and proceed sequentially.**

Good luck! 🎵
