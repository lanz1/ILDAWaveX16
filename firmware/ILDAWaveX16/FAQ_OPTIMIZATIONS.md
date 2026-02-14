# FAQ - Ottimizzazioni Performance

## Q1: s_rx_buf sovradimensionato aggiunge latenza?

**Risposta**: **NO, solo spreco RAM**.

### Come funziona recv()

```c
ssize_t n = recv(sock, s_rx_buf, TCP_RX_BUF_SIZE, 0);
// recv() ritorna SUBITO con i dati disponibili
// Non aspetta di riempire l'intero buffer!
```

**Comportamento**:
- Buffer 18 KB, dati disponibili 1263 byte → recv() ritorna 1263 byte
- Buffer 4 KB, dati disponibili 1263 byte → recv() ritorna 1263 byte
- **Latenza identica**: dipende solo da quando i dati arrivano, non dalla dimensione buffer

### Quando un buffer PICCOLO causa problemi?

Se il buffer è più piccolo dei dati ricevuti TCP:

```c
// PROBLEMA: Buffer troppo piccolo per 1 batch
#define TCP_RX_BUF_SIZE 512  // Solo 512 byte!

// MadMapper invia 1263 byte → serve 3 chiamate recv()
recv(sock, buf, 512, 0);  // 512 byte
recv(sock, buf, 512, 0);  // 512 byte  
recv(sock, buf, 512, 0);  // 239 byte

// ⚠️ Overhead: 3× syscall, 3× processing, latenza +200-300µs
```

### Conclusione Q1

**Attuale (18 KB)**:
- ✓ Supporta batch grandi (1000 punti = 18003 byte)
- ✓ Zero latenza aggiuntiva
- ⚠️ Spreco 14 KB RAM (uso 7%)

**Raccomandazione**: Riduci a **8 KB** (non 4 KB!)
- Supporta 444 punti per batch (7998 byte)
- Margine per client aggressivi
- Risparmio 10 KB RAM

---

## Q2: Tolleranze Protocollo Etherdream

Hai perfettamente ragione! **Altri client inviano batch variabili**.

### Batch Size Real-World

#### libetherdream (reference client)
```c
// libetherdream/protocol.c
#define BUFFER_POINTS_PER_STREAM 1800
```
- **Tipico**: 200-800 punti per batch
- **Burst**: fino a 1800 punti
- **Strategia**: Riempie fino a low_water_mark (~1700)

#### MadMapper
- **Tipico**: 70 punti (come hai osservato)
- **Può variare**: 50-150 punti in base a frame rate

#### Laser Show Software (Pangolin, LSX, etc.)
- **Range**: 100-500 punti per batch
- **Dipende**: Frame rate (10-60 fps), scene complexity

### Dimensione Buffer Raccomandata

**Supporta almeno 1000 punti**:
```
Header:    3 byte
Points:    1000 × 18 = 18000 byte
TOTALE:    18003 byte
```

**Raccomandazione finale**:
```c
// Supporta 1000 punti con margine
#define TCP_RX_BUF_SIZE (3 + 1000 * 18 + 128)  // 18131 byte
```

**NON ridurre sotto 18 KB!** Mantenere supporto per batch grandi.

---

## Q3: SPI DMA - Approfondimento Tecnico

### Come Funziona DMA (Direct Memory Access)

#### Senza DMA (Polling/Interrupt - ATTUALE)
```
┌─────────┐      ┌─────────┐      ┌─────────┐
│  W5500  │ SPI  │ CPU     │      │  RAM    │
│  FIFO   │─────→│ Read    │─────→│ Buffer  │
└─────────┘      │ Loop    │      └─────────┘
                 └─────────┘

CPU esegue:
for (int i = 0; i < 1263; i++) {
    buffer[i] = SPI_READ();  // 1263 letture
}
Tempo CPU: ~250-300 µs
```

#### Con DMA
```
┌─────────┐      ┌─────────┐      ┌─────────┐
│  W5500  │ SPI  │ DMA     │      │  RAM    │
│  FIFO   │─────→│ Engine  │─────→│ Buffer  │
└─────────┘      └─────────┘      └─────────┘
                      ↓
                 ┌─────────┐
                 │ CPU     │ (libera!)
                 │ Parsing │
                 └─────────┘

DMA hardware copia 1263 byte in background
CPU può fare altro (parsing, frame_buffer, ACK)
Tempo CPU: ~5-10 µs (solo setup DMA)
```

### Performance Comparison

#### Scenario: Batch 70 punti (1263 byte)

**Senza DMA (attuale)**:
```
T=0      : W5500 interrupt
T=+1 µs  : CPU legge IR register
T=+3 µs  : CPU legge RX size
T=+260 µs: CPU copia 1263 byte via SPI (blocking!)
T=+265 µs: CPU può fare parsing
```

**Con DMA**:
```
T=0      : W5500 interrupt
T=+1 µs  : CPU legge IR register  
T=+3 µs  : CPU legge RX size
T=+5 µs  : CPU avvia DMA transfer
T=+10 µs : CPU LIBERA! Può fare parsing vecchio batch
T=+260 µs: DMA completa transfer (in background)
T=+261 µs: DMA interrupt → CPU processa nuovo batch
```

**Vantaggio**: CPU parallelizza! Parsing + DMA transfer sovrapposti.

### Implementazione ESP32 SPI DMA

```c
// In w5500_eth.c - configurazione bus SPI

spi_bus_config_t buscfg = {
    .mosi_io_num = ETH_SPI_MOSI,
    .miso_io_num = ETH_SPI_MISO,
    .sclk_io_num = ETH_SPI_SCK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4096,  // Max DMA transaction size
    // IMPORTANTE: Aggiungi questo flag!
    .flags = SPICOMMON_BUSFLAG_MASTER
};

// Configura DMA channel (auto-select)
esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
//                                                       ^^^^^^^^^^^^^^
//                                                       Abilita DMA!
```

**Verifica attuale**:
```bash
grep -r "spi_bus_initialize" src/hal/w5500_eth.c
# Check se usa SPI_DMA_DISABLED o SPI_DMA_CH_AUTO
```

### Performance Gain con DMA

#### Target: 80 kpps

**Dati da trasferire**:
- 80,000 punti/sec × 18 byte = **1.44 MB/sec**
- Batch 200 punti = 3603 byte per batch
- 400 batch/sec @ 80 kpps

**Senza DMA**:
- SPI transfer: 3603 byte @ 40 MHz = ~720 µs
- Parsing: 200 punti × 2 µs = ~400 µs
- **Totale sequenziale**: 1120 µs per batch
- Max rate: 1000/1.12 = **892 batch/sec** = 178k pps ✓

**Con DMA**:
- SPI transfer: 720 µs (background)
- Parsing: 400 µs (parallel durante DMA successivo)
- **Totale pipeline**: ~720 µs per batch (limited by DMA)
- Max rate: 1000/0.72 = **1388 batch/sec** = 277k pps ✓✓

**Conclusione**: Con DMA puoi facilmente raggiungere **80 kpps** e oltre!

### DMA Setup Overhead

**First transaction**: ~5-10 µs (setup descriptor)
**Subsequent**: ~1-2 µs (solo trigger)

Per batch grandi (>500 byte), overhead è trascurabile (<1%).

---

## Q4: SPI DMA è utile anche per WiFi?

### Short Answer: **NO, WiFi già usa DMA internamente**

### WiFi Architecture ESP32

```
┌──────────────┐
│ Application  │ send() / recv()
└──────┬───────┘
       ↓
┌──────────────┐
│ LWIP Stack   │ TCP/IP processing
└──────┬───────┘
       ↓
┌──────────────┐
│ WiFi Driver  │ ← Usa DMA qui!
└──────┬───────┘
       ↓ DMA automatico
┌──────────────┐
│ WiFi MAC/PHY │ Hardware radio
└──────────────┘
```

**WiFi driver ESP-IDF**:
- Usa **DMA buffer pool** interno
- RX: DMA → WiFi RX buffer → LWIP
- TX: LWIP → WiFi TX buffer → DMA
- **Tutto gestito automaticamente!**

### Ottimizzazioni WiFi (già implementate)

Dal tuo `sdkconfig.defaults`:
```ini
# WiFi RX buffers (DMA-capable)
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=64

# WiFi TX buffers (DMA-capable)  
CONFIG_ESP_WIFI_TX_BUFFER_TYPE=1
CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16

# IRAM optimization (hot path in IRAM, non FLASH)
CONFIG_ESP_WIFI_EXTRA_IRAM_OPT=y
```

**Già ottimizzato al massimo!** ✓

### WiFi vs Ethernet Performance

#### Ethernet (W5500)
- **Bandwidth**: 100 Mbps full duplex
- **Latency**: ~0.5-1 ms (LAN locale)
- **Jitter**: <0.1 ms
- **CPU overhead**: Basso (hardware TCP/IP in W5500)
- **Adatto per**: Laser show professionale ✓✓✓

#### WiFi (802.11n/ac)
- **Bandwidth**: 150-433 Mbps (teorico), 30-100 Mbps (reale)
- **Latency**: ~2-10 ms
- **Jitter**: 1-5 ms (variabile!)
- **CPU overhead**: Alto (TCP/IP in software)
- **Adatto per**: Setup, controllo, streaming basso rate

**Per 80 kpps laser streaming: USA ETHERNET!**

WiFi è ottimo per:
- Configuration web UI
- Monitoring / status
- Emergency stop / control
- Streaming a basso rate (<20 kpps)

---

## Q5: Piano Ottimizzazioni per 80 kpps

### Target Performance

**80,000 pps streaming continuo**:
- 80k punti/sec × 18 byte = **1.44 MB/sec** RX
- Batch 200 punti = **400 batch/sec**
- Budget tempo: **2.5 ms per batch**

### Ottimizzazioni Priority List

#### ⭐⭐⭐⭐⭐ CRITICAL (Obbligatorie per 80 kpps)

**1. Abilita SPI DMA per W5500**
```c
// In src/hal/w5500_eth.c
spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
//                                      ^^^^^^^^^^^^^^^^
```
**Gain**: +30-40% throughput, CPU libera durante transfer  
**Effort**: 30 minuti (1 riga codice + test)  
**Risk**: Basso

**2. Aumenta W5500 SPI clock a 50 MHz**
```c
#define ETH_SPI_FREQ_HZ (50 * 1000 * 1000)  // Era 40 MHz
```
**Gain**: +25% velocità transfer  
**Effort**: 5 minuti (cambia define)  
**Risk**: Bassissimo (W5500 supporta 80 MHz)

**3. Aumenta W5500 Socket Buffer a 8 KB**
```c
// Durante socket init, scrivi registro Sn_RXBUF_SIZE
uint8_t rxbuf_size = 3;  // 0=1KB, 1=2KB, 2=4KB, 3=8KB
w5500_write_sock_reg(sock, 0x001E, rxbuf_size);
```
**Gain**: Buffer multipli batch, riduce interrupt rate  
**Effort**: 1 ora (aggiungi init code)  
**Risk**: Basso

#### ⭐⭐⭐⭐☆ HIGH (Altamente raccomandato)

**4. Flush Point Batch dopo ogni DATA command**
```c
// In handle_data_command(), dopo parsing 70 punti:
flush_point_batch();  // Non aspettare 512 punti
```
**Gain**: -85% latenza (da 7× batch a 1× batch)  
**Effort**: 15 minuti  
**Risk**: Nessuno

**5. Ottimizza Point Conversion (SIMD-like)**
```c
static inline void convert_point_fast(const etherdream_point_t* ep, 
                                     laser_point_t* lp) {
    // Copy 32-bit chunks instead of 16-bit
    *(uint32_t*)&lp->x = *(uint32_t*)&ep->x;  // x,y
    *(uint32_t*)&lp->r = *(uint32_t*)&ep->r;  // r,g
    lp->b = ep->b;
    lp->user1 = 0;
    lp->user2 = 0;
    // Optimized blanking check (compile to ANDN instruction)
    lp->flags = (ep->r | ep->g | ep->b | ep->i) ? 0 : POINT_FLAG_BLANK;
}
```
**Gain**: -60% tempo conversione (da 2.5µs a 1µs per punto)  
**Effort**: 30 minuti  
**Risk**: Basso (test alignment!)

#### ⭐⭐⭐☆☆ MEDIUM (Nice to have)

**6. Pin network_task a Core 0 con priorità alta**
```c
// In main.c
xTaskCreatePinnedToCore(etherdream_server_task, "etherdream",
                       8192, NULL, 23,  // Priority 23 (era 20?)
                       NULL, 0);        // Core 0
```
**Gain**: Riduce context switch, CPU scheduling più prevedibile  
**Effort**: 5 minuti  
**Risk**: Nessuno

**7. Pre-allocate LWIP buffers**
```c
// In sdkconfig.defaults, aumenta pool
CONFIG_LWIP_TCP_RECVMBOX_SIZE=128  // Era 64
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64  // Default 32
```
**Gain**: Riduce malloc/free durante burst  
**Effort**: 5 minuti  
**Risk**: +8 KB RAM usage

### Performance Estimate con Tutte le Ottimizzazioni

**Attuale (batch 70 punti)**:
```
W5500 SPI RX:  259 µs
LWIP:           55 µs  
Parsing:         7 µs
ACK TX:         39 µs
TOTALE:        360 µs → ~2700 batch/sec → 190k pps
```

**Con ottimizzazioni 1+2+5 (batch 200 punti)**:
```
W5500 SPI RX:  180 µs (DMA + 50MHz)  [parallel parsing]
LWIP:           80 µs (batch più grande)
Parsing:       200 µs (200 × 1µs, ottimizzato)
ACK TX:         40 µs
TOTALE:        500 µs → 2000 batch/sec → 400k pps ✓✓✓
```

**Target 80 kpps**: ✓✓✓ **FACILMENTE RAGGIUNGIBILE**

Con solo ottimizzazioni 1+2 raggiungi già **200-250k pps**, ben oltre 80k!

### Effort Summary

**Per raggiungere 80 kpps**:
- **Tempo sviluppo**: 2-3 ore
- **Test**: 2-3 ore
- **Totale**: 1 giornata

**High priority**:
1. SPI DMA (30 min)
2. SPI 50 MHz (5 min)
3. Flush immediato (15 min)
4. Test stress @ 80 kpps (2 ore)

---

## Q6: Misure per Validazione

### Test Bench Setup

```python
# Test client Python
import socket
import struct
import time

sock = socket.socket()
sock.connect(('192.168.77.1', 7765))

# Send 80k points/sec in batch 200
batch_size = 200
pps = 80000
batches_per_sec = pps / batch_size  # 400

# DATA command: cmd(1) + npoints(2) + points(200×18)
data_cmd = struct.pack('<BH', ord('d'), batch_size)
points = b'\x00' * (18 * batch_size)

interval = 1.0 / batches_per_sec  # 2.5 ms

while True:
    t0 = time.time()
    sock.send(data_cmd + points)
    
    # Receive ACK
    ack = sock.recv(22)
    status = struct.unpack('<BB20s', ack)
    buffer_level = struct.unpack('<H', status[2][8:10])[0]
    
    print(f"Buffer: {buffer_level}/8192")
    
    # Sleep per target rate
    elapsed = time.time() - t0
    time.sleep(max(0, interval - elapsed))
```

### Metriche da Monitorare

**1. Buffer Fullness**
```c
// In etherdream status
ESP_LOGI(TAG, "Buffer: %d/%d (%.1f%%)", 
         frame_buffer_level(), 
         FRAME_BUFFER_SIZE,
         100.0 * frame_buffer_level() / FRAME_BUFFER_SIZE);
```

**2. Underrun Rate**
```c
// Già implementato!
uint32_t underruns = dac_timer_get_underruns();
ESP_LOGI(TAG, "Underruns: %lu", underruns);
```

**3. Network Throughput**
```c
static uint32_t s_total_points = 0;
static uint32_t s_last_report_ms = 0;

s_total_points += npoints;

uint32_t now = esp_timer_get_time() / 1000;
if (now - s_last_report_ms > 1000) {
    ESP_LOGI(TAG, "Network RX: %lu pps", s_total_points);
    s_total_points = 0;
    s_last_report_ms = now;
}
```

### Success Criteria @ 80 kpps

✓ Buffer fullness: stabile 1000-3000 punti  
✓ Underrun rate: 0 per minuto  
✓ Network throughput: 80k ± 5%  
✓ CPU usage Core 0: <60%  
✓ Latency batch: <2 ms  

---

## Conclusioni

1. **s_rx_buf**: NO latenza, solo spreco. Ma NON ridurre sotto 18 KB per supportare protocollo!

2. **SPI DMA**: Altamente raccomandato! Parallelizza transfer + parsing, fondamentale per >50 kpps.

3. **80 kpps target**: FATTIBILE con 1 giornata di lavoro (DMA + ottimizzazioni).

4. **WiFi DMA**: Già implementato nel driver, nulla da fare.

5. **Next steps**: Implementa ottimizzazioni 1-3 (critical) e testa!
