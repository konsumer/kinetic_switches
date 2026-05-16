#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#define SYMBOL_US  35    // measured from IQ captures: ~35µs per symbol
#define TOLERANCE  40    // percent

// #define DEBUG_TIMING  // enable to see pulse histograms

// B and C share prefix "8eeee88eeeee". Full 25-char packets from IQ captures:
//   A: 8e8e88e88888eeee88ee888e8
//   B: 8eeee88eeeee8888e8e8888e8  ← unique suffix pos 12-22: "8888e8e8888"
//   C: 8eeee88eeeee888888ee888e8  ← unique suffix pos 12-22: "888888ee888"
// A's prefix is already unique. B/C use their unique suffixes.
const char* PATTERNS[] = {
  "8e8e88e88888",  // Switch A — unique prefix
  "8888e8e8888",   // Switch B — unique suffix
  "888888ee888",   // Switch C — unique suffix
};
const char* NAMES[] = {"Switch A", "Switch B", "Switch C"};
#define N_SW 3

SPIClass spi(FSPI);
CC1101 radio = new Module(PIN_CS, PIN_GDO0, RADIOLIB_NC, PIN_GDO2, spi);

// ---- CC1101 SPI helpers --------------------------------------------------------

static uint8_t cc1101_read(uint8_t addr) {
  spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  spi.transfer(addr | 0x80);   // read bit set
  uint8_t val = spi.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  return val;
}

static uint8_t cc1101_read_status(uint8_t addr) {
  spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  spi.transfer(addr | 0xC0);   // read + burst (required for status regs 0x30+)
  uint8_t val = spi.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  return val;
}

static void cc1101_write(uint8_t addr, uint8_t val) {
  spi.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  spi.transfer(addr & 0x3F);   // write: clear read and burst bits
  spi.transfer(val);
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
}

// ---- Pulse timing ISR ----------------------------------------------------------

#define MAX_NIB 128
volatile char     nib_buf[MAX_NIB + 1];
volatile int      nib_cnt     = 0;
volatile bool     pkt_rdy     = false;
volatile uint32_t rise_t       = 0;
volatile uint32_t last_valid_t = 0;  // last time a classifiable symbol was seen
volatile uint32_t isr_fires    = 0;

#ifdef DEBUG_TIMING
volatile uint32_t dbg_us[MAX_NIB];
volatile int      dbg_cnt = 0;
#endif

static inline bool near_sym(uint32_t v, uint32_t target) {
  uint32_t m = target * TOLERANCE / 100;
  return v > target - m && v < target + m;
}

static inline char classify(uint32_t us) {
  if (us < 15) return '?';
  // Nearest-match: pick the symbol with smallest error within tolerance.
  // Prevents 140µs ('f') from being swallowed by 'e' (105µs) range first-match.
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
  isr_fires++;
  uint32_t t = micros();
  if (digitalRead(PIN_GDO0)) {
    rise_t = t;
  } else {
    uint32_t high_us = t - rise_t;
#ifdef DEBUG_TIMING
    if (!pkt_rdy && dbg_cnt < MAX_NIB)
      dbg_us[dbg_cnt++] = high_us;
#endif
    if (!pkt_rdy && nib_cnt < MAX_NIB) {
      char c = classify(high_us);
      if (c != '?') {
        nib_buf[nib_cnt++] = c;
        last_valid_t = t;  // only advance on real symbols, not noise
      }
    }
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

  state = radio.setRxBandwidth(200.0);  // wide = catches freq-offset transmitters
  Serial.printf("setRxBandwidth(68.0):   %s (code %d)\n",
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

  // GDO2 = carrier sense (0x0E)
  cc1101_write(0x00, 0x0E);  // IOCFG2

  // Read back key registers to verify config
  Serial.printf("--- CC1101 register readback ---\n");
  Serial.printf("  IOCFG2   (0x00) = 0x%02X (want 0x0E carrier sense)\n", cc1101_read(0x00));
  Serial.printf("  IOCFG0   (0x02) = 0x%02X (want 0x0D async data on GDO0)\n", cc1101_read(0x02));
  Serial.printf("  PKTCTRL0 (0x08) = 0x%02X (want 0x32 async serial)\n", cc1101_read(0x08));
  Serial.printf("  MDMCFG2  (0x12) = 0x%02X (want 0x30 OOK, no sync)\n", cc1101_read(0x12));
  Serial.printf("  AGCCTRL2 (0x1B) = 0x%02X (want 0xC7 max gain)\n", cc1101_read(0x1B));

  // Verify MARCSTATE = RX (0x0D)
  uint8_t marc = cc1101_read_status(0x35) & 0x1F;
  Serial.printf("MARCSTATE:              0x%02X (%s)\n", marc, marc == 0x0D ? "RX OK" : "NOT RX");

  pinMode(PIN_GDO0, INPUT);
  pinMode(PIN_GDO2, INPUT);
  pinMode(PIN_LED, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_GDO0), gdo0_data_isr, CHANGE);

  Serial.println("Listening — press a switch...\n");
  Serial.println("Watch RSSI: should spike when switch pressed near antenna.");
}

void loop() {
  // Sample RSSI frequently to catch brief switch-press spikes
  static int     peak_rssi    = -120;
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
    uint32_t rate = (fires_now - prev_fires) / 5;  // edges/sec
    prev_fires = fires_now;
    last_hb = now;

    uint8_t marc = cc1101_read_status(0x35) & 0x1F;
    Serial.printf("[hb] edges/s=%lu peak_RSSI=%ddBm GDO0=%d GDO2=%d state=0x%02X\n",
      rate, peak_rssi, digitalRead(PIN_GDO0), digitalRead(PIN_GDO2), marc);
    Serial.printf("  regs: IOCFG0=0x%02X IOCFG2=0x%02X PKTCTRL0=0x%02X MDMCFG2=0x%02X AGCCTRL2=0x%02X\n",
      cc1101_read(0x02), cc1101_read(0x00), cc1101_read(0x08),
      cc1101_read(0x12), cc1101_read(0x1B));
    peak_rssi = -120;  // reset for next window
  }

  // Fire when no valid symbol seen for >800µs (> inter-symbol gap, < 1023µs inter-packet gap)
  if (!pkt_rdy && nib_cnt > 4 && (micros() - last_valid_t) > 800)
    pkt_rdy = true;
  if (!pkt_rdy && nib_cnt >= MAX_NIB)
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

  if (len < 12) return;  // URH patterns are 12 chars minimum

#ifdef DEBUG_TIMING
  noInterrupts();
  int dcnt = dbg_cnt;
  uint32_t dtmp[MAX_NIB];
  memcpy(dtmp, (void*)dbg_us, dcnt * sizeof(uint32_t));
  dbg_cnt = 0;
  interrupts();

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

  char expanded[MAX_NIB * 2 + 1];
  int ei = 0;
  for (int i = 0; i < len && ei < (int)sizeof(expanded) - 2; i++) {
    if (buf[i] == 'F') { expanded[ei++] = 'f'; expanded[ei++] = 'f'; }
    else                  expanded[ei++] = buf[i];
  }
  expanded[ei] = '\0';

  // Fire on first match; suppress same switch for 1.5s (covers all 3 repeat packets).
  // Signal degrades each repeat so first packet is most reliable — don't wait for confirmation.
  static int      last_sw      = -1;
  static uint32_t last_fire_ms = 0;
  #define REFIRE_GUARD_MS 1500

  for (int i = 0; i < N_SW; i++) {
    if (!strstr(expanded, PATTERNS[i])) continue;
    uint32_t now_ms = millis();
    if (i == last_sw && (now_ms - last_fire_ms) < REFIRE_GUARD_MS) break;
    Serial.printf(">>> %s pressed!\n", NAMES[i]);
    digitalWrite(PIN_LED, HIGH);
    delay(100);
    digitalWrite(PIN_LED, LOW);
    last_sw      = i;
    last_fire_ms = now_ms;
    break;  // only one switch per packet
  }
}
