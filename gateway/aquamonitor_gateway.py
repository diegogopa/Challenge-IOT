#!/usr/bin/env python3
"""
AquaMonitor - Gateway IoT (Raspberry Pi)

Flujo:
  ESP32 --MQTT--> Mosquitto local --> este servicio --> SQLite
                                           |  procesamiento local (validacion, agregacion)
                                           |  modulo de IA (pronostico, anomalias, recomendacion)
                                           v
                                     Ubidots (MQTT)  -->  tablero global + notificaciones
  Ubidots (interruptor "silenciar") --MQTT--> este servicio --MQTT--> ESP32

Respaldo sin Internet: todo queda en SQLite con enviado=0 y se reenvia,
con su marca de tiempo original, cuando vuelve la conexion con la nube.

Uso:  python3 aquamonitor_gateway.py --config gateway.ini
"""
import argparse
import configparser
import json
import logging
import os
import signal
import sqlite3
import threading
import time

import paho.mqtt.client as mqtt

from ia import ModuloIA

log = logging.getLogger("gateway")

RANGOS = {  # validacion local: lo que esta fuera de rango no se sube a la nube
    "nivel": (0, 100), "temp": (-40, 85), "hum": (0, 100),
    "presion": (300, 1100), "uv": (0, 20), "evap": (0, 100),
}
VARIABLES_NUBE = ["nivel", "temp", "hum", "presion", "uv", "evap", "tasa", "perdida"]


# ---------------------------------------------------------------------------
# Configuracion
# ---------------------------------------------------------------------------
def cargar_config(ruta):
    cp = configparser.ConfigParser()
    if not cp.read(ruta, encoding="utf-8"):
        raise SystemExit(f"No se encontro el archivo de configuracion: {ruta}")
    g = lambda s, k, d=None: os.environ.get(f"AQM_{s}_{k}".upper(), cp.get(s, k, fallback=d))
    return {
        "local": {
            "host": g("local", "host", "localhost"), "port": int(g("local", "port", "1883")),
            "user": g("local", "user", ""), "password": g("local", "password", ""),
            "base": g("local", "topic_base", "aquamonitor"),
        },
        "ubidots": {
            "enabled": g("ubidots", "enabled", "true").lower() == "true",
            "host": g("ubidots", "host", "industrial.api.ubidots.com"),
            "port": int(g("ubidots", "port", "8883")),
            "tls": g("ubidots", "tls", "true").lower() == "true",
            "token": g("ubidots", "token", ""),
            "device": g("ubidots", "device_label", "aquamonitor-est01"),
            "intervalo_s": int(g("ubidots", "intervalo_s", "60")),
            "max_lotes": int(g("ubidots", "max_lotes_por_envio", "20")),
        },
        "ia": {
            "enabled": g("ia", "enabled", "true").lower() == "true",
            "intervalo_s": int(g("ia", "intervalo_s", "300")),
            "ventana_h": float(g("ia", "ventana_h", "6")),
            "min_puntos": int(g("ia", "min_puntos", "6")),
            "min_span_s": int(g("ia", "min_span_min", "20")) * 60,
            "nivel_critico": float(g("ia", "nivel_critico", "20")),
            "umbral_z": float(g("ia", "umbral_z", "3.5")),
            "gemini_key": g("ia", "gemini_api_key", ""),
            "gemini_model": g("ia", "gemini_model", "gemini-2.5-flash"),
            "gemini_min_interval_s": int(g("ia", "gemini_min_interval_s", "1800")),
        },
        "db": {"path": g("db", "path", "aquamonitor.db"), "retencion_dias": int(g("db", "retencion_dias", "30"))},
        "estacion": g("general", "estacion", "est01"),
    }


# ---------------------------------------------------------------------------
# Base de datos
# ---------------------------------------------------------------------------
class BaseDatos:
    def __init__(self, ruta):
        self.conn = sqlite3.connect(ruta, check_same_thread=False)
        self.lock = threading.Lock()
        esquema = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "schema.sql"), encoding="utf-8").read()
        with self.lock:
            self.conn.executescript(esquema)

    def ejecutar(self, sql, params=()):
        with self.lock:
            cur = self.conn.execute(sql, params)
            self.conn.commit()
            return cur

    def consultar(self, sql, params=()):
        with self.lock:
            return self.conn.execute(sql, params).fetchall()


# ---------------------------------------------------------------------------
# Gateway
# ---------------------------------------------------------------------------
class Gateway:
    def __init__(self, cfg):
        self.cfg = cfg
        self.db = BaseDatos(cfg["db"]["path"])
        self.ia = ModuloIA(cfg["ia"], cfg["db"]["path"])
        self.est = cfg["estacion"]
        b = cfg["local"]["base"]
        self.t_tele, self.t_ev = f"{b}/+/telemetria", f"{b}/+/eventos"
        self.t_estado = f"{b}/+/estado"
        self.parar = threading.Event()
        self.ia_pendiente = threading.Event()
        self.eventos_pendientes = threading.Event()
        self.ultimo_estado = {}
        self.nube_ok = False

        # --- cliente del broker local (Mosquitto) ---
        self.local = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"gateway-{self.est}")
        if cfg["local"]["user"]:
            self.local.username_pw_set(cfg["local"]["user"], cfg["local"]["password"])
        self.local.on_connect = self._local_conectado
        self.local.on_message = self._local_mensaje
        self.local.reconnect_delay_set(1, 30)

        # --- cliente de Ubidots ---
        u = cfg["ubidots"]
        self.nube = None
        if u["enabled"]:
            self.t_nube = f"/v1.6/devices/{u['device']}"
            self.t_silenciar = f"/v1.6/devices/{u['device']}/silenciar/lv"
            self.nube = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"aqm-gw-{self.est}-{os.getpid()}")
            self.nube.username_pw_set(u["token"], "")   # Ubidots: usuario = token, clave vacia
            if u["tls"]:
                self.nube.tls_set()
            self.nube.on_connect = self._nube_conectada
            self.nube.on_disconnect = self._nube_desconectada
            self.nube.on_message = self._nube_mensaje
            self.nube.reconnect_delay_set(2, 60)

    # ------------------------------------------------------------------ local
    def _local_conectado(self, cli, ud, flags, rc, props=None):
        if rc == 0:
            log.info("Conectado al broker local")
            cli.subscribe([(self.t_tele, 0), (self.t_ev, 1), (self.t_estado, 1)])
        else:
            log.error("Broker local rechazo la conexion: %s", rc)

    def _local_mensaje(self, cli, ud, msg):
        ahora = time.time()
        partes = msg.topic.split("/")
        if len(partes) < 3:
            return
        est, tipo = partes[1], partes[2]
        texto = msg.payload.decode("utf-8", "replace")
        try:
            if tipo == "telemetria":
                self._guardar_telemetria(est, json.loads(texto), ahora)
            elif tipo == "eventos":
                d = json.loads(texto)
                self.db.ejecutar("INSERT INTO eventos(ts, estacion, evento, codigo) VALUES (?,?,?,?)",
                                 (ahora, est, d.get("evento", "?"), d.get("codigo")))
                log.info("[%s] evento %s", est, d.get("evento"))
                self.ia_pendiente.set()
                self.eventos_pendientes.set()               # el ciclo principal los sube de inmediato
            elif tipo == "estado":
                self.db.ejecutar("INSERT INTO conexion(ts, estacion, estado) VALUES (?,?,?)", (ahora, est, texto))
                log.info("[%s] ESP32 %s", est, texto)
                self._publicar_nube({"esp32_online": {"value": 1 if texto == "online" else 0}})
        except (ValueError, KeyError) as e:
            log.warning("Mensaje invalido en %s: %s (%s)", msg.topic, texto[:120], e)

    @staticmethod
    def limpiar(d):
        """Procesamiento local: anula las variables de sensores en falla o fuera de rango."""
        d = dict(d)
        fallas = int(d.get("fallas") or 0)
        por_falla = {1: ("temp", "hum"), 2: ("presion", "presDelta"), 4: ("uvIdx",), 8: ("nivel", "dist")}
        for bit, claves in por_falla.items():
            if fallas & bit:
                for k in claves:
                    d[k] = None
        for k, (lo, hi) in {"nivel": RANGOS["nivel"], "temp": RANGOS["temp"], "hum": RANGOS["hum"],
                            "presion": RANGOS["presion"], "uvIdx": RANGOS["uv"], "evap": RANGOS["evap"]}.items():
            v = d.get(k)
            try:
                if v is not None and not (lo <= float(v) <= hi):
                    d[k] = None
            except (TypeError, ValueError):
                d[k] = None
        if (fallas & 3) and d.get("evap") is not None:
            d["evap"] = None          # el indice de evaporacion depende de T/HR
        valida = d.get("nivel") is not None and d.get("estado") != "ERROR_SENSOR"
        return d, valida

    def _guardar_telemetria(self, est, d, ahora):
        d, valida = self.limpiar(d)
        self.db.ejecutar(
            "INSERT INTO mediciones(ts, estacion, seq, nivel, dist, temp, hum, presion, pres_delta, presion_cae,"
            " uv, evap, tasa, esperada, perdida, tendencia, estado, estado_cod, fallas, silenciada, rssi, valida)"
            " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            (ahora, est, d.get("seq"), d.get("nivel"), d.get("dist"), d.get("temp"), d.get("hum"),
             d.get("presion"), d.get("presDelta"), d.get("presionCae"), d.get("uvIdx"), d.get("evap"),
             d.get("tasa"), d.get("esperada"), d.get("perdida"), d.get("tendencia"), d.get("estado"),
             d.get("estadoCod"), d.get("fallas"), d.get("silenciada"), d.get("rssi"), 1 if valida else 0))
        if self.ultimo_estado.get(est) != d.get("estado"):
            self.ultimo_estado[est] = d.get("estado")
            self.ia_pendiente.set()                         # cambio de estado -> IA de inmediato

    # ------------------------------------------------------------------- nube
    def _nube_conectada(self, cli, ud, flags, rc, props=None):
        if rc == 0:
            self.nube_ok = True
            log.info("Conectado a Ubidots")
            cli.subscribe(self.t_silenciar, 1)
            # crea/deja en 0 la variable del interruptor para que exista en el tablero
            cli.publish(self.t_nube, json.dumps({"silenciar": {"value": 0}}), qos=1)
        else:
            log.error("Ubidots rechazo la conexion (revise el token): %s", rc)

    def _nube_desconectada(self, cli, ud, flags, rc, props=None):
        self.nube_ok = False
        log.warning("Sin conexion con Ubidots; los datos quedan en SQLite para reenviar")

    def _nube_mensaje(self, cli, ud, msg):
        """Interruptor 'silenciar' del tablero de Ubidots."""
        try:
            valor = float(msg.payload.decode())
        except ValueError:
            return
        if valor < 1:
            return                                          # es el reinicio del interruptor
        ahora = time.time()
        cur = self.db.ejecutar("INSERT INTO ordenes(ts, estacion, origen, orden) VALUES (?,?,?,?)",
                               (ahora, self.est, "ubidots", "silenciar"))
        topic = f"{self.cfg['local']['base']}/{self.est}/cmd"
        info = self.local.publish(topic, json.dumps({"orden": "silenciar", "origen": "ubidots"}), qos=1)
        if info.rc == mqtt.MQTT_ERR_SUCCESS:
            self.db.ejecutar("UPDATE ordenes SET reenviada=1 WHERE id=?", (cur.lastrowid,))
        log.info("Orden 'silenciar' desde Ubidots reenviada al ESP32 (%s)", topic)
        self._publicar_nube({"silenciar": {"value": 0}})    # deja el interruptor listo para la proxima

    def _publicar_nube(self, payload, esperar=False):
        if not (self.nube and self.nube_ok):
            return False
        info = self.nube.publish(self.t_nube, json.dumps(payload), qos=1)
        if esperar:
            try:
                info.wait_for_publish(timeout=5)
            except (RuntimeError, ValueError):
                return False
            return info.is_published()
        return info.rc == mqtt.MQTT_ERR_SUCCESS

    def _enviar_mediciones(self):
        """Agrega las mediciones pendientes por intervalos y las sube con su marca de tiempo."""
        if not (self.nube and self.nube_ok):
            return
        intervalo = self.cfg["ubidots"]["intervalo_s"]
        filas = self.db.consultar(
            "SELECT id, ts, nivel, temp, hum, presion, uv, evap, tasa, perdida, estado_cod, fallas FROM mediciones "
            "WHERE enviado=0 AND estacion=? ORDER BY ts LIMIT 5000", (self.est,))
        if not filas:
            return
        lotes = {}
        for f in filas:
            lotes.setdefault(int(f[1] // intervalo), []).append(f)
        claves = sorted(lotes)
        if claves[-1] == int(time.time() // intervalo) and len(claves) > 0:
            claves = claves[:-1]                            # el intervalo actual aun no termina
        for k in claves[: self.cfg["ubidots"]["max_lotes"]]:
            grupo = lotes[k]
            validas = [f for f in grupo if f[10] != 4]      # 4 = ERROR_SENSOR
            ts_ms = int(grupo[-1][1] * 1000)
            payload = {}
            for i, nombre in enumerate(VARIABLES_NUBE, start=2):
                vals = [f[i] for f in validas if f[i] is not None]
                if vals:
                    payload[nombre] = {"value": round(sum(vals) / len(vals), 2), "timestamp": ts_ms}
            payload["estado"] = {"value": grupo[-1][10] or 0, "timestamp": ts_ms}
            mascara = 0
            for f in grupo:
                mascara |= f[11] or 0
            payload["fallas"] = {"value": mascara, "timestamp": ts_ms}
            if not self._publicar_nube(payload, esperar=True):
                return                                      # se reintenta en el siguiente ciclo
            ids = [f[0] for f in grupo]
            self.db.ejecutar(f"UPDATE mediciones SET enviado=1 WHERE id IN ({','.join('?' * len(ids))})", ids)
        if claves:
            log.info("Enviados %d intervalos a Ubidots", min(len(claves), self.cfg["ubidots"]["max_lotes"]))

    def _enviar_eventos(self):
        if not (self.nube and self.nube_ok):
            return
        for id_, ts, ev, cod in self.db.consultar(
                "SELECT id, ts, evento, codigo FROM eventos WHERE enviado=0 AND estacion=? ORDER BY ts", (self.est,)):
            payload = {"evento": {"value": cod if cod is not None else -1, "timestamp": int(ts * 1000),
                                  "context": {"evento": ev, "estacion": self.est}}}
            if not self._publicar_nube(payload, esperar=True):
                return
            self.db.ejecutar("UPDATE eventos SET enviado=1 WHERE id=?", (id_,))

    # --------------------------------------------------------------------- IA
    def _ejecutar_ia(self):
        res = self.ia.evaluar(self.est)          # usa su propia conexion de solo lectura
        if not res:
            return
        # -1 = estable o subiendo; -2 = aun no hay historico suficiente para pronosticar
        horas = res["horas_critico"] if res["horas_critico"] is not None else -2
        self.db.ejecutar(
            "INSERT INTO predicciones(ts, estacion, nivel, pendiente, r2, horas_critico, anomalia, z_anomalia,"
            " recomendacion, fuente) VALUES (?,?,?,?,?,?,?,?,?,?)",
            (time.time(), self.est, res["nivel"], res["pendiente"], res["r2"], res["horas_critico"],
             1 if res["anomalia"] else 0, res["z"], res["recomendacion"], res["fuente"]))
        log.info("IA: estado=%s horas=%s pendiente=%s anomalia=%s fuente=%s", res["estado"], horas,
                 res["pendiente"], res["anomalia"], res["fuente"])
        # al ESP32 (tablero local); retenido para que lo reciba apenas se conecte
        local = {"horas_critico": horas, "anomalia": 1 if res["anomalia"] else 0,
                 "recomendacion": res["recomendacion"][:190]}
        self.local.publish(f"{self.cfg['local']['base']}/{self.est}/ia",
                           json.dumps(local, ensure_ascii=False), qos=1, retain=True)
        # a la nube
        self._publicar_nube({
            "ia_horas_critico": {"value": horas, "context": {"recomendacion": res["recomendacion"],
                                                             "fuente": res["fuente"]}},
            "ia_anomalia": {"value": 1 if res["anomalia"] else 0},
        })

    # ------------------------------------------------------------------ ciclo
    def _limpieza(self):
        limite = time.time() - self.cfg["db"]["retencion_dias"] * 86400
        for tabla in ("mediciones", "eventos", "predicciones"):
            self.db.ejecutar(f"DELETE FROM {tabla} WHERE ts < ? AND enviado = 1", (limite,))
        self.db.ejecutar("DELETE FROM conexion WHERE ts < ?", (limite,))
        self.db.ejecutar("DELETE FROM ordenes WHERE ts < ?", (limite,))

    def ejecutar(self):
        c = self.cfg
        self.local.connect_async(c["local"]["host"], c["local"]["port"], keepalive=30)
        self.local.loop_start()
        if self.nube:
            if not c["ubidots"]["token"]:
                log.warning("Ubidots habilitado sin token: no se podra conectar")
            self.nube.connect_async(c["ubidots"]["host"], c["ubidots"]["port"], keepalive=30)
            self.nube.loop_start()

        t_envio = t_ia = t_limp = 0.0
        while not self.parar.is_set():
            ahora = time.time()
            try:
                if self.eventos_pendientes.is_set():
                    self.eventos_pendientes.clear()
                    self._enviar_eventos()
                if ahora - t_envio >= min(10, c["ubidots"]["intervalo_s"]):
                    t_envio = ahora
                    self._enviar_eventos()
                    self._enviar_mediciones()
                if c["ia"]["enabled"] and (self.ia_pendiente.is_set() or ahora - t_ia >= c["ia"]["intervalo_s"]):
                    self.ia_pendiente.clear()
                    t_ia = ahora
                    self._ejecutar_ia()
                if ahora - t_limp >= 86400:
                    t_limp = ahora
                    self._limpieza()
            except Exception:                                # el servicio nunca debe caerse por un ciclo
                log.exception("Error en el ciclo principal")
            self.parar.wait(1)

        self.local.loop_stop()
        self.local.disconnect()
        if self.nube:
            self.nube.loop_stop()
            self.nube.disconnect()
        log.info("Gateway detenido")


def main():
    ap = argparse.ArgumentParser(description="Gateway IoT AquaMonitor")
    ap.add_argument("--config", default="gateway.ini")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    gw = Gateway(cargar_config(a.config))
    signal.signal(signal.SIGTERM, lambda *_: gw.parar.set())
    signal.signal(signal.SIGINT, lambda *_: gw.parar.set())
    gw.ejecutar()


if __name__ == "__main__":
    main()
