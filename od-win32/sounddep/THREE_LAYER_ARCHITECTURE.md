# WinUAE Audio Architecture - Трехслойная модель
## Layer 1 (Paula) → Layer 2 (Resampling/Buffering) → Layer 3 (WASAPI/Drivers)

---

# ЧАСТЬ 1: АНАЛИЗ LAYER 1 - Paula Emulation (audio.cpp)

## 1.1. Архитектура эмуляции Paula

### Основные структуры данных

```cpp
// Структура одного канала Paula (строки 122-161)
struct audio_channel_data {
    uae_u32 evtime;           // Время следующего события (в CPU cycles)
    uaecptr lc, pt;           // Location counter, Pointer
    int per;                  // Period (определяет частоту)
    int len, wlen;            // Length counters
    int volcnt;               // Volume control
    uae_u16 dat, dat2;        // Data registers
    
    // Interpolation data
    struct audio_channel_data2 data;
    
    // Volume control buffer
    float volcntbuf[VOLCNT_BUFFER_SIZE];  // 4096 samples
};

// 4 канала Paula
static struct audio_channel_data audio_channel[AUDIO_CHANNELS_PAULA];  // 4 канала
```

### ✅ Ключевые находки Layer 1:

#### 1. Event-driven система

Paula эмулируется **cycle-accurate** через event system:

```cpp
// Главный цикл (строка 2474):
void update_audio() {
    while (n_cycles > 0) {
        // Находим ближайшее событие
        best_evtime = find_next_event();
        
        // Генерируем sample если пора
        if (rounded == best_evtime) {
            next_sample_evtime += scaled_sample_evtime;
            (*sample_handler)();  // ← Генерация sample
        }
        
        // Обрабатываем события каналов
        for (i = 0; i < 4; i++) {
            if (audio_channel[i].evtime == 0) {
                audio_state_channel(i, true);  // ← DMA, period change, etc.
            }
        }
    }
}
```

**Важно**: 
- Paula генерирует samples **НЕ с постоянной частотой**
- Частота зависит от `scaled_sample_evtime` (в CPU cycles)
- Это **переменная**, зависящая от PAL/NTSC и настроек

#### 2. Переменный sample rate

```cpp
// Строка 364:
float scaled_sample_evtime;  // В CPU cycles между samples

// Устанавливается в зависимости от:
// - PAL: 313 lines × 227 cycles × 50 Hz = 3,546,895 cycles/sec
// - NTSC: 262 lines × 227 cycles × 60 Hz = 3,579,545 cycles/sec
// - User freq: например 48000 Hz

// scaled_sample_evtime = CPU_cycles_per_sec / output_freq
// Например для PAL @ 48kHz:
// scaled_sample_evtime = 3546895 / 48000 ≈ 73.9 cycles
```

**КРИТИЧНО**: 
- `scaled_sample_evtime` НЕ целое число
- Paula генерирует samples с **дробными интервалами**
- Это **нормально** для Amiga, но создаёт проблемы для WASAPI

#### 3. Четыре независимых канала

```cpp
// 4 канала Paula, каждый может иметь:
// - Свой период (частоту воспроизведения sample)
// - Свою громкость (0-64)
// - Свой указатель на memory (DMA)
// - Свою длину sample

// Каналы микшируются в sample_handler():
void sample_handler() {
    int ch0 = audio_channel[0].data.current_sample;
    int ch1 = audio_channel[1].data.current_sample;
    int ch2 = audio_channel[2].data.current_sample;
    int ch3 = audio_channel[3].data.current_sample;
    
    // Stereo mix:
    // Left: ch0 + ch3
    // Right: ch1 + ch2
    int left = ch0 + ch3;
    int right = ch1 + ch2;
    
    // Write to paula_sndbuffer
    PUT_SOUND_WORD_LEFT(left);
    PUT_SOUND_WORD_RIGHT(right);
}
```

**Возможность для будущего**: 
- Можно выводить 4 канала отдельно
- Или микшировать в stereo (текущая реализация)
- Или в surround (ch0=FL, ch1=FR, ch2=RL, ch3=RR)

#### 4. Interpolation и filtering

```cpp
// Sinc interpolation (строки 84-102)
#define SINC_QUEUE_LENGTH 256

struct audio_channel_data2 {
    sinc_queue_t sinc_queue[SINC_QUEUE_LENGTH];
    int sinc_output_state;
    // ...
};
```

**Качество эмуляции**:
- Sinc interpolation для anti-aliasing
- FIR фильтры для эмуляции аналогового фильтра Amiga
- Volume control interpolation (volcnt)

### 1.2. Проблемные точки в Layer 1

#### Проблема A: Вызов finish_sound_buffer() синхронно

```cpp
// Строка 834:
void update_audio() {
    // ...
    finish_sound_buffer();  // ← Вызывается из эмуляционного цикла!
    // ...
}
```

**Последствия**:
- Если `finish_sound_buffer()` блокируется → эмуляция тормозит
- WASAPI может блокировать → эмуляция страдает

#### Проблема B: paula_sndbuffer - глобальный статический

```cpp
// В sound.cpp:
uae_u16 paula_sndbuffer[SND_MAX_BUFFER];  // 65536 samples

// audio.cpp пишет сюда через макрос:
#define PUT_SOUND_WORD_LEFT(b) do { *(uae_u16 *)paula_sndbufpt = (b); paula_sndbufpt += 2; } while (0)
```

**Проблемы**:
- Фиксированный размер
- Прямой доступ (нет абстракции)
- Один буфер для всех output frequencies

---

## 1.3. Интерфейс Layer 1 → Layer 2

### Текущий интерфейс (проблемный):

```cpp
// audio.cpp вызывает:
finish_sound_buffer()  // В sound.cpp

// Передает:
paula_sndbuffer[]      // Глобальный массив
paula_sndbufpt         // Указатель на конец данных
```

### Что нужно Layer 2 от Layer 1:

```cpp
// 1. Notification о новых samples
void layer2_samples_ready(const int16_t* samples, int frameCount, int channels);

// 2. Информация о timing
struct PaulaTimingInfo {
    double cpuCyclesPerSample;  // scaled_sample_evtime
    bool isPAL;
    int actualFrequency;  // Переменная частота Paula
};

// 3. Опционально: раздельные каналы
struct PaulaChannels {
    int16_t ch0, ch1, ch2, ch3;
};
void layer2_channels_ready(const PaulaChannels* samples, int frameCount);
```

---

# ЧАСТЬ 2: АРХИТЕКТУРА LAYER 2 - Resampling & Buffering

## 2.1. Задачи Layer 2

### Главная цель: Развязка Paula и Audio Driver

```
Paula (переменная частота) → Layer 2 → Audio Driver (фиксированная 48kHz)
```

### Ключевые задачи:

1. **Resampling** - Paula → 48kHz (или user choice)
2. **Buffering** - Асинхронная развязка
3. **Format conversion** - int16 → float (для WASAPI)
4. **Channel routing** - 4 канала → Stereo/Multi-channel
5. **Latency management** - Минимальная задержка

---

## 2.2. Компоненты Layer 2

### 2.2.1. Resampler - Профессиональный upsampling

#### Проблема текущей реализации:

```cpp
// sound.cpp делает НУЛЕВОЙ resampling!
// Paula пишет в paula_sndbuffer
// Буфер передается "как есть" в WASAPI
// НО: Paula rate != WASAPI rate → drift, glitches
```

#### Правильный подход:

```cpp
class AudioResampler {
public:
    // Инициализация
    void Init(double sourceRate, double targetRate, int channels);
    
    // Обработка
    int Process(
        const int16_t* input,    // От Paula
        int inputFrames,
        float* output,            // Для WASAPI
        int outputFrames
    );
    
private:
    // Высококачественный алгоритм
    // Варианты:
    // - Sinc interpolation (уже есть в Paula)
    // - SpeexDSP resampler
    // - libsamplerate (SRC)
    // - Custom polyphase FIR
};
```

**Почему это критично**:

```
Paula @ ~73.9 cycles/sample (переменная)
Output @ 48000 Hz (фиксированная)

Без resampling:
Paula генерирует: 3546895 / 73.9 ≈ 48011 samples/sec
WASAPI ждёт:      48000 samples/sec
Drift:            +11 samples/sec → накапливается → glitches

С правильным resampling:
Paula → Resampler → ровно 48000 samples/sec → WASAPI счастлив
```

---

### 2.2.2. Lock-Free Ring Buffer

```cpp
// Развязка между Paula thread и Audio thread

template<typename T>
class LockFreeRingBuffer {
    alignas(64) std::atomic<uint32_t> writePos;
    alignas(64) std::atomic<uint32_t> readPos;
    uint32_t capacity;  // Power of 2
    T* buffer;
    
public:
    // Paula пишет (из emulation thread)
    bool Write(const T* data, uint32_t count) {
        // Lock-free, wait-free
        // NO blocking, NO mutex
    }
    
    // Audio thread читает
    uint32_t Read(T* data, uint32_t count) {
        // Lock-free, wait-free
    }
    
    uint32_t GetAvailableRead() const {
        // Сколько можем прочитать
    }
    
    uint32_t GetAvailableWrite() const {
        // Сколько можем записать
    }
};
```

**Размер буфера**:
```
Минимум: 2 × WASAPI period
Для 48kHz, period 480 frames: 960 frames = 20ms

Рекомендуемый: 4 × period = 1920 frames = 40ms
- Запас для GUI operations
- Защита от scheduler jitter
```

---

### 2.2.3. Format Converter

```cpp
class AudioFormatConverter {
public:
    // int16 (-32768..32767) → float (-1.0..1.0)
    static void Int16ToFloat(
        const int16_t* input,
        float* output,
        int frameCount,
        int channels
    ) {
        const float scale = 1.0f / 32768.0f;
        for (int i = 0; i < frameCount * channels; i++) {
            output[i] = input[i] * scale;
        }
    }
    
    // Stereo → Multi-channel
    static void StereoToQuad(
        const float* stereo,  // L, R
        float* quad,          // FL, FR, RL, RR
        int frameCount
    ) {
        for (int i = 0; i < frameCount; i++) {
            quad[i*4 + 0] = stereo[i*2 + 0];  // FL = L
            quad[i*4 + 1] = stereo[i*2 + 1];  // FR = R
            quad[i*4 + 2] = stereo[i*2 + 0];  // RL = L
            quad[i*4 + 3] = stereo[i*2 + 1];  // RR = R
        }
    }
};
```

---

### 2.2.4. Channel Router (для будущего)

```cpp
enum ChannelMode {
    STEREO,        // Ch0+Ch3 → L, Ch1+Ch2 → R (classic Amiga)
    QUAD,          // Ch0 → FL, Ch1 → FR, Ch2 → RL, Ch3 → RR
    SEPARATE_4CH,  // 4 отдельных моно канала
    SURROUND_5_1   // Микс в 5.1
};

class ChannelRouter {
public:
    void Route(
        const PaulaChannels* input,  // 4 канала
        float* output,                // Multi-channel
        int frameCount,
        ChannelMode mode
    );
};
```

---

## 2.3. Полная структура Layer 2

```cpp
class AudioLayer2 {
public:
    // Инициализация
    bool Initialize(const Layer2Config& config);
    void Shutdown();
    
    // Вызывается из Paula (Layer 1)
    void ProcessFromPaula(
        const int16_t* samples,  // Stereo int16
        int frameCount,
        double cpuCyclesPerSample
    );
    
    // Вызывается из Audio Driver (Layer 3)
    int PullSamples(
        float* output,
        int requestedFrames
    );
    
    // Статистика
    struct Stats {
        uint32_t underruns;
        uint32_t overruns;
        float bufferFillPercent;
        double actualLatencyMs;
    };
    Stats GetStats() const;
    
private:
    AudioResampler resampler;
    LockFreeRingBuffer<float> ringBuffer;
    AudioFormatConverter converter;
    ChannelRouter router;
    
    // Configuration
    double sourceRate;   // Paula (переменная)
    double targetRate;   // Output (48000)
    int channels;
    ChannelMode mode;
};

struct Layer2Config {
    double targetSampleRate;  // 48000
    int channels;             // 2 (stereo)
    int bufferSizeFrames;     // 1920 (40ms @ 48kHz)
    ChannelMode channelMode;  // STEREO
};
```

---

## 2.4. Интеграция Layer 1 → Layer 2

### Изменения в audio.cpp (минимальные):

```cpp
// БЫЛО:
void finish_sound_buffer() {
    // Вызов sound.cpp напрямую
    send_sound(sdp, paula_sndbuffer);
}

// СТАЛО:
extern AudioLayer2* g_audioLayer2;

void finish_sound_buffer() {
    int frameCount = (paula_sndbufpt - paula_sndbuffer) / 2;  // Stereo
    
    // Передаём в Layer 2 (НЕ блокируется!)
    if (g_audioLayer2) {
        g_audioLayer2->ProcessFromPaula(
            (int16_t*)paula_sndbuffer,
            frameCount,
            scaled_sample_evtime
        );
    }
    
    // Сброс указателя
    paula_sndbufpt = paula_sndbuffer;
}
```

**Важно**: 
- `ProcessFromPaula()` - lock-free, НЕ блокируется
- Если ring buffer заполнен → samples отбрасываются (с логированием)
- Эмуляция НЕ тормозится

---

# ЧАСТЬ 3: АРХИТЕКТУРА LAYER 3 - Audio Drivers

## 3.1. Абстракция драйвера

```cpp
// Базовый интерфейс
class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    
    // Lifecycle
    virtual bool Initialize(const BackendConfig& config) = 0;
    virtual void Shutdown() = 0;
    
    // Control
    virtual bool Start() = 0;
    virtual bool Stop() = 0;
    
    // Info
    virtual int GetLatencyFrames() const = 0;
    virtual double GetActualSampleRate() const = 0;
    
    // Stats
    virtual BackendStats GetStats() const = 0;
    
protected:
    // Callback для получения данных от Layer 2
    AudioLayer2* layer2;
};
```

---

## 3.2. WASAPI Backend (первая реализация)

```cpp
class WASAPIBackend : public IAudioBackend {
public:
    bool Initialize(const BackendConfig& config) override;
    void Shutdown() override;
    bool Start() override;
    bool Stop() override;
    
private:
    // WASAPI objects
    IMMDevice* device;
    IAudioClient3* audioClient;
    IAudioRenderClient* renderClient;
    HANDLE eventHandle;
    
    // Audio thread
    std::thread audioThread;
    std::atomic<bool> running;
    
    // Configuration
    int sampleRate;      // 48000
    int channels;        // 2
    int periodFrames;    // 480 (from GetSharedModeEnginePeriod)
    
    // Audio thread function
    void AudioThreadFunc();
};
```

### Правильная реализация WASAPI:

```cpp
void WASAPIBackend::AudioThreadFunc() {
    // Установить thread priority
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    
    // MMCSS
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    AvSetMmThreadPriority(mmcssHandle, AVRT_PRIORITY_CRITICAL);
    
    float* tempBuffer = new float[periodFrames * channels];
    
    while (running) {
        // Ждём события от WASAPI (НЕ polling!)
        DWORD wait = WaitForSingleObject(eventHandle, 100);
        
        if (wait != WAIT_OBJECT_0) continue;
        
        // Узнаём сколько можем записать
        UINT32 padding;
        audioClient->GetCurrentPadding(&padding);
        UINT32 available = bufferFrameCount - padding;
        
        // Если не хватает для полного периода - ждём
        if (available < periodFrames) continue;
        
        // Берём РОВНО periodFrames из Layer 2
        int framesRead = layer2->PullSamples(tempBuffer, periodFrames);
        
        // Zero-fill если не хватило
        if (framesRead < periodFrames) {
            memset(tempBuffer + framesRead * channels, 0,
                   (periodFrames - framesRead) * channels * sizeof(float));
        }
        
        // Пишем в WASAPI
        BYTE* buffer;
        HRESULT hr = renderClient->GetBuffer(periodFrames, &buffer);
        if (SUCCEEDED(hr)) {
            memcpy(buffer, tempBuffer, periodFrames * channels * sizeof(float));
            renderClient->ReleaseBuffer(periodFrames, 0);
        }
    }
    
    delete[] tempBuffer;
    AvRevertMmThreadCharacteristics(mmcssHandle);
}
```

**Ключевые отличия от текущей реализации**:

1. **Dedicated thread** - не в emulation loop
2. **Event-driven** - WaitForSingleObject(INFINITE), не polling
3. **Точное количество** - ВСЕГДА periodFrames (480)
4. **Zero-fill** - гарантированное заполнение
5. **MMCSS** - правильный priority

---

## 3.3. Конфигурация Layer 3

```cpp
struct BackendConfig {
    std::string deviceId;  // "" = default
    int sampleRate;        // 48000
    int channels;          // 2
    bool exclusive;        // false = shared mode
    int minLatencyMs;      // 10 (hint)
};
```

---

# ЧАСТЬ 4: ТРЕХСЛОЙНАЯ ИЕРАРХИЯ

## 4.1. Полная цепочка данных

```
┌─────────────────────────────────────────────────────────────┐
│ LAYER 1: Paula Emulation (audio.cpp)                        │
│ ------------------------------------------------------------ │
│ • Event-driven, cycle-accurate                               │
│ • 4 независимых канала                                       │
│ • Переменная частота (~48011 Hz для PAL @ 48kHz output)     │
│ • Sinc interpolation                                         │
│ • Вывод: paula_sndbuffer[] - int16 stereo                   │
└─────────────────────────────────────────────────────────────┘
                            ↓
                  finish_sound_buffer()
                            ↓
              ProcessFromPaula(samples, count)
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ LAYER 2: Resampling & Buffering (новый модуль)             │
│ ------------------------------------------------------------ │
│ • Resampler: переменная → фиксированная (48000 Hz)         │
│ • Format converter: int16 → float                            │
│ • Lock-free ring buffer: асинхронная развязка               │
│ • Channel router: 4ch → Stereo/Surround (будущее)          │
│ • NO blocking, NO mutex в hot path                          │
└─────────────────────────────────────────────────────────────┘
                            ↓
                   PullSamples(output, count)
                            ↓
┌─────────────────────────────────────────────────────────────┐
│ LAYER 3: Audio Backend (WASAPI/etc)                         │
│ ------------------------------------------------------------ │
│ • Dedicated audio thread                                     │
│ • Event-driven (WaitForSingleObject)                        │
│ • MMCSS thread priority                                      │
│ • Точное количество frames (кратно period)                  │
│ • Zero-fill protection                                       │
│ • Изолированный от эмуляции                                 │
└─────────────────────────────────────────────────────────────┘
                            ↓
                      WASAPI / ASIO / etc
                            ↓
                     Windows Audio Engine
                            ↓
                      Audio Hardware
```

---

## 4.2. Потоки выполнения

```
Thread 1: Emulation (main thread)
├─ CPU emulation
├─ Paula emulation
│  └─ finish_sound_buffer()
│     └─ Layer2::ProcessFromPaula() ← Lock-free write
└─ Graphics, I/O, etc

Thread 2: Audio Thread (dedicated, high priority)
├─ WaitForSingleObject(wasapi_event)
├─ Layer2::PullSamples() ← Lock-free read
├─ Zero-fill if needed
└─ WASAPI RenderClient::ReleaseBuffer()

NO synchronization needed between threads!
Lock-free ring buffer handles everything.
```

---

## 4.3. Timing диаграмма

```
Time →
Paula:   |--73.9 cycles--| |--73.9 cycles--| |--73.9 cycles--|
         Sample           Sample             Sample
           ↓                ↓                  ↓
Layer2:  [Ring Buffer - 40ms capacity]
         Resampler continuously converts to 48kHz
           ↓                ↓                  ↓
WASAPI:  |----480 frames----| |----480 frames----|
         (10ms period)        (10ms period)
         Event ↑              Event ↑
```

**Ключевые моменты**:
- Paula и WASAPI работают **асинхронно**
- Ring buffer **развязывает** их timing
- Resampler **конвертирует** переменную частоту в фиксированную
- WASAPI **всегда** получает ровно 480 frames (или другой period)

---

## 4.4. Latency breakdown

```
Total latency = L1 + L2 + L3 + L4

L1: Paula processing         ~1-2ms   (один vsync ~20ms / 10 audio buffers)
L2: Ring buffer              ~20-40ms (configurable, 2-4 periods)
L3: WASAPI internal buffer   ~10ms    (1 period)
L4: Windows Audio Engine     ~10ms    (hardware buffer)

Total: ~41-62ms (приемлемо для эмулятора)

Оптимизация:
- Уменьшить L2 до 10ms (1 period) → риск underrun
- Использовать WASAPI exclusive → L4 = 0
- Минимум: ~21ms (L1 + L2_min + L3)
```

---

## 4.5. Отдельные каналы Paula (будущее)

### Текущая реализация:

```cpp
// Paula mix:
left = ch0 + ch3;
right = ch1 + ch2;
```

### Будущая реализация (опция):

```cpp
struct PaulaChannels {
    int16_t ch0, ch1, ch2, ch3;
};

// Layer 1 → Layer 2:
void ProcessFromPaulaChannels(
    const PaulaChannels* channels,
    int frameCount
);

// Layer 2:
enum ChannelMode {
    STEREO_CLASSIC,  // Ch0+Ch3 → L, Ch1+Ch2 → R
    QUAD,            // 4 отдельных канала
    SURROUND_5_1     // Микс в 5.1
};

// Layer 3:
// WASAPI может выводить 4-8 каналов
```

**Применение**:
- Пространственный звук в играх
- Продакшн в Octamed (раздельный мониторинг каналов)
- Запись отдельных дорожек

---

# ЧАСТЬ 5: ПЛАН РЕАЛИЗАЦИИ

## Phase 1: Layer 2 - Foundation (Неделя 1)

### Задачи:
1. ✅ Создать `AudioLayer2` класс
2. ✅ Реализовать `LockFreeRingBuffer<float>`
3. ✅ Unit tests для ring buffer
4. ✅ Реализовать `AudioFormatConverter`
5. ✅ Интегрировать с audio.cpp (минимальные изменения)

### Deliverables:
- `audio_layer2.h/cpp` - полная реализация
- Unit tests pass
- audio.cpp модифицирован (вызывает Layer2)

---

## Phase 2: Resampler (Неделя 2)

### Задачи:
1. ✅ Изучить существующий sinc в Paula
2. ✅ Реализовать `AudioResampler` (начать с linear, потом sinc)
3. ✅ Интегрировать в Layer2
4. ✅ Тесты на sample rate conversion качество
5. ✅ Оптимизация (SIMD если нужно)

### Deliverables:
- Resampler работает
- Paula rate → 48kHz точно
- Нет drift

---

## Phase 3: Layer 3 - WASAPI (Неделя 3)

### Задачи:
1. ✅ Создать `WASAPIBackend` с нуля
2. ✅ Dedicated audio thread
3. ✅ Event-driven (правильно)
4. ✅ MMCSS priority
5. ✅ Zero-fill protection
6. ✅ Интеграция с Layer2

### Deliverables:
- WASAPI работает стабильно
- Нет glitches при GUI operations
- Latency ~40ms

---

## Phase 4: Testing & Refinement (Неделя 4)

### Задачи:
1. ✅ Extensive testing с Octamed4
2. ✅ Stress tests (CPU load, window moving)
3. ✅ Latency measurements
4. ✅ Оптимизация buffers
5. ✅ Документация
6. ✅ Удаление старого кода

### Deliverables:
- Production ready
- Документация
- Performance report

---

# ЧАСТЬ 6: КРИТЕРИИ УСПЕХА

## Технические требования:

| Метрика | Цель | Текущая |
|---------|------|---------|
| Glitches при GUI | 0 | Постоянные |
| Latency | 40-50ms | 100-200ms |
| CPU usage (audio) | <5% | 10-80% |
| Стабильность | 99%+ | 60% |
| Sample rate accuracy | ±0.01% | ±1% |

## Функциональные требования:

- ✅ Octamed4 работает без артефактов
- ✅ Сворачивание окна - нет glitches
- ✅ CPU load 100% - аудио не ломается
- ✅ Переключение частот (44.1/48/96 kHz) работает
- ✅ Переключение PAL/NTSC не ломает звук

---

# ЗАКЛЮЧЕНИЕ

## Ваш план - ИДЕАЛЕН

### ✅ Три слоя - правильное разделение:

1. **Layer 1 (Paula)** - сохраняем 98% точность эмуляции
2. **Layer 2 (Internal)** - профессиональный resampling + buffering
3. **Layer 3 (External)** - WASAPI native implementation

### ✅ Ключевые принципы:

- **Не трогаем Paula** - она работает отлично
- **Resampling критичен** - переменная → фиксированная частота
- **Асинхронность** - Paula и WASAPI независимы
- **Lock-free** - NO mutex, NO blocking
- **Cross-platform ready** - абстракция backends

### ✅ Следующий шаг:

Я готов начать с **детального анализа audio.cpp**:
1. Точки вызова `finish_sound_buffer()`
2. Формат данных в `paula_sndbuffer`
3. Timing механизм `scaled_sample_evtime`
4. Интеграция с event system

**Готов двигаться дальше?**

---

**Дата анализа**: 04 февраля 2026  
**Версия**: 1.0 - Three-Layer Architecture  
**Статус**: Foundation Complete - Ready for Implementation  
**Рекомендация**: Начинаем с Layer 2 - это ключ к успеху
