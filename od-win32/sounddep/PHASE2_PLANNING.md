# PHASE 2 PLANNING - WASAPI Backend Integration
## WinUAE Audio Layer 3 Implementation

---

## ✅ PHASE 1 STATUS - COMPLETE!

Layer 2 успешно работает:
```
[AUDIO:Layer2] Initialized: 48000 Hz, 2 ch, 1920 frames buffer (40.0 ms)
AUDIO: Layer2 initialized successfully
```

### 🔴 Текущая проблема:
```
[AUDIO:Layer2] Overrun! Dropped 1048065 frames (fill: 0.0%)
```

**Причина:** Layer 3 (WASAPI backend) НЕ читает из ring buffer!
- Paula (Layer 1) → Layer 2 → Ring Buffer ✅
- Ring Buffer → Layer 3 (WASAPI) ❌ ОТСУТСТВУЕТ

---

## 📊 АНАЛИЗ ТЕКУЩЕЙ WASAPI РЕАЛИЗАЦИИ

### Структура sound_dp (строка 51-136)

```cpp
struct sound_dp {
    // WASAPI fields (строки 93-117)
    IMMDevice *pDevice;                  // Аудио устройство
    IAudioClient3 *pAudioClient;         // Audio client (v1 или v3)
    int AudioClientVersion;              // 1 или 3
    IAudioRenderClient *pRenderClient;   // Для записи в WASAPI
    ISimpleAudioVolume *pAudioVolume;    // Громкость
    
    UINT32 bufferFrameCount;             // Размер WASAPI буфера (frames)
    int wasapiexclusive;                 // Exclusive/Shared режим
    int pullmode;                        // 1 = pull, 0 = push
    
    HANDLE pullevent;                    // Event для pull callback
    HANDLE pullevent2;                   // Event для синхронизации
    uae_u8 *pullbuffer;                  // Временный буфер для pull
    int pullbufferlen;                   // Текущая длина данных
    int pullbuffermaxlen;                // Максимальная длина
    bool gotpullevent;                   // Флаг события
};
```

---

## 🔍 WASAPI INITIALIZATION FLOW

### 1. Device Selection (строка 1350-1356)
```cpp
if (index < 0)
    hr = s->pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &s->pDevice);
else
    hr = s->pEnumerator->GetDevice(sound_devices[index]->alname, &s->pDevice);
```

**Источник:** `sound_devices[]` массив (заполняется из GUI)

### 2. Audio Client Creation (строка 1364-1373)
```cpp
s->AudioClientVersion = 3;
hr = s->pDevice->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, (void**)&s->pAudioClient);
if (FAILED(hr)) {
    s->AudioClientVersion = 1;
    hr = s->pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&s->pAudioClient);
}
```

**Поддержка:** IAudioClient3 (Win10+) с fallback на IAudioClient1

### 3. Format Negotiation (строка 1386-1446)
```cpp
hr = s->pAudioClient->GetMixFormat(&pwfx);
hr = s->pAudioClient->GetDevicePeriod(&hnsDefaultDevicePeriod, &phnsMinimumDevicePeriod);

// Try different channel configurations
if (sd->channels == 2) {
    rn[0] = KSAUDIO_SPEAKER_STEREO;
}

hr = s->pAudioClient->IsFormatSupported(sharemode, &wavfmt.Format, &pwfx);
```

**Параметры из GUI:**
- `sd->freq` - sample rate (44100, 48000, 96000, etc)
- `sd->channels` - 1, 2, 4, 6, 8
- `sd->bits` - 16 или 32
- `sharemode` - Exclusive или Shared

### 4. Buffer Size Configuration (строка 1543-1642)

```cpp
// For IAudioClient3 low-latency mode
if (s->AudioClientVersion >= 3 && sharemode == AUDCLNT_SHAREMODE_SHARED) {
    int p10ms = sd->freq / 100;
    if (s->bufferFrameCount <= p10ms) {
        UINT32 period = s->bufferFrameCount;
        hr = s->pAudioClient->InitializeSharedAudioStream(
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 
            period, 
            pwfx, 
            &audioguid
        );
    }
}

// Fallback standard initialization
hr = s->pAudioClient->Initialize(
    sharemode, 
    AUDCLNT_STREAMFLAGS_NOPERSIST | (s->pullmode ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0),
    hnsRequestedDuration, 
    exclusive ? hnsRequestedDuration : 0, 
    pwfx, 
    &audioguid
);
```

**Важно:** `s->bufferFrameCount` определяет размер WASAPI буфера!

### 5. Pull Mode Setup (строка 1659-1660)
```cpp
if (s->pullmode) {
    hr = s->pAudioClient->SetEventHandle(s->pullevent);
}
```

**Механизм:** WASAPI вызывает event когда нужны данные

---

## 🎯 WASAPI PULL MODE MECHANISM

### Event Flow:
```
WASAPI Hardware Buffer Empty
    ↓
SetEvent(s->pullevent)
    ↓
audio_is_pull_event() → true (строка 2832)
    ↓
audio_finish_pull() → send_sound_do() (строка 2853)
    ↓
finish_sound_buffer_wasapi_pull_do() (строка 2441)
    ↓
GetBuffer() → memcpy() → ReleaseBuffer()
```

### Ключевая функция: finish_sound_buffer_wasapi_pull_do()

```cpp
static bool finish_sound_buffer_wasapi_pull_do(struct sound_data *sd)
{
    struct sound_dp *s = sd->data;
    BYTE *pData;
    
    // 1. Check how many frames available in WASAPI
    UINT32 numFramesPadding;
    hr = s->pAudioClient->GetCurrentPadding(&numFramesPadding);
    int avail = s->bufferFrameCount - numFramesPadding;
    
    // 2. Get WASAPI buffer
    hr = s->pRenderClient->GetBuffer(avail, &pData);
    
    // 3. Copy from pullbuffer to WASAPI
    memcpy(pData, s->pullbuffer, avail * sd->samplesize);
    
    // 4. Release buffer
    hr = s->pRenderClient->ReleaseBuffer(avail, 0);
    
    return true;
}
```

**Текущая схема:**
```
Paula → extrasndbuf → pullbuffer → WASAPI
```

---

## 🚀 PHASE 2 ARCHITECTURE - NEW FLOW

### Цель: Заменить pullbuffer на Layer 2 ring buffer

```
┌─────────────────────────────────────────────────────────────┐
│  Paula (Layer 1)                                            │
│  ├─ Generates samples @ variable rate                       │
│  └─ finish_sound_buffer() (sound.cpp:2903)                  │
│                                                              │
│              ↓ ProcessFromPaula()                           │
│                                                              │
│  Layer 2 (Resampling & Buffering)                          │
│  ├─ audio_layer2.cpp                                        │
│  ├─ Resample to 48000 Hz                                   │
│  └─ Write to ring buffer                                    │
│                                                              │
│              ↓ PullSamples()                                │
│                                                              │
│  Layer 3 (WASAPI Backend) ← NEW!                           │
│  ├─ audio_wasapi_layer3.cpp                                │
│  ├─ Wait for WASAPI event                                  │
│  ├─ PullSamples() from Layer 2                             │
│  └─ GetBuffer() → memcpy() → ReleaseBuffer()               │
│                                                              │
│              ↓ Hardware                                     │
└─────────────────────────────────────────────────────────────┘
```

---

## 📝 PHASE 2 IMPLEMENTATION PLAN

### Файлы для создания:

#### 1. audio_wasapi_layer3.h
```cpp
class AudioWASAPILayer3 {
public:
    // Initialization
    bool Initialize(
        IMMDevice* device,
        IAudioClient3* audioClient,
        IAudioRenderClient* renderClient,
        int sampleRate,
        int channels,
        UINT32 bufferFrameCount
    );
    
    // Pull callback (вызывается когда WASAPI готов)
    bool OnPullEvent(AudioLayer2* layer2);
    
    // Query
    UINT32 GetAvailableFrames();
    
private:
    IAudioClient3* pAudioClient;
    IAudioRenderClient* pRenderClient;
    int sampleRate;
    int channels;
    UINT32 bufferFrameCount;
    
    float* tempBuffer;  // Temporary buffer для pull
};
```

#### 2. audio_wasapi_layer3.cpp
```cpp
bool AudioWASAPILayer3::OnPullEvent(AudioLayer2* layer2)
{
    // 1. Query WASAPI available space
    UINT32 numFramesPadding;
    pAudioClient->GetCurrentPadding(&numFramesPadding);
    UINT32 availFrames = bufferFrameCount - numFramesPadding;
    
    // 2. Pull from Layer 2
    int pulled = layer2->PullSamples(tempBuffer, availFrames);
    
    // 3. Get WASAPI buffer
    BYTE* pData;
    pRenderClient->GetBuffer(pulled, &pData);
    
    // 4. Copy float → WASAPI format (int16 or float32)
    // TODO: Format conversion if needed
    memcpy(pData, tempBuffer, pulled * channels * sizeof(float));
    
    // 5. Release
    pRenderClient->ReleaseBuffer(pulled, 0);
    
    return true;
}
```

### Интеграция в sound.cpp:

#### Изменения в finish_sound_buffer_wasapi_pull_do():
```cpp
// БЫЛО:
memcpy(pData, s->pullbuffer, avail * sd->samplesize);

// СТАЛО:
#ifdef USE_AUDIO_LAYER2
if (g_audioLayer2 && g_audioWASAPILayer3) {
    g_audioWASAPILayer3->OnPullEvent(g_audioLayer2);
    return true;
}
#endif
// Fallback to old code
```

---

## 🔧 GUI PARAMETERS - КАК ПОЛУЧИТЬ

### Источник параметров:

#### 1. Sample Rate (sd->freq)
```cpp
// sound.cpp, строка ~1200
sd->freq = currprefs.sound_freq;
```

**Откуда:** `currprefs.sound_freq` из конфигурации
**Возможные значения:** 11025, 22050, 44100, 48000, 96000

#### 2. Channels (sd->channels)
```cpp
sd->channels = get_audio_nativechannels(currprefs.sound_stereo);
```

**Функция get_audio_nativechannels():**
- 0 = Mono (1 channel)
- 1 = Stereo (2 channels)
- 2 = Stereo swap (2 channels)
- 3 = 4ch (4 channels)
- 4 = 5.1 (6 channels)
- 5 = 7.1 (8 channels)

#### 3. Bits (sd->bits)
```cpp
sd->bits = currprefs.sound_interpol == 0 ? 16 : 32;
```

#### 4. Device Selection
```cpp
int index = sound_devices_index(currprefs.win32_soundcard);
hr = s->pEnumerator->GetDevice(sound_devices[index]->alname, &s->pDevice);
```

**Откуда:** `currprefs.win32_soundcard` - GUID устройства из GUI

#### 5. Buffer Size
```cpp
// Из GUI настроек:
int sndbufsize = currprefs.sound_maxbsiz;  // Frames

// WASAPI период вычисляется:
s->bufferFrameCount = sndbufsize;
```

**Типичные значения:**
- 512 frames @ 48kHz = 10.7 ms
- 1024 frames @ 48kHz = 21.3 ms
- 2048 frames @ 48kHz = 42.7 ms

---

## ⚠️ ВЛИЯНИЕ СТАРОГО КОДА НА LAYER 2

### Проблема #1: Двойная обработка

```cpp
void finish_sound_buffer (void)
{
    // ...
    
    #ifdef USE_AUDIO_LAYER2
    if (g_audioLayer2) {
        g_audioLayer2->ProcessFromPaula(...);
        return;  // ← ВАЖНО! Пропускаем старый код
    }
    #endif
    
    // Старый код (НЕ выполняется если Layer 2 активен)
    if (extrasndbuf) { ... }
    // ...
}
```

**Статус:** ✅ Корректно! `return` предотвращает двойную обработку.

### Проблема #2: send_sound() не вызывается

```cpp
// Старый путь (ОТКЛЮЧЕН):
finish_sound_buffer() → send_sound() → finish_sound_buffer_wasapi()

// Новый путь (НУЖЕН):
finish_sound_buffer() → Layer 2 ✓
Layer 2 → Layer 3 ❌ ОТСУТСТВУЕТ!
Layer 3 → WASAPI
```

**Решение:** Создать Layer 3 который вызывается из WASAPI pull event.

---

## 📋 PHASE 2 IMPLEMENTATION CHECKLIST

### Step 1: Create Layer 3 files
- [ ] audio_wasapi_layer3.h
- [ ] audio_wasapi_layer3.cpp
- [ ] Add to Visual Studio project

### Step 2: Modify sound.cpp initialization
- [ ] Create g_audioWASAPILayer3 global
- [ ] Initialize Layer 3 after WASAPI init
- [ ] Pass WASAPI interfaces to Layer 3

### Step 3: Hook into pull mechanism
- [ ] Modify finish_sound_buffer_wasapi_pull_do()
- [ ] Call g_audioWASAPILayer3->OnPullEvent()
- [ ] Test with WASAPI shared mode

### Step 4: Testing
- [ ] Verify no overruns in Layer 2
- [ ] Verify WASAPI receives data
- [ ] Check latency (ring buffer fill %)
- [ ] Test with Octamed4

---

## 🎯 NEXT STEPS

1. **Создать diff для audio_wasapi_layer3.h/cpp**
2. **Создать diff для sound.cpp интеграции**
3. **Тестирование с реальным WASAPI**

---

**ГОТОВЫ К PHASE 2?** 🚀
