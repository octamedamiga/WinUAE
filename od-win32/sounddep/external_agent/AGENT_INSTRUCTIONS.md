# INSTRUCTIONS FOR LOCAL REPOSITORY AGENT

## Mission

Apply Phase 6 "Burst-Aware Control Algorithm" to fix critical audio buffer instability in WinUAE Audio Layer 2.

---

## Background (Quick Summary)

**Problem:** Paula emulation generates audio in 20ms bursts (960 frames at once during vsync), not continuously. This creates a sawtooth waveform in the ring buffer fill level. The current PID controller tries to "fix" this sawtooth, causing oscillation and audio crackling.

**Solution:** Measure the TIME between bursts instead of buffer fill level. Calculate Paula rate from timing. Ignore the sawtooth completely. Use buffer level only for emergency detection (<15% or >85%).

**Expected Result:** Stable, crackle-free audio with native Amiga timing.

---

## Files Provided

You have received 7 files:

1. **burst_aware_audio_control_algorithm.docx** - Full technical specification (75 pages, DOCX format)
2. **burst_aware_audio_control_algorithm.md** - Same specification in Markdown
3. **README.md** - Implementation guide and testing instructions
4. **QUICK_REFERENCE.md** - Quick reference for algorithm and troubleshooting
5. **patch_001_audio_layer2_header.patch** - Changes to audio_layer2.h
6. **patch_002_audio_layer2_implementation.patch** - Changes to audio_layer2.cpp
7. **patch_003_debug_variables.patch** - Debug variables extension

---

## Step-by-Step Application

### Step 1: Verify Repository State

```bash
# Ensure you're on the correct commit (or compatible)
git log --oneline -1
# Should show commit b059c623 or recent WinUAE development branch

# Check that target files exist
ls -la od-win32/sounddep/audio_layer2.h
ls -la od-win32/sounddep/audio_layer2.cpp
```

### Step 2: Create Backup Branch

```bash
# CRITICAL: Create a new branch for safety
git checkout -b phase6-burst-aware-algorithm
git status  # Should show "On branch phase6-burst-aware-algorithm"
```

### Step 3: Apply Patches

```bash
# Test patches first (dry run)
git apply --check patch_001_audio_layer2_header.patch
git apply --check patch_002_audio_layer2_implementation.patch
git apply --check patch_003_debug_variables.patch

# If all checks pass, apply for real
git apply patch_001_audio_layer2_header.patch
git apply patch_002_audio_layer2_implementation.patch
git apply patch_003_debug_variables.patch

# Verify
git status
git diff od-win32/sounddep/audio_layer2.h
git diff od-win32/sounddep/audio_layer2.cpp
```

### Step 4: Handle Conflicts (If Any)

If patches don't apply cleanly:

```bash
# Try with 3-way merge
git apply --3way patch_001_audio_layer2_header.patch
git apply --3way patch_002_audio_layer2_implementation.patch
git apply --3way patch_003_debug_variables.patch

# Or apply manually using the technical specification
# See Section 5 "Implementation Specification" in the .docx/.md file
```

### Step 5: Review Changes

**Critical areas to verify:**

1. **audio_layer2.h:**
   - `BurstTracker` struct added (lines ~89-111)
   - Helper methods declared (lines ~140-145)
   - Debug variables extended (lines ~37-41)

2. **audio_layer2.cpp:**
   - `GetMicroseconds()` implemented
   - `HandleBurst()` implemented
   - `UpdateRateEstimate()` implemented
   - `CheckEmergencyConditions()` implemented
   - `ConfigureResampler()` implemented
   - `ProcessFromPaula()` completely rewritten (lines ~200-340)

3. **Logic flow in ProcessFromPaula():**
   ```
   Burst detection → HandleBurst() → 
   CheckEmergencyConditions() → 
   ConfigureResampler() → 
   Resample → Write to ring buffer
   ```

### Step 6: Build

```bash
# Using Visual Studio (recommended)
msbuild WinUAE.sln /p:Configuration=Release /p:Platform=x64

# Or open in IDE
start WinUAE.sln
# Then Build → Build Solution (Ctrl+Shift+B)
```

**Watch for:**
- No compilation errors (should build cleanly)
- Warnings about unused variables are OK during initial testing
- Linker errors about QueryPerformanceCounter mean Windows.h not included (should be fine in WinUAE)

### Step 7: Test

```bash
# Launch WinUAE
./WinUAE.exe

# Or from Visual Studio: Debug → Start Without Debugging (Ctrl+F5)
```

**Test procedure:**

1. **Load Kickstart ROM** (e.g., Kickstart 3.1)
2. **Boot to Workbench**
3. **Enable verbose logging:** 
   - Before launch, edit `audio_layer2.cpp` line ~20:
     ```cpp
     int g_audioLogLevel = 3;  // 0=Errors, 1=Warnings, 2=Info, 3=Verbose
     ```
   - Rebuild
4. **Watch log output** (WinUAE writes to winuae.log or debug console)
5. **Listen for audio quality:**
   - No crackling/popping
   - No stuttering
   - Smooth playback

**Expected log output:**
```
[AUDIO:Layer2] Burst #1: 960 frames, interval=20.03 ms, rate=47913.2 Hz
[AUDIO:Layer2] Rate=47913.2 Hz, Fill=42.3%, Bursts=50, Emergency=0
[AUDIO:Layer2] Rate=47915.8 Hz, Fill=41.8%, Bursts=100, Emergency=0
```

**Success criteria:**
- ✅ Burst intervals: ~20ms ± 0.1ms (PAL)
- ✅ Estimated rate: 47000-49000 Hz (stable within ±50 Hz)
- ✅ Fill level: 35-45% (steady state)
- ✅ Emergency mode: 0-2 activations during startup, then 0
- ✅ Zero underruns after preroll
- ✅ No audible artifacts

### Step 8: Validation Tests

Run the test suite from **Section 6** of the technical specification:

**Quick validation tests:**

1. **Steady State Test (60 seconds):**
   - Play audio for 60 seconds
   - Check: `g_audioDebugVars.layer2Underruns` should be 0
   - Check: Buffer fill variance < 5%

2. **Pause/Resume Test:**
   - Pause emulation for 5 seconds
   - Resume
   - Check: Audio recovers within 500ms, no crackling

3. **Long-Term Stability (8 hours):**
   - Run overnight
   - Check: Underrun counter remains at 0

**Advanced validation (optional):**
- Wow/flutter measurement: <0.05% WRMS (requires ADC capture + analysis)
- Rate change test: Switch PAL↔NTSC, verify clean transition

### Step 9: Commit

If tests pass:

```bash
git add od-win32/sounddep/audio_layer2.h
git add od-win32/sounddep/audio_layer2.cpp
git commit -m "Implement Phase 6 Burst-Aware Control Algorithm for Audio Layer 2

- Replace PID-based buffer fill control with event-driven burst timing
- Measure Paula rate from burst intervals instead of buffer level
- Implement dual-mode operation: tracking + emergency
- Emergency mode only activates when buffer <15% or >85%
- Expected result: Zero underruns, stable audio, no crackling

See: burst_aware_audio_control_algorithm.md for full specification"

# Optional: Tag the commit
git tag phase6-burst-aware-v1.0
```

### Step 10: Merge to Main (After Validation)

```bash
# Switch back to main branch
git checkout main

# Merge the feature branch
git merge phase6-burst-aware-algorithm

# Push if using remote repository
git push origin main
git push --tags
```

---

## Troubleshooting

### Patches Don't Apply

**Symptom:** `git apply` fails with "error: patch failed"

**Solution 1:** Use 3-way merge
```bash
git apply --3way patch_*.patch
# Resolve conflicts manually
```

**Solution 2:** Apply manually
- Read Section 5 of `burst_aware_audio_control_algorithm.md`
- Copy code snippets directly
- Follow implementation guide step-by-step

**Solution 3:** Check file paths
```bash
# Patches expect this structure:
# od-win32/sounddep/audio_layer2.h
# od-win32/sounddep/audio_layer2.cpp

# If your paths differ, edit patch headers:
# --- a/od-win32/sounddep/audio_layer2.h
# +++ b/od-win32/sounddep/audio_layer2.h
```

### Build Errors

**Error:** "BurstTracker: undeclared identifier"
- **Cause:** Patch didn't apply to header file
- **Solution:** Verify `BurstTracker` struct is in audio_layer2.h lines ~89-111

**Error:** "GetMicroseconds: unresolved external"
- **Cause:** Implementation not added to .cpp file
- **Solution:** Verify helper functions are in audio_layer2.cpp lines ~204-270

**Error:** "LARGE_INTEGER: undeclared identifier"
- **Cause:** Windows.h not included
- **Solution:** WinUAE includes it via sysconfig.h/sysdeps.h - check includes order

### Runtime Issues

**No audio:**
- Check `g_audioLogLevel` is set (rebuild after changing)
- Look for "Layer2 Initialized" in logs
- Check preroll hasn't timed out (increase `prerollThreshold` if needed)

**Crackling:**
- Check for underruns: Watch `g_audioDebugVars.layer2Underruns`
- Verify burst detection: Should see "Burst #N" messages
- Try increasing ring buffer capacity in `Reconfigure()`

**Emergency mode stuck ON:**
- Check resampler is updating: Add logging to `ConfigureResampler()`
- Verify rate estimate is reasonable: Should be 47000-49000 Hz
- May need to tune Kp (currently 200.0) or thresholds

**Rate estimate unstable:**
- Check burst intervals: Should be consistent ~20ms
- Verify EMA alpha (0.15) is reasonable
- Check for outlier rejection: Should reject <40kHz or >56kHz

---

## Rollback Procedure

If tests fail and you need to revert:

```bash
# If you created a branch (recommended)
git checkout main
git branch -D phase6-burst-aware-algorithm

# If you committed to main
git revert HEAD

# If patches applied but not committed
git checkout od-win32/sounddep/audio_layer2.h
git checkout od-win32/sounddep/audio_layer2.cpp
git clean -fd

# Rebuild
msbuild WinUAE.sln /t:Clean
msbuild WinUAE.sln /p:Configuration=Release /p:Platform=x64
```

---

## Performance Notes

**CPU Impact:** +0.002% (negligible - 350ns per burst @ 50Hz)
**Memory Impact:** +64 bytes (BurstTracker struct)
**Latency Impact:** 0ms (algorithm is event-driven, no added delay)

The algorithm is extremely lightweight and should not affect emulation performance.

---

## Key Differences from Phase 5.5

| Aspect | Phase 5.5 (Old) | Phase 6 (New) |
|--------|-----------------|---------------|
| Measurement | Buffer fill level (continuous) | Burst timing (event-driven) |
| Control | PI controller | Rate matching + emergency override |
| Sawtooth handling | Filter with EMA+deadzone | Ignore (orthogonal measurement) |
| Response time | 200-400ms | 134ms |
| Stability | Marginal (oscillates) | Excellent |
| Code complexity | ~350 lines | ~300 lines |

**Net result:** Simpler code, better stability, faster response.

---

## References

For detailed understanding:

1. **Full specification:** `burst_aware_audio_control_algorithm.docx` (Section 1-7)
2. **Implementation guide:** `README.md`
3. **Quick reference:** `QUICK_REFERENCE.md`
4. **Troubleshooting:** Technical spec Section 6.6 + README "Debugging" section

---

## Expected Outcome

After successful application:

✅ **Audio quality:** Smooth, crackle-free, indistinguishable from native Amiga
✅ **Timing:** Native Amiga timing (no core warping, no tempo drift)
✅ **Stability:** Zero underruns in steady-state operation
✅ **Performance:** No measurable CPU/latency impact

🎯 **Goal achieved:** Pro-audio quality with Amiga-accurate timing

---

## Summary Checklist

- [ ] Created backup branch (`git checkout -b phase6-burst-aware-algorithm`)
- [ ] Applied patches (`git apply patch_*.patch`)
- [ ] Verified changes (diff shows BurstTracker, helper methods, new ProcessFromPaula)
- [ ] Built successfully (no errors)
- [ ] Tested with Kickstart ROM (boots, audio plays)
- [ ] Checked logs (burst detection working, rate estimate stable)
- [ ] Validated audio quality (no crackling, no underruns)
- [ ] Ran validation tests (60s steady state, pause/resume)
- [ ] Committed changes (`git commit -m "..."`)
- [ ] (Optional) Merged to main after extended testing

---

**IMPORTANT:** This is a critical audio subsystem change. Test thoroughly before deploying to production builds. The algorithm is designed to be safe (emergency mode prevents underruns), but validation is essential.

**Questions?** Refer to the technical specification for mathematical details, implementation guidance, and troubleshooting.

Good luck! 🎵

---

**End of Instructions**
