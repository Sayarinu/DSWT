"""Render the companion's original menu score and button sound to DS PCM8."""
from math import exp, sin, tau
from pathlib import Path
import sys

RATE = 12000
BEAT = 60 / 84

# Eight bars in D, voiced for a rounded bass and fingerpicked upper strings.
# Each melody entry is (beat, MIDI note, held beats).
BARS = [
    ((50, 57, 61, 66), [(0.5, 69, 1.25), (2, 66, 0.65), (3, 64, 0.75)]),
    ((47, 54, 57, 62), [(0, 62, 1.65), (2.5, 66, 0.6), (3.25, 69, 0.55)]),
    ((43, 55, 59, 66), [(0, 71, 1.35), (1.75, 69, 0.6), (2.5, 66, 1.15)]),
    ((45, 52, 59, 61), [(0.25, 64, 1.6), (2.5, 61, 1.1)]),
    ((42, 57, 62, 66), [(0.5, 66, 0.65), (1.5, 69, 0.65), (2.5, 74, 1.25)]),
    ((43, 54, 59, 62), [(0, 71, 1.15), (1.5, 69, 0.6), (2.25, 67, 1.35)]),
    ((40, 55, 59, 62), [(0, 66, 0.8), (1, 64, 1.4), (3, 62, 0.7)]),
    ((45, 52, 57, 61), [(0.25, 61, 1.5), (2.5, 64, 0.8)]),
]


def note(mix, beat, pitch, held, gain, decay, brightness):
    start = round(beat * BEAT * RATE)
    omega = tau * 440 * 2 ** ((pitch - 69) / 12)
    release = held * BEAT
    for i in range(round((release + 0.36) * RATE)):
        t = i / RATE
        attack = 1 - exp(-t / 0.014)
        env = attack * exp(-t / decay - max(0, t - release) / 0.055)
        phase = omega * t
        body = sin(phase) + 0.20 * sin(phase * 2) + brightness * exp(-5*t) * sin(phase * 3.002)
        mix[(start + i) % len(mix)] += gain * env * body


def theme():
    frames = 4 * round(len(BARS) * 4 * BEAT * RATE / 4)
    mix = [0.0] * frames
    for bar, (chord, melody) in enumerate(BARS):
        downbeat = 4 * bar
        note(mix, downbeat, chord[0], 2.8, 0.32, 1.4, 0.18)
        for beat, tone, gain in [(0.05, 1, .14), (.56, 2, .12), (1.05, 3, .15),
                                 (2.04, 1, .13), (2.56, 3, .11), (3.06, 2, .12)]:
            note(mix, downbeat + beat, chord[tone], 1.1, gain, .45, .34)
        for beat, pitch, held in melody:
            note(mix, downbeat + beat, pitch, held, .40, 1.05, .27)
    # Wrap the room reflections at the loop point, just like the note tails.
    tap = round(0.145 * RATE)
    return [value + .12 * mix[i-tap] + .045 * mix[i-2*tap] for i, value in enumerate(mix)]


def emit(file, name, samples):
    gain = 112 / max(map(abs, samples))
    pcm = bytes(round(x * gain) & 255 for x in samples)
    assert len(pcm) % 4 == 0
    file.write(f".balign 4\n.global {name}, {name}Bytes\n{name}:\n")
    for i in range(0, len(pcm), 32):
        file.write(".byte " + ",".join(map(str, pcm[i:i+32])) + "\n")
    file.write(f"{name}Bytes: .word {len(pcm)}\n")


if __name__ == "__main__":
    tap = [0.0] * 1152
    for i in range(len(tap)):
        t = i / RATE
        tap[i] = (1 - exp(-t / .003)) * exp(-t / .014) * sin(tau * 660 * t)
    with Path(sys.argv[1]).open("w") as file:
        file.write('.section .rodata,"a"\n')
        emit(file, "CpMenuTheme", theme())
        emit(file, "CpMenuTap", tap)
