"""compose_music.py: the Siege battle theme as a Music Mode song, Assets/Music/Siege_Battle.dsong (open it in the editor's
Music Mode to change it). Render it to the clip the game plays with

    DimensionEditor <Projects/Siege> --render-song Music/Siege_Battle.dsong Audio/Music/Siege_Battle.ogg

D minor at 92 BPM, built from the engine's own instruments: war drums (low toms and kick), a low string pad (two
detuned saws through a slow low-pass), a drone bass, a horn-like lead (saw and square, the filter opening on each
note) and reverb. Sections: drums alone, the pad comes in, the lead's theme, a quieter bridge, the theme again
over the full drums, and an ending that rings out."""
import json
import os

import siege as S

TPB = 96          # ticks per beat
BAR = TPB * 4     # 4/4
STEP = TPB // 4   # sixteenth notes

# MIDI keys (60 = middle C)
D2, F2, G2, A2, Bb2, C3 = 38, 41, 43, 45, 46, 48
D3 = 50


def synth(wave1, wave2, mix, detune, octave2, cutoff, resonance, filter_env, amp, filt, pw=0.5, glide=0.0):
    return {"wave1": wave1, "wave2": wave2, "mix": mix, "detune": detune, "octave2": octave2, "pulseWidth": pw,
            "cutoff": cutoff, "resonance": resonance, "filterEnv": filter_env, "glide": glide, "amp": amp, "filter": filt}


def channel(name, kind, volume, pan=0.0, insert=1, key=60, **params):
    c = {"name": name, "type": kind, "volume": volume, "pan": pan, "mute": False, "solo": False, "key": key,
         "insert": insert, "color": 0}
    c.update(params)
    return c


SAW, SQUARE, TRIANGLE, SINE = 0, 1, 2, 3
CHANNELS = [
    channel("War Drum", "Drum", 0.85, drum={"kind": 5, "tune": -14.0, "decay": 1.6, "tone": 0.25}),   # 0: low tom
    channel("War Drum Hi", "Drum", 0.75, 0.25, drum={"kind": 5, "tune": -7.0, "decay": 1.1, "tone": 0.35}),  # 1
    channel("Kick", "Drum", 0.75, drum={"kind": 0, "tune": -5.0, "decay": 1.4, "tone": 0.3}),  # 2
    channel("Snare", "Drum", 0.45, -0.2, drum={"kind": 1, "tune": -6.0, "decay": 0.9, "tone": 0.3}),  # 3: march rolls
    channel("Strings", "Synth", 0.5, synth=synth(SAW, SAW, 0.5, 0.12, 0, 900.0, 0.15, 0.25,
                                                  [0.55, 0.4, 0.85, 1.4], [0.8, 0.6, 0.6, 1.2])),  # 4
    channel("Drone", "Synth", 0.38, synth=synth(SAW, SINE, 0.45, 0.05, -1, 420.0, 0.2, 0.15,
                                                [0.3, 0.3, 0.9, 1.0], [0.3, 0.5, 0.7, 1.0])),  # 5
    channel("Horns", "Synth", 0.85, 0.1, synth=synth(SAW, SQUARE, 0.35, 0.07, 0, 700.0, 0.3, 0.55,
                                                    [0.06, 0.3, 0.75, 0.35], [0.09, 0.45, 0.35, 0.4], pw=0.42)),  # 6
]


def drums_full(bars, accent=1.0):
    """The battle rhythm: a heavy tom on 1 and the 'and' of 2, kick under it, the high drum answering, snare rolls."""
    notes = []
    for b in range(bars):
        o = b * BAR
        for t, v in ((0, 1.0), (6, 0.7), (10, 0.9), (12, 0.6)):
            notes.append([0, o + t * STEP, STEP, 60, v * accent])
        for t in (0, 8):
            notes.append([2, o + t * STEP, STEP, 60, 0.9 * accent])
        for t, v in ((4, 0.75), (14, 0.6), (15, 0.7)):
            notes.append([1, o + t * STEP, STEP, 60, v * accent])
        for t in range(16):
            if t % 2 == 1 or t >= 12:
                notes.append([3, o + t * STEP, STEP, 60, (0.35 + 0.25 * (t >= 12)) * accent])
    return notes


def drums_sparse(bars):
    """The opening: single heavy hits, a beat apart, growing."""
    notes = []
    for b in range(bars):
        o = b * BAR
        notes.append([0, o, STEP, 60, 0.75 + 0.06 * b])
        notes.append([2, o, STEP, 60, 0.6])
        notes.append([0, o + 2 * TPB + TPB // 2, STEP, 60, 0.55])
        if b % 2 == 1:
            notes += [[1, o + 3 * TPB + s * STEP, STEP, 60, 0.5 + 0.1 * s] for s in range(4)]
    return notes


# The harmony, a chord a bar: i - VI - III - VII, i - iv - V - V (D minor; the A major's C# lifts it home).
PROGRESSION = [(D3, (0, 3, 7)), (Bb2, (0, 4, 7)), (F2, (0, 4, 7)), (C3, (0, 4, 7)),
               (D3, (0, 3, 7)), (G2, (0, 3, 7)), (A2, (0, 4, 7)), (A2, (0, 4, 7))]


def pad(bars, velocity=0.7):
    notes = []
    for b in range(bars):
        root, chord = PROGRESSION[b % len(PROGRESSION)]
        for i in chord:
            notes.append([4, b * BAR, BAR - 4, root + 12 + i, velocity])
        notes.append([5, b * BAR, BAR - 4, root - 12, velocity])
    return notes


# The theme (a bar per chord above): key offsets from D4 (62), with lengths in beats.
THEME = [
    [(0, 1.5), (3, 0.5), (5, 1), (7, 1)],
    [(8, 2), (7, 1), (5, 1)],
    [(3, 1.5), (5, 0.5), (7, 2)],
    [(10, 2), (7, 1), (5, 1)],
    [(0, 1.5), (3, 0.5), (5, 1), (7, 1)],
    [(12, 2), (10, 1), (8, 1)],
    [(7, 1.5), (8, 0.5), (7, 1), (6, 1)],
    [(7, 4)],
]


def lead(bars, octave=0, velocity=0.85):
    notes = []
    for b in range(bars):
        t = b * BAR
        for offset, beats in THEME[b % len(THEME)]:
            length = int(beats * TPB)
            notes.append([6, t, length - 6, 62 + offset + 12 * octave, velocity])
            t += length
    return notes


def main():
    patterns = [
        {"name": "Drums Intro", "bars": 4, "notes": drums_sparse(4)},
        {"name": "Drums", "bars": 4, "notes": drums_full(4)},
        {"name": "Strings", "bars": 8, "notes": pad(8)},
        {"name": "Theme", "bars": 8, "notes": lead(8)},
        {"name": "Strings Soft", "bars": 8, "notes": pad(8, 0.45)},
        {"name": "Theme High", "bars": 8, "notes": lead(8, octave=1, velocity=0.75)},
        {"name": "Ending", "bars": 2, "notes": [[0, 0, STEP, 60, 1.0], [2, 0, STEP, 60, 1.0], [4, 0, BAR * 2 - 8, D3 + 12, 0.8],
                                                [4, 0, BAR * 2 - 8, D3 + 15, 0.8], [4, 0, BAR * 2 - 8, D3 + 19, 0.8],
                                                [5, 0, BAR * 2 - 8, D2, 0.9], [6, 0, BAR * 2 - 8, 62, 0.8]]},
    ]
    P = {p["name"]: i for i, p in enumerate(patterns)}
    playlist = []

    def place(name, track, bar, times=1):
        length = patterns[P[name]]["bars"]
        for k in range(times):
            playlist.append([P[name], track, (bar + k * length) * BAR, 0])

    # 0-8 drums alone; 8-16 strings join; 16-32 the theme over full drums; 32-40 bridge (soft strings, sparse drums);
    # 40-56 the theme up an octave over everything; 56-58 the ending.
    place("Drums Intro", 0, 0, 2)
    place("Drums", 0, 8, 2)
    place("Strings", 1, 8)
    place("Drums", 0, 16, 4)
    place("Strings", 1, 16, 2)
    place("Theme", 2, 16, 2)
    place("Drums Intro", 0, 32, 2)
    place("Strings Soft", 1, 32)
    place("Drums", 0, 40, 4)
    place("Strings", 1, 40, 2)
    place("Theme", 2, 40)
    place("Theme High", 3, 48)
    place("Theme", 2, 48)
    place("Ending", 0, 56)

    song = {
        "format": "dsong", "version": 1, "name": "Siege Battle", "tempo": 92.0, "beatsPerBar": 4, "stepsPerBeat": 4,
        "swing": 0.0, "channels": CHANNELS, "patterns": patterns, "playlist": playlist,
        "tracks": [{"name": n, "mute": False} for n in ("Drums", "Strings", "Theme", "Theme High")],
        "mixer": [
            {"name": "Master", "volume": 0.85, "pan": 0.0, "mute": False, "output": 0,
             "effects": [{"type": "Compressor", "enabled": True, "mix": 1.0, "params": [-16.0, 3.0, 20.0, 200.0, 3.0]}]},
            # Every channel plays through a hall: a big reverb, the low end cleaned up.
            {"name": "Hall", "volume": 1.0, "pan": 0.0, "mute": False, "output": 0,
             "effects": [{"type": "EQ", "enabled": True, "mix": 1.0, "params": [-3.0, 300.0, -2.0, 1.0, 1.5, 0.0]},
                         {"type": "Reverb", "enabled": True, "mix": 0.32, "params": [0.88, 0.45, 1.0]}]},
        ],
        "automation": [], "audio": [],
    }
    rel = "Music/Siege_Battle.dsong"
    path = os.path.join(S.ASSETS, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(song, f, indent=1)
    S.write_meta(rel)
    print(f"wrote {rel}: {len(playlist)} clips, {58 * 4 * 60 / 92:.0f} s")


if __name__ == "__main__":
    main()
