"""
MOSA A/B Audio Quality Analyzer
================================
Compares RAW (unprocessed) vs PROCESSED (AFE+post) recordings
to measure noise suppression effectiveness.

Usage:
  python analyze_ab.py                          # Analyze latest A/B pair
  python analyze_ab.py Test_voice/rec_..._RAW_mono_16000Hz.wav
  python analyze_ab.py --raw file_raw.wav --proc file_processed.wav

Outputs:
  - SNR comparison (raw vs processed)
  - Noise reduction in dB
  - Spectral analysis plots
  - Time-domain waveform comparison

Requirements: pip install numpy matplotlib scipy
"""

import os
import sys
import glob
import struct
import wave
import argparse
import numpy as np

def load_wav(filepath):
    """Load WAV file and return (samples_float, sample_rate)"""
    with wave.open(filepath, 'r') as wf:
        nframes = wf.getnframes()
        nch = wf.getnchannels()
        rate = wf.getframerate()
        raw = wf.readframes(nframes)

    samples = np.array(struct.unpack(f'<{nframes * nch}h', raw), dtype=np.float64)
    if nch == 2:
        samples = samples[::2]  # Take left channel only
    return samples, rate

def rms(x):
    return np.sqrt(np.mean(x**2)) if len(x) > 0 else 0

def db(x):
    return 20 * np.log10(x + 1e-10)

def dbfs(x):
    return 20 * np.log10(rms(x) / 32768 + 1e-10)

def estimate_snr(samples, rate, speech_threshold_db=-40):
    """Estimate SNR by detecting speech vs silence segments"""
    frame_len = int(rate * 0.025)  # 25ms frames
    hop = int(rate * 0.010)        # 10ms hop

    frame_rms = []
    for i in range(0, len(samples) - frame_len, hop):
        frame = samples[i:i+frame_len]
        frame_rms.append(rms(frame))

    frame_rms = np.array(frame_rms)
    frame_db = np.array([db(r) for r in frame_rms])

    # Classify frames
    noise_frames = frame_rms[frame_db < speech_threshold_db]
    speech_frames = frame_rms[frame_db >= speech_threshold_db]

    if len(noise_frames) < 5 or len(speech_frames) < 5:
        # Fallback: bottom 20% = noise, top 30% = speech
        sorted_rms = np.sort(frame_rms)
        n = len(sorted_rms)
        noise_frames = sorted_rms[:int(n * 0.2)]
        speech_frames = sorted_rms[int(n * 0.7):]

    noise_rms = np.mean(noise_frames)
    speech_rms = np.mean(speech_frames)

    snr = db(speech_rms) - db(noise_rms)
    return snr, noise_rms, speech_rms

def spectral_analysis(samples, rate, label=""):
    """Compute power spectral density"""
    from scipy.signal import welch
    freqs, psd = welch(samples, fs=rate, nperseg=1024, noverlap=512)
    return freqs, 10 * np.log10(psd + 1e-10)

def find_latest_ab_pair():
    """Find the most recent RAW + PROCESSED pair in Test_voice/"""
    base = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Test_voice")
    raw_files = sorted(glob.glob(os.path.join(base, "rec_*_RAW_mono_*.wav")))
    if not raw_files:
        return None, None
    raw_file = raw_files[-1]  # latest
    # Derive processed filename from same timestamp
    basename = os.path.basename(raw_file)
    timestamp = basename.split('_')[1] + '_' + basename.split('_')[2]
    proc_file = raw_file.replace("_RAW_", "_PROCESSED_")
    if os.path.exists(proc_file):
        return raw_file, proc_file
    return raw_file, None

def analyze(raw_path, proc_path):
    """Full A/B analysis"""
    print()
    print("  +===============================================+")
    print("  |     MOSA A/B Audio Quality Analysis           |")
    print("  +===============================================+")
    print()

    # Load files
    raw, rate = load_wav(raw_path)
    proc, _ = load_wav(proc_path)
    n = min(len(raw), len(proc))
    raw = raw[:n]
    proc = proc[:n]
    duration = n / rate

    print(f"  Files:")
    print(f"    RAW:       {os.path.basename(raw_path)}")
    print(f"    PROCESSED: {os.path.basename(proc_path)}")
    print(f"    Duration:  {duration:.1f}s  Rate: {rate}Hz  Samples: {n:,}")
    print()

    # ─── Overall Level ───
    raw_rms_val = rms(raw)
    proc_rms_val = rms(proc)
    print("  +--- Overall Level ------------------------------+")
    print(f"  |  RAW  RMS:  {raw_rms_val:8.1f}  ({dbfs(raw):5.1f} dBFS)  Peak: {np.max(np.abs(raw)):5.0f}")
    print(f"  |  PROC RMS:  {proc_rms_val:8.1f}  ({dbfs(proc):5.1f} dBFS)  Peak: {np.max(np.abs(proc)):5.0f}")
    print(f"  |  Gain:      {db(proc_rms_val) - db(raw_rms_val):+.1f} dB")
    print("  +------------------------------------------------+")
    print()

    # ─── SNR Estimation ───
    raw_snr, raw_noise, raw_speech = estimate_snr(raw, rate)
    proc_snr, proc_noise, proc_speech = estimate_snr(proc, rate)

    print("  +--- SNR Analysis -------------------------------+")
    print(f"  |  RAW  SNR:  {raw_snr:5.1f} dB  (speech={db(raw_speech):5.1f}  noise={db(raw_noise):5.1f})")
    print(f"  |  PROC SNR:  {proc_snr:5.1f} dB  (speech={db(proc_speech):5.1f}  noise={db(proc_noise):5.1f})")
    print(f"  |")
    print(f"  |  >> SNR IMPROVEMENT: {proc_snr - raw_snr:+.1f} dB")
    print(f"  |  >> Noise reduction: {db(raw_noise) - db(proc_noise):+.1f} dB")
    print(f"  |  >> Speech preserved: {db(proc_speech) - db(raw_speech):+.1f} dB")
    print("  +------------------------------------------------+")
    print()

    # ─── Segmental Analysis (500ms windows) ───
    seg_ms = 500
    seg_len = int(rate * seg_ms / 1000)
    print(f"  +--- Segment Analysis ({seg_ms}ms windows) ------------+")
    print(f"  |  Time    RAW dBFS   PROC dBFS   Reduction  Type")
    print(f"  |  -----   --------   ---------   ---------  ----")
    for i in range(0, n - seg_len, seg_len):
        t = i / rate
        seg_raw = raw[i:i+seg_len]
        seg_proc = proc[i:i+seg_len]
        raw_db = dbfs(seg_raw)
        proc_db = dbfs(seg_proc)
        reduction = raw_db - proc_db
        seg_type = "SPEECH" if raw_db > -40 else "noise "
        print(f"  |  {t:5.1f}s  {raw_db:7.1f}    {proc_db:7.1f}     {reduction:+6.1f}dB   {seg_type}")
    print("  +------------------------------------------------+")
    print()

    # ─── Spectral Comparison ───
    try:
        import matplotlib
        matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        from scipy.signal import welch

        fig, axes = plt.subplots(3, 1, figsize=(14, 12))
        fig.suptitle('MOSA A/B Voice Quality Analysis', fontsize=14, fontweight='bold')

        # Plot 1: Waveform comparison
        t_axis = np.arange(n) / rate
        axes[0].plot(t_axis, raw / 32768, alpha=0.6, label='RAW (unprocessed)', color='#cc4444')
        axes[0].plot(t_axis, proc / 32768, alpha=0.7, label='PROCESSED (AFE+post)', color='#2266cc')
        axes[0].set_xlabel('Time (s)')
        axes[0].set_ylabel('Amplitude (normalized)')
        axes[0].set_title('Waveform: Raw vs Processed')
        axes[0].legend(loc='upper right')
        axes[0].set_xlim(0, duration)
        axes[0].grid(True, alpha=0.3)

        # Plot 2: Power Spectral Density
        freqs_r, psd_r = welch(raw, fs=rate, nperseg=1024, noverlap=512)
        freqs_p, psd_p = welch(proc, fs=rate, nperseg=1024, noverlap=512)
        axes[1].plot(freqs_r, 10*np.log10(psd_r+1e-10), alpha=0.7, label='RAW', color='#cc4444')
        axes[1].plot(freqs_p, 10*np.log10(psd_p+1e-10), alpha=0.7, label='PROCESSED', color='#2266cc')
        axes[1].set_xlabel('Frequency (Hz)')
        axes[1].set_ylabel('Power (dB)')
        axes[1].set_title('Power Spectral Density')
        axes[1].legend(loc='upper right')
        axes[1].set_xlim(0, rate/2)
        axes[1].grid(True, alpha=0.3)

        # Shade speech frequency band
        axes[1].axvspan(300, 3400, alpha=0.1, color='green', label='Speech band')

        # Plot 3: Noise Reduction per frequency band
        reduction_db = 10*np.log10(psd_r+1e-10) - 10*np.log10(psd_p+1e-10)
        axes[2].bar(freqs_r[1:], reduction_db[1:], width=freqs_r[1]-freqs_r[0],
                    alpha=0.7, color=['#22aa44' if r > 0 else '#cc4444' for r in reduction_db[1:]])
        axes[2].axhline(y=0, color='black', linewidth=0.5)
        axes[2].set_xlabel('Frequency (Hz)')
        axes[2].set_ylabel('Reduction (dB)')
        axes[2].set_title('Noise Reduction per Frequency Band (positive = noise removed)')
        axes[2].set_xlim(0, rate/2)
        axes[2].grid(True, alpha=0.3)
        axes[2].axvspan(300, 3400, alpha=0.1, color='green')

        plt.tight_layout()
        plot_path = os.path.join(os.path.dirname(raw_path),
                                  f"analysis_{os.path.basename(raw_path).split('_')[1]}_{os.path.basename(raw_path).split('_')[2]}.png")
        plt.savefig(plot_path, dpi=150)
        print(f"  Plot saved: {os.path.basename(plot_path)}")
        print()

        # Open the plot
        if sys.platform == 'win32':
            os.startfile(plot_path)

    except ImportError:
        print("  [WARN] matplotlib/scipy not installed — skipping plots")
        print("  Install with: pip install matplotlib scipy")
        print()

    # ─── Quality Score ───
    snr_improvement = proc_snr - raw_snr
    noise_reduction = db(raw_noise) - db(proc_noise)

    print("  +===============================================+")
    if snr_improvement > 15:
        grade = "EXCELLENT"
    elif snr_improvement > 10:
        grade = "GOOD"
    elif snr_improvement > 5:
        grade = "FAIR"
    else:
        grade = "POOR"

    print(f"  |  QUALITY GRADE: {grade:>10s}                      |")
    print(f"  |  SNR Improvement:  {snr_improvement:+6.1f} dB                   |")
    print(f"  |  Noise Reduction:  {noise_reduction:+6.1f} dB                   |")
    print(f"  |                                               |")
    print(f"  |  Target: >15 dB improvement = EXCELLENT        |")
    print(f"  +===============================================+")
    print()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MOSA A/B Audio Quality Analyzer")
    parser.add_argument("file", nargs="?", help="RAW wav file (auto-finds PROCESSED pair)")
    parser.add_argument("--raw", help="Explicit RAW file path")
    parser.add_argument("--proc", help="Explicit PROCESSED file path")
    args = parser.parse_args()

    if args.raw and args.proc:
        raw_path, proc_path = args.raw, args.proc
    elif args.file:
        raw_path = args.file
        proc_path = args.file.replace("_RAW_", "_PROCESSED_")
        if not os.path.exists(proc_path):
            print(f"  [ERROR] Cannot find matching PROCESSED file: {proc_path}")
            sys.exit(1)
    else:
        raw_path, proc_path = find_latest_ab_pair()

    if not raw_path or not proc_path:
        print("  [ERROR] No A/B recording pair found.")
        print("  Record first: python wav_recorder.py --ab")
        sys.exit(1)

    if not os.path.exists(raw_path):
        print(f"  [ERROR] File not found: {raw_path}")
        sys.exit(1)
    if not os.path.exists(proc_path):
        print(f"  [ERROR] File not found: {proc_path}")
        sys.exit(1)

    analyze(raw_path, proc_path)
