# Root Cause Analysis: Underrun a 19.5 kpps

## Sintomi dal Log

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
```

### Core 1 (CORE_REALTIME)  
```c
dac_refill_task:
  - Priority: configMAX_PRIORITIES-2 = 23 (MOLTO ALTA)
  - Stack: 4096 byte
  - Responsabilità:
    * Legge frame_buffer
    * Scrive ISR buffer (512→1024 punti)

gptimer_isr_callback:
  - Priority: 1 (MASSIMA - ISR)
  - Frequency: 19500 Hz (ogni 51.2 µs)
  - Responsabilità:
    * Legge 1 punto dall'ISR buffer
    * 6 transazioni SPI al DAC80508 (~9 µs totale)
    * Output laser point
```

## Interrupt Allocation

### W5500 Ethernet Interrupt (GPIO 8)
```c
// In w5500_eth.c
gpio_install_isr_service(0);  // Flag 0 = default
```

**PROBLEMA CRITICO**: `gpio_install_isr_service(0)` alloca ISR sul **CORE CHE CHIAMA** la funzione!

Quindi:
- `w5500_eth_init()` chiamata da `main()` (task `app_main`)
- `app_main` gira su **Core 0** (default)
- **W5500 ISR → Core 0** ✓

### DAC Timer ISR
```c
// In dac_timer.c
gptimer_register_event_callbacks(s_timer, &cbs, NULL);
```

ESP-IDF assegna il timer ISR al **core dove il timer è stato creato**.

- `dac_timer_init()` chiamato da `app_main` su Core 0
- MA il task `dac_refill_task` è su Core 1
- **GPTimer ISR → Core 0 o Core 1?** ⚠️ DA VERIFICARE

## Timing Analysis @ 19.5 kpps

### DAC ISR (ogni 51.2 µs)
```
Tempo disponibile:  51.2 µs
Tempo ISR:          ~9 µs (6× SPI writes @ 50MHz)
CPU usage:          17.6% di un core
```

### Se ISR è su Core 1:
```
Core 1 Timeline:
T=0:     ISR trigger (9 µs)
T=9:     ISR done
T=10:    refill_task può girare (42 µs disponibili)
T=51.2:  ISR trigger di nuovo

Refill task può girare per ~42µs ogni 51µs = 82% tempo disponibile ✓
```

### Se ISR è su Core 0: ⚠️ PROBLEMA!
```
Core 0 Timeline:
T=0:     ISR DAC trigger (9 µs) - BLOCCA TUTTO
T=9:     ISR done
T=10:    network_task può girare
T=20:    W5500 interrupt arriva
T=20:    W5500 ISR (legge pacchetto) - 200-300 µs
T=320:   network_task parsing + frame_buffer write
T=400:   ACK send
T=51.2:  ISR DAC trigger di nuovo - INTERROMPE network_task!

PROBLEMA: network_task viene interrotto ogni 51µs dalla DAC ISR!
```

## Bottleneck Identificato

**Se GPTimer ISR è su Core 0** (dove gira anche network_task):

1. ISR DAC consuma 17.6% Core 0 (9µs/51µs)
2. network_task (priority 19) viene **preemptato** dall'ISR (priority 1)
3. Ogni batch 70 punti richiede ~400 µs
4. Durante questi 400 µs, ISR trigger 7-8 volte
5. network_task viene interrotto 7-8 volte per 9µs = **63µs persi**
6. Tempo effettivo: 400 + 63 = **463 µs per batch**

### Throughput Massimo con ISR su Core 0
```
463 µs/batch × 70 punti = 6.6 µs/punto
Max rate: 1000000/6.6 = 151k punti/sec teorico

MA: W5500 interrupt (ogni 1-2 batch) aggiunge altri 200µs
Quindi: ~663µs per batch in media
→ 1509 batch/sec × 70 = 105k pps teorico

In pratica @ 19.5 kpps:
- 19500/70 = 278 batch/sec richiesti
- 463µs × 278 = 128ms CPU time
- Ma con W5500 ISR overhead: ~150-180ms/sec
- **Margine stretto!** 15-20% spare capacity
```

## Verifica Allocazione ISR

Dobbiamo verificare su quale core gira il GPTimer ISR:

```c
// In dac_timer.c, aggiungi log in ISR:
static bool IRAM_ATTR timer_isr_callback(gptimer_handle_t timer, ...) {
    static uint32_t isr_count = 0;
    if (++isr_count % 10000 == 0) {
        // Questo log sarà su quale core?
        ets_printf("ISR on core: %d\n", xPortGetCoreID());
    }
    ...
}
```

## Soluzione: Pin GPTimer ISR a Core 1

### Opzione 1: Creare Timer su Core 1

```c
// In dac_timer_init(), PRIMA di creare il timer:
// Sposta la creazione del timer dentro una funzione chiamata da Core 1

static void timer_init_on_core1(void* arg) {
    gptimer_config_t timer_cfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,  // 1 MHz
    };
    gptimer_new_timer(&timer_cfg, &s_timer);
    
    // Register callback - ISR will be on Core 1!
    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_isr_callback,
    };
    gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    
    xSemaphoreGive(init_done);
    vTaskDelete(NULL);
}

esp_err_t dac_timer_init(void) {
    SemaphoreHandle_t init_done = xSemaphoreCreateBinary();
    
    // Create temporary task on Core 1 to initialize timer
    xTaskCreatePinnedToCore(timer_init_on_core1, "timer_init", 
                           2048, NULL, 20, NULL, CORE_REALTIME);
    
    xSemaphoreTake(init_done, portMAX_DELAY);
    vSemaphoreDelete(init_done);
    
    // Continue with refill_task creation...
}
```

### Opzione 2: Usare ESP-IDF Timer Groups (già su Core 1)

Timer Groups sono allocati per core:
- TIMER_GROUP_0 → Core 0
- TIMER_GROUP_1 → Core 1 (se disponibile)

Ma ESP32-S3 ha solo un Timer Group con 2 timer, condivisi.

### Opzione 3: Aumentare Priority network_task

```c
// In main.c
#define TASK_PRIORITY_EDREAM 24  // Era 19, ora più alta di refill (23)
```

**NO!** Questo farebbe sì che network_task preempti refill_task, causando underrun ISR buffer.

### Opzione 4: CORRETTA - Ridurre Overhead ISR

Il vero problema è che ISR fa **6 transazioni SPI busy-wait** ogni 51µs.

```c
// In dac80508.c - dac_output_point()
void dac_output_point(const laser_point_t* pt) {
    // Questo viene chiamato dall'ISR!
    dac_write_direct(DAC_CHANNEL_X, pt->x);  // ~1.5µs busy-wait
    dac_write_direct(DAC_CHANNEL_Y, pt->y);  // ~1.5µs
    dac_write_direct(DAC_CHANNEL_R, pt->r);  // ~1.5µs
    dac_write_direct(DAC_CHANNEL_G, pt->g);  // ~1.5µs
    dac_write_direct(DAC_CHANNEL_B, pt->b);  // ~1.5µs
    dac_write_direct(DAC_CHANNEL_TRIGGER, 0xFFFF);  // ~1.5µs
    // TOTALE: ~9µs di BUSY WAIT in ISR context!
}
```

## Soluzione Finale

### A. Pin GPTimer a Core 1 (Opzione 1)

Questo separa completamente:
- Core 0: network (W5500 ISR + network_task)
- Core 1: realtime (DAC ISR + refill_task)

### B. Aumentare ISR Buffer Size (GIÀ FATTO)

```c
#define ISR_BUFFER_SIZE 1024  // Era 512
#define REFILL_THRESHOLD 128  // Era 64
```

Questo dà più margine al refill_task.

### C. Ottimizzare DAC SPI Transactions

Invece di 6 transazioni da 24 bit, fare **1 transazione multi-write**:

```c
// DAC80508 supporta auto-increment mode
void dac_output_point_fast(const laser_point_t* pt) {
    uint8_t tx_buf[18];  // 6 channels × 3 byte
    
    // Pack all channels
    tx_buf[0] = (DAC_CHANNEL_X << 4) | 0x0F;  // Auto-increment ON
    tx_buf[1] = pt->x >> 8;
    tx_buf[2] = pt->x & 0xFF;
    
    tx_buf[3] = pt->y >> 8;
    tx_buf[4] = pt->y & 0xFF;
    // ... pack r,g,b,trigger
    
    // Single SPI transaction!
    spi_transaction_t t = {
        .length = 18 * 8,
        .tx_buffer = tx_buf,
    };
    spi_device_transmit(s_spi, &t);  // ~3µs invece di 9µs!
}
```

**Guadagno**: 9µs → 3µs = **-66% ISR time!**

### D. Aumentare Network Task Priority (Opzione Sicura)

```c
#define TASK_PRIORITY_EDREAM 21  // Tra refill(23) e default(1-19)
```

Questo permette a network_task di avere più CPU quando refill_task è idle.

## Test Plan

1. **Verificare Core ISR**:
   ```c
   // In timer_isr_callback, aggiungi:
   ets_printf("C%d\n", xPortGetCoreID());
   ```

2. **Implementare Opzione A** (Pin Timer a Core 1)

3. **Testare @ 19.5 kpps**:
   - Buffer dovrebbe salire a 2000-4000 punti
   - Underrun = 0
   - Client stabile (no disconnect)

4. **Se ancora problemi, implementare Opzione C** (SPI multi-write)

## Expected Results

**Prima** (ISR su Core 0):
```
Buffer: 138-466 punti (1-5%)
Underruns: +6000/sec
Client: Disconnette ogni 1-2 sec
```

**Dopo** (ISR su Core 1):
```
Buffer: 2000-4000 punti (25-50%)  
Underruns: 0
Client: Stabile, no disconnect
```

## Conclusione: PROBLEMA IDENTIFICATO!

### Il GPTimer ISR è GIÀ su Core 1! ✓

Il codice attuale **crea già il GPTimer dentro `dac_refill_task`** (Core 1), quindi l'ISR del DAC gira su Core 1 come dovrebbe.

### Il VERO Problema: W5500 Ethernet Interrupt Overhead

#### Architettura ESP32 W5500:

```c
// In w5500_eth.c
gpio_install_isr_service(0);  // → ISR allocato su Core 0
gpio_isr_handler_add(GPIO_NUM_8, eth_w5500_isr, ...);
```

**L'ISR W5500 gira su Core 0** e deve:

1. **Leggere interrupt flags** via SPI (~10µs)
2. **Leggere packet size register** via SPI (~10µs)  
3. **DMA transfer del pacchetto** da W5500 FIFO (~200-300µs per 1500 byte @ 50MHz)
4. **Passare pacchetto a LWIP** (copia + scheduling)

**Tempo totale ISR**: 200-350µs per pacchetto Ethernet!

#### Timeline @ 19.5 kpps con MadMapper

MadMapper invia batch di ~70 punti ogni ~3.6ms:
- 70 punti × 18 byte = 1260 byte TCP payload
- + TCP/IP headers = ~1340 byte Ethernet frame

```
T=0:       W5500 riceve pacchetto Ethernet (1340 byte)
T=0:       W5500 trigger interrupt (GPIO 8)
T=0:       Core 0 entra in W5500 ISR (BLOCCA TUTTO SU CORE 0!)
T=0-250:   ISR legge pacchetto via DMA SPI (250µs)
T=250:     ISR passa pacchetto a LWIP e esce
T=250:     LWIP tcpip_task (priority 18) processa TCP
T=300:     network_task (priority 19) può fare recv()
T=350:     Parsing 70 punti (~70µs)
T=420:     frame_buffer_write() (~10µs)
T=430:     Torna a select()
T=3600:    Prossimo batch da MadMapper
```

**Ma durante i 250µs di W5500 ISR, Core 0 è BLOCCATO!**

#### Impact sul System

@ 19500 pps:
- 19500 punti/sec ÷ 70 punti/batch = **278 batch/sec**
- 278 batch/sec × 250µs ISR = **69.5ms/sec** in W5500 ISR (**7% Core 0**)

**Ma il problema è la LATENCY:**

Ogni 3.6ms:
- 250µs in ISR W5500 (Core 0 blocked)
- 50µs LWIP processing
- 100µs network_task processing
- **400µs processing time per batch**

Questo lascia solo **3.2ms idle** prima del prossimo batch.

#### Perché il Buffer Non Si Riempie

Dal log:
```
I (25088) EDREAM: RX: 20166 pps | 2906 kbps | Buf: 138/8192 (1%) | Underruns: 126134
```

**Il problema è il FEEDBACK LOOP negativo:**

1. Buffer basso (138 punti) → **MadMapper vede buffer basso nel broadcast**
2. MadMapper **rallenta** invio o invia burst (invece di steady stream)
3. Sistema non riceve dati steady → **buffer continua basso**
4. DAC ISR consuma punti @ 19500 pps costante → **underrun**
5. Underrun incrementano → **MadMapper si disconnette** (vede troppi underrun)
6. Client reconnect → **ciclo ripete**

#### La Vera Causa: Network Task Non Riceve Dati Abbastanza Velocemente

Il problema **NON** è CPU overhead (7% è OK), ma **JITTER** causato da:

**A. W5500 ISR Blocking (250µs ogni 3.6ms)**
- Durante ISR, `network_task` NON può girare
- TCP ACK ritardato → MadMapper pensa che rete sia congestionata
- MadMapper applica TCP congestion control → **rallenta invio**

**B. select() Timeout Troppo Alto**

```c
// In etherdream_server_loop()
uint32_t timeout_us = (s_playback_state == PLAYBACK_PLAYING) ? 1000 : 50000;
struct timeval tv = { .tv_sec = 0, .tv_usec = timeout_us };
```

**1ms timeout** significa che `network_task` dorme per 1ms se non ci sono dati.

Ma @ 19.5kpps con batch 70:
- 278 batch/sec = **1 batch ogni 3.6ms**
- Se `select()` timeout @ T=1ms, prossimo batch arriva @ T=3.6ms
- network_task si sveglia @ T=1ms, non ci sono dati, **dorme altri 1ms**
- Batch arriva @ T=3.6ms, network_task dorme fino @ T=2ms
- **Latency aggiunta: fino a 1ms!**

**C. LWIP Priority Troppo Bassa**

```
LWIP tcpip_task: priority 18 (default ESP-IDF)
network_task:    priority 19
```

`network_task` può preemptare `tcpip_task`! Ma `tcpip_task` DEVE processare TCP prima che `network_task` possa fare `recv()`.

**Sequenza sbagliata:**
1. W5500 ISR passa pacchetto a LWIP
2. tcpip_task (pri 18) scheduled
3. network_task (pri 19) preempts tcpip_task!
4. network_task fa `recv()` → **NO DATA** (tcpip_task non ha ancora processato!)
5. network_task fa `select(1ms)` → dorme
6. tcpip_task finalmente gira, processa TCP
7. network_task si sveglia dopo 1ms → legge dati

**Latency inutile: fino a 1ms per batch!**

---

## Soluzione Multi-Step

### Fix 1: Ridurre select() Timeout (CRITICO!)

```c
// In etherdream_server_loop()
uint32_t timeout_us = (s_playback_state == PLAYBACK_PLAYING) ? 100 : 50000;
//                                                               ^^^^ 1ms → 100µs!
```

**Rationale:**
- @ 19.5kpps, punti consumati ogni 51µs
- Batch ogni 3.6ms
- Timeout 100µs = **20× più veloce**, ma ancora abbastanza lungo da non busy-loop
- Riduce latency max da 1ms → 100µs

### Fix 2: Aumentare LWIP Priority (CRITICO!)

Modificare `sdkconfig.defaults`:

```ini
# LWIP Configuration
CONFIG_LWIP_TCPIP_TASK_PRIO=20
```

O in codice (se possibile):

```c
// In w5500_eth.c o main.c, prima di inizializzare LWIP
#define TCPIP_THREAD_PRIO 20  // Più alta di network_task (19)
```

**Rationale:**
- LWIP DEVE processare TCP prima che network_task faccia recv()
- Priorità: LWIP (20) > network_task (19)
- Assicura che tcpip_task non sia mai preemptato da network_task

### Fix 3: Pin W5500 ISR Work a Core 0 (già OK)

Il W5500 ISR è già su Core 0, quindi OK.

### Fix 4: Ottimizzare W5500 DMA Transfer (OPZIONALE)

Se W5500 ISR prende 250µs per leggere 1340 byte @ 50MHz SPI:
- 1340 byte × 8 bit = 10720 bit
- @ 50MHz: 10720 / 50M = **214µs** (teorico)
- Overhead: 250µs - 214µs = **36µs** (accettabile)

Nessuna ottimizzazione necessaria qui.

### Fix 5: Aumentare ISR Buffer Size (GIÀ FATTO)

```c
#define ISR_BUFFER_SIZE 1024  // OK
#define REFILL_THRESHOLD 128  // OK
```

---

## Expected Results

**Dopo Fix 1 + Fix 2:**

```
Buffer: 2000-4000 punti (25-50%) ✓
RX rate: 19500 pps steady (no jitter) ✓
Underruns: 0 ✓
Client: Stabile, no disconnect ✓
```

**Cycle time per batch:**
```
T=0:       W5500 ISR (250µs)
T=250:     LWIP task (pri 20) processa TCP (50µs)
T=300:     network_task (pri 19) recv() + parse (100µs)
T=400:     DONE, torna a select(100µs timeout)
T=500:     Timeout, check di nuovo
T=600:     Timeout, check di nuovo
...
T=3600:    Prossimo batch
```

**Total processing: 400µs**
**Idle time: 3200µs**
**Spare capacity: 3200/3600 = 89%** ✓

---

## Conclusione Finale

### Root Cause: NETWORK LATENCY, non CPU Overhead!

1. ❌ **WRONG**: DAC ISR blocca network (ISR è su Core 1, network su Core 0)
2. ❌ **WRONG**: CPU overhead troppo alto (solo 7% W5500 + 11% processing = 18%)
3. ✅ **CORRECT**: Network latency causata da:
   - `select()` timeout troppo alto (1ms)
   - LWIP priority troppo bassa (preemptata da network_task)
   - TCP congestion control di MadMapper (vede ACK ritardati)

### Fix Applicare (in ordine):

1. **CRITICO**: `select()` timeout 1ms → 100µs
2. **CRITICO**: LWIP priority 18 → 20
3. **Verifica**: Testare @ 19.5kpps per 10 minuti
4. **Se ancora problemi**: Analizzare TCP window size e buffering
