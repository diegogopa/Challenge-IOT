#!/usr/bin/env python3
"""
Simulador del ESP32 AquaMonitor: publica por MQTT los mismos mensajes que
el firmware v4.0 (telemetria, eventos, LWT) y responde a la orden de
silenciar. Sirve para probar el Gateway, la base de datos, Ubidots y la IA
sin el hardware.

Escenarios:
  normal  nivel casi estable (solo evaporacion)
  sequia  el nivel baja hasta entrar en SEQUIA
  fuga    descenso rapido -> FUGA
  mixto   normal -> sequia -> recuperacion

Ejemplo (tiempo x60: cada segundo real = 1 minuto simulado):
  python3 simulador_esp32.py --host localhost --user esp32_est01 --password X --escenario sequia --acelerado 60
"""
import argparse
import json
import random
import time

import paho.mqtt.client as mqtt

NOMBRES = ["OK", "DESBORDE", "FUGA", "SEQUIA", "ERROR_SENSOR"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user", default="")
    ap.add_argument("--password", default="")
    ap.add_argument("--estacion", default="est01")
    ap.add_argument("--escenario", choices=["normal", "sequia", "fuga", "mixto"], default="mixto")
    ap.add_argument("--acelerado", type=float, default=1.0, help="factor de tiempo simulado")
    ap.add_argument("--periodo", type=float, default=10.0, help="segundos reales entre telemetrias")
    ap.add_argument("--duracion", type=float, default=0, help="segundos reales (0 = infinito)")
    ap.add_argument("--nivel-inicial", type=float, default=60.0)
    a = ap.parse_args()

    base = f"aquamonitor/{a.estacion}"
    estado = {"silenciada": False, "codigo": 0}
    cli = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sim-{a.estacion}")
    if a.user:
        cli.username_pw_set(a.user, a.password)
    cli.will_set(f"{base}/estado", "offline", qos=1, retain=True)

    def evento(nombre, codigo):
        cli.publish(f"{base}/eventos", json.dumps({"est": a.estacion, "up": int(time.time() * 1000),
                                                   "evento": nombre, "codigo": codigo}), qos=0)
        print(f"[sim] evento {nombre}")

    def al_conectar(c, ud, flags, rc, props=None):
        print("[sim] conectado, rc =", rc)
        c.publish(f"{base}/estado", "online", qos=1, retain=True)
        c.subscribe([(f"{base}/cmd", 1), (f"{base}/ia", 1)])

    def al_mensaje(c, ud, msg):
        txt = msg.payload.decode()
        if msg.topic.endswith("/cmd") and "silenciar" in txt:
            if estado["codigo"] != 0 and not estado["silenciada"]:
                estado["silenciada"] = True
                evento("SILENCIADA_NUBE", 20)
            print("[sim] orden silenciar recibida:", txt)
        elif msg.topic.endswith("/ia"):
            print("[sim] IA recibida:", txt)

    cli.on_connect, cli.on_message = al_conectar, al_mensaje
    cli.connect(a.host, a.port, 30)
    cli.loop_start()

    nivel, seq, t_sim, inicio = a.nivel_inicial, 0, 0.0, time.time()
    try:
        while a.duracion == 0 or time.time() - inicio < a.duracion:
            dt_h = a.periodo * a.acelerado / 3600.0           # horas simuladas por paso
            t_sim += dt_h
            hora_dia = (8 + t_sim) % 24
            sol = max(0.0, 1 - abs(hora_dia - 12) / 6)        # 0 de noche, 1 al mediodia
            temp = 12 + 10 * sol + random.gauss(0, 0.3)
            hum = 80 - 35 * sol + random.gauss(0, 1)
            uv = 9 * sol
            evap = max(0, min(100, 0.4 * (temp - 15) / 25 * 100 + 0.3 * uv / 11 * 100 + 0.3 * (100 - hum)))
            esperada = -(evap / 100) * 4

            esc = a.escenario
            if esc == "mixto":
                esc = "normal" if t_sim < 3 else ("sequia" if t_sim < 20 else "recupera")
            tasa = {"normal": esperada * 0.3, "sequia": -2.5 + esperada * 0.3,
                    "fuga": -9.0, "recupera": 6.0}[esc]
            nivel = max(0.0, min(100.0, nivel + tasa * dt_h + random.gauss(0, 0.15)))
            perdida = tasa - esperada

            if perdida <= -2.5 and tasa < 0 and esc == "fuga":
                cod = 2
            elif nivel <= 20 or (estado["codigo"] == 3 and nivel <= 25) or (nivel <= 35 and evap >= 70):
                cod = 3
            elif nivel >= 95:
                cod = 1
            else:
                cod = 0
            if cod != estado["codigo"]:
                estado["codigo"], estado["silenciada"] = cod, False
                evento(NOMBRES[cod], cod)

            msg = {"est": a.estacion, "seq": seq, "up": int((time.time() - inicio) * 1000),
                   "nivel": round(nivel, 1), "dist": round(9 - nivel / 100 * 5, 2), "temp": round(temp, 1),
                   "hum": round(hum, 1), "presion": round(752 + random.gauss(0, 0.2), 1), "presDelta": 0.0,
                   "presionCae": 0, "uvIdx": round(uv, 2), "evap": round(evap, 1), "tasa": round(tasa, 2),
                   "esperada": round(esperada, 2), "perdida": round(perdida, 2), "tendencia": -1 if tasa < -0.5 else 0,
                   "estado": NOMBRES[cod], "estadoCod": cod, "fallas": 0,
                   "silenciada": int(estado["silenciada"]), "rssi": -60}
            cli.publish(f"{base}/telemetria", json.dumps(msg), qos=0)
            print(f"[sim] seq={seq} nivel={msg['nivel']} estado={msg['estado']} t_sim={t_sim:.1f}h")
            seq += 1
            time.sleep(a.periodo)
    except KeyboardInterrupt:
        pass
    cli.publish(f"{base}/estado", "offline", qos=1, retain=True).wait_for_publish(3)
    cli.loop_stop()
    cli.disconnect()


if __name__ == "__main__":
    main()
