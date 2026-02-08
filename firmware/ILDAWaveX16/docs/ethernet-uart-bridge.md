# Ethernet UART Bridge - Pin Configuration

## Pin Mapping Stanley ILDAWaveX16

```cpp
#define PIN_BTN 0      // Boot button (anche USB Serial)
#define PIN_LED 7      // NeoPixel LED
#define PIN_Shutter 8  // Shutter control
#define PIN_MISO 9     // SPI MISO (DAC)
#define PIN_CS 10      // SPI CS (DAC)
#define PIN_MOSI 11    // SPI MOSI (DAC)
#define PIN_SCK 12     // SPI SCK (DAC)
```

## Pin USB-Serial/JTAG ESP32-S3

L'ESP32-S3 usa **GPIO19 e GPIO20** per USB-Serial/JTAG integrato:
- **GPIO19**: USB D- (UART RX in modalità CDC)
- **GPIO20**: USB D+ (UART TX in modalità CDC)

## ✅ Risposta: SÌ, puoi usare USB per UART bridge!

**Pin disponibili per Ethernet Bridge UART:**
- **RX**: GPIO19 (USB D-, attualmente usato per upload/debug)
- **TX**: GPIO20 (USB D+, attualmente usato per upload/debug)

### Implicazioni

**Pro:**
- Pin USB liberi dopo primo upload
- Hardware dedicato (USB-Serial/JTAG controller)
- Baudrate fino a 12 Mbps teorico (USB Full Speed)
- Zero conflitti con SPI DAC (GPIO 9-12)

**Contro:**
- Perdi debug seriale durante runtime
- Serve upload via OTA o altre UART per debug

## Schema Funzionale Finale

```
ESP32-C3 Bridge          ESP32-S3 Main Controller
┌─────────────┐          ┌────────────────────────┐
│  W5500      │          │  GPIO19 (USB D-)  RX   │
│  Ethernet   │  UART    │  GPIO20 (USB D+)  TX   │
│  TCP:7765   ├─────────►│                        │
│             │  4Mbaud  │  Frame Buffer          │
│             │          │  DAC Engine            │
│             │          │  ├─ GPIO12 SCK         │
│             │          │  ├─ GPIO11 MOSI        │
│             │          │  ├─ GPIO9  MISO        │
│             │          │  └─ GPIO10 CS          │
└─────────────┘          └────────────────────────┘
```

## Configurazione Necessaria

Nel tuo progetto ESP32-S3 attuale, per usare GPIO19/20 come UART normale:

```cpp
// Disabilita USB-CDC, usa GPIO19/20 come UART1
HardwareSerial BridgeSerial(1); // UART1

void setup() {
    // RX=GPIO19, TX=GPIO20, baudrate 4000000
    BridgeSerial.begin(4000000, SERIAL_8N1, 19, 20);
}
```

Nel `platformio.ini` rimuovi:
```ini
-D ARDUINO_USB_CDC_ON_BOOT=1  // ← Rimuovi questa flag
```

## Protocollo UART Ottimizzato

```c
// Framing minimale per ridurre overhead
typedef struct __attribute__((packed)) {
    uint8_t sync;          // 0xAA (1 byte)
    uint8_t cmd;           // 0x01=point, 0x02=control (1 byte)
    uint16_t count;        // Numero punti nel batch (2 bytes)
    // Seguito da N × 18 bytes (punti Ether Dream)
    // CRC8 finale (1 byte)
} uart_header_t;          // 4 bytes header + data + 1 CRC

// Esempio batch 32 punti:
// 4 (header) + 32×18 (points) + 1 (CRC) = 581 bytes
// @ 4Mbaud = 1.16 ms per batch
```

## Bandwidth Analysis

**Dati Ether Dream:**
- Point size: 18 bytes (X, Y, R, G, B, I, control, status)
- 30kpps × 18 bytes = **540 KB/s = 4.32 Mbps**
- 20kpps × 18 bytes = **360 KB/s = 2.88 Mbps**

**Capacità UART:**
- 921600 baud → ~92 KB/s (insufficiente)
- 2 Mbaud → ~200 KB/s (limite per 20kpps)
- 4 Mbaud → ~400 KB/s (limite per 30kpps)
- 8 Mbaud → ~800 KB/s (margine per 30kpps)

## Opzione Consigliata: ESP32-C3 + W5500

**Hardware:**
- ESP32-C3 (RISC-V, 160MHz, WiFi opzionale)
- W5500 Ethernet controller (hardwired TCP/IP, SPI)
- Costo: ~$5 totale

**Vantaggi:**
- W5500 ha TCP/IP stack hardware (zero CPU load)
- ESP32-C3 UART DMA nativi
- 4-8 Mbaud stabile
- Consumo ~30mA
- WiFi libero per configurazione web
- Ethernet cablato per streaming laser (zero jitter)

## Next Steps

1. **Firmware ESP32-C3 Bridge** (W5500 → UART @ 4Mbaud)
2. **Modifica ESP32-S3** per ricevere da GPIO19/20 invece di WiFi Ethernet
3. **Testing** con 20kpps → 30kpps progressivo
