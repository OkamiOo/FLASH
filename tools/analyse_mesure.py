"""Analyse du journal de mesure écrit par le plugin (variable FLASH_MESURE).

Usage : python tools/analyse_mesure.py mesure.csv [--ignorer 3]

Toutes les heures du journal sont dans l'horloge d'OBS. En boucle locale, l'heure de
capture inscrite dans la mire est dans la même horloge : la latence se lit directement.
"""

import argparse
import csv
import statistics


def pct(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    k = min(len(s) - 1, max(0, round(p / 100 * (len(s) - 1))))
    return s[k]


def resume(nom, values_ms):
    if not values_ms:
        print(f"  {nom:<34} aucune donnée")
        return
    print(f"  {nom:<34} médiane {statistics.median(values_ms):6.2f}  p95 {pct(values_ms, 95):6.2f}  "
          f"p99 {pct(values_ms, 99):6.2f}  max {max(values_ms):6.2f}  (n={len(values_ms)})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("journal")
    ap.add_argument("--ignorer", type=float, default=3.0, help="secondes ignorées au début")
    a = ap.parse_args()

    frames, shown, beeps, clocks = {}, {}, [], []
    with open(a.journal, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0] == "type":
                continue
            t = row[0]
            if t == "V":
                n, cap, arr, dec, rel, key, flash = row[1:8]
                frames[int(key)] = dict(n=int(n), cap=int(cap), arr=int(arr), dec=int(dec), rel=int(rel),
                                        flash=flash == "1")
            elif t == "D":
                shown.setdefault(int(row[1]), int(row[2]))
            elif t == "B":
                beeps.append(int(row[1]))
            elif t == "C":
                clocks.append((int(row[1]), int(row[2])))

    if not frames:
        print("Aucune image dans le journal.")
        return

    start = min(v["cap"] for v in frames.values()) + int(a.ignorer * 1e9)
    keys = sorted(k for k, v in frames.items() if v["cap"] >= start)
    ms = lambda ns: ns / 1e6

    total, transport, decode, attente, rendu = [], [], [], [], []
    jamais = 0
    for k in keys:
        v = frames[k]
        if k not in shown:
            jamais += 1
            continue
        d = shown[k]
        total.append(ms(d - v["cap"]))
        transport.append(ms(v["arr"] - v["cap"]))
        decode.append(ms(v["dec"] - v["arr"]))
        attente.append(ms(v["rel"] - v["dec"]))
        rendu.append(ms(d - v["rel"]))

    duree = (frames[keys[-1]]["cap"] - frames[keys[0]]["cap"]) / 1e9 if keys else 0
    numeros = sorted(frames[k]["n"] for k in keys)
    manquantes = (numeros[-1] - numeros[0] + 1 - len(numeros)) if numeros else 0

    print(f"Durée analysée : {duree:.1f} s, {len(keys)} images reçues, {manquantes} perdues avant décodage, "
          f"{jamais} jamais affichées")
    print("Latence image-à-image (ms), capture -> image composée par OBS :")
    resume("total", total)
    resume("capture -> réception (encodage+SRT)", transport)
    resume("réception -> décodée", decode)
    resume("décodée -> remise à OBS", attente)
    resume("remise -> rendu OBS", rendu)

    # Synchro : chaque flash affiché face au bip le plus proche
    ecarts = []
    flashes = sorted(shown[k] for k in keys if frames[k]["flash"] and k in shown)
    for t in flashes:
        if not beeps:
            break
        b = min(beeps, key=lambda x: abs(x - t))
        if abs(b - t) < 300e6:
            ecarts.append((t, ms(t - b)))
    print("Synchro son/image (ms, positif = image en retard sur le son) :")
    if ecarts:
        vals = [e for _, e in ecarts]
        print(f"  moyenne {statistics.mean(vals):+.2f}  écart-type {statistics.pstdev(vals):.2f}  "
              f"min {min(vals):+.2f}  max {max(vals):+.2f}  (n={len(vals)})")
        if len(ecarts) >= 10:
            t0 = ecarts[0][0]
            xs = [(t - t0) / 1e9 for t, _ in ecarts]
            ys = vals
            mx, my = statistics.mean(xs), statistics.mean(ys)
            sxx = sum((x - mx) ** 2 for x in xs)
            pente = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0
            print(f"  dérive : {pente * 3600:+.2f} ms par heure")
    else:
        print("  aucun couple flash/bip trouvé")

    if clocks:
        offsets = [ms(o) for o, _ in clocks[len(clocks) // 2:]]
        phases = [ms(p) for _, p in clocks[len(clocks) // 2:]]
        print(f"Horloge : décalage {statistics.median(offsets):.2f} ms, phase {statistics.median(phases):+.2f} ms")


if __name__ == "__main__":
    main()
