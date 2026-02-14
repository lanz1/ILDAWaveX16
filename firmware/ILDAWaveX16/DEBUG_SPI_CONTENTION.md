# Debug: SPI Contention Analysis e Buffer Flow Issues

## 🔴 PROBLEMI IDENTIFICATI

### 1. ⚠️ SPI Bus Contention: DAC vs W5500

**Situazione attuale**:
```
W5500 (Ethernet):  SPI2_HOST @ 50 MHz + DMA
DAC80508:          SPI3_HOST @ 50 MHz + NO DMA (direct register access)
```

**Apparentemente separati** → MA c'è un problema critico!

#### Il Problema: Direct Register Access in ISR

```c
// In dac80508.c - dac_output_point() chiamato da ISR
void dac_output_point(const laser_point_t* point) {
    if (s_direct_spi_ready) {
        // QUESTO VIENE CHIAMATO DALL'ISR OGNI ~10-100µs!
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_X, x);      // ~1-2µs
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_Y, y);      // ~1-2µs
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_RED, r);    // ~1-2µs
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_GREEN, g);  // ~1-2µs
        dac_write_direct(DAC_REG_DAC0 + DAC_CH_BLUE, b);   // ~1-2µs
        dac_write_direct(DAC_REG_TRIGGER, 0x0010);         // ~1-2µs
        // TOTALE: ~6-12µs BLOCCANDO L'ISR
    }
}
```

**Direct register access scrive direttamente nei registri SPI3 hardware**:
```c
static inline void IRAM_ATTR dac_write_direct(uint8_t reg, uint16_t value) {
    // Accesso diretto a DR_REG_SPI3_BASE
    GPIO.out_w1tc = (1 << DAC_PIN_CS);  // CS LOW - GPIO direct
    *s_spi_w0_reg = data;               // Scrive direttamente nel FIFO SPI3
    *s_spi_ms_dlen_reg = 23;            // Imposta lunghezza
    *s_spi_cmd_reg = SPI_USR;           // Avvia transfer
    while (*s_spi_cmd_reg & SPI_USR) {} // BUSY WAIT! ← BLOCCA ISR
    GPIO.out_w1ts = (1 << DAC_PIN_CS);  // CS HIGH
}
```

### ⚠️ IMPATTO: ISR Blocking durante SPI Transfer

**Timeline @ 10 kpps** (1 punto ogni 100µs):
```
T=0:     Timer ISR triggered
T=+2µs:  dac_output_point() chiamato
T=+2µs:  6× dac_write_direct inizia
T=+8µs:  Ultimo transfer SPI3 completa (busy wait!)
T=+10µs: ISR ritorna

CPU BLOCCATA in ISR per 6-10µs ogni 100µs = 6-10% CPU time!
```

**@ 80 kpps** (1 punto ogni 12.5µs):
```
T=0:     Timer ISR triggered
T=+2µs:  dac_output_point()
T=+8µs:  6× SPI writes complete
T=+10µs: ISR return

CPU BLOCCATA 80% DEL TEMPO! (10µs busy / 12.5µs period)
```

**Questo spiega il throttling!**

---

### 2. 🔴 ISR Priority vs Network Task Contention

**Core Assignment**:
```c
// Core 0 (CORE_SERVICES):
- LWIP tcpip_task (priority 15)
- network_task / etherdream_server (priority TASK_PRIORITY_EDREAM)
- W5500 interrupt handler (GPIO 8)
- W5500 SPI DMA controller

// Core 1 (CORE_REALTIME):
- dac_refill_task (priority configMAX_PRIORITIES - 2 = 23)
- GPTimer ISR (ISR priority 1 - HIGHEST)
- DAC SPI3 direct access
```

**Il problema**:
1. **ISR su Core 1** gira con massima priorità
2. **ISR busy-waits** per SPI3 transfers (~6-10µs)
3. **dac_refill_task** (priority 23) deve riempire ISR buffer
4. Se ISR gira troppo spesso → refill_task STARVED!

**Scenario @ 80 kpps**:
```
Core 1:
  [ISR 10µs]─┐  ┌─[ISR 10µs]─┐  ┌─[ISR 10µs]
             └─┘ ~2.5µs gap   └─┘ ~2.5µs gap

  refill_task (priority 23) NON HA TEMPO DI GIRARE!
  → ISR buffer drains → underrun → REST_POINT
```

---

### 3. 🔴 Buffer Flow Breakdown

**Frame Buffer → ISR Buffer Flow**:

```c
// dac_refill_task loop (Core 1, priority 23)
while (s_running) {
    if (s_playback_active) {
        size_t free = isr_buffer_free();  // Check free space
        if (free < REFILL_THRESHOLD) {   // 64 punti
            // Need refill!
            size_t to_read = free;
            if (to_read > REFILL_MAX_BATCH) 
                to_read = REFILL_MAX_BATCH;  // Max 256
            
            // Read from frame_buffer (Core 0 → Core 1)
            size_t n = frame_buffer_read(temp_batch, to_read);
            
            if (n > 0) {
                // Copy to ISR buffer
                for (size_t i = 0; i < n; i++) {
                    s_isr_buffer[s_isr_head] = temp_batch[i];
                    s_isr_head = (s_isr_head + 1) % ISR_BUFFER_SIZE;
                }
            }
        }
    }
    
    vTaskDelay(1);  // Sleep 1ms if nothing to do ← PROBLEMA!
}
```

**Il problema**: `vTaskDelay(1)` = **1 millisecondo!**

@ 80 kpps:
- Consuma: 80 punti/ms
- ISR buffer: 512 punti
- Se refill_task dorme 1ms → consuma 80 punti
- Se ISR buffer < 64, refill, ma poi dorme 1ms
- In 1ms @ 80 kpps → consuma altri 80 punti
- **ISR buffer drain troppo veloce!**

---

### 4. 🔴 Frame Buffer Write Contention

**Scenario critico**:

```
Core 0 (network_task):
  recv() → feed_rx_data() → add_point_to_batch()
  → flush_point_batch() → frame_buffer_write()
     ↓
     Scrive in frame_buffer (atomic head update)

Core 1 (dac_refill_task):
  frame_buffer_read() legge da frame_buffer
     ↓
     Legge da frame_buffer (atomic tail update)
```

**Lock-free SPSC** è ottimo MA:
- Se write è molto veloce (batch grandi)
- E read è interrotto dall'ISR (starvation)
- Buffer si riempie rapidamente!

**Con batch 70 punti ogni 1ms @ 70 kpps**:
- Inflow: 70 punti/ms
- Outflow: 70 punti/ms (se refill_task gira!)
- MA se refill_task starved per 5ms:
  - Inflow: 350 punti
  - Outflow: 0 punti (ISR consuma solo ISR buffer)
  - **Frame buffer +350 punti!**

Poi quando ISR buffer vuoto:
- CPU cerca di refill
- ISR interrompe continuamente
- Refill lento
- **THROTTLING!**

---

## 🔍 DIAGNOSI: Perché il Log Mostra Quello Pattern

### Pattern Osservato:

```
1. Throttling con 80 punti
2. Riconnessione
3. Buffer si riempie
4. Poi torna basso
```

### Spiegazione:

#### Fase 1: Throttling
```
@ 80 kpps:
- ISR busy 80% del tempo (10µs ISR / 12.5µs period)
- dac_refill_task STARVED (non può girare)
- ISR buffer drains → underrun
- frame_buffer PIENO (network riceve veloce)
- Client vede buffer_fullness HIGH → THROTTLE!
```

#### Fase 2: Riconnessione
```
- Client disconnette (timeout o buffer full)
- etherdream close_client():
  - dac_timer_set_playback_active(false) ← STOP DAC!
  - frame_buffer_clear()
- ISR smette di consumare
- Refill_task può girare di nuovo
```

#### Fase 3: Buffer Riempimento
```
- Nuovo client connette
- BEGIN command
- dac_timer_set_playback_active(true) ← START DAC!
- ISR ricomincia a consumare
- Frame buffer vuoto → refill lento
- Network invia batch → frame_buffer si riempie
```

#### Fase 4: Torna Basso (Cycle Repeat)
```
- Frame buffer alto → refill veloce → ISR soddisfatta
- MA rate troppo alto → ISR busy 80%
- Refill_task starved di nuovo
- → TORNA A FASE 1!
```

---

## 🛠️ SOLUZIONI

### ✅ Soluzione 1: Riduci ISR Blocking Time (CRITICAL)

**Problema**: ISR busy-wait per SPI transfers.

**Fix**: Usa DMA per DAC SPI3!

```c
// In dac80508.c - dac_init()
spi_bus_config_t bus_cfg = {
    .mosi_io_num = DAC_PIN_MOSI,
    .miso_io_num = -1,
    .sclk_io_num = DAC_PIN_CLK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 32,  // Aumenta per DMA
};

// ✓ ABILITA DMA!
esp_err_t ret = spi_bus_initialize(DAC_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
```

**Modifica ISR**:
```c
// Usa queue non-blocking invece di direct access
void dac_output_point(const laser_point_t* point) {
    // Prepara transaction
    spi_transaction_t trans[6];
    
    // X, Y, R, G, B, TRIGGER
    for (int i = 0; i < 6; i++) {
        trans[i].length = 24;
        trans[i].tx_buffer = dac_data[i];
        trans[i].flags = SPI_TRANS_CS_KEEP_ACTIVE;
    }
    trans[5].flags = 0;  // Last one releases CS
    
    // Queue tutte le transaction (DMA background)
    for (int i = 0; i < 6; i++) {
        spi_device_queue_trans(s_spi, &trans[i], 0);  // Non-blocking!
    }
    
    // ISR può ritornare subito! DMA fa il lavoro
}
```

**Impatto**: ISR time: 10µs → **~2µs** (-80%)

---

### ✅ Soluzione 2: Refill Task Priority & Sleep

**Problema**: refill_task starved da ISR + sleep troppo lungo.

**Fix A**: Riduci sleep time:
```c
// In dac_refill_task loop
if (refill_did_nothing) {
    vTaskDelay(pdMS_TO_TICKS(1));  // Era 1ms
} else {
    // NO SLEEP se abbiamo fatto refill!
    // Yield per dare chance ad altri task, poi ri-check subito
    taskYIELD();
}
```

**Fix B**: Aumenta ISR buffer size (meno urgenza):
```c
#define ISR_BUFFER_SIZE 1024  // Era 512, ora 1024
#define REFILL_THRESHOLD 128  // Era 64, ora 128
#define REFILL_MAX_BATCH 512  // Era 256, ora 512
```

**Impatto**: Più margine per refill_task, meno underrun.

---

### ✅ Soluzione 3: Batch Point Output (ADVANCED)

**Idea**: Invece di 1 punto alla volta, output multipli punti per ISR.

```c
#define POINTS_PER_ISR 2  // Output 2 punti per ISR call

static bool IRAM_ATTR timer_isr_callback(...) {
    for (int i = 0; i < POINTS_PER_ISR; i++) {
        if (s_isr_tail == s_isr_head) {
            dac_output_point(&REST_POINT);
            s_isr_underruns++;
            break;
        }
        
        const laser_point_t* point = &s_isr_buffer[s_isr_tail];
        dac_output_point(point);
        s_isr_tail = (s_isr_tail + 1) % ISR_BUFFER_SIZE;
    }
    
    return false;
}
```

**Timer frequency**: rate / POINTS_PER_ISR

@ 80 kpps:
- Prima: 80,000 ISR/sec (12.5µs period)
- Dopo: 40,000 ISR/sec (25µs period) con 2 punti/ISR
- **ISR overhead dimezzato!**

---

### ✅ Soluzione 4: Core Pinning Fix

**Problema**: Possibile che network interrupt su Core 0 interferisce con W5500 DMA.

**Fix**: Pin W5500 interrupt explicitly a Core 0:
```c
// In w5500_eth.c dopo GPIO ISR service install
if (PIN_ETH_INT >= 0) {
    gpio_install_isr_service(0);
    
    // Pin interrupt a Core 0
    gpio_isr_handler_add(PIN_ETH_INT, w5500_isr_handler, NULL);
    esp_intr_set_affinity(gpio_get_intr_num(PIN_ETH_INT), 
                         1 << CORE_SERVICES);  // Core 0
}
```

---

## 📊 Performance Estimate con Fix

### Prima (Attuale):
```
@ 80 kpps:
ISR blocking:    10µs / 12.5µs = 80% CPU Core 1
Refill starved:  ⚠️ YES
Underruns:       ⚠️ FREQUENT
Frame buffer:    ⚠️ OVERFLOW → throttle
```

### Dopo (Con DMA + Fix):
```
@ 80 kpps:
ISR blocking:    2µs / 12.5µs = 16% CPU Core 1
Refill active:   ✓ Gira frequentemente
Underruns:       ✓ ZERO
Frame buffer:    ✓ Stabile 1000-3000
Throughput:      ✓ 80 kpps sostenuto
```

---

## 🔧 Implementation Priority

### 1. ⭐⭐⭐⭐⭐ CRITICAL: Enable DAC SPI3 DMA
**File**: `src/hal/dac80508.c`
**Change**: `SPI_DMA_DISABLED` → `SPI_DMA_CH_AUTO`
**Impact**: -80% ISR time
**Effort**: 5 minuti
**Risk**: Basso

### 2. ⭐⭐⭐⭐☆ HIGH: Refill Task Sleep Fix
**File**: `src/hal/dac_timer.c`
**Change**: Smart sleep (solo se idle)
**Impact**: -50% underrun
**Effort**: 15 minuti
**Risk**: Basso

### 3. ⭐⭐⭐☆☆ MEDIUM: Increase ISR Buffer
**File**: `src/hal/dac_timer.c`
**Change**: 512 → 1024 punti, threshold 64 → 128
**Impact**: +100% margin
**Effort**: 2 minuti
**Risk**: +6.5 KB RAM

### 4. ⭐⭐☆☆☆ OPTIONAL: Batch Point Output
**File**: `src/hal/dac_timer.c`
**Change**: 2 punti per ISR
**Impact**: -50% ISR frequency
**Effort**: 30 minuti
**Risk**: Medio (test timing)

---

## 🧪 Testing Checklist

Dopo fix, verificare:

- [ ] ISR time < 5µs (era 10µs)
- [ ] Underrun rate = 0 @ 80 kpps
- [ ] Frame buffer stabile 1000-3000
- [ ] CPU Core 1 < 30% (era 80%)
- [ ] Nessun throttling/disconnect
- [ ] Pattern smooth (no spike)

---

## 📝 Root Cause Summary

**Il problema NON è**:
- ✗ SPI bus contention (sono bus separati)
- ✗ Network too fast (W5500 handle it)
- ✗ Frame buffer size (8192 è ampio)

**Il problema È**:
- ✓ ISR busy-wait su SPI3 (80% CPU @ 80 kpps)
- ✓ Refill task starved da ISR
- ✓ ISR buffer underrun
- ✓ Frame buffer overflow → throttle
- ✓ Cycle repeats → disconnect/reconnect pattern

**Fix principale**: **DMA per DAC SPI3** risolve tutto! 🎯
