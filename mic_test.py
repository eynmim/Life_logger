"""
XIAO RP2040 - Dual I2S MEMS Microphone Test Script
====================================================
Hardware:
  - Board: Seeed Studio XIAO RP2040
  - Mics:  2x I2S MEMS (INMP441-type), stereo configuration

Wiring:
  SCK (both mics)  -> D8  (GP8)
  WS  (both mics)  -> D9  (GP9)
  SD  (both mics)  -> D10 (GP10)
  VDD (both mics)  -> 3V3
  GND (both mics)  -> GND
  Mic1 L/R pin     -> GND  (Left channel)
  Mic2 L/R pin     -> 3V3  (Right channel)

Onboard peripherals:
  Green User LED   -> GP25 (D13)
  NeoPixel Power   -> GP11 (power-gate, must be HIGH to enable)
  NeoPixel Data    -> GP12

Audio Intensity Calculation:
  We read stereo 16-bit signed PCM samples from the I2S bus.
  Samples are interleaved: [L_lo, L_hi, R_lo, R_hi, ...].
  Each pair of bytes is unpacked as a signed 16-bit little-endian
  integer (range -32768 to +32767). The intensity is computed as
  the mean of the absolute values of all samples in the buffer.
  This gives a simple, fast approximation of loudness (0 = silence,
  ~32768 = full-scale clipping).
"""

import struct
import time
from machine import Pin, I2S
import neopixel

# ──────────────────────────────────────────────
# PIN DEFINITIONS
# ──────────────────────────────────────────────
I2S_SCK_PIN = 8          # D8  / GP8  - Bit Clock (shared)
I2S_WS_PIN  = 9          # D9  / GP9  - Word Select (shared)
I2S_SD_PIN  = 10         # D10 / GP10 - Serial Data (shared)

LED_PIN            = 25   # GP25 - Onboard green user LED
NEOPIXEL_POWER_PIN = 11   # GP11 - NeoPixel power-gate (XIAO RP2040 specific)
NEOPIXEL_DATA_PIN  = 12   # GP12 - NeoPixel data line

# ──────────────────────────────────────────────
# I2S CONFIGURATION
# ──────────────────────────────────────────────
SAMPLE_RATE     = 16000
BITS_PER_SAMPLE = 16
BUFFER_LEN      = 2048   # Bytes per read (~64 ms of stereo audio at 16 kHz/16-bit)
INTERNAL_BUF    = 8192   # I2S driver internal ring-buffer size

# ──────────────────────────────────────────────
# THRESHOLDS & TUNING
# ──────────────────────────────────────────────
MIC_WORKING_THRESHOLD = 200    # Avg abs amplitude above which mics are "working"
INTENSITY_MAX         = 5000   # Expected max intensity for full-scale NeoPixel color
DEBOUNCE_COUNT        = 5      # Consecutive readings needed to toggle the green LED
LOOP_DELAY_MS         = 50     # Delay between reads (ms)

# ──────────────────────────────────────────────
# HARDWARE INIT
# ──────────────────────────────────────────────

# Green status LED
led = Pin(LED_PIN, Pin.OUT, value=0)

# NeoPixel - XIAO RP2040 has a power-gate on GP11 that must
# be driven HIGH before the NeoPixel on GP12 will respond.
neo_pwr = Pin(NEOPIXEL_POWER_PIN, Pin.OUT, value=1)
time.sleep_ms(10)
np = neopixel.NeoPixel(Pin(NEOPIXEL_DATA_PIN), 1)
np[0] = (0, 0, 0)
np.write()

# I2S receiver
audio_in = I2S(
    0,                            # I2S peripheral 0
    sck=Pin(I2S_SCK_PIN),
    ws=Pin(I2S_WS_PIN),
    sd=Pin(I2S_SD_PIN),
    mode=I2S.RX,
    bits=BITS_PER_SAMPLE,
    format=I2S.STEREO,
    rate=SAMPLE_RATE,
    ibuf=INTERNAL_BUF,
)

# Pre-allocate read buffer
buf = bytearray(BUFFER_LEN)


# ──────────────────────────────────────────────
# HELPER FUNCTIONS
# ──────────────────────────────────────────────

def calculate_intensity(buffer, num_bytes):
    """Return the mean |sample| across all 16-bit signed PCM samples.

    For stereo 16-bit data the bytes are laid out as:
        [L_lo, L_hi, R_lo, R_hi, L_lo, L_hi, R_lo, R_hi, ...]
    We unpack every 2-byte pair as a signed int16 (little-endian)
    and average the absolute values.  The result ranges from
    0 (digital silence) to ~32768 (full scale).
    """
    num_samples = num_bytes // 2
    if num_samples == 0:
        return 0

    total = 0
    for i in range(num_samples):
        sample = struct.unpack_from("<h", buffer, i * 2)[0]
        total += abs(sample)

    return total // num_samples


def intensity_to_color(intensity):
    """Map an intensity value to an RGB tuple for the NeoPixel.

    Colour ramp:
        Low  (0-33 %):  dim blue  ->  purple
        Mid  (33-66 %): purple    ->  yellow / orange
        High (66-100%): orange    ->  bright red
    Overall brightness also scales with intensity.
    """
    ratio = min(intensity / INTENSITY_MAX, 1.0)

    if ratio < 0.33:
        t = ratio / 0.33
        r, g, b = int(30 * t), 0, int(40 + 60 * t)
    elif ratio < 0.66:
        t = (ratio - 0.33) / 0.33
        r = int(30 + 225 * t)
        g = int(100 * t)
        b = int(100 * (1.0 - t))
    else:
        t = (ratio - 0.66) / 0.34
        r = 255
        g = int(100 * (1.0 - t))
        b = 0

    # Brightness envelope (10 % floor so the pixel is never fully off
    # when there is any signal at all)
    bright = 0.1 + 0.9 * ratio
    r = min(int(r * bright), 255)
    g = min(int(g * bright), 255)
    b = min(int(b * bright), 255)

    return (r, g, b)


# ──────────────────────────────────────────────
# MAIN LOOP
# ──────────────────────────────────────────────

print("=" * 44)
print(" XIAO RP2040 - Dual I2S Mic Test")
print("=" * 44)
print(f" Sample rate : {SAMPLE_RATE} Hz")
print(f" Bit depth   : {BITS_PER_SAMPLE}-bit")
print(f" Format      : Stereo (L=Mic1, R=Mic2)")
print(f" Read buffer : {BUFFER_LEN} bytes")
print(f" Threshold   : {MIC_WORKING_THRESHOLD}")
print(" Starting capture ...\n")

consec_above = 0
consec_below = 0

try:
    while True:
        num_read = audio_in.readinto(buf)

        if num_read == 0:
            continue

        intensity = calculate_intensity(buf, num_read)

        # ── Logic A: Green LED = "mics are working" ──
        if intensity > MIC_WORKING_THRESHOLD:
            consec_above += 1
            consec_below = 0
            if consec_above >= DEBOUNCE_COUNT:
                led.value(1)
        else:
            consec_below += 1
            consec_above = 0
            if consec_below >= DEBOUNCE_COUNT:
                led.value(0)

        # ── Logic B: NeoPixel intensity visualisation ──
        color = intensity_to_color(intensity)
        np[0] = color
        np.write()

        # ── Serial monitor output ──
        bar = "#" * min(intensity // 100, 40)
        status = "ON " if led.value() else "OFF"
        print(f"I={intensity:5d} | LED:{status} | RGB:{color} | {bar}")

        time.sleep_ms(LOOP_DELAY_MS)

except KeyboardInterrupt:
    print("\nStopping ...")
    led.value(0)
    np[0] = (0, 0, 0)
    np.write()
    audio_in.deinit()
    print("Cleanup complete. Done.")
