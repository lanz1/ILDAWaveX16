# Root Cause Analysis: Underrun & Network Latency

> **Stato**: Fix applicati 2025-03-06. Documento aggiornato per riflettere codice attuale.
> 
> **Contesto attuale**: DAC output funziona. Liberation Laser OK a 30kpps con figure
> basiche. Problemi con disegni complessi (batch irregolari → buffer starvation).
> MadMapper invia batch fisse da 70 punti.

---

## Sintomi Originali (pre-fix)

```
I (25088) EDREAM: RX: 20166 pps | 2906 kbps | Buf: 138/8192 (1%) | Underruns: 126134
W (25300) EDREAM: Client closed connection (FIN)
```

- **Buffer fullness**: 138-466 punti (1-5% di 8192) - SEMPRE TROPPO BASSO
- **Underrun rate**: ~6000-11000 per secondo
- **Network RX**: 14000-20000 pps (instabile, dovrebbe essere costante 19500)
- **Client behavior**: Si disconnette ogni 1-2 secondi (vede buffer basso)

## Architettura Core e Task Assignment

### Core 0 (CORE_SERVICES)
```c
network_task:
  - Priority: 19 (TASK_PRIORITY_EDREAM)
  - Stack: 8192 byte
  - Function: etherdream_server_loop()
  - Responsabilità: 
    * Riceve TCP da W5500
    * Parsing protocol
    * Scrive frame_buffer (lock-free atomic)

tcpip_task (LWIP):
  - Priority: 20 (CONFIG_LWIP_TCPIP_TASK_PRIO)  ← FIX: era 18 (default)
  - Responsabilità:
    * Processa segmenti TCP dal W5500 rx_task
    * DEVE girare PRIMA di network_task per evitare recv() vuoti
```

### Core 1 (CORE_REALTIME)  
```c
dac_refill_task:
  - Priority: configMAX_PRIORITIES-2 = 23 (MOLTO ALTA)
  - Stack: 4096 byte
  - Responsabilità:
    * Legge frame_buffer
    * Scrive ISR buffer (1024 punti)

gptimer_isr_callback:
  - Priority: 1 (MASSIMA - ISR)
  - Frequency: scan_rate Hz (es. 30000 = ogni 33.3 µs)
  - Responsabilità:
    * Legge 1 punto dall'ISR buffer
    * 6 transazioni SPI al DAC80508 (~6-12 µs totale via direct register)
    * Output laser point
```

## Interrupt & Task Allocation (VERIFICATO)

```
Core 0:
  ├─ W5500 GPIO ISR         (gpio_install_isr_service → Core 0)     [brevissimo: sveglia rx_task]
  ├─ W5500 rx_task           priority 15                             [SPI read pacchetti]
  ├─ tcpip_task (LWIP)       priority 20  ← FIX applicato           [processa TCP]
  ├─ network_task            priority 19                             [recv + parse + frame_buffer write]
  └─ httpd, event loop, etc  priorità basse

Core 1:
  ├─ GPTimer ISR             (creato dentro dac_refill_task)         [1 punto → DAC SPI, ~6-12µs]
  └─ dac_refill_task         priority 23                             [frame_buffer → ISR buffer]
```

**Conferme dal codice:**
- GPTimer creato in [dac_timer.c#L119](src/hal/dac_timer.c#L119) dentro `dac_refill_task` → ISR su Core 1 ✓
- W5500 ISR da [w5500_eth.c#L126](src/hal/w5500_eth.c#L126) `gpio_install_isr_service(0)` in `app_main` → Core 0 ✓
- Nessun conflitto SPI: DAC su SPI3 (Core 1), W5500 su SPI2 (Core 0) ✓

---

## Root Cause Identificate e Fix Applicati

### Root Cause 1: select() timeout troppo alto — ✅ FIXATO

**Prima:**
```c
uint32_t timeout_us = (s_playback_state == PLAYBACK_PLAYING) ? 1000 : 50000;  // 1ms!
```

**Problema:** A 30kpps con batch variabili (Liberation) o fisse 70pt (MadMapper), il timeout
di 1ms aggiunge latenza inutile tra un `recv()` e il successivo. Se il pacchetto arriva a 
T=0.1ms dopo l'inizio del select(), network_task dorme fino a T=1ms → 0.9ms persi.

**Dopo (FIX):**
```c
uint32_t timeout_us = (s_playback_state == PLAYBACK_PLAYING) ? 100 : 50000;  // 100µs
```

**Impatto:** Latency max ridotta 10×. A 100µs non c'è busy-loop (solo ~10k wakeup/sec).

---

### Root Cause 2: LWIP priority inversion — ✅ FIXATO

**Prima:** LWIP tcpip_task priority = 18 (default ESP-IDF), network_task = 19.

**Problema:** Sequenza sbagliata ogni volta che arriva un pacchetto:
1. W5500 rx_task (15) passa pacchetto a LWIP
2. tcpip_task (18) scheduled per processare TCP
3. network_task (19) **preempta** tcpip_task! 
4. network_task fa `recv()` → **NIENTE** (TCP non ancora processato)
5. `select(1ms)` → dorme
6. tcpip_task finalmente processa TCP
7. network_task si sveglia dopo 1ms → finalmente legge dati

**Dopo (FIX):**
```ini
CONFIG_LWIP_TCPIP_TASK_PRIO=20  # > network_task (19)
```

**Impatto:** tcpip_task ora gira PRIMA di network_task. Sequenza corretta:
1. W5500 rx_task passa pacchetto → tcpip_task (20) preempta tutto
2. TCP processato → dati pronti in socket buffer
3. network_task (19) fa `recv()` → dati disponibili immediatamente

---

### Root Cause 3: ISR buffer troppo piccolo — ✅ FIXATO

**Prima:**
```c
#define ISR_BUFFER_SIZE 512
#define REFILL_THRESHOLD 64
```

**Problema:** A 30kpps, ISR buffer da 512 = solo ~17ms di headroom.
Se refill_task ha uno stall momentaneo (cache miss, scheduler), buffer si svuota → underrun.

**Dopo (FIX):**
```c
#define ISR_BUFFER_SIZE 1024    // ~34ms headroom @ 30kpps
#define REFILL_THRESHOLD 128    // Refill più spesso
```

**Impatto:** +13KB RAM (55.9% vs 53.8%), doppio margine per il refill task.

---

## Nota su DAC SPI e Multi-Write

Il documento originale proponeva "Opzione C: SPI multi-write con auto-increment".
**Questo NON è possibile col DAC80508** — il chip richiede una transazione SPI separata
da 24 bit per ogni registro. Non esiste auto-increment mode. Ogni `dac_output_point()` 
richiede 6 transazioni separate (~6-12µs totale con direct register access).

Questo è comunque accettabile:
- @ 30kpps: periodo = 33.3µs, ISR ~12µs = **36% Core 1** → OK
- @ 45kpps: periodo = 22.2µs, ISR ~12µs = **54% Core 1** → borderline
- @ 60kpps+: servirebbe ottimizzazione SPI (DMA linked list o QSPI)

---

## Comportamento Liberation Laser vs MadMapper

| Client | Batch Size | Comportamento |
|---|---|---|
| MadMapper | Fisso 70pt | Steady stream, prevedibile |
| Liberation | Variabile | Si adatta alla complessità del disegno |

**Liberation con figure complesse:**
- Più punti per frame → software impiega più tempo a generare
- Batch più piccole e irregolari nel tempo
- Buffer si svuota tra burst → underrun
- I 3 fix applicati migliorano la situazione riducendo la latenza di ricezione

**Se dopo i fix ci sono ancora underrun con Liberation su disegni complessi:**
- Verificare con i log `PIPE|` se `fb=` (frame_buffer level) crolla a 0
- Se sì, è Liberation che non invia abbastanza velocemente → problema client-side
- Se `fb` > 0 ma `isr_buf` = 0 → problema refill task (improbabile con buffer 1024)

---

## Test Plan

1. **Flash firmware** con i 3 fix
2. **Liberation Laser @ 30kpps, disegno complesso** → verificare:
   - `PIPE| fb=... isr_buf=... ur=...` nel serial monitor
   - Obiettivo: `ur=0`, `fb > 500`, playback stabile
3. **Se underrun persistono**: abilitare `LOG_PERF=1` in platformio.ini per timing dettagliato
4. **Se tutto OK**: testare a 45kpps per trovare il limite

---

## Changelog Fix

| Data | Fix | File | Dettaglio |
|---|---|---|---|
| 2025-03-06 | select() 1ms → 100µs | etherdream_server.c | Riduce latency recv 10× |
| 2025-03-06 | LWIP priority 18 → 20 | sdkconfig.defaults | Elimina priority inversion |
| 2025-03-06 | ISR buffer 512 → 1024 | dac_timer.c | Doppio headroom, +13KB RAM |
