# VISUAL ARCHITECTURE DIAGRAM
## WinUAE Audio Layer 2 - Phase 1

```
┌───────────────────────────────────────────────────────────────────────────┐
│                         WINUAE AUDIO PIPELINE                             │
└───────────────────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────────────────┐
│  LAYER 1: Paula Emulation (Existing WinUAE Code)                          │
├───────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  File: audio.cpp                                                          │
│  ┌──────────────────────────────────────────────────────────────┐        │
│  │ finish_sound_buffer()                                         │        │
│  │  ├─ Generates audio samples                                  │        │
│  │  ├─ Rate: Variable (~48011 Hz for PAL @ 48kHz)              │        │
│  │  ├─ Format: int16_t stereo interleaved                       │        │
│  │  └─ Buffer: paula_sndbuffer[] (up to 65536 samples)         │        │
│  └──────────────────────────────────────────────────────────────┘        │
│                                                                            │
│                        ▼ ProcessFromPaula()                               │
└───────────────────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────────────────┐
│  LAYER 2: Resampling & Buffering (NEW - This Package!)                   │
├───────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ audio_layer2.cpp - Main Coordinator                                │  │
│  │                                                                     │  │
│  │  ProcessFromPaula(samples, frameCount, cpuCyclesPerSample)        │  │
│  │  ├─ Calculate actual Paula rate                                   │  │
│  │  ├─ Initialize/update resampler if rate changed                   │  │
│  │  ├─ Resample to target rate (48000 Hz)                           │  │
│  │  └─ Write to ring buffer                                          │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                            │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ audio_resampler.cpp - Rate Conversion                              │  │
│  │                                                                     │  │
│  │  ┌─────────────┐                                                   │  │
│  │  │ Input       │  Linear Interpolation                             │  │
│  │  │ ~48011 Hz   │────────────────────────────►  Output: 48000 Hz   │  │
│  │  │ int16       │  Convert to float [-1, 1]                         │  │
│  │  └─────────────┘                                                   │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                            │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ audio_ringbuffer.h - Lock-Free Buffer (Template)                   │  │
│  │                                                                     │  │
│  │     Write (Paula thread)                Read (Audio thread)        │  │
│  │          ▼                                     ▲                    │  │
│  │  ┌────────────────────────────────────────────────┐                │  │
│  │  │  [  ] [  ] [██] [██] [██] [  ] [  ] [  ]      │                │  │
│  │  │   ^                             ^              │                │  │
│  │  │   Read Pos                      Write Pos      │                │  │
│  │  └────────────────────────────────────────────────┘                │  │
│  │                                                                     │  │
│  │  Features:                                                          │  │
│  │  • Single producer / Single consumer                               │  │
│  │  • Lock-free atomic operations                                     │  │
│  │  • Power-of-2 size (fast modulo with bit mask)                    │  │
│  │  • Track underruns/overruns                                        │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                            │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │ Debug Variables (Global)                                            │  │
│  │                                                                     │  │
│  │  g_audioDebugVars {                                                 │  │
│  │    paulaActualRate:         48011.234 Hz                           │  │
│  │    ringBufferFillPercent:   0.453 (45.3%)                          │  │
│  │    layer2Underruns:         0                                      │  │
│  │    layer2Overruns:          0                                      │  │
│  │    resamplerInputRate:      48011                                  │  │
│  │    resamplerOutputRate:     48000                                  │  │
│  │  }                                                                  │  │
│  │                                                                     │  │
│  │  ← Visible in VS Watch Window for debugging!                       │  │
│  └────────────────────────────────────────────────────────────────────┘  │
│                                                                            │
│                        ▼ PullSamples()                                    │
└───────────────────────────────────────────────────────────────────────────┘

┌───────────────────────────────────────────────────────────────────────────┐
│  LAYER 3: Audio Backend (Phase 2 - Not in this package)                  │
├───────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  File: sound.cpp (to be modified in Phase 2)                             │
│  ┌──────────────────────────────────────────────────────────────┐        │
│  │ WASAPI Callback                                               │        │
│  │  ├─ PullSamples() from Layer 2                               │        │
│  │  ├─ Format: float samples                                     │        │
│  │  └─ Send to Windows audio device                             │        │
│  └──────────────────────────────────────────────────────────────┘        │
│                                                                            │
│                        ▼ Hardware                                         │
└───────────────────────────────────────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════

DATA FLOW EXAMPLE:

1. Paula generates 512 samples @ ~48011 Hz
   ┌─────────────────────────────────────────┐
   │ [s0][s1][s2]...[s511]                   │ int16 stereo
   │ Time: ~10.66 ms                         │
   └─────────────────────────────────────────┘

2. Resampler converts to 48000 Hz
   ┌─────────────────────────────────────────┐
   │ [s0][s1][s2]...[s510]                   │ float stereo  
   │ Time: ~10.67 ms (511 samples)           │ (-1 sample drift compensated)
   └─────────────────────────────────────────┘

3. Ring buffer stores for later
   ┌─────────────────────────────────────────┐
   │ Buffer: 1920 frames (40 ms @ 48kHz)    │
   │ Fill: ~45% (865 frames available)       │
   │ Status: ✓ Healthy                       │
   └─────────────────────────────────────────┘

4. Audio backend pulls 480 samples (10 ms)
   ┌─────────────────────────────────────────┐
   │ PullSamples(buffer, 480)                │
   │ Returns: 480 samples                    │
   │ Buffer after: ~40% (385 frames left)    │
   └─────────────────────────────────────────┘

═══════════════════════════════════════════════════════════════════════════

KEY BENEFITS:

✅ Rate Mismatch Handling
   Paula ~48011 Hz → Resampler → Exact 48000 Hz output
   No drift over time!

✅ Thread Safety
   Lock-free ring buffer allows Paula and Audio threads to run independently
   No mutex contention = no audio glitches

✅ Underrun/Overrun Protection
   40ms buffer provides cushion for timing variations
   Graceful degradation if buffer empties

✅ Debug Visibility
   All critical metrics exposed via g_audioDebugVars
   Easy to diagnose issues in real-time

✅ Clean Architecture
   3 independent layers with clear interfaces
   Easy to test, modify, and extend

═══════════════════════════════════════════════════════════════════════════

FILES IN THIS PACKAGE:

├─ audio_layer2.h          - Layer 2 main interface + debug vars
├─ audio_layer2.cpp        - Coordinator implementation
├─ audio_ringbuffer.h      - Lock-free ring buffer (header-only)
├─ audio_resampler.h       - Resampler interface
├─ audio_resampler.cpp     - Linear interpolation implementation
├─ PATCH_audio_cpp.txt     - Integration with audio.cpp
└─ PATCH_sound_cpp.txt     - Integration with sound.cpp

═══════════════════════════════════════════════════════════════════════════
```
