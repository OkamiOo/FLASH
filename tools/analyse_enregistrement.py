"""Vérifie la synchro son/image dans un enregistrement OBS de la mire.

Usage : python tools/analyse_enregistrement.py fichier.mov [--ffmpeg dossier]

Repère dans le fichier les images où le carré blanc de la mire est allumé et les débuts
de bip, puis mesure l'écart entre chaque flash et le bip le plus proche. C'est la synchro
telle qu'elle sera diffusée, indépendamment du journal du plugin.
"""

import argparse
import json
import os
import subprocess

import numpy as np

# Zone centrale du carré blanc de la mire (voir core/include/flash/mire.h)
FLASH_X, FLASH_Y, FLASH_SIZE = 1360, 340, 400
PATCH = 32


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fichier")
    ap.add_argument("--ffmpeg", default=os.path.join(os.path.dirname(__file__), "..", ".deps", "obs-deps", "bin"))
    a = ap.parse_args()
    ffmpeg = os.path.join(a.ffmpeg, "ffmpeg.exe")
    ffprobe = os.path.join(a.ffmpeg, "ffprobe.exe")

    info = json.loads(run([ffprobe, "-v", "error", "-show_entries", "stream=codec_type,start_time,avg_frame_rate",
                           "-of", "json", a.fichier]))
    streams = {s["codec_type"]: s for s in info["streams"]}
    v_start = float(streams["video"].get("start_time", 0))
    a_start = float(streams["audio"].get("start_time", 0))
    num, den = (int(x) for x in streams["video"]["avg_frame_rate"].split("/"))
    fps = num / den

    # Luminance moyenne de la zone du flash, image par image
    cx = FLASH_X + FLASH_SIZE // 2 - PATCH // 2
    cy = FLASH_Y + FLASH_SIZE // 2 - PATCH // 2
    raw = run([ffmpeg, "-v", "error", "-i", a.fichier, "-map", "0:v:0", "-fps_mode", "passthrough",
               "-vf", f"crop={PATCH}:{PATCH}:{cx}:{cy},format=gray", "-f", "rawvideo", "-"])
    frames = np.frombuffer(raw, dtype=np.uint8).reshape(-1, PATCH * PATCH).mean(axis=1)
    lit = frames > 200
    flash_times = [v_start + i / fps for i in range(1, len(lit)) if lit[i] and not lit[i - 1]]

    pcm = np.frombuffer(run([ffmpeg, "-v", "error", "-i", a.fichier, "-map", "0:a:0", "-ac", "1", "-ar", "48000",
                             "-f", "f32le", "-"]), dtype=np.float32)
    loud = np.abs(pcm) > 0.05
    beep_times = []
    quiet = 0
    for i, on in enumerate(loud):
        if on:
            if quiet > 4800:
                beep_times.append(a_start + i / 48000)
            quiet = 0
        else:
            quiet += 1

    ecarts = []
    for t in flash_times:
        if beep_times:
            b = min(beep_times, key=lambda x: abs(x - t))
            if abs(b - t) < 0.3:
                ecarts.append((t - b) * 1000)

    print(f"{len(flash_times)} flashs, {len(beep_times)} bips, {len(ecarts)} couples")
    if ecarts:
        e = np.array(ecarts)
        print(f"Écart image - son (ms, positif = image en retard) : moyenne {e.mean():+.2f}, "
              f"écart-type {e.std():.2f}, min {e.min():+.2f}, max {e.max():+.2f}")
        print(f"Résolution de la mesure : une image = {1000 / fps:.1f} ms")


if __name__ == "__main__":
    main()
