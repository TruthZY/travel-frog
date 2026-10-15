"""Synthesize the game's sound effects as original 16-bit PCM WAV files.

Everything here is generated from oscillators — no sampled audio, so nothing can
carry a third party's rights. Output format is fixed to 22050 Hz / mono / 16-bit
because that is what the board's I2S is wired for (BSP_I2S_DUPLEX_MONO_CFG(22050)
in the Waveshare BSP); feeding anything else means resampling or channel
conversion at play time.

    python tools/gen_sfx.py

Uses numpy only, no external audio libraries.
"""
import os
import wave

import numpy as np

SR = 22050
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "assets", "audio")

TWO_PI = 2 * np.pi


def t(seconds):
    return np.arange(int(SR * seconds), dtype=np.float64) / SR


def fade(x, ms=4):
    """Short fade in/out; without it every effect starts with a click."""
    n = min(int(SR * ms / 1000), len(x) // 2)
    if n <= 0:
        return x
    ramp = np.linspace(0.0, 1.0, n)
    x[:n] *= ramp
    x[-n:] *= ramp[::-1]
    return x


def glide(freq_start, freq_end, dur, phase_pow=1.0):
    """Sine sweep — integrate instantaneous frequency so the pitch moves smoothly."""
    tt = t(dur)
    frac = tt / dur
    inst = freq_start + (freq_end - freq_start) * (frac ** phase_pow)
    phase = TWO_PI * np.cumsum(inst) / SR
    return np.sin(phase)


def bell(freq, dur, harmonics=(1.0, 0.45, 0.2)):
    """Additive partials with per-partial decay: a struck bell rather than a bare sine."""
    tt = t(dur)
    out = np.zeros_like(tt)
    for i, amp in enumerate(harmonics):
        f = freq * (2 ** i)
        decay = np.exp(-tt * (5.0 + 4.0 * i))
        out += amp * decay * np.sin(TWO_PI * f * tt)
    return out


def noise(dur):
    return np.random.default_rng(7).uniform(-1.0, 1.0, int(SR * dur))


def norm(x, peak=0.72):
    m = np.max(np.abs(x))
    return x * (peak / m) if m else x


def to_pcm(x):
    return (np.clip(x, -1.0, 1.0) * 32767.0).astype("<i2")


def write(name, samples):
    path = os.path.join(OUT, name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(to_pcm(fade(norm(samples))).tobytes())
    return path, len(samples) / SR


# --- the five effects the UI asks for, one per tf_port_sfx() id ---

def sfx_click():
    """Dock tap: 20 ms of a damped 1.1 kHz tone plus a pinch of noise for attack."""
    tt = t(0.022)
    return (np.sin(TWO_PI * 1100 * tt) * np.exp(-tt * 180)
            + 0.25 * noise(0.022) * np.exp(-tt * 300))


def sfx_harvest():
    """Clover picked: a short upward pluck, the 'pop' of something coming loose."""
    tt = t(0.09)
    body = glide(520, 980, 0.09, phase_pow=0.6) * np.exp(-tt * 26)
    return body + 0.3 * np.sin(TWO_PI * 1960 * tt) * np.exp(-tt * 90)


def sfx_croak():
    """Angry frog: low growl whose amplitude is chopped at ~28 Hz, sliding down."""
    tt = t(0.26)
    carrier = glide(150, 92, 0.26)
    chop = 0.55 + 0.45 * np.sin(TWO_PI * 28 * tt)
    return carrier * chop * np.exp(-tt * 4) + 0.18 * noise(0.26) * np.exp(-tt * 12)


def sfx_sad():
    """Recalled with nothing: a deflating descending whistle."""
    tt = t(0.34)
    return glide(760, 250, 0.34, phase_pow=1.6) * np.exp(-tt * 5.5)


def sfx_reward():
    """Came home with a postcard: two bell strikes, a rising fifth."""
    first = bell(880, 0.20)
    second = bell(1174, 0.34)
    gap = np.zeros(int(SR * 0.05))
    return np.concatenate([first, gap, second])


def main():
    os.makedirs(OUT, exist_ok=True)
    effects = {
        "click": sfx_click,
        "harvest": sfx_harvest,
        "croak": sfx_croak,
        "sad": sfx_sad,
        "reward": sfx_reward,
    }
    for name, fn in effects.items():
        path, secs = write(name, fn())
        print("%-8s %5.0f ms  %7.1f KB  %s"
              % (name, secs * 1000, os.path.getsize(path) / 1024.0, path))


if __name__ == "__main__":
    main()
