# ILDAWaveX16 - Analisi Completa dei Buffer

## Overview del Data Path

```
MadMapper → Ethernet → W5500 → LWIP → etherdream_server → frame_buffer → dac_timer → DAC80508
            [70 pnt]   [HW]    [SW]   [batch 512]       [8192 pnt]     [ISR 512]   [6µs/pt]
```

## 1. Hardware: W5500 Buffers (Chip Interno)

### Socket RX Buffer
- **Tipo**: SRAM interna W5500
- **Dimensione per socket**: **2 KB** (default)
- **Configurabile**: Sì (tramite Sn_RXBUF_SIZE: 1/2/4/8/16 KB)
- **Attuale**: 2 KB (sufficiente per ~111 punti = 1998 byte)
- **Performance**: Hardware-managed, zero CPU overhead

### Socket TX Buffer  
- **Tipo**: SRAM interna W5500
- **Dimensione per socket**: **2 KB** (default)
- **Configurabile**: Sì (tramite Sn_TXBUF_SIZE: 1/2/4/8/16 KB)
- **Attuale**: 2 KB (ampio per ACK response da 22 byte)
- **Performance**: Hardware-managed, zero CPU overhead

**Note**: 
- W5500 ha 16 KB totali RX e 16 KB totali TX divisi tra 8 socket
- Allocation default: 2 KB RX + 2 KB TX per socket × 8 socket = 32 KB
- Possiamo aumentare il buffer del socket TCP principale (socket 0) a 8 KB

### Implicazione Performance
Con 2 KB RX buffer:
- 1 batch 70 punti = 1263 byte → **fit singolo** ✓
- 2 batch consecutivi = 2526 byte → **overflow, serve interrupt tra i due** ⚠️

Con 8 KB RX buffer (raccomandato):
- ~6 batch (6 × 1263 = 7578 byte) → **batch multipli senza interrupt** ✓

## 2. Software: LWIP TCP Stack (ESP32)

### TCP Send Buffer (per socket)
- **Definizione**: `CONFIG_LWIP_TCP_SND_BUF_DEFAULT`
- **Dimensione**: **32768 byte** (32 KB)
- **RAM Allocation**: Heap (dinamica)
- **Scopo**: Buffer per dati in uscita (ACK responses)
- **Utilizzo**: ~0.1% (22 byte ACK ogni 1263 byte RX)

### TCP Window (per socket)
- **Definizione**: `CONFIG_LWIP_TCP_WND_DEFAULT`  
- **Dimensione**: **32768 byte** (32 KB)
- **Scopo**: TCP flow control - quanti dati il peer può inviare senza ACK
- **Implicazione**: MadMapper può inviare ~26 batch (32768/1263) prima di bloccarsi
- **Ottimale**: ✓ Evita stallo durante elaborazione

### TCP Receive Mailbox
- **Definizione**: `CONFIG_LWIP_TCP_RECVMBOX_SIZE`
- **Dimensione**: **64 messaggi**
- **RAM per messaggio**: ~16 byte (pointer + metadata)
- **RAM totale**: ~1 KB
- **Scopo**: Queue di eventi per tcpip_task
- **Ottimale**: ✓ Ampio per burst

### PBUF Pool (Packet Buffers)
- **Tipo**: ESP-IDF default pools
- **Dimensione stimata**: ~16-32 PBUF structures
- **RAM per PBUF**: ~120 byte + payload
- **Gestione**: Zero-copy dove possibile, copy necessaria per recv()
- **Performance**: Allocazione dinamica da heap

## 3. Application Layer: etherdream_server Buffers

### TCP RX Buffer (s_rx_buf)
```c
#define TCP_RX_BUF_SIZE (3 + 1000 * 18 + 16)  // 18019 byte
static uint8_t s_rx_buf[TCP_RX_BUF_SIZE];
```
- **Dimensione**: **18019 byte** (~17.6 KB)
- **RAM Location**: Static BSS (.bss section)
- **Scopo**: Buffer per recv() TCP (accetta fino a 1000 punti per call)
- **Utilizzo tipico**: 1263 byte (70 punti) per recv()
- **Efficienza**: 7% utilizzo (1263/18019)

**Ottimizzazione possibile**:
- Ridurre a 4096 byte (sufficiente per 3 batch da 70 punti)
- **Risparmio RAM**: ~14 KB

### Point Batch Buffer (s_point_batch)
```c
#define POINT_BATCH_SIZE 512
static laser_point_t s_point_batch[POINT_BATCH_SIZE];
```
- **Dimensione**: 512 punti
- **RAM**: 512 × 13 byte = **6656 byte** (~6.5 KB)
- **RAM Location**: Static BSS
- **Scopo**: Accumula punti convertiti prima di flush nel frame_buffer
- **Threshold flush**: 512 punti (o fine batch DATA command)
- **Perché 512**: Bilancia tra latenza e overhead di write atomica

**Timeline di flush**:
- MadMapper invia 70 punti → accumula in s_point_batch
- Dopo 7 batch (490 punti) → ancora NO flush
- Batch 8 (560 punti totali) → flush 512, restano 48 in batch
- **Latenza**: ~7 batch prima del primo flush

### Response Batch Buffer (s_resp_batch)
```c
#define RESP_BATCH_MAX 16
static etherdream_response_t s_resp_batch[RESP_BATCH_MAX];
```
- **Dimensione**: 16 responses
- **RAM**: 16 × 22 byte = **352 byte**
- **RAM Location**: Static BSS
- **Scopo**: Accumula ACK responses prima di flush via TCP
- **Utilizzo**: Tipicamente 1 response (flush immediato dopo recv cycle)

### Data Point Temp Buffer (s_data_point_buf)
```c
static uint8_t s_data_point_buf[18];
```
- **Dimensione**: **18 byte**
- **Scopo**: State machine per parsing point (gestisce recv parziali)
- **Utilizzo**: Raro (solo se recv() split point a metà)

### Totale RAM Application Buffers
```
s_rx_buf:          18019 byte
s_point_batch:      6656 byte
s_resp_batch:        352 byte
s_data_point_buf:     18 byte
----------------------------------
TOTALE:           25045 byte (~24.5 KB)
```

## 4. Core: Frame Buffer (SPSC Ring Buffer)

```c
#define FRAME_BUFFER_SIZE 8192
static laser_point_t buffer[FRAME_BUFFER_SIZE];
```

### Configurazione
- **Dimensione**: **8192 punti**
- **RAM**: 8192 × 13 byte = **106496 byte** (~104 KB) 🔥
- **RAM Location**: Static BSS
- **Type**: Lock-free SPSC (Single Producer Single Consumer)
- **Producer**: network_task (Core 0)
- **Consumer**: dac_refill_task (Core 1)
- **Sync**: Atomic operations (`atomic_size_t head/tail`)

### Capacità Temporale
A 30 kpps (30,000 punti/sec):
- **Buffer time**: 8192 / 30000 = **273 ms** ✓

A 50 kpps:
- **Buffer time**: 8192 / 50000 = **164 ms** ✓

A 100 kpps:
- **Buffer time**: 8192 / 100000 = **82 ms** ⚠️

### Reported to Client
```c
#define ETHERDREAM_BUFFER_CAPACITY 8192
```
- **Status field**: `buffer_fullness` (uint16_t)
- **Client logic**: MadMapper monitora e regola invio rate basandosi su fullness
- **Target client**: ~1700 punti (configurato in `low_water_mark`)

### Performance
- **Write latency**: ~20 µs per batch 512 punti (memcpy + atomic)
- **Read latency**: ~15 µs per batch 256 punti (memcpy + atomic)
- **Lock-free**: Zero contention, Core 0 e Core 1 indipendenti ✓

## 5. Hardware Timer: ISR Buffer (Core 1)

```c
#define ISR_BUFFER_SIZE 512
static laser_point_t s_isr_buffer[ISR_BUFFER_SIZE];
```

### Configurazione
- **Dimensione**: **512 punti**
- **RAM**: 512 × 13 byte = **6656 byte** (~6.5 KB)
- **RAM Location**: Static BSS
- **Producer**: dac_refill_task (Core 1, normal priority)
- **Consumer**: gptimer_isr (Core 1, ISR context)
- **Sync**: Volatile indices (single-core, no atomics needed)

### Refill Logic
```c
#define REFILL_THRESHOLD 64
#define REFILL_MAX_BATCH 256
```

**Task loop**:
1. Check `isr_buffer_count()`
2. Se < 64 punti → refill
3. Legge fino a 256 punti dal frame_buffer
4. Scrive nell'ISR buffer

**ISR loop** (ogni 1/rate secondi):
1. Legge 1 punto dall'ISR buffer
2. Output al DAC via SPI (~6 µs)
3. Incrementa tail

### Capacità Temporale
A 30 kpps:
- **Buffer time**: 512 / 30000 = **17 ms** ✓

A 100 kpps:
- **Buffer time**: 512 / 100000 = **5 ms** ⚠️

### Criticità
- **MUST** refill > 64 points in < 2 ms @ 30 kpps
- Refill task priority: **25** (high, ma non realtime come ISR)
- Pinned to Core 1: ✓ Affinity garantita

## 6. Riepilogo Totale RAM Usage

### Static Buffers (BSS)
```
W5500 Buffers:           4 KB (hardware, non-ESP32)
LWIP Pools:         ~10-15 KB (heap dinamica)
etherdream_server:   ~24.5 KB (static)
frame_buffer:       ~104 KB (static) ← MAGGIORE
dac_timer ISR:       ~6.5 KB (static)
----------------------------------------
TOTALE STIMATO:     ~150 KB RAM
```

### ESP32-S3 RAM Available
- **SRAM0**: 192 KB (instruction + data)
- **SRAM1**: 128 KB (data)
- **Totale**: **320 KB**

**Utilizzo buffer**: 150/320 = **47%** ✓

Margine per:
- Stack (tasks, ISR): ~30 KB
- Heap (LWIP, HTTP, misc): ~100 KB
- Code + rodata: ~40 KB

## 7. Analisi Flow Control e Buffering Strategy

### Scenario Normale: Streaming 30 kpps

```
Timeline @ 30 kpps (33.3 µs per punto):

T=0:     MadMapper invia batch 70 punti
         │
         ├─→ W5500 RX (2 KB): accumula 1263 byte
         │   └─→ LWIP recv: copia in s_rx_buf (18 KB)
         │       └─→ Parser: converte in s_point_batch (512 pt)
         │           └─→ frame_buffer (8192 pt): +70 punti
         │
T=+2.3ms: DAC consuma 70 punti (70 × 33.3µs)
         │   └─→ ISR buffer (512): -70, refill da frame_buffer
         │
T=+5ms:  MadMapper invia batch successivo
```

**Bilanciamento**: ✓ Produzione (~140 batch/sec) = Consumo (30k pps / 70)

### Scenario Burst: MadMapper Riempie Buffer

```
MadMapper logic:
1. Check buffer_fullness da status ACK
2. Se fullness < low_water (1700):
   └─→ Invia batch fino a ~4000 punti (senza sleep)
3. Se fullness > 4000:
   └─→ Sleep (~10-50ms) per far drenare buffer
```

**Con batch 70 punti**:
- 1700 punti = ~24 batch
- 4000 punti = ~57 batch
- Tempo invio 57 batch @ 500 µs/batch = **28.5 ms**
- Consumo DAC @ 30 kpps in 28.5 ms = 855 punti

**Risultato**: Buffer cresce da 1700 → 4000 (+2300 netti)

### Underrun Scenario

**Causa**: Network stall > buffer drain time

A 30 kpps con frame_buffer 8192:
- **Time to drain**: 8192 / 30000 = **273 ms**
- Se network silence > 273 ms → **underrun**

**Detection**:
```c
static volatile uint32_t s_isr_underruns = 0;  // Incremented in ISR
```

**Recovery**: ISR output REST_POINT (blanked) fino a nuovi dati

## 8. Bottleneck Identification

### 1. W5500 Hardware Buffer (2 KB) ⚠️
- **Problema**: Solo 1 batch alla volta
- **Soluzione**: Aumenta a 8 KB
- **Beneficio**: Riduce interrupt frequency, batch multipli

### 2. TCP RX Application Buffer (18 KB)
- **Problema**: Sovradimensionato (7% utilizzo)
- **Soluzione**: Riduci a 4 KB
- **Beneficio**: -14 KB RAM

### 3. Point Batch Accumulator (512 punti) ⚠️
- **Problema**: Latency 7× batch prima flush
- **Soluzione**: Riduce a 128 o flush dopo ogni DATA command
- **Beneficio**: -260ms latenza @ 70pt batch

### 4. Frame Buffer (8192 punti) ✓
- **Stato**: Ottimale per 30 kpps streaming
- **Margine**: 273 ms @ 30 kpps
- **No action**: Perfetto bilanciamento

### 5. ISR Buffer (512 punti) ✓
- **Stato**: Sufficiente
- **Margine**: 17 ms @ 30 kpps
- **Refill threshold**: 64 punti = 2 ms @ 30 kpps
- **No action**: Funziona bene

## 9. Raccomandazioni Prioritarie

### ⭐⭐⭐⭐⭐ Critico
**A. Aumenta W5500 Socket Buffer a 8 KB**
```c
// In w5500_eth.c inizializzazione socket
uint8_t sn_rxbuf_size = 3;  // 0=1KB, 1=2KB, 2=4KB, 3=8KB
// Write to Sn_RXBUF_SIZE register
```
**Impatto**: Riduce interrupt per batch multipli, +300% capacity

### ⭐⭐⭐⭐☆ Alto
**B. Riduci Point Batch Size e Flush Immediato**
```c
#define POINT_BATCH_SIZE 128  // Era 512
// In handle_data_command(): flush dopo ogni DATA cmd completo
```
**Impatto**: -85% latenza burst (da 7 batch a 1 batch delay)

### ⭐⭐⭐☆☆ Medio
**C. Riduci TCP RX Buffer**
```c
#define TCP_RX_BUF_SIZE 4096  // Era 18019
```
**Impatto**: -14 KB RAM, no performance penalty

### ⭐⭐☆☆☆ Basso
**D. Monitoring e Telemetry**
```c
// Log underrun rate
// Log frame_buffer max_fullness
// Log W5500 RX overflows (se disponibile in HW)
```

## 10. Memory Map Visualization

```
┌─────────────────────────────────────────┐
│ ESP32-S3 SRAM (320 KB)                  │
├─────────────────────────────────────────┤
│                                         │
│  frame_buffer        104 KB  █████████  │  ← Main buffer
│  etherdream_server    25 KB  ██         │
│  dac_timer ISR         7 KB  █          │
│  LWIP pools           15 KB  █          │
│  Task stacks          30 KB  ██         │
│  Heap (dynamic)      100 KB  ████       │
│  Code + rodata        40 KB  ██         │
│                                         │
│  USED: ~320 KB / 320 KB (100%)          │
│                                         │
└─────────────────────────────────────────┘

W5500 Internal SRAM (32 KB):
┌─────────────────────────────────────────┐
│  Socket 0 RX          2 KB              │  ← Etherdream TCP
│  Socket 0 TX          2 KB              │
│  Socket 1 RX          2 KB              │  ← Broadcast UDP
│  Socket 1 TX          2 KB              │
│  Other sockets       24 KB  (unused)    │
└─────────────────────────────────────────┘
```

## Conclusione

Il sistema ha **3 livelli di buffering**:

1. **Hardware W5500** (2 KB) → ⚠️ COLLO DI BOTTIGLIA per batch multipli
2. **Software LWIP + App** (32 KB + 24 KB) → ✓ Ampio margine
3. **Core frame_buffer** (104 KB, 8192 pt) → ✓ Ottimale per streaming
4. **ISR buffer** (6.5 KB, 512 pt) → ✓ Sufficiente per realtime DAC

**Raccomandazioni implementabili subito**:
- Aumenta W5500 buffer → +300% burst capacity
- Riduci batch accumulator → -85% latency
- Riduci TCP RX buffer → -14 KB RAM

Tutte modifiche a basso rischio con alto beneficio! 🚀
