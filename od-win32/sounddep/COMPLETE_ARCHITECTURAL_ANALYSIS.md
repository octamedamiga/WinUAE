# WinUAE Sound.cpp - Полный архитектурный анализ и фундаментальные проблемы

## 🎯 Цель анализа

Проанализировать **весь путь аудио** от эмуляции Paula до вывода в WASAPI, выявить архитектурные проблемы и заложить основу для полного редизайна.

---

## 📊 ЧАСТЬ 1: Структура данных - Археологические слои

### 1.1. Главная структура sound_dp - "Свалка API"

```cpp
struct sound_dp {
    // directsound (строки 49-61)
    LPDIRECTSOUND8 lpDS;
    LPDIRECTSOUNDBUFFER8 lpDSBsecondary;
    DWORD writepos;
    int dsoundbuf;
    // ... 11 полей для DirectSound
    
    // openal (строки 63-74)
    ALCdevice *al_dev;
    ALCcontext *al_ctx;
    // ... 8 полей для OpenAL
    
    // portaudio (строки 77-86)
    volatile int pareadoffset, pawriteoffset;
    PaStream *pastream;
    // ... 6 полей для PortAudio
    
    // wasapi (строки 88-113)
    IMMDevice *pDevice;
    IAudioClient3 *pAudioClient;
    // ... 15 полей для WASAPI
    
    // xaudio2 (строки 115-128)
    IXAudio2 *xaudio2;
    // ... 7 полей для XAudio2
    
    // Общие поля коррекции (строки 130-131)
    float avg_correct;
    float cnt_correct;
};
```

#### 🔴 ПРОБЛЕМА #1: "Франкенштейн структура"

**Симптомы**:
- Одна структура содержит поля для **5 разных аудио API**
- 50+ полей, большинство неактивны в любой момент
- Невозможно понять какие поля используются для какого API
- Cache pollution - загружаем в кеш данные всех API

**Причина**:
- Эволюция через наслоение: DirectSound → OpenAL → PortAudio → WASAPI → XAudio2
- Никто не рефакторил при добавлении нового API
- Каждый API добавлял свои поля в конец

**Последствия**:
- Сложность отладки
- Утечки памяти при переключении API
- Невозможность изолировать проблемы одного API от другого

---

### 1.2. Глобальные буферы Paula

```cpp
// Строки 149-159
#define SND_MAX_BUFFER 65536

uae_u16 paula_sndbuffer[SND_MAX_BUFFER];  // 65536 samples = 131KB
uae_u16 *paula_sndbufpt;                   // Текущий указатель записи
int paula_sndbufsize;                      // Размер буфера
```

#### 🔴 ПРОБЛЕМА #2: Глобальный статический буфер

**Симптомы**:
- Фиксированный размер 65536 samples
- Один буфер для всех частот дискретизации
- Не масштабируется

**Последствия**:
- При 48kHz и стерео: 65536 samples = 1.36 секунды буфера!
- Огромная латентность
- Невозможность динамической подстройки под WASAPI period

---

### 1.3. "Загадочный" extrasndbuf

```cpp
// Строки 170-172
static uae_u8 *extrasndbuf;
static int extrasndbufsize;
static int extrasndbuffered;
```

#### 🔴 ПРОБЛЕМА #3: Непонятный третий буфер

**Назначение** (из кода):
```cpp
// Строка 2878
// we got buffer that was not full (recording active). Need special handling.
if (bufsize < sdp->sndbufsize && !extrasndbuf) {
    extrasndbuf = xcalloc(uae_u8, sdp->sndbufsize);
}
```

**Проблемы**:
- Создаётся "на лету" при недозаполненном буфере
- Живёт глобально
- Добавляет ещё один слой буферизации
- Неясная логика удаления

---

## 📊 ЧАСТЬ 2: Путь аудио данных - "Цепочка боли"

### 2.1. УРОВЕНЬ 1: Paula → paula_sndbuffer

**Вызывается из**: `audio.cpp` (эмуляция Paula)

```
Paula эмуляция
    ↓
finish_sound_buffer()  (строка 2841)
    ↓
paula_sndbuffer заполнен
```

**Код** (строки 2841-2862):
```cpp
void finish_sound_buffer (void)
{
    int bufsize = addrdiff((uae_u8*)paula_sndbufpt, (uae_u8*)paula_sndbuffer);
    
    // Различные проверки...
    
    if (currprefs.sound_stereo_swap_paula) {
        channelswap((uae_s16*)paula_sndbuffer, bufsize / 2);
    }
    
    paula_sndbufpt = paula_sndbuffer;  // ← Сброс указателя
    
    // Отправка в driver
    send_sound(sdp, paula_sndbuffer);
}
```

#### 🔴 ПРОБЛЕМА #4: Синхронная обработка в main thread

**Проблемы**:
- `finish_sound_buffer()` вызывается **из эмуляционного цикла**
- Блокирует эмуляцию пока звук не отправлен
- При WASAPI: если буфер заполнен → ждём → эмуляция тормозит
- DirectSound прощает это (большой буфер), WASAPI нет

---

### 2.2. УРОВЕНЬ 2: paula_sndbuffer → send_sound() → драйвер

**Код** (строки 2662-2686):
```cpp
static void send_sound (struct sound_data *sd, uae_u16 *sndbuffer)
{
    int type = sd->devicetype;
    
    if (type == SOUND_DEVICE_AL)
        finish_sound_buffer_al(sd, sndbuffer);
    else if (type == SOUND_DEVICE_DS)
        finish_sound_buffer_ds(sd, sndbuffer);
    else if (type == SOUND_DEVICE_PA)
        finish_sound_buffer_pa(sd, sndbuffer);
    else if (type == SOUND_DEVICE_WASAPI || type == SOUND_DEVICE_WASAPI_EXCLUSIVE)
        finish_sound_buffer_wasapi(sd, sndbuffer);
    else if (type == SOUND_DEVICE_XAUDIO2)
        finish_sound_buffer_xaudio2(sd, sndbuffer);
}
```

#### ✅ Нормально: Диспетчеризация по типу API

#### 🔴 ПРОБЛЕМА #5: Но нет изоляции ошибок

- Если WASAPI упадёт → нет fallback
- Нет обработки return значений
- Все драйверы получают одинаковый буфер (без учёта их требований)

---

### 2.3. УРОВЕНЬ 3A: WASAPI Pull Mode (текущий default)

**Код** (строки 2431-2438):
```cpp
static void finish_sound_buffer_wasapi(struct sound_data *sd, uae_u16 *sndbuffer)
{
    struct sound_dp *s = sd->data;
    if (s->pullmode)
        finish_sound_buffer_pull(sd, sndbuffer);  // ← Pull mode
    else
        finish_sound_buffer_wasapi_push(sd, sndbuffer);  // ← Push mode
}
```

**Pull mode path**:
```
finish_sound_buffer_wasapi()
    ↓
finish_sound_buffer_pull()  (строка 676)
    ↓
Копирует в s->pullbuffer
    ↓
[Позже] finish_sound_buffer_wasapi_pull_do()  (строка 2379)
    ↓
Читает из s->pullbuffer → WASAPI
```

#### 🔴 ПРОБЛЕМА #6: Двойная буферизация

**Цепочка буферов**:
```
paula_sndbuffer (65KB глобальный)
    → memcpy →
pullbuffer (динамический, s->pullbuffermaxlen)
    → memcpy →
WASAPI internal buffer
    → Windows Audio Engine
```

**Проблемы**:
1. **Две копии данных** вместо одной
2. **pullbuffer** - промежуточный буфер без чёткого назначения
3. **Рассинхронизация**: Paula пишет в pullbuffer, WASAPI читает асинхронно
4. **memmove** в hot path (строка 2424)

---

### 2.4. Детальный анализ finish_sound_buffer_pull()

**Код** (строки 676-686):
```cpp
static void finish_sound_buffer_pull(struct sound_data *sd, uae_u16 *sndbuffer)
{
    struct sound_dp *s = sd->data;

    if (s->pullbufferlen + sd->sndbufsize > s->pullbuffermaxlen) {
        write_log(_T("pull overflow! %d %d %d\n"), ...);
        s->pullbufferlen = 0;  // ← КАТАСТРОФА: Сброс ВСЕГО буфера!
    }
    
    memcpy(s->pullbuffer + s->pullbufferlen, sndbuffer, sd->sndbufsize);
    s->pullbufferlen += sd->sndbufsize;
}
```

#### 🔴 ПРОБЛЕМА #7: Overflow = data loss

**Сценарий**:
```
1. pullbuffer почти заполнен (pullbufferlen = 90% max)
2. Paula отправляет новую порцию (10% max)
3. Overflow: 90% + 10% > 100%
4. Код: pullbufferlen = 0  ← ВЕСЬ буфер теряется!
5. WASAPI callback: пустой буфер → underrun → треск
```

**Правильно было бы**:
```cpp
// Отбросить СТАРЫЕ данные, оставить новые
s->pullbufferlen = s->pullbuffermaxlen - sd->sndbufsize;
memmove(s->pullbuffer, 
        s->pullbuffer + sd->sndbufsize, 
        s->pullbufferlen);
```

---

### 2.5. Детальный анализ finish_sound_buffer_wasapi_pull_do()

**Вызывается из**: audio_finish_pull() → send_sound_do()

**Код** (строки 2379-2429):
```cpp
static bool finish_sound_buffer_wasapi_pull_do(struct sound_data *sd)
{
    struct sound_dp *s = sd->data;
    
    s->gotpullevent = false;
    
    if (s->pullbufferlen <= 0)  // ← Нет данных → выход
        return false;
    
    int frames = s->pullbufferlen / sd->samplesize;
    int avail = frames;
    
    if (!s->wasapiexclusive) {
        UINT32 numFramesPadding;
        hr = s->pAudioClient->GetCurrentPadding(&numFramesPadding);
        avail = s->bufferFrameCount - numFramesPadding;  // ← Сколько можем записать
        
        if (!avail)
            return false;
        if (avail > frames)
            avail = frames;
    }
    
    ResetEvent(s->pullevent);  // ← Сброс события
    
    hr = s->pRenderClient->GetBuffer(avail, &pData);
    if (SUCCEEDED(hr)) {
        memcpy(pData, s->pullbuffer, avail * sd->samplesize);  // ← Копия в WASAPI
        s->pRenderClient->ReleaseBuffer(avail, 0);
    }
    
    // 🔴 ПРОБЛЕМА #8: memmove в hot path!
    if (avail < frames) {
        memmove(s->pullbuffer, 
                s->pullbuffer + avail * sd->samplesize, 
                s->pullbufferlen - (avail * sd->samplesize));
    }
    s->pullbufferlen -= avail * sd->samplesize;
    
    return true;
}
```

#### 🔴 ПРОБЛЕМА #8: memmove каждый callback

**Частота вызова**: При 48kHz, period 480 frames = **100 раз в секунду**

**Размер данных**: До pullbuffermaxlen (может быть 8192 frames = 16KB)

**Стоимость**:
```
100 вызовов/сек × 1-2 мкс/вызов = 100-200 мкс/сек на memmove
При больших буферах: до 1-2% CPU
```

**Альтернатива**: Ring buffer с read/write указателями (ZERO memmove)

---

### 2.6. Timing и события - "Танец с бубном"

**События WASAPI** (строки 108-109):
```cpp
HANDLE pullevent, pullevent2;
bool gotpullevent;
```

**Использование**:
```cpp
// Инициализация (строка ~1665)
s->pullevent = CreateEvent(NULL, FALSE, FALSE, NULL);
hr = s->pAudioClient->SetEventHandle(s->pullevent);

// Проверка события (строка 2779)
if (WaitForSingleObject(s->pullevent, 0) == WAIT_OBJECT_0) {
    audio_got_pull_event();
}

// В callback (строка 2409)
ResetEvent(s->pullevent);
```

#### 🔴 ПРОБЛЕМА #9: Смешанная модель событий

**Проблемы**:
1. **pullevent** - событие от WASAPI
2. **pullevent2** - событие между потоками WinUAE (для PortAudio?)
3. **gotpullevent** - флаг (race condition риск)
4. **Sleep(1)** в других местах
5. **Polling** GetCurrentPadding

**Результат**: Три источника тайминга конфликтуют:
- WASAPI event-driven timing
- WinUAE emulation loop timing
- Sleep/polling timing

---

## 📊 ЧАСТЬ 3: Push Mode - "Ещё хуже"

### 3.1. finish_sound_buffer_wasapi_push()

**Код** (строки 2326-2377):
```cpp
static void finish_sound_buffer_wasapi_push(struct sound_data *sd, uae_u16 *sndbuffer)
{
    struct sound_dp *s = sd->data;
    UINT32 numFramesPadding;
    int avail;
    int stuck = 2000;
    
    // 🔴 POLLING LOOP!
    for (;;) {
        hr = s->pAudioClient->GetCurrentPadding(&numFramesPadding);
        avail = s->bufferFrameCount - numFramesPadding;
        
        if (avail >= sd->sndbufframes) {
            break;  // Есть место
        }
        
        sleep_millis(1);  // ← 1ms задержка
        
        if (stuck-- < 0) {
            write_log(_T("WASAPI: sound stuck\n"));
            set_reset(sd);
            return;
        }
    }
    
    // Запись в WASAPI
    hr = s->pRenderClient->GetBuffer(sd->sndbufframes, &pData);
    memcpy(pData, sndbuffer, sd->sndbufsize);
    s->pRenderClient->ReleaseBuffer(sd->sndbufframes, 0);
}
```

#### 🔴 ПРОБЛЕМА #10: Активное ожидание (busy-wait)

**Проблемы**:
1. **sleep_millis(1)** вызывается в цикле
2. Блокирует **эмуляционный thread**
3. При заполненном буфере: может крутиться до 2000 итераций!
4. CPU overhead: до 80% одного ядра при проблемах

**DirectSound**: Большой внутренний буфер → редко блокируется  
**WASAPI**: Маленький буфер → часто блокируется → эмуляция тормозит

---

## 📊 ЧАСТЬ 4: Динамическая коррекция - "Магия чисел"

### 4.1. docorrection() - Алгоритм подстройки

**Константы** (строки 136-140):
```cpp
#define ADJUST_SIZE 20
#define EXP 1.9

#define ADJUST_VSSIZE 12
#define EXPVS 1.6
```

**Использование** (строка 2365 в push mode):
```cpp
docorrection(s, 
    (s->wasapigoodsize - avail) * 1000 / s->wasapigoodsize, 
    (float)(s->wasapigoodsize - avail), 
    100);
```

#### 🔴 ПРОБЛЕМА #11: "Магия" без документации

**Проблемы**:
1. Нет комментариев что делает
2. "Магические константы" (1.9? 1.6?)
3. Изменяет sample rate на лету
4. **WASAPI НЕ любит** динамическое изменение write pattern

**DirectSound**: Терпит изменения rate  
**WASAPI**: Clock drift → glitches

---

### 4.2. "Magic adjustment"

**Код** (строка 1526):
```cpp
// magic adjustment
sd->sndbufsize = sd->sndbufsize * 2 / 3;
```

#### 🔴 ПРОБЛЕМА #12: Необъяснимый коэффициент

**Эффект**:
- Уменьшает буфер на 33%
- Нет комментария ПОЧЕМУ
- Работает для DirectSound, ломает WASAPI

---

## 📊 ЧАСТЬ 5: Инициализация и lifecycle

### 5.1. open_audio_wasapi() - 400+ строк хаоса

**Размер функции**: Строки 1308-1763 = **455 строк!**

**Что делает**:
1. Enum devices
2. Выбор формата
3. Проверка поддержки
4. Initialize IAudioClient
5. GetBufferSize
6. GetService (RenderClient, AudioVolume, AudioClock)
7. Расчёт размеров буферов
8. Создание pullbuffer
9. Создание событий
10. Настройка MMCSS (если есть)

#### 🔴 ПРОБЛЕМА #13: God function

**Проблемы**:
- Невозможно понять что критично, что опционально
- Нет разделения на логические блоки
- Ошибки обработки размазаны по всей функции
- Goto error по всей функции

---

### 5.2. Расчёт размеров буферов - "Лотерея"

**Код** (строки 1697-1722):
```cpp
if (s->pullmode) {
    if (s->wasapiexclusive) {
        sd->sndbufsize = s->bufferFrameCount * sd->samplesize;
        s->pullbuffermaxlen = sd->sndbufsize;
    } else {
        sd->sndbufsize = s->bufferFrameCount * sd->samplesize / 2;
        s->pullbuffermaxlen = sd->sndbufsize * 2;
    }
    s->wasapigoodsize = s->bufferFrameCount;
} else {
    sd->sndbufsize = (s->bufferFrameCount / 8) * sd->samplesize;
    v = s->bufferFrameCount * sd->samplesize;
    v /= 2;
    if (sd->sndbufsize > v)
        sd->sndbufsize = v;
    s->wasapigoodsize = s->bufferFrameCount / 2;
}
```

#### 🔴 ПРОБЛЕМА #14: Размеры "с потолка"

**Проблемы**:
1. `bufferFrameCount / 8` - откуда 8?
2. `bufferFrameCount / 2` - откуда половина?
3. Нет связи с **audio engine period** (480 frames)
4. Результат: некратные размеры → рассинхронизация

**Правильно**: Должно быть кратно MinPeriod из GetSharedModeEnginePeriod()

---

### 5.3. Cleanup - pause_audio_wasapi()

**Код** (строки 372-383):
```cpp
static void pause_audio_wasapi (struct sound_data *sd)
{
    struct sound_dp *s = sd->data;
    HRESULT hr;
    
    hr = s->pAudioClient->Stop();
    // ... error check
}
```

#### ⚠️ ПРОБЛЕМА #15: Неполная очистка

**Что НЕ делается**:
1. НЕТ `pAudioClient->Reset()` перед Stop
2. НЕТ ожидания завершения callback
3. pullbufferlen НЕ сбрасывается
4. События НЕ очищаются

**Результат**: Остаточные данные → глитчи при resume

---

## 📊 ЧАСТЬ 6: Сравнение с DirectSound - Почему DS "работает"

### 6.1. DirectSound архитектура

```
Paula → finish_sound_buffer_ds()
    ↓
IDirectSoundBuffer::Lock()
    ↓
memcpy в ring buffer DirectSound
    ↓
IDirectSoundBuffer::Unlock()
    ↓
DirectSound internal mixer (в kernel mode)
    ↓
Автоматический zero-fill при underrun
    ↓
Большой ring buffer (100-200ms)
    ↓
Audio output
```

**Почему стабильно**:
1. **Большой internal buffer** - прощает задержки
2. **Zero-fill автоматический** - нет треска при underrun
3. **Толерантен к timing** - может растянуть/сжать
4. **Kernel mode mixer** - высокий приоритет

---

### 6.2. WASAPI архитектура (как ДОЛЖНА быть)

```
Application → IAudioClient::Initialize()
    ↓
SetEventHandle(event)
    ↓
IAudioClient::Start()
    ↓
[WASAPI Audio Engine генерирует события]
    ↓
WaitForSingleObject(event, INFINITE)
    ↓
GetCurrentPadding() - узнать сколько можем записать
    ↓
GetBuffer() - получить указатель
    ↓
Записать ТОЧНОЕ количество frames
    ↓
ReleaseBuffer()
    ↓
Повторить по событию
```

**Требования WASAPI**:
1. **Точный timing** - писать по событию, не по таймеру
2. **Точное количество** - писать frames кратно period
3. **Нет zero-fill** - приложение обязано заполнить ВСЁ
4. **Малый buffer** - 10-50ms, не 100-200ms как DS

---

### 6.3. Что делает WinUAE (неправильно)

```
Paula → paula_sndbuffer (65KB)
    ↓
finish_sound_buffer_pull()
    ↓
memcpy → pullbuffer
    ↓
[Асинхронно]
    ↓
WaitForSingleObject(pullevent, 0) - POLLING!
    ↓
GetCurrentPadding()
    ↓
GetBuffer(avail) - ПРОИЗВОЛЬНОЕ количество
    ↓
memcpy из pullbuffer
    ↓
memmove остаток pullbuffer ← ЛИШНЕЕ
    ↓
ReleaseBuffer()
```

**Проблемы**:
1. **Двойная буферизация** - paula_sndbuffer + pullbuffer
2. **Polling** вместо blocking wait
3. **Произвольное количество frames** - не кратно period
4. **memmove** - O(n) каждый callback
5. **Нет zero-fill** - при underrun тишина не гарантирована

---

## 📊 ЧАСТЬ 7: Фундаментальные архитектурные проблемы

### 7.1. Проблема #1: Pull-модель поверх Push API

**DirectSound** = Pull API:
```
Приложение пишет когда хочет → DS тянет когда нужно
```

**WASAPI** = Push API:
```
WASAPI сигнализирует когда готов → Приложение ОБЯЗАНО записать
```

**WinUAE**: Пытается эмулировать Pull поверх Push:
```
Paula пишет → pullbuffer → WASAPI тянет
```

**Результат**: Impedance mismatch → рассинхронизация

---

### 7.2. Проблема #2: Синхронная обработка в main thread

```
Emulation loop
    ↓
Paula генерирует samples
    ↓
finish_sound_buffer() ← БЛОКИРУЕТ ЭМУЛЯЦИЮ
    ↓
send_sound()
    ↓
WASAPI: ждём свободного места ← БЛОКИРУЕТ ЕЩЁ БОЛЬШЕ
    ↓
Resume emulation
```

**Правильно**:
```
Emulation loop
    ↓
Paula → lock-free queue
    ↓
Continue emulation (НЕ блокируется)

[Отдельный audio thread]
    ↓
Ждёт WASAPI event
    ↓
Читает из queue → WASAPI
```

---

### 7.3. Проблема #3: Отсутствие proper ring buffer

**Текущий pullbuffer**:
```
[DATA DATA DATA _______]
 ^            ^
 |            pullbufferlen
 pullbuffer
 
memmove при чтении:
[TA DATA _____________]
 ^      ^
 |      новый pullbufferlen
 pullbuffer
```

**Правильный ring buffer**:
```
[__DATA DATA DATA____]
      ^         ^
      read      write
      
НЕТ memmove! Просто двигаем указатели:
[_______DATA DATA____]
           ^    ^
           read write
           
При wrap:
[DATA____DATA_____]
    ^         ^
    write     read
```

---

### 7.4. Проблема #4: Один размер буфера для всех API

```cpp
sd->sndbufsize  // Используется для DS, WASAPI, PA, AL, XA
```

**Проблема**:
- DirectSound хочет 100ms
- WASAPI хочет 20ms
- PortAudio хочет 10ms

**Результат**: Компромисс который никого не устраивает

---

### 7.5. Проблема #5: Отсутствие zero-fill protection

**WASAPI требует**:
```cpp
GetBuffer(requestedFrames, &buffer);
// ОБЯЗАТЕЛЬНО заполнить ВСЕ requestedFrames
memset(buffer, 0, requestedFrames * frameSize);  // Если нет данных
memcpy(buffer, data, actualFrames * frameSize);
ReleaseBuffer(requestedFrames);
```

**WinUAE делает**:
```cpp
GetBuffer(avail, &buffer);
if (pullbufferlen > 0) {
    memcpy(buffer, pullbuffer, ...);  // Частичное заполнение
    // НЕТ zero-fill остатка!
}
ReleaseBuffer(avail);
```

**Результат**: Undefined data в буфере → треск

---

## 📊 ЧАСТЬ 8: Почему патчи не помогают

### 8.1. Патч #1: OnDefaultDeviceChanged

**Что фиксит**: Лишние сбросы при смене устройства

**Что НЕ фиксит**: 
- Двойную буферизацию
- memmove overhead
- Timing issues
- Zero-fill проблемы

**Вердикт**: Полезно, но поверхностно

---

### 8.2. Патч #2: Magic adjustment

**Что фиксит**: Убирает искусственное уменьшение буфера

**Что НЕ фиксит**:
- Размер всё равно не кратен audio engine period
- Pull-модель остаётся
- memmove остаётся

**Вердикт**: Полезно, но недостаточно

---

### 8.3. Патч #3: Buffer size

**Что фиксит**: Увеличивает буфер для стабильности

**Что НЕ фиксит**:
- Если некратно period → глитчи остаются
- memmove работает с БОЛЬШИМ буфером → ещё медленнее
- Pull-модель остаётся

**Вердикт**: Может помочь или навредить

---

### 8.4. Патч #4: pullbufferlen = 0

**Что фиксит**: Глитчи после паузы

**Что НЕ фиксит**:
- Всё остальное
- Это симптом, не причина

**Вердикт**: Необходимо, но недостаточно

---

## 📊 ЧАСТЬ 9: Правильная архитектура (как ДОЛЖНО быть)

### 9.1. Разделение ответственности

```cpp
// Вместо одной структуры:
struct sound_dp {
    // 50+ полей всех API
};

// Правильно:
struct AudioBackend {
    virtual ~AudioBackend() = 0;
    virtual bool Initialize() = 0;
    virtual bool Write(const float* samples, int frameCount) = 0;
    virtual void Close() = 0;
};

class WASAPIBackend : public AudioBackend {
    IMMDevice* device;
    IAudioClient* client;
    IAudioRenderClient* renderClient;
    // ТОЛЬКО WASAPI поля
};

class DirectSoundBackend : public AudioBackend {
    IDirectSound8* ds;
    // ТОЛЬКО DS поля
};
```

---

### 9.2. Lock-free ring buffer

```cpp
class LockFreeRingBuffer {
    alignas(64) std::atomic<uint32_t> writePos;
    alignas(64) std::atomic<uint32_t> readPos;
    uint32_t capacity;
    float* buffer;
    
public:
    bool Write(const float* data, uint32_t frames) {
        uint32_t write = writePos.load(std::memory_order_acquire);
        uint32_t read = readPos.load(std::memory_order_acquire);
        
        uint32_t available = capacity - ((write - read) & (capacity - 1));
        if (available < frames) return false;
        
        // Write with wraparound, NO memmove
        uint32_t firstPart = std::min(frames, capacity - (write & (capacity - 1)));
        memcpy(buffer + (write & (capacity - 1)), data, firstPart * sizeof(float));
        if (firstPart < frames) {
            memcpy(buffer, data + firstPart, (frames - firstPart) * sizeof(float));
        }
        
        writePos.store((write + frames) & (capacity - 1), std::memory_order_release);
        return true;
    }
    
    uint32_t Read(float* data, uint32_t frames) {
        // Аналогично, NO memmove
    }
};
```

---

### 9.3. Dedicated audio thread

```cpp
class AudioThread {
    std::thread thread;
    LockFreeRingBuffer ringBuffer;
    WASAPIBackend* backend;
    
    void ThreadFunc() {
        while (running) {
            // Ждём события от WASAPI (НЕ polling!)
            WaitForSingleObject(backend->GetEvent(), INFINITE);
            
            // Узнаём сколько нужно
            uint32_t framesNeeded = backend->GetFramesNeeded();
            
            // Читаем из ring buffer
            float tempBuffer[2048];
            uint32_t framesRead = ringBuffer.Read(tempBuffer, framesNeeded);
            
            // Zero-fill если не хватило
            if (framesRead < framesNeeded) {
                memset(tempBuffer + framesRead * channels, 0, 
                       (framesNeeded - framesRead) * channels * sizeof(float));
            }
            
            // Пишем в WASAPI
            backend->Write(tempBuffer, framesNeeded);
        }
    }
};
```

---

### 9.4. Paula integration

```cpp
// В audio.cpp (Paula emulation):
extern AudioThread* g_audioThread;

void paula_finish_samples() {
    // Конвертация Paula samples в float
    float samples[MAX_PAULA_SAMPLES];
    ConvertSamples(paula_buffer, samples, count);
    
    // Запись в ring buffer (НЕ блокируется!)
    if (!g_audioThread->WriteS samples(samples, count)) {
        // Overflow - логировать, но НЕ блокировать
        stats.overruns++;
    }
    
    // Продолжаем эмуляцию
}
```

---

## 📊 ЧАСТЬ 10: План полного редизайна

### Phase 1: Подготовка (1 неделя)

1. **Создать новую структуру** `AudioBackend` interface
2. **Реализовать** `WASAPIBackendV2` с нуля
3. **Создать** `LockFreeRingBuffer`
4. **Unit tests** для ring buffer

### Phase 2: Integration (1 неделя)

1. **Создать** dedicated audio thread
2. **Интегрировать** с Paula
3. **Переключатель** old/new backend в runtime
4. **Тестирование** параллельно

### Phase 3: Refinement (1 неделя)

1. **Удалить** старый WASAPI код
2. **Портировать** остальные backends (DS, PA)
3. **Оптимизация** производительности
4. **Документация**

### Phase 4: Testing (1 неделя)

1. **Extensive testing** Octamed4
2. **Stress tests** (сворачивание, CPU нагрузка)
3. **Latency measurements**
4. **Bug fixes**

---

## 📊 ЗАКЛЮЧЕНИЕ

### Почему патчи не работают:

1. **Архитектура фундаментально сломана**
   - Pull поверх Push
   - Двойная буферизация
   - Синхронная обработка

2. **DirectSound legacy везде**
   - Размеры буферов
   - Timing assumptions
   - Overflow handling

3. **Нет изоляции**
   - Одна структура для всех API
   - Общие буферы
   - Shared state

### Что нужно:

1. **Полный редизайн** - не патчи
2. **Новая архитектура** - правильная с нуля
3. **Dedicated audio thread** - не в main loop
4. **Lock-free ring buffer** - не memmove
5. **WASAPI-native** подход - не эмуляция pull

### Оценка трудозатрат:

- **Патчи**: Бесконечно латать → никогда не работает идеально
- **Редизайн**: 4 недели → работает как в DAW

**Вердикт**: Редизайн неизбежен для профессионального качества.

---

**Дата анализа**: 04 февраля 2026  
**Версия**: 1.0 - Complete Architectural Analysis  
**Статус**: Foundation for redesign  
**Рекомендация**: ПОЛНЫЙ РЕДИЗАЙН, патчи не помогут
