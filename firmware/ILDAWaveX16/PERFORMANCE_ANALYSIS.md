# ILDAWaveX16 - Analisi Performance Throughput (DETTAGLIATA)

## Scenario Reale: MadMapper Batch da 70 Punti

### Formato Dati Esatti
- **DATA command**: 3 byte (cmd:1 + npoints:2)
- **Point**: 18 byte ciascuno (control:2 + x:2 + y:2 + r:2 + g:2 + b:2 + i:2 + u1:2 + u2:2)
- **Batch 70 punti**: 3 + (70 × 18) = **1263 byte**
- **ACK response**: 22 byte (resp:1 + cmd:1 + status:20)

## 1. Ricezione TCP Batch (1263 byte) via W5500

### W5500 Operazioni SPI per RX

Il W5500 usa **interrupt-driven** reception (GPIO 8). Quando arrivano dati TCP:

#### Fase 1: Interrupt Handler
1. **Read interrupt register** (IR)
   - SPI frame: 3 byte control + 1 byte data
   - Tempo @ 40MHz: ~1 µs

#### Fase 2: Check RX Size
2. **Read Sn_RX_RSR** (RX Received Size Register - 16 bit)
   - SPI frame: 3 byte control + 2 byte data
   - Tempo @ 40MHz: ~1.5 µs

#### Fase 3: Read TCP Data
3. **Read from RX buffer** (1263 byte)
   - W5500 usa block read mode
   - SPI frame: 3 byte control + 1263 byte data = 1266 byte totali
   - Tempo @ 40MHz: ~253 µs (1266 byte × 8 bit / 40 MHz)
   - CS overhead: ~1 µs (assert + deassert)

#### Fase 4: Update RX Read Pointer
4. **Write Sn_RX_RD** (pointer update)
   - SPI frame: 3 byte control + 2 byte data
   - Tempo @ 40MHz: ~1.5 µs

5. **Write Sn_CR** (command RECV)
   - SPI frame: 3 byte control + 1 byte data  
   - Tempo @ 40MHz: ~1 µs

**Totale W5500 SPI per RX 1263 byte: ~259 µs**

### LWIP Stack Overhead
- TCP packet processing: ~30 µs
- Socket buffer copy: ~15 µs
- Event signaling: ~10 µs

**Totale LWIP: ~55 µs**

### Totale Ricezione Batch
**W5500 + LWIP: 259 + 55 = ~314 µs**

## 2. Protocol Parsing (70 punti)

### Step by Step nel Parser

```c
// feed_rx_data() processa 1263 byte
```

#### Header Parsing (3 byte)
- State machine: ~1 µs
- Estrae npoints=70: ~0.5 µs

#### Point Conversion Loop (70 iterazioni)
```c
for (size_t i = 0; i < 70; i++) {
    etherdream_point_t* ep = (etherdream_point_t*)&data[pos];  // Cast: ~0
    add_point_to_batch(ep);  // Conversione
    pos += 18;
}
```

**Per ogni punto** `add_point_to_batch()`:
```c
laser_point_t* lp = &s_point_batch[s_batch_count];

lp->x = ep->x;        // 2 byte copy
lp->y = ep->y;        // 2 byte copy  
lp->r = ep->r;        // 2 byte copy
lp->g = ep->g;        // 2 byte copy
lp->b = ep->b;        // 2 byte copy
lp->user1 = 0;        // 1 byte assign
lp->user2 = 0;        // 1 byte assign
lp->flags = (ep->r == 0 && ep->g == 0 && ep->b == 0 && ep->i == 0)  // Branch + compare
            ? POINT_FLAG_BLANK : 0;

// Rate change check (raro)
if (ep->control & POINT_CONTROL_RATE_CHANGE) { ... }

s_batch_count++;
```

**Stima per punto**:
- 5× 16-bit copies: ~5 cicli
- 2× 8-bit assigns: ~2 cicli
- Blanking check (branch + 4 compares): ~8 cicli
- Rate check (1 test): ~2 cicli
- Array increment: ~1 ciclo
- **Totale: ~18 cicli @ 240 MHz = ~75 ns per punto**

**70 punti**: 70 × 0.075 µs = **5.25 µs**

#### Frame Buffer Write (dopo 70 o 512 punti)
MadMapper invia 70 punti → accumula in `s_point_batch[512]`.
Dopo 512 punti accumul ati, fa `flush_point_batch()`:

```c
size_t frame_buffer_write(const laser_point_t* points, size_t count) {
    // Atomic operations + memcpy
    memcpy(&buffer[h], points, count * sizeof(laser_point_t));  // Bulk copy
    atomic_store_explicit(&head, new_head, memory_order_release);
}
```

Ma con batch da 70, flush avviene ogni ~7 batch.
**Per questo singolo batch**: NO flush, solo accumulo in RAM.

**Totale parsing 70 punti: ~7 µs** (header 1 µs + conversion 5 µs + overhead 1 µs)

## 3. Send ACK Response (22 byte)

### Batching System
Il codice usa **response batching**:
```c
static etherdream_response_t s_resp_batch[16];  // Max 16 response
static size_t s_resp_batch_count = 0;
```

Dopo ogni DATA command:
```c
send_response(sock, RESP_ACK, CMD_DATA);  // Accumula in batch
// NO flush qui! 
```

Alla fine di `handle_client()`:
```c
flush_responses(s_client_socket);  // Flush TUTTE le response accumulate
```

**Quindi**: Se MadMapper invia 1 batch → 1 ACK accumulato → flush immediato.
Se invia N batch veloci → N ACK accumulati → 1 flush alla fine.

### Flush ACK via W5500 SPI

Per scrivere 22 byte response:

#### Write TX Data
1. **Write to TX buffer** (22 byte)
   - SPI frame: 3 byte control + 22 byte data = 25 byte
   - Tempo @ 40MHz: ~5 µs

#### Update TX Pointer
2. **Write Sn_TX_WR** (pointer update)
   - SPI frame: 3 byte control + 2 byte data
   - Tempo @ 40MHz: ~1.5 µs

#### Send Command
3. **Write Sn_CR** (command SEND)
   - SPI frame: 3 byte control + 1 byte data
   - Tempo @ 40MHz: ~1 µs

#### Wait Transmission Complete (TCP ACK)
4. **Poll Sn_IR** fino a SEND_OK
   - Tipicamente 1-3 poll
   - ~3 µs per poll × 2 = ~6 µs

**Totale W5500 TX: 5 + 1.5 + 1 + 6 = ~13.5 µs**

### TCP Stack Overhead
- send() syscall: ~10 µs
- TCP checksum + header: ~15 µs
- **Totale TCP: ~25 µs**

**Totale Send ACK: 13.5 + 25 = ~38.5 µs**

## 4. Timeline Completa - 1 Batch da 70 Punti

```
T=0      : MadMapper invia 1263 byte TCP
T=0      : W5500 riceve packet, genera INT
T=+10 µs : ESP32 ISR interrupt handler
T=+11 µs : Legge IR register (1 µs)
T=+13 µs : Legge RX size (1.5 µs)
T=+267 µs: Legge 1263 byte RX (253 µs)
T=+269 µs: Update RX pointer (1.5 µs)
T=+271 µs: Command RECV (1 µs)
T=+326 µs: LWIP processing (55 µs)
T=+333 µs: Protocol parsing 70 punti (7 µs)
T=+334 µs: send_response() - accumula ACK
T=+372 µs: flush_responses() - SPI TX (13.5 µs)
T=+397 µs: TCP send complete (25 µs)
T=+405 µs: W5500 trasmette 22 byte ACK su wire (~8 µs @ 100Mbps)
```

### Round-Trip Time
**Totale: ~405 µs** (dall'arrivo INT a ACK inviato)

Breakdown:
- **RX dal W5500**: 259 µs (64%)
- **LWIP processing**: 55 µs (14%)
- **Protocol parsing**: 7 µs (2%)
- **TX ACK via W5500**: 39 µs (10%)
- **Overhead vari**: 45 µs (11%)

## 5. Latenza e Throughput

### MadMapper Streaming Pattern

MadMapper tipicamente:
1. Invia batch 70 punti
2. **Aspetta ACK** prima di inviare il prossimo batch
3. Regola il rate basandosi sul buffer_fullness nello status

**Latenza client-perceived**: 405 µs + network RTT
- LAN locale: +0.5 ms → **~0.9 ms totale**
- Questo limita il rate a ~1100 batch/sec

### Throughput Effettivo

**Scenario veloce** (client invia continuamente):
- 405 µs per batch
- ~2469 batch/sec
- 2469 × 70 = **~172,830 pps**

**Ma**: Il client aspetta gli ACK e controlla il buffer!

**Scenario reale** (con latenza rete):
- ~1 ms per batch (RT T completo)
- 1000 batch/sec
- 1000 × 70 = **~70,000 pps**

Perfetto per streaming @ 10-30 kpps!

## 6. Identificazione Bottleneck

### Classifica Reale

1. **W5500 SPI RX** (259 µs) - 64% ⚠️
   - Read 1263 byte @ 40MHz
   - **BOTTLENECK PRINCIPALE**

2. **LWIP processing** (55 µs) - 14%
   - TCP stack overhead
   - Non ottimizzabile facilmente

3. **W5500 SPI TX** (39 µs) - 10%
   - Write 22 byte ACK

4. **Protocol parsing** (7 µs) - 2% ✓
   - Già molto efficiente!

5. **Overhead** (45 µs) - 11%
   - Context switch, interrupts, etc.

## 7. Ottimizzazioni Realistiche

### A. Aumenta W5500 SPI Clock ★★★★★
```c
#define ETH_SPI_FREQ_HZ (40 * 1000 * 1000)  // Attuale
// → 
#define ETH_SPI_FREQ_HZ (50 * 1000 * 1000)  // +25% speed
```

**Impatto**: 259 µs → **207 µs** (-52 µs, -20% totale)
**Rischio**: Basso (W5500 supporta 80 MHz)

### B. DMA per W5500 SPI ★★★★☆
```c
// Usa SPI DMA per read/write W5500
spi_bus_config_t buscfg = {
    // ...
    .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_IOMUX_PINS,
};
// DMA_CH_AUTO in esp_eth_mac_new_w5500()
```

**Impatto**: CPU free durante transfer, -30 µs overhead
**Rischio**: Medio (richiede test)

### C. Response Pipelining ★★★☆☆
```c
// Non aspettare flush dopo OGNI recv
// Flush solo ogni N ms o quando buffer pieno
// → Riduce overhead TCP/SPI
```

**Impatto**: Minimo, client aspetta ACK comunque
**Rischio**: Basso

### D. Increase Batch Size (Client-Side) ★★★★★
Invece di 70 punti → 200-300 punti per batch
- Riduce overhead per punto
- Meno ACK/sec
- Migliore utilizzo bandwidth

**Impatto**: -30% latenza per punto
**Rischio**: Nessuno (configurazione client)

## 8. Stima con Ottimizzazioni

### Con SPI@50MHz + DMA

**Timeline ottimizzata**:
```
W5500 RX: 207 µs (-52)
LWIP: 55 µs
Parsing: 7 µs
W5500 TX: 31 µs (-8)
Overhead: 30 µs (-15)
TOTALE: ~330 µs (-75 µs, -18%)
```

**Throughput**:
- 3030 batch/sec (vs 2469)
- **212,100 pps** (vs 172,830)

**Con batch 200 punti**:
- 850 µs per batch @ 50MHz SPI
- 1176 batch/sec
- **235,200 pps** 🚀

## Conclusione Finale

**Il VERO bottleneck è il W5500 SPI transfer @ 40MHz** (non il parsing come pensavo inizialmente!).

**Raccomandazioni prioritarie**:
1. ⭐⭐⭐⭐⭐ Aumenta SPI W5500 a 50-60 MHz
2. ⭐⭐⭐⭐☆ Implementa DMA per SPI W5500
3. ⭐⭐⭐☆☆ Aumenta batch size client-side (se possibile)

Il parsing è già ottimo (~7 µs per 70 punti). Non c'è bisogno di ottimizzarlo ulteriormente.

## Architettura Flusso Dati

```
Client → W5500 (SPI@40MHz) → ESP32 LWIP → etherdream_server → frame_buffer → dac_timer → DAC80508 (SPI@50MHz)
```

## 1. Ricezione Dati da Ether Dream (W5500)

### Configurazione W5500
- **SPI Bus**: SPI2_HOST @ 40 MHz
- **Modalità**: Interrupt-driven (GPIO 8)
- **Driver**: ESP-IDF esp_eth con W5500 MAC+PHY
- **Link**: 100 Mbps Full Duplex
- **TCP**: NODELAY attivo, RCVBUF 65KB

### Formato Dati Ether Dream
- **Punto**: 18 byte (control:2 + x:2 + y:2 + r:2 + g:2 + b:2 + i:2 + u1:2 + u2:2)
- **Comando DATA**: 3 byte header (cmd:1 + npoints:2) + N*18 byte
- **Response ACK**: 22 byte (resp:1 + cmd:1 + status:20)
- **Buffer RX**: 18,019 byte (3 + 1000*18 + 16)

### Transazioni SPI W5500 per Ricezione

Per ogni batch di dati TCP:

1. **Interrupt W5500** → ESP32 legge registro interrupt (1 SPI tx)
2. **Legge RX size** (2 SPI tx - 16-bit read)
3. **Legge dati TCP** da W5500 RX buffer:
   - Header SPI: 3 byte (control + addr)
   - Payload: N byte
   - **Totale**: ~4-5 SPI transactions per batch TCP

**Esempio batch 1000 punti (18,003 byte)**:
- Interrupt check: 1 SPI tx (~1 µs)
- Size read: 2 SPI tx (~2 µs)
- Data read: potenzialmente suddivisa in chunk da 2-4KB ciascuno
  - A 40 MHz: ~450 µs per 18KB (40 bit/µs → 5 byte/µs)
  - Overhead frame SPI: ~5-10 µs per chunk
- **Totale stimato**: ~500-600 µs per 1000 punti

### Overhead TCP/IP Stack
- **LWIP processing**: ~50-100 µs per packet
- **Context switch**: ~10-20 µs
- **Totale overhead rete**: ~100-200 µs per batch

## 2. Elaborazione Ether Dream Protocol

### Parser Efficienza
- **Zero-copy parsing**: cast diretto dal buffer TCP
- **Batch processing**: 512 punti alla volta nel frame_buffer
- **State machine**: evita memcpy inutili

```c
// BULK: process complete points directly from recv buffer
for (size_t i = 0; i < complete_points; i++) {
    etherdream_point_t* ep = (etherdream_point_t*)&data[pos];
    add_point_to_batch(ep);  // Conversione diretta
    pos += 18;
}
```

**Tempo per 1000 punti**:
- Conversione formato: ~2-3 µs per punto → ~2500 µs totale
- Write frame_buffer (atomic): ~50-100 µs
- **Totale parsing**: ~2600 µs

### Response ACK Transmission

**Batching Responses**:
- Accumula fino a 16 response prima di flush
- 1 send() invece di N send() → riduce SPI overhead

**Per ACK (22 byte)**:
- SPI W5500 write: ~5 µs (header + data)
- TCP processing: ~20 µs
- **Totale**: ~25 µs per ACK

## 3. Output DAC (DAC80508)

### Configurazione SPI DAC
- **SPI Bus**: SPI3_HOST @ 50 MHz
- **Modalità**: Direct register access (bypass driver)
- **CS**: Manual control per minima latenza

### Output Point Timing

**6 transazioni SPI per punto**:
1. Write X (24 bit) → ~1.5 µs
2. Write Y (24 bit) → ~1.5 µs  
3. Write R (24 bit) → ~1.5 µs
4. Write G (24 bit) → ~1.5 µs
5. Write B (24 bit) → ~1.5 µs
6. Write TRIGGER (24 bit) → ~1.5 µs

**Totale**: ~9 µs per punto (misurato ~6 µs ottimizzato)

### GPTimer ISR Frequency
- Timer fires a scan_rate Hz (default 10kHz)
- Ogni 100 µs @ 10kpps
- ISR esegue in ~6-9 µs → **margine: 91-94 µs**

## 4. Stima Throughput Totale

### Scenario: Client invia 1000 punti

**Timeline**:

1. **TCP RX da W5500**: 500-600 µs (SPI + LWIP)
2. **Protocol parsing**: 2600 µs (conversione + buffer write)
3. **ACK response**: 25 µs (SPI write W5500)

**Totale round-trip**: ~3100-3200 µs per 1000 punti

### Points Per Second (PPS) Limit

**Batch size ottimale**: 1000 punti
- Round-trip time: ~3.2 ms
- Frequency: ~312 batch/sec
- **Max throughput**: 312,000 punti/sec

**Limite teorico**:
- W5500 @ 100 Mbps: ~5.5 MB/s payload
- 18 byte/punto: ~300k punti/sec
- **Bottleneck**: Protocol parsing (~2.5-3 ms per 1000 punti)

### Confronto con Limiti Hardware

**DAC output**: 100 kpps max (100 µs/punto se consecutive)
**Buffer**: 8192 punti → 819 ms @ 10kpps, 81.9 ms @ 100kpps

**Conclusione**: Il sistema può ricevere **~300k pps** via rete, bufferizzare 8192 punti, e outputtare fino a **100k pps** al DAC.

## 5. Ottimizzazioni Possibili

### Già Implementate ✓
1. **Direct SPI access** (DAC) - bypass driver overhead
2. **Zero-copy parsing** - cast diretto buffer TCP
3. **Response batching** - accumula ACK prima di flush
4. **Interrupt W5500** - latenza minima
5. **TCP_NODELAY** - no Nagle algorithm
6. **Large RX buffer** (65KB) - riduce frammentazione

### Ulteriori Ottimizzazioni Possibili

#### A. Reduce Protocol Overhead (~30% gain)
```c
// Batch multiple data commands in one TCP packet
// Client side: concatena 5-10 CMD_DATA back-to-back
// → Riduce ACK overhead da 25µs/batch a 25µs/5batch = 5µs/batch
```

#### B. DMA per W5500 SPI (~20% gain)
```c
// Attualmente: polling SPI transactions
// Con DMA: CPU-free transfer durante RX da W5500
// Risparmio: ~100-150µs per 18KB
```

#### C. Ottimizza Point Conversion (~40% gain)
```c
// Current: ~2.5µs per punto per conversione
// Optimization: SIMD-like processing o lookup table
static inline void convert_point_fast(const etherdream_point_t* ep, laser_point_t* lp) {
    // Use uint32_t operations instead of 16-bit
    *(uint32_t*)&lp->x = *(uint32_t*)&ep->x;  // Copy x,y at once
    *(uint32_t*)&lp->r = *(uint32_t*)&ep->r;  // Copy r,g at once
    lp->b = ep->b;
    lp->flags = (ep->r | ep->g | ep->b | ep->i) ? 0 : POINT_FLAG_BLANK;
}
// Stima: ~1µs per punto → 1000µs invece di 2500µs
```

#### D. Parallel SPI Buses
```c
// W5500 su SPI2 @ 40MHz
// DAC su SPI3 @ 50MHz
// → Già paralleli! ✓ Nessun contention
```

### Stima Miglioramento Totale
- **Attuale**: ~312k pps network throughput
- **Con ottimizzazioni A+C**: ~550k pps (~75% faster)
- **Limite pratico**: Parsing sempre > DAC output time

## 6. Bottleneck Analysis

### Classifica Colli di Bottiglia

1. **Protocol Parsing** (2600 µs) - 81% del tempo
   - Conversione formato: 2500 µs
   - Buffer write: 100 µs

2. **W5500 RX** (500 µs) - 16% del tempo
   - SPI transfer: 450 µs
   - Overhead: 50 µs

3. **ACK Response** (25 µs) - 1% del tempo

4. **DAC Output** (non limita) - Run in parallel via ISR

### Conclusione Finale

**Il sistema è CPU-bound sul parsing del protocollo**, non I/O-bound sulle SPI.

**Raccomandazione**: Ottimizzare la funzione `convert_point_fast()` usando operazioni word-size e riducendo i branch può portare un guadagno del 40-75% nel throughput totale.

Il W5500 con interrupt è già molto efficiente e non rappresenta il bottleneck principale.
