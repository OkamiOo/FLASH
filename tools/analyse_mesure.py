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
                # Horodatage de l'émetteur (capture) ; absent des anciens journaux : celui de la mire
                pts = int(row[8]) if len(row) > 8 and row[8] else int(cap)
                frames[int(key)] = dict(n=int(n), cap=int(cap), arr=int(arr), dec=int(dec), rel=int(rel),
                                        flash=flash == "1", pts=int(pts))
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

    total, compo, transport, decode, attente, rendu = [], [], [], [], [], []
    jamais = 0
    for k in keys:
        v = frames[k]
        if k not in shown:
            jamais += 1
            continue
        d = shown[k]
        total.append(ms(d - v["cap"]))
        compo.append(ms(v["pts"] - v["cap"]))
        transport.append(ms(v["arr"] - v["pts"]))
        decode.append(ms(v["dec"] - v["arr"]))
        attente.append(ms(v["rel"] - v["dec"]))
        rendu.append(ms(d - v["rel"]))

    duree = (frames[keys[-1]]["cap"] - frames[keys[0]]["cap"]) / 1e9 if keys else 0
    numeros = sorted(frames[k]["n"] for k in keys)
    manquantes = (numeros[-1] - numeros[0] + 1 - len(numeros)) if numeros else 0

    print(f"Durée analysée : {duree:.1f} s, {len(keys)} images reçues, {manquantes} perdues avant décodage, "
          f"{jamais} jamais affichées")
    print("Latence image-à-image (ms), rendu -> image composée par OBS :")
    resume("total", total)
    resume("rendu -> capture (composition Windows)", compo)
    resume("capture -> réception (encodage+SRT)", transport)
    resume("réception -> décodée", decode)
    resume("décodée -> remise à OBS", attente)
    resume("remise -> rendu OBS", rendu)

    # Synchro. Flashs et bips tombent sur des secondes pleines de l'horloge commune : pour
    # chaque seconde, on prend la première image affichée avec le flash et le bip le plus
    # proche. Écart brut, puis rapporté à l'heure de rendu du flash (retard de l'image sur
    # la seconde pleine), puis à son horodatage de capture : ce dernier est la part due à FLASH.
    premiers = {}
    for k in keys:
        v = frames[k]
        if v["flash"] and k in shown:
            g = v["cap"] // 1_000_000_000 * 1_000_000_000
            if g not in premiers or shown[k] < shown[premiers[g]]:
                premiers[g] = k
    brut, vs_rendu, vs_capture, temps = [], [], [], []
    for g, k in sorted(premiers.items()):
        t = shown[k]
        if not beeps:
            break
        b = min(beeps, key=lambda x: abs(x - t))
        if abs(b - t) < 300e6:
            v = frames[k]
            temps.append(t)
            brut.append(ms(t - b))
            vs_rendu.append(ms(t - b - (v["cap"] - g)))
            vs_capture.append(ms(t - b - (v["pts"] - g)))

    def ligne(nom, vals):
        print(f"  {nom:<30} moyenne {statistics.mean(vals):+6.2f}  écart-type {statistics.pstdev(vals):5.2f}  "
              f"min {min(vals):+6.2f}  max {max(vals):+6.2f}  (n={len(vals)})")

    print("Synchro son/image (ms, positif = image en retard sur le son) :")
    if brut:
        ligne("brute", brut)
        ligne("rapportée au rendu", vs_rendu)
        ligne("rapportée à la capture", vs_capture)
        if (temps[-1] - temps[0]) / 1e9 >= 120:
            xs = [(t - temps[0]) / 1e9 for t in temps]
            mx, my = statistics.mean(xs), statistics.mean(vs_capture)
            sxx = sum((x - mx) ** 2 for x in xs)
            pente = sum((x - mx) * (y - my) for x, y in zip(xs, vs_capture)) / sxx if sxx else 0
            print(f"  dérive : {pente * 3600:+.2f} ms par heure")
    else:
        print("  aucun couple flash/bip trouvé")

    if clocks:
        offsets = [ms(o) for o, _ in clocks[len(clocks) // 2:]]
        phases = [ms(p) for _, p in clocks[len(clocks) // 2:]]
        print(f"Horloge : décalage {statistics.median(offsets):.2f} ms, phase {statistics.median(phases):+.2f} ms")


if __name__ == "__main__":
    main()
