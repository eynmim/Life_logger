/*
 * XIAO ESP32-S3 — Dual INMP441 Calibrated System
 * =================================================
 * HP filter, noise gate, SNR metering, Serial Plotter,
 * and WAV streaming to PC.
 *
 * Mic1 (I2S0): SCK=GPIO5  WS=GPIO43 SD=GPIO6  L/R=GPIO44
 * Mic2 (I2S1): SCK=GPIO9  WS=GPIO7  SD=GPIO8  L/R=GPIO4
 *
 * Commands:
 *   1=Mic1  2=Mic2  B=Both  C=Calibrate
 *   P=Serial Plotter mode (use Tools>Serial Plotter)
 *   W=WAV stream mode (use Python recorder script)
 *   D=Dashboard mode (default)
 *   H=Help
 */

#include <driver/i2s.h>
#include <math.h>

// ── Mic1 (I2S0) ──
#define M1_SCK  5
#define M1_WS   43
#define M1_SD   6
#define M1_LR   44

// ── Mic2 (I2S1) ──
#define M2_SCK  9
#define M2_WS   7
#define M2_SD   8
#define M2_LR   4

// ── I2S ──
#define SAMPLE_RATE   44100
#define DMA_BUF_COUNT 8
#define DMA_BUF_LEN   512
#define BUF_SAMPLES   1024

// ── Calibration ──
#define CAL_ROUNDS      80
#define CAL_SKIP        20
#define NOISE_MARGIN    1.5f
#define DC_HP_ALPHA     0.995f
#define SMOOTH_ALPHA    0.85f

// ── Serial ──
#define SERIAL_BAUD     2000000  // 2 Mbaud — needed for 44.1kHz stereo
#define WAV_DURATION_S  10       // Default recording duration

int32_t raw1[BUF_SAMPLES];
int32_t raw2[BUF_SAMPLES];

// ── Per-mic state ──
struct Mic {
  float hpPrev, hpOut;
  bool  hpReady;
  // WAV-mode HP filter (separate state so dashboard HP is not disrupted)
  float wavHpPrev, wavHpOut;
  bool  wavHpReady;
  float calNoiseRMS, calNoisePeak, calDC, gateThreshold, gain;
  bool  calibrated;
  float rawDC, acRMS, acPeak, smoothAC;
  int16_t rawMin, rawMax;
  bool  active;
  float snr;
};

Mic mic1 = {}, mic2 = {};
char mode = 'B';       // 1, 2, B
char viewMode = 'D';   // D=Dashboard, P=Plotter, W=WAV
int printCount = 0;
bool wavActive = false;
unsigned long wavStart = 0;

// ══════════════════════════════════════
// I2S
// ══════════════════════════════════════

void setupI2S(i2s_port_t port, int sck, int ws, int sd) {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = SAMPLE_RATE;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = DMA_BUF_COUNT;
  cfg.dma_buf_len          = DMA_BUF_LEN;
  cfg.use_apll             = true;
  cfg.tx_desc_auto_clear   = false;
  cfg.fixed_mclk           = 0;

  i2s_driver_install(port, &cfg, 0, NULL);

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = sck;
  pins.ws_io_num    = ws;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = sd;

  i2s_set_pin(port, &pins);
  i2s_zero_dma_buffer(port);
}

size_t readMic(i2s_port_t port, int32_t *buf) {
  size_t bytesRead = 0;
  i2s_read(port, buf, BUF_SAMPLES * sizeof(int32_t), &bytesRead, portMAX_DELAY);
  return bytesRead / sizeof(int32_t);
}

// ══════════════════════════════════════
// HP FILTER + PROCESSING
// ══════════════════════════════════════

void processMic(int32_t *raw, size_t totalSamples, Mic &m) {
  if (totalSamples < 2) return;

  m.rawMin = 32767; m.rawMax = -32768;
  float dcSum = 0, sqSum = 0, peakAbs = 0;
  size_t count = 0;

  for (size_t i = 0; i < totalSamples; i += 2) {
    int16_t s = (int16_t)(raw[i] >> 16);
    float x = (float)s;
    dcSum += x;
    if (s < m.rawMin) m.rawMin = s;
    if (s > m.rawMax) m.rawMax = s;

    float hp;
    if (!m.hpReady) {
      m.hpPrev = x; m.hpOut = 0; m.hpReady = true; hp = 0;
    } else {
      hp = DC_HP_ALPHA * (m.hpOut + x - m.hpPrev);
      m.hpPrev = x; m.hpOut = hp;
    }

    float absHP = fabsf(hp);
    sqSum += hp * hp;
    if (absHP > peakAbs) peakAbs = absHP;
    count++;
  }
  if (count == 0) return;

  m.rawDC = dcSum / count;
  m.acRMS = sqrtf(sqSum / count);
  m.acPeak = peakAbs;

  if (m.smoothAC == 0) m.smoothAC = m.acRMS;
  else m.smoothAC = SMOOTH_ALPHA * m.smoothAC + (1.0f - SMOOTH_ALPHA) * m.acRMS;

  if (m.calibrated) {
    m.active = (m.smoothAC > m.gateThreshold);
    m.snr = (m.calNoiseRMS > 0) ? 20.0f * log10f(m.acRMS / m.calNoiseRMS) : 0;
  } else {
    m.active = (m.acRMS > 100);
    m.snr = 0;
  }
}

// ══════════════════════════════════════
// CALIBRATION
// ══════════════════════════════════════

void calibrate() {
  Serial.println("\n  Calibrating — SILENCE PLEASE!");
  mic1 = {}; mic2 = {};

  for (int i = 0; i < CAL_SKIP; i++) {
    readMic(I2S_NUM_0, raw1); readMic(I2S_NUM_1, raw2);
    processMic(raw1, BUF_SAMPLES, mic1); processMic(raw2, BUF_SAMPLES, mic2);
  }

  float sR1=0, sR2=0, sD1=0, sD2=0, mP1=0, mP2=0;
  for (int i = 0; i < CAL_ROUNDS; i++) {
    size_t n1 = readMic(I2S_NUM_0, raw1);
    size_t n2 = readMic(I2S_NUM_1, raw2);
    processMic(raw1, n1, mic1); processMic(raw2, n2, mic2);
    sR1 += mic1.acRMS; sR2 += mic2.acRMS;
    sD1 += mic1.rawDC;  sD2 += mic2.rawDC;
    if (mic1.acPeak > mP1) mP1 = mic1.acPeak;
    if (mic2.acPeak > mP2) mP2 = mic2.acPeak;
    if (i % 20 == 0)
      Serial.printf("  [%2d/%d] M1: RMS=%5.0f DC=%6.0f | M2: RMS=%5.0f DC=%6.0f\n",
                     i, CAL_ROUNDS, mic1.acRMS, mic1.rawDC, mic2.acRMS, mic2.rawDC);
  }

  mic1.calNoiseRMS = sR1/CAL_ROUNDS; mic2.calNoiseRMS = sR2/CAL_ROUNDS;
  mic1.calNoisePeak = mP1; mic2.calNoisePeak = mP2;
  mic1.calDC = sD1/CAL_ROUNDS; mic2.calDC = sD2/CAL_ROUNDS;
  mic1.gateThreshold = mic1.calNoiseRMS * NOISE_MARGIN;
  mic2.gateThreshold = mic2.calNoiseRMS * NOISE_MARGIN;
  mic1.gain = 1.0f; mic2.gain = 1.0f;
  mic1.calibrated = true; mic2.calibrated = true;

  Serial.printf("\n  M1: NoiseRMS=%.0f  Peak=%.0f  DC=%.0f  Gate=%.0f\n",
                mic1.calNoiseRMS, mP1, mic1.calDC, mic1.gateThreshold);
  Serial.printf("  M2: NoiseRMS=%.0f  Peak=%.0f  DC=%.0f  Gate=%.0f\n",
                mic2.calNoiseRMS, mP2, mic2.calDC, mic2.gateThreshold);

  float ratio = (mic2.calNoiseRMS > 0) ? mic1.calNoiseRMS / mic2.calNoiseRMS : 999;
  if (ratio > 5) Serial.printf("  [WARN] M1 is %.0fx noisier than M2\n", ratio);
  Serial.println();
}

// ══════════════════════════════════════
// MODE D: DASHBOARD
// ══════════════════════════════════════

void printBar(float value, float maxVal, int width) {
  int f = (maxVal > 0) ? min((int)((value * width) / maxVal), width) : 0;
  if (f < 0) f = 0;
  for (int i = 0; i < f; i++) Serial.print('#');
  for (int i = f; i < width; i++) Serial.print('-');
}

void modeDashboard() {
  size_t n1 = readMic(I2S_NUM_0, raw1);
  size_t n2 = readMic(I2S_NUM_1, raw2);
  processMic(raw1, n1, mic1);
  processMic(raw2, n2, mic2);

  bool s1 = (mode=='B' || mode=='1'), s2 = (mode=='B' || mode=='2');

  if (printCount % 25 == 0) {
    const char* mn = (mode=='B') ? "BOTH" : (mode=='1') ? "MIC1" : "MIC2";
    Serial.printf("\n═══ %s ═══  [1][2][B] [C]al [P]lot [W]av [H]elp\n", mn);
    Serial.println("Mic │ Gate │ DC(raw)│ HP RMS│Smooth│  SNR  │ Min~Max │ Level");
    Serial.println("────┼──────┼────────┼───────┼──────┼───────┼─────────┼─────────");
  }

  float dm1 = max(mic1.gateThreshold * 10.0f, 3000.0f);
  float dm2 = max(mic2.gateThreshold * 10.0f, 3000.0f);

  if (s1) {
    Serial.printf(" M1 │%s │%6.0f  │%5.0f  │%5.0f │%5.1fdB│%6d~%5d│ ",
                  mic1.active?" OPEN":" shut", mic1.rawDC, mic1.acRMS,
                  mic1.smoothAC, mic1.snr, mic1.rawMin, mic1.rawMax);
    printBar(mic1.smoothAC, dm1, 9);
    Serial.println();
  }
  if (s2) {
    Serial.printf(" M2 │%s │%6.0f  │%5.0f  │%5.0f │%5.1fdB│%6d~%5d│ ",
                  mic2.active?" OPEN":" shut", mic2.rawDC, mic2.acRMS,
                  mic2.smoothAC, mic2.snr, mic2.rawMin, mic2.rawMax);
    printBar(mic2.smoothAC, dm2, 9);
    Serial.println();
  }

  printCount++;
  delay(100);
}

// ══════════════════════════════════════
// MODE P: SERIAL PLOTTER
// Prints values as CSV — open Tools > Serial Plotter
// Shows HP-filtered waveform samples
// ══════════════════════════════════════

void modePlotter() {
  size_t n1 = readMic(I2S_NUM_0, raw1);
  size_t n2 = readMic(I2S_NUM_1, raw2);

  // HP filter state (persistent across calls)
  static float hp1_prev = 0, hp1_out = 0, hp1_ready = false;
  static float hp2_prev = 0, hp2_out = 0, hp2_ready = false;

  bool s1 = (mode=='B' || mode=='1'), s2 = (mode=='B' || mode=='2');

  // Print every Nth sample to avoid flooding serial
  // At 16kHz stereo, 256 L samples per buffer, print every 4th = 64 lines
  int step = 8;  // Print every 8th Left sample

  for (size_t i = 0; i < n1 && i < n2; i += 2 * step) {
    int16_t s1_raw = (int16_t)(raw1[i] >> 16);
    int16_t s2_raw = (int16_t)(raw2[i] >> 16);

    // HP filter for Mic1
    float x1 = (float)s1_raw;
    float h1;
    if (!hp1_ready) { hp1_prev = x1; hp1_out = 0; hp1_ready = true; h1 = 0; }
    else { h1 = DC_HP_ALPHA * (hp1_out + x1 - hp1_prev); hp1_prev = x1; hp1_out = h1; }

    // HP filter for Mic2
    float x2 = (float)s2_raw;
    float h2;
    if (!hp2_ready) { hp2_prev = x2; hp2_out = 0; hp2_ready = true; h2 = 0; }
    else { h2 = DC_HP_ALPHA * (hp2_out + x2 - hp2_prev); hp2_prev = x2; hp2_out = h2; }

    // Print CSV for Serial Plotter
    if (s1 && s2)
      Serial.printf("%d,%d\n", (int)h1, (int)h2);
    else if (s1)
      Serial.printf("%d\n", (int)h1);
    else
      Serial.printf("%d\n", (int)h2);
  }
}

// ══════════════════════════════════════
// MODE W: WAV STREAM
// Sends raw 16-bit PCM over serial.
// Python script captures and saves as .wav
// Protocol: SYNC_WORD(4) + LEN(2) + PCM_DATA(LEN*2)
// ══════════════════════════════════════

void startWavStream() {
  wavActive = true;
  wavStart = millis();

  // Reset WAV HP filter state for clean start
  mic1.wavHpPrev = 0; mic1.wavHpOut = 0; mic1.wavHpReady = false;
  mic2.wavHpPrev = 0; mic2.wavHpOut = 0; mic2.wavHpReady = false;

  Serial.println("WAV_START");
  Serial.printf("RATE:%d\n", SAMPLE_RATE);
  Serial.printf("BITS:16\n");
  Serial.printf("CHANNELS:%d\n", (mode == 'B') ? 2 : 1);
  Serial.printf("DURATION:%d\n", WAV_DURATION_S);
  Serial.println("DATA_BEGIN");
  Serial.flush();
  delay(50);
}

// HP filter helper for WAV mode — removes DC offset for clean audio
static inline int16_t applyWavHP(float x, Mic &m) {
  if (!m.wavHpReady) {
    m.wavHpPrev = x; m.wavHpOut = 0; m.wavHpReady = true;
    return 0;
  }
  float hp = DC_HP_ALPHA * (m.wavHpOut + x - m.wavHpPrev);
  m.wavHpPrev = x; m.wavHpOut = hp;
  // Clamp to int16 range
  if (hp >  32767.0f) hp =  32767.0f;
  if (hp < -32768.0f) hp = -32768.0f;
  return (int16_t)hp;
}

void modeWavStream() {
  if (!wavActive) return;

  // Check timeout
  if (millis() - wavStart > WAV_DURATION_S * 1000UL) {
    Serial.write('\n');
    Serial.println("DATA_END");
    Serial.flush();
    delay(50);
    wavActive = false;
    viewMode = 'D';
    Serial.printf("\n  WAV done (%d sec). Back to dashboard.\n\n", WAV_DURATION_S);
    return;
  }

  bool s1 = (mode=='B' || mode=='1'), s2 = (mode=='B' || mode=='2');

  // Read from active mic(s)
  size_t n1 = 0, n2 = 0;
  if (s1) n1 = readMic(I2S_NUM_0, raw1);
  if (s2) n2 = readMic(I2S_NUM_1, raw2);

  // Convert to 16-bit, apply HP filter, and send in one bulk write
  static int16_t sendBuf[BUF_SAMPLES];  // Reusable output buffer
  size_t outCount = 0;

  size_t maxN = max(n1, n2);
  for (size_t i = 0; i < maxN; i += 2) {
    if (s1 && i < n1) {
      float x = (float)(raw1[i] >> 16);
      sendBuf[outCount++] = applyWavHP(x, mic1);
    }
    if (s2 && i < n2) {
      float x = (float)(raw2[i] >> 16);
      sendBuf[outCount++] = applyWavHP(x, mic2);
    }
  }

  // Bulk write — much faster than sample-by-sample
  if (outCount > 0)
    Serial.write((uint8_t*)sendBuf, outCount * 2);
}

// ══════════════════════════════════════
// COMMANDS
// ══════════════════════════════════════

void handleCommands() {
  while (Serial.available()) {
    char c = toupper(Serial.read());
    if (c <= ' ') continue;

    if (c=='1') { mode='1'; printCount=0; if(viewMode=='D') Serial.println("\n  >> Mic1\n"); }
    if (c=='2') { mode='2'; printCount=0; if(viewMode=='D') Serial.println("\n  >> Mic2\n"); }
    if (c=='B') { mode='B'; printCount=0; if(viewMode=='D') Serial.println("\n  >> Both\n"); }
    if (c=='C') { viewMode='D'; calibrate(); printCount=0; }
    if (c=='D') {
      viewMode='D'; printCount=0; wavActive=false;
      Serial.println("\n  >> Dashboard mode\n");
    }
    if (c=='P') {
      viewMode='P'; wavActive=false;
      Serial.println("\n  >> Plotter mode — open Tools > Serial Plotter");
      Serial.println("  >> Send D to return to dashboard\n");
      delay(500);
    }
    if (c=='W') {
      viewMode='W';
      Serial.printf("\n  >> WAV stream: %d sec, %d Hz, %s\n",
                    WAV_DURATION_S, SAMPLE_RATE,
                    (mode=='B') ? "stereo" : "mono");
      Serial.println("  >> Start Python recorder, then send any key...\n");
      // Wait for go signal
      while (!Serial.available()) delay(10);
      Serial.read(); // consume the key
      startWavStream();
    }
    if (c=='H') {
      Serial.println("\n  ┌─────────────────────────────────────┐");
      Serial.println("  │ 1=Mic1  2=Mic2  B=Both              │");
      Serial.println("  │ D=Dashboard  P=Plotter  W=WAV record │");
      Serial.println("  │ C=Calibrate  H=Help                  │");
      Serial.println("  └─────────────────────────────────────┘\n");
    }
  }
}

// ══════════════════════════════════════
// MAIN
// ══════════════════════════════════════

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  pinMode(M1_LR, OUTPUT); digitalWrite(M1_LR, LOW);
  pinMode(M2_LR, OUTPUT); digitalWrite(M2_LR, LOW);

  setupI2S(I2S_NUM_0, M1_SCK, M1_WS, M1_SD);
  setupI2S(I2S_NUM_1, M2_SCK, M2_WS, M2_SD);

  Serial.println("\n  ╔═══════════════════════════════════════════════╗");
  Serial.println("  ║  Dual INMP441 — Calibrated + Plotter + WAV   ║");
  Serial.println("  ║  M1: SCK=5 WS=43 SD=6   (I2S0)              ║");
  Serial.println("  ║  M2: SCK=9 WS=7  SD=8   (I2S1)              ║");
  Serial.println("  ║  [D]ash [P]lot [W]av [C]al [1][2][B] [H]elp ║");
  Serial.println("  ╚═══════════════════════════════════════════════╝");

  calibrate();
}

void loop() {
  handleCommands();

  switch (viewMode) {
    case 'D': modeDashboard(); break;
    case 'P': modePlotter();   break;
    case 'W': modeWavStream(); break;
  }
}
