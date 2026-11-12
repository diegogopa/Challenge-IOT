#!/usr/bin/env python3
"""
Reporte para el banco de pruebas del Challenge #3, a partir de la base SQLite.

  python3 reporte_pruebas.py --db aquamonitor.db            # resumen en pantalla
  python3 reporte_pruebas.py --db aquamonitor.db --csv out  # ademas exporta CSV

Calcula:
  - Persistencia: mensajes recibidos vs. esperados segun el contador seq del ESP32.
  - Pendientes de envio a la nube (respaldo sin Internet).
  - Eventos, ordenes desde la nube y predicciones de la IA.
"""
import argparse
import csv
import os
import sqlite3
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="aquamonitor.db")
    ap.add_argument("--desde-min", type=float, default=0, help="solo los ultimos N minutos (0 = todo)")
    ap.add_argument("--csv", help="carpeta donde exportar CSV")
    a = ap.parse_args()
    con = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    desde = time.time() - a.desde_min * 60 if a.desde_min else 0

    print("== Persistencia (contador seq del ESP32) ==")
    for est, mn, mx, n in con.execute(
            "SELECT estacion, MIN(seq), MAX(seq), COUNT(DISTINCT seq) FROM mediciones WHERE ts>=? GROUP BY estacion", (desde,)):
        esperados = (mx - mn + 1) if mx is not None else 0
        pct = 100 * n / esperados if esperados else 0
        print(f"  {est}: recibidos {n} de {esperados} esperados -> {pct:.2f} %  (perdidos: {esperados - n})")

    pend = con.execute("SELECT COUNT(*) FROM mediciones WHERE enviado=0 AND ts>=?", (desde,)).fetchone()[0]
    env = con.execute("SELECT COUNT(*) FROM mediciones WHERE enviado=1 AND ts>=?", (desde,)).fetchone()[0]
    print(f"\n== Nube ==\n  mediciones enviadas: {env}   pendientes: {pend}")
    pe = con.execute("SELECT COUNT(*) FROM eventos WHERE enviado=0 AND ts>=?", (desde,)).fetchone()[0]
    print(f"  eventos pendientes: {pe}")

    print("\n== Eventos ==")
    for ts, est, ev, envd in con.execute("SELECT ts, estacion, evento, enviado FROM eventos WHERE ts>=? ORDER BY ts", (desde,)):
        print(f"  {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(ts))}  {est}  {ev:<22} {'enviado' if envd else 'PENDIENTE'}")

    print("\n== Ordenes desde la nube ==")
    for ts, origen, orden, reenv in con.execute("SELECT ts, origen, orden, reenviada FROM ordenes WHERE ts>=? ORDER BY ts", (desde,)):
        print(f"  {time.strftime('%H:%M:%S', time.localtime(ts))}  {origen}  {orden}  {'reenviada al ESP32' if reenv else 'NO reenviada'}")

    print("\n== IA (ultimas 10) ==")
    for row in con.execute("SELECT ts, nivel, pendiente, r2, horas_critico, anomalia, z_anomalia, fuente, recomendacion "
                           "FROM predicciones WHERE ts>=? ORDER BY ts DESC LIMIT 10", (desde,)):
        ts, nivel, m, r2, h, an, z, fuente, reco = row
        print(f"  {time.strftime('%H:%M:%S', time.localtime(ts))} nivel={nivel} pend={m}%/h r2={r2} horas={h} "
              f"anom={an} z={z} [{fuente}]\n     {reco}")

    if a.csv:
        os.makedirs(a.csv, exist_ok=True)
        for tabla in ("mediciones", "eventos", "ordenes", "predicciones", "conexion"):
            cur = con.execute(f"SELECT * FROM {tabla} WHERE ts>=? ORDER BY ts", (desde,))
            with open(os.path.join(a.csv, f"{tabla}.csv"), "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow([d[0] for d in cur.description])
                w.writerows(cur)
        print(f"\nCSV exportados en {a.csv}/")


if __name__ == "__main__":
    main()
