#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#define SYMBOL_US  35    // measured from IQ captures: ~35µs per symbol
#define TOLERANCE  40    // percent
#define SIG_BITS   25

// #define DEBUG_TIMING   // pulse histograms
// #define CAPTURE_MODE   // dump raw µs timings + classified string, no matching
                          // use this to compare CC1101 captures against URH/PlutoSDR

// ---- Detector base class -------------------------------------------------------

class KineticSwitch {
public:
    int id;
    const char* name;
    KineticSwitch(int id, const char* name) : id(id), name(name) {}
    virtual bool match(const char* buf, int len) = 0;
};

// Sliding-window full-signature match with Hamming tolerance.
// maxErr=1 handles a single misclassified symbol.
class SigSwitch : public KineticSwitch {
    uint32_t sig;
    int maxErr;
public:
    SigSwitch(int id, const char* name, uint32_t sig, int maxErr = 1)
        : KineticSwitch(id, name), sig(sig), maxErr(maxErr) {}

    bool match(const char* buf, int len) override {
        for (int s = 0; s <= len - SIG_BITS; s++) {
            uint32_t v = 0;
            bool ok = true;
            for (int j = 0; j < SIG_BITS; j++) {
                char c = buf[s + j];
                if (c != '8' && c != 'e') { ok = false; break; }
                v = (v << 1) | (c == 'e' ? 1 : 0);
            }
            if (ok && __builtin_popcount(v ^ sig) <= maxErr) return true;
        }
        return false;
    }

    static constexpr uint32_t makeSig(const char* s) {
        uint32_t v = 0;
        for (int i = 0; s[i]; i++)
            v = (v << 1) | (s[i] == 'e' ? 1 : 0);
        return v;
    }
};

// Substring match — for devices where only a partial pattern is known.
class PatternSwitch : public KineticSwitch {
    const char* pat;
public:
    PatternSwitch(int id, const char* name, const char* pat)
        : KineticSwitch(id, name), pat(pat) {}
    bool match(const char* buf, int len) override {
        return strstr(buf, pat) != nullptr;
    }
};

// ---- Switch list ---------------------------------------------------------------

// Full 25-char OOK packets from IQ captures (8=0, e=1):
//   A: 8e8e88e88888eeee88ee888e8  — unique prefix
//   B: 8eeee88eeeee8888e8e8888e8  — unique suffix pos 12-22
//   C: 8eeee88eeeee888888ee888e8  — unique suffix pos 12-22
// Short unique substrings used for robustness against timing jitter.
// SigSwitch available for full-signature matching when captures are verified.
KineticSwitch* switches[] = {
    new PatternSwitch(1, "Switch A", "8e8e88e88888"),      // unique prefix (12 chars)
    new PatternSwitch(2, "Switch B", "eeeee8888e8e8888"),  // preamble-end + unique suffix (16 chars)
    new PatternSwitch(3, "Switch C", "eeeee888888ee888"),  // preamble-end + unique suffix (16 chars)
};
#define N_SW (sizeof(switches) / sizeof(switches[0]))

SPIClass spi(FSPI);
CC1101 radio = new Module(PIN_CS, PIN_GDO0, RADIOLIB_NC, PIN_GDO2, spi);

// ---- CC1101 SPI helpers --------------------------------------------------------

static uint8_t cc1101_read(uint8_t addr) {
  spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  spi.transfer(addr | 0x80);
  uint8_t val = spi.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  return val;
}

static uint8_t cc1101_read_status(uint8_t addr) {
  spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  spi.transfer(addr | 0xC0);
  uint8_t val = spi.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  return val;
}

static void cc1101_write(uint8_t addr, uint8_t val) {
  spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  spi.transfer(addr & 0x3F);
  spi.transfer(val);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
}

// ---- Pulse timing ISR ----------------------------------------------------------

#define MAX_NIB 128
volatile char     nib_buf[MAX_NIB + 1];
volatile int      nib_cnt      = 0;
volatile bool     pkt_rdy      = false;
volatile bool     carrier_present = false;
volatile uint32_t rise_t       = 0;
volatile uint32_t last_valid_t = 0;
volatile uint32_t isr_fires    = 0;

#if defined(DEBUG_TIMING) || defined(CAPTURE_MODE)
volatile uint32_t dbg_us[MAX_NIB];
volatile int      dbg_cnt = 0;
#endif

static inline char classify(uint32_t us) {
  if (us < 15) return '?';
  static const struct { uint8_t mult; char ch; } syms[] = {
    {1,'8'}, {3,'e'}, {4,'f'}, {8,'F'}
  };
  char best = '?';
  uint32_t best_err = UINT32_MAX;
  for (int i = 0; i < 4; i++) {
    uint32_t target = (uint32_t)SYMBOL_US * syms[i].mult;
    uint32_t tol    = target * TOLERANCE / 100;
    uint32_t err    = us > target ? us - target : target - us;
    if (err <= tol && err < best_err) { best = syms[i].ch; best_err = err; }
  }
  return best;
}

void IRAM_ATTR gdo0_data_isr() {
  if (!carrier_present) return;  // gate: ignore all GDO0 data when no carrier
  isr_fires++;
  uint32_t t = micros();
  if (digitalRead(PIN_GDO0)) {
    rise_t = t;
  } else {
    uint32_t high_us = t - rise_t;
#if defined(DEBUG_TIMING) || defined(CAPTURE_MODE)
    if (!pkt_rdy && dbg_cnt < MAX_NIB)
      dbg_us[dbg_cnt++] = high_us;
#endif
    if (!pkt_rdy && nib_cnt < MAX_NIB) {
      char c = classify(high_us);
      if (c != '?') {
        nib_buf[nib_cnt++] = c;
        last_valid_t = t;
      }
    }
  }
}

// GDO2 = carrier sense (CHANGE): rising = carrier appeared, falling = burst done.
void IRAM_ATTR gdo2_cs_isr() {
  carrier_present = digitalRead(PIN_GDO2);
  if (carrier_present) {
    nib_cnt = 0;  // fresh burst starting, discard any accumulated noise
  } else if (nib_cnt > 4) {
    pkt_rdy = true;
  }
}

// ---- Setup / Loop --------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  Serial.println("\n=== kinetic switch detector ===");

  spi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  int state = radio.begin();
  Serial.printf("CC1101 begin:           %s (code %d)\n",
    state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);
  if (state != RADIOLIB_ERR_NONE) { while (1) delay(100); }

  state = radio.setFrequency(433.92);
  Serial.printf("setFrequency(433.92):   %s (code %d)\n",
    state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);

  state = radio.setOOK(true);
  Serial.printf("setOOK:                 %s (code %d)\n",
    state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);

  state = radio.setBitRate(28.6);
  Serial.printf("setBitRate(28.6):       %s (code %d)\n",
    state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);

  state = radio.setRxBandwidth(200.0);
  Serial.printf("setRxBandwidth(200.0):  %s (code %d)\n",
    state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);

  state = radio.receiveDirectAsync();
  Serial.printf("receiveDirectAsync:     %s (code %d)\n",
    state == RADIOLIB_ERR_NONE ? "OK" : "FAIL", state);
  if (state != RADIOLIB_ERR_NONE) { while (1) delay(100); }

  // Maximize sensitivity — critical for weak kinetic switch signals.
  // Reference: billmaterial/ESP-Home-CC1101-Kinetic-Switch-Transceiver
  cc1101_write(0x07, 0x06);  // FSCTRL1: IF = 152 kHz
  cc1101_write(0x1B, 0xC7);  // AGCCTRL2: max LNA + DVGA gain, 42dB target
  cc1101_write(0x1C, 0x00);  // AGCCTRL1: no relative carrier sense threshold
  cc1101_write(0x1D, 0xB2);  // AGCCTRL0: medium hysteresis, 16 samples wait

  cc1101_write(0x00, 0x0E);  // IOCFG2: GDO2 = carrier sense

  Serial.printf("--- CC1101 register readback ---\n");
  Serial.printf("  IOCFG2   (0x00) = 0x%02X (want 0x0E carrier sense)\n", cc1101_read(0x00));
  Serial.printf("  IOCFG0   (0x02) = 0x%02X (want 0x0D async data on GDO0)\n", cc1101_read(0x02));
  Serial.printf("  PKTCTRL0 (0x08) = 0x%02X (want 0x32 async serial)\n", cc1101_read(0x08));
  Serial.printf("  MDMCFG2  (0x12) = 0x%02X (want 0x30 OOK, no sync)\n", cc1101_read(0x12));
  Serial.printf("  AGCCTRL2 (0x1B) = 0x%02X (want 0xC7 max gain)\n", cc1101_read(0x1B));

  uint8_t marc = cc1101_read_status(0x35) & 0x1F;
  Serial.printf("MARCSTATE:              0x%02X (%s)\n", marc, marc == 0x0D ? "RX OK" : "NOT RX");

  pinMode(PIN_GDO0, INPUT);
  pinMode(PIN_GDO2, INPUT);
  pinMode(PIN_LED, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_GDO0), gdo0_data_isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_GDO2), gdo2_cs_isr, CHANGE);

  Serial.println("Listening — press a switch...\n");
}

void loop() {
  static int      peak_rssi   = -120;
  static uint32_t last_hb     = 0;
  static uint32_t prev_fires  = 0;
  uint32_t now = millis();

  {
    uint8_t rssi_raw = cc1101_read_status(0x34);
    int rssi_dbm = (rssi_raw >= 128) ? (rssi_raw - 256) / 2 - 74
                                      : rssi_raw / 2 - 74;
    if (rssi_dbm > peak_rssi) peak_rssi = rssi_dbm;
  }

  if (now - last_hb > 5000) {
    uint32_t fires_now = isr_fires;
    uint32_t rate = (fires_now - prev_fires) / 5;
    prev_fires = fires_now;
    last_hb = now;

    uint8_t marc = cc1101_read_status(0x35) & 0x1F;
    Serial.printf("[hb] edges/s=%lu peak_RSSI=%ddBm GDO0=%d GDO2=%d state=0x%02X\n",
      rate, peak_rssi, digitalRead(PIN_GDO0), digitalRead(PIN_GDO2), marc);
    peak_rssi = -120;
  }

  if (!pkt_rdy && nib_cnt >= MAX_NIB)  // buffer full = process now
    pkt_rdy = true;
  if (!pkt_rdy) return;

  noInterrupts();
  char buf[MAX_NIB + 1];
  int  len = nib_cnt;
  memcpy(buf, (void*)nib_buf, len);
  buf[len] = '\0';
  nib_cnt  = 0;
  pkt_rdy  = false;
  interrupts();

  if (len < 8) return;

#if defined(DEBUG_TIMING) || defined(CAPTURE_MODE)
  noInterrupts();
  int dcnt = dbg_cnt;
  uint32_t dtmp[MAX_NIB];
  memcpy(dtmp, (void*)dbg_us, dcnt * sizeof(uint32_t));
  dbg_cnt = 0;
  interrupts();
#endif

  char expanded[MAX_NIB * 2 + 1];
  int ei = 0;
  for (int i = 0; i < len && ei < (int)sizeof(expanded) - 2; i++) {
    if (buf[i] == 'F') { expanded[ei++] = 'f'; expanded[ei++] = 'f'; }
    else                  expanded[ei++] = buf[i];
  }
  expanded[ei] = '\0';

#ifdef CAPTURE_MODE
  // Print raw µs pulse widths (what URH sees internally) + classified string.
  // Press a switch, compare this output against your PlutoSDR/URH capture.
  Serial.printf("burst: %s\n", expanded);
  Serial.print("timings(us):");
  for (int i = 0; i < dcnt; i++) Serial.printf(" %lu", dtmp[i]);
  Serial.println();
  return;  // no matching in capture mode
#endif

#ifdef DEBUG_TIMING
  uint8_t hist[50] = {};
  int noise_cnt = 0;
  for (int i = 0; i < dcnt; i++) {
    if (dtmp[i] < 15) { noise_cnt++; continue; }
    int b = dtmp[i] / 20;
    if (b < 50) hist[b]++;
  }
  Serial.printf("  noise(<15us)=%d histogram(20us buckets):\n", noise_cnt);
  for (int b = 0; b < 50; b++) {
    if (hist[b] == 0) continue;
    Serial.printf("  %3d-%3d us: %d\n", b*20, b*20+19, hist[b]);
  }
#endif

  static int      last_sw      = -1;
  static uint32_t last_fire_ms = 0;
  #define REFIRE_GUARD_MS 1500

  for (int i = 0; i < (int)N_SW; i++) {
    if (!switches[i]->match(expanded, ei)) continue;
    uint32_t now_ms = millis();
    if (i == last_sw && (now_ms - last_fire_ms) < REFIRE_GUARD_MS) break;
    Serial.printf(">>> %s pressed! (burst: %s)\n", switches[i]->name, expanded);
    digitalWrite(PIN_LED, HIGH);
    delay(100);
    digitalWrite(PIN_LED, LOW);
    last_sw      = i;
    last_fire_ms = now_ms;
    break;
  }
}
