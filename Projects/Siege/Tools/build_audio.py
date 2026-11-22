"""build_audio.py [--force]: the Siege sample's sounds. Downloads CC0 recordings from OpenGameArt into
.dimension/AudioSources (not committed), cuts the takes it uses out of them and writes them to Assets/Audio:

  SFX/*.wav    short one-shots, 48 kHz (or the source's rate) mono 16-bit, each take on its own, peaks at -3 dB
  Loops/*.ogg  ambience and burning loops, copied as they come (Ogg Vorbis)

Every source is CC0 (CREDITS.md lists them). The battle scripts (Scripts/BattleAudio.cs) play the clips by name.
Archives are unpacked with Windows' tar (libarchive reads 7z)."""
import os
import shutil
import struct
import subprocess
import sys
import urllib.request

import numpy as np

import siege as S

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, ".dimension", "AudioSources")
OUT = os.path.join(ROOT, "Assets", "Audio")
OGA = "https://opengameart.org/sites/default/files/"
AGENT = {"User-Agent": "Mozilla/5.0"}
FORCE = "--force" in sys.argv

# OpenGameArt files (all CC0): archives are unpacked into CACHE/<archive name>/.
DOWNLOADS = [
    "medieval_sfx_weapon_on_weapon_1_of_2.7z", "medieval_sfx_weapon_on_weapon_2_of_2.7z",  # Ben Jaszczak, Brian Nelson
    "medieval_sfx_textures_1_of_2.7z", "medieval_sfx_textures_2_of_2.7z",
    "sword_-_starninjas_1.zip",                                                             # StarNinjas
    "independent_nu_ljudbank-wood_crack_hit_destruction.7z",                                # independent.nu
    "crowd_shouting.ogg", "cannon_hit_wall_no_splash.ogg", "cannon_hit_cannon_1.ogg", "fire-1.ogg", "Trot.ogg",
]

# One-shots: output name -> (source file name, how many takes, the longest a take may run in seconds).
TAKES = {
    "Clash_Sword": ("Sabre Norse Sword Blade on Blade.wav", 4, 1.2),
    "Clash_Sword2": ("Norse Sword Katana Blade on Blade.wav", 3, 1.2),
    "Clash_Spear": ("Spear Norse Sword Blade on Blade.wav", 3, 1.2),
    "Clash_Haft": ("Spear Spear Haft on Haft.wav", 3, 1.0),
    "Swing": ("Sabre Swing.wav", 3, 0.7),
    "Bow_Shoot": ("English Longbow Shoot.wav", 3, 1.0),
    "Bow_Draw": ("English Longbow Draw.wav", 2, 1.6),
    "Arrow_Passby": ("Scythian Recurve Arrow Passby.wav", 4, 1.2),
    "Arrow_Approach": ("Scythian Recurve Heavy Arrow Approach.wav", 3, 1.6),
}
# Copied as they are: output path under Assets/Audio -> source file name.
COPIES = {
    "SFX/Impact_1.ogg": "cannon_hit_wall_no_splash.ogg",
    "SFX/Impact_2.ogg": "cannon_hit_cannon_1.ogg",
    "Loops/Battle_Crowd.ogg": "crowd_shouting.ogg",
    "Loops/Fire.ogg": "fire-1.ogg",
    "Loops/Horse_Trot.ogg": "Trot.ogg",
}
# Catapult arms: wood creaks and knocks from independent.nu's pack (FLAC files; copied).
WOOD = {"SFX/Catapult_Creak.flac": "crack05", "SFX/Catapult_Thud.flac": "impactwood07"}


def download():
    os.makedirs(CACHE, exist_ok=True)
    for name in DOWNLOADS:
        dest = os.path.join(CACHE, name)
        if not os.path.exists(dest) or FORCE:
            print("  " + name)
            with urllib.request.urlopen(urllib.request.Request(OGA + name, headers=AGENT)) as r, open(dest + ".part", "wb") as f:
                shutil.copyfileobj(r, f)
            os.replace(dest + ".part", dest)
        if name.endswith((".7z", ".zip")):
            unpacked = os.path.join(CACHE, os.path.splitext(name)[0])
            if not os.path.isdir(unpacked) or FORCE:
                os.makedirs(unpacked, exist_ok=True)
                subprocess.run([os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe"),
                                "-xf", dest, "-C", unpacked], check=True)


def find(name):
    for base, _, files in os.walk(CACHE):
        for f in files:
            if f == name or os.path.splitext(f)[0].startswith(name) and f.endswith(".flac"):
                return os.path.join(base, f)
    raise FileNotFoundError(name)


def read_wav(path):
    """PCM (16/24/32-bit) or float WAV -> (mono float32, rate). Walks every chunk (these files carry bext/iXML first)."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", path
    i, fmt, pcm = 12, None, None
    while i + 8 <= len(data):
        cid, size = data[i:i + 4], struct.unpack("<I", data[i + 4:i + 8])[0]
        body = data[i + 8:i + 8 + size]
        if cid == b"fmt ":
            tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, channels, rate, bits)
        elif cid == b"data":
            pcm = body
        i += 8 + size + (size & 1)
    tag, channels, rate, bits = fmt
    if tag == 3:
        x = np.frombuffer(pcm, dtype=np.float32 if bits == 32 else np.float64).astype(np.float32)
    elif bits == 16:
        x = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
    elif bits == 24:
        b = np.frombuffer(pcm[:len(pcm) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = (np.where(v & 0x800000, v - 0x1000000, v) / 8388608.0).astype(np.float32)
    elif bits == 32:
        x = np.frombuffer(pcm, dtype="<i4").astype(np.float32) / 2147483648.0
    else:
        raise ValueError(f"{path}: {bits}-bit PCM")
    x = x[:len(x) // channels * channels].reshape(-1, channels).mean(axis=1)
    return x, rate


def decimate(x, rate):
    """Down to 48 kHz when the source is a whole multiple of it (96/192 kHz): a windowed-sinc low-pass, then every n-th."""
    n = rate // 48000 if rate % 48000 == 0 else 1
    if n <= 1:
        return x, rate
    taps = np.arange(-64, 65)
    h = np.sinc(taps / n) * np.blackman(len(taps))
    h /= h.sum()
    return np.convolve(x, h, mode="same")[::n].astype(np.float32), rate // n


def envelope(x, rate, ms=8.0):
    w = max(1, int(rate * ms / 1000))
    return np.convolve(np.abs(x), np.ones(w) / w, mode="same")


def takes(x, rate, longest):
    """The separate takes in a recording: runs above a threshold, joined over short gaps, each from a little before
    its attack until it has died away (or `longest` seconds)."""
    env = envelope(x, rate)
    peak = env.max()
    floor = np.percentile(env, 20)
    loud = env > max(peak * 0.04, floor * 6)
    edges = np.flatnonzero(np.diff(loud.astype(np.int8)))
    starts = list(edges[::2] + 1) if not loud[0] else [0] + list(edges[1::2] + 1)
    out = []
    for s in starts:
        if out and s < out[-1][1] + int(0.3 * rate):
            continue  # part of the take before (its ring or a bounce)
        level = env[s:s + int(0.15 * rate)].max()
        end = s + int(longest * rate)
        quiet = np.flatnonzero(env[s:end] < level * 0.003)
        quiet = quiet[quiet > int(0.08 * rate)]
        e = s + int(quiet[0]) if len(quiet) else min(end, len(x))
        out.append((max(0, s - int(0.015 * rate)), e, level))
    return out


def write_wav(path, x, rate):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pcm = (np.clip(x, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)


def write_meta(path):
    S.write_meta(os.path.relpath(path, os.path.join(ROOT, "Assets")).replace("\\", "/"))


def main():
    print("Downloading")
    download()
    print("Cutting takes")
    for name, (source, count, longest) in TAKES.items():
        x, rate = decimate(*read_wav(find(source)))
        found = takes(x, rate, longest)
        # The strongest takes that are not cut off at the start of the file, in recording order.
        best = sorted(sorted(found, key=lambda t: -t[2])[:count], key=lambda t: t[0])
        for k, (s, e, _) in enumerate(best, 1):
            clip = x[s:e].copy()
            fade = min(len(clip) // 4, int(0.04 * rate))
            clip[-fade:] *= np.linspace(1.0, 0.0, fade)
            clip -= clip.mean()
            clip *= 0.708 / max(np.abs(clip).max(), 1e-6)  # -3 dB peak
            path = os.path.join(OUT, "SFX", f"{name}_{k}.wav")
            write_wav(path, clip, rate)
            write_meta(path)
        print(f"  {name}: {len(best)} of {len(found)} takes ({rate} Hz)")
    print("Copying")
    for rel, source in list(COPIES.items()) + list(WOOD.items()):
        path = os.path.join(OUT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        shutil.copyfile(find(source), path)
        write_meta(path)
        print("  " + rel)


if __name__ == "__main__":
    main()
