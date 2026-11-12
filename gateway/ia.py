"""
Modulo de IA del Gateway AquaMonitor.

Dos partes, con responsabilidades separadas:

1. IA predictiva (modelo propio, sin dependencias externas):
   - Pronostico: regresion lineal por minimos cuadrados sobre el nivel
     reciente guardado en SQLite -> pendiente (%/h) -> horas que faltan
     para llegar al nivel critico.
   - Deteccion de anomalias: puntaje z robusto (mediana y MAD) de la tasa
     de cambio mas reciente frente a su historial. Un descenso muy por
     fuera de lo habitual se marca como anomalia.

2. IA generativa (API de Gemini):
   - Recibe SOLO datos ya calculados (estado, nivel, pronostico, anomalia,
     clima) y redacta una recomendacion corta para la autoridad, alineada
     con las fases del plan de contingencia. No calcula ni inventa cifras.
   - Si no hay clave, no hay Internet o la API falla, se usa una
     recomendacion por reglas (el sistema nunca se queda sin respuesta).
"""
import json
import logging
import sqlite3
import statistics
import time
import urllib.error
import urllib.request

log = logging.getLogger("ia")


# ----------------------------------------------------------------------------
# 1. IA PREDICTIVA
# ----------------------------------------------------------------------------
def regresion_lineal(xs, ys):
    """Minimos cuadrados. Devuelve (pendiente, intercepto, r2)."""
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return 0.0, my, 0.0
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    m = sxy / sxx
    b = my - m * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (m * x + b)) ** 2 for x, y in zip(xs, ys))
    r2 = 1 - ss_res / ss_tot if ss_tot > 0 else 0.0
    return m, b, r2


def pronostico_nivel(serie, nivel_critico, min_puntos=6, min_span_s=1200,
                     pendiente_minima=0.05):
    """
    serie: lista de (ts_epoch_s, nivel_%) ordenada por tiempo.
    Devuelve dict con nivel actual estimado, pendiente (%/h), r2 y
    horas_critico (-1 si el nivel esta estable o subiendo, None si no hay
    datos suficientes).
    """
    if len(serie) < min_puntos or serie[-1][0] - serie[0][0] < min_span_s:
        return {"horas_critico": None, "pendiente": None, "r2": None,
                "nivel": serie[-1][1] if serie else None, "puntos": len(serie)}
    t0 = serie[0][0]
    xs = [(t - t0) / 3600.0 for t, _ in serie]          # horas
    ys = [n for _, n in serie]
    m, b, r2 = regresion_lineal(xs, ys)
    nivel_ajustado = min(100.0, max(0.0, m * xs[-1] + b))   # valor ajustado (menos ruido), acotado
    nivel_medido = ys[-1]
    if nivel_medido <= nivel_critico or nivel_ajustado <= nivel_critico:
        horas = 0.0
    elif m < -pendiente_minima:
        horas = (nivel_ajustado - nivel_critico) / (-m)
    else:
        horas = -1.0                                     # estable o subiendo
    return {"horas_critico": round(horas, 2), "pendiente": round(m, 3),
            "r2": round(r2, 3), "nivel": round(nivel_medido, 1), "puntos": len(serie)}


def detectar_anomalia(tasas, umbral_z=3.5, mad_minimo=0.2):
    """
    tasas: historial de la tasa de cambio del nivel (%/h), la ultima es la actual.
    Puntaje z robusto: z = (x - mediana) / (1.4826 * MAD).
    Solo interesan los descensos anormales (z negativo).
    """
    if len(tasas) < 10:
        return {"anomalia": False, "z": None}
    historial, actual = tasas[:-1], tasas[-1]
    med = statistics.median(historial)
    mad = statistics.median([abs(x - med) for x in historial])
    escala = max(1.4826 * mad, mad_minimo)
    z = (actual - med) / escala
    return {"anomalia": z <= -umbral_z, "z": round(z, 2)}


# ----------------------------------------------------------------------------
# 2. RECOMENDACION (reglas y Gemini)
# ----------------------------------------------------------------------------
def texto_horas(h):
    if h is None or h <= -1.5:
        return "sin datos suficientes para pronosticar"
    if h < 0:
        return "nivel estable o en aumento"
    if h == 0:
        return "nivel critico alcanzado o inminente"
    if h >= 48:
        return f"nivel critico en aprox. {h / 24:.1f} dias"
    return f"nivel critico en aprox. {h:.1f} h"


def recomendacion_reglas(ctx):
    """Recomendacion de respaldo basada en las fases del plan de contingencia."""
    estado, nivel, h = ctx["estado"], ctx.get("nivel") or 0, ctx.get("horas_critico")
    if estado == "ERROR_SENSOR":
        return "Falla del sensor de nivel: enviar tecnico y usar la lectura manual de la regla mientras se repara."
    if estado == "DESBORDE":
        return "Riesgo de rebose: abrir la descarga o maniobrar compuertas y avisar aguas abajo."
    if estado == "FUGA" or ctx.get("anomalia"):
        return "Descenso anormal no explicado por el clima: inspeccionar valvulas, tuberias y conexiones ilegales hoy."
    if (estado == "SEQUIA" and nivel <= 20) or (h == 0 and nivel <= 25):
        return "Alerta roja: activar el plan de contingencia (racionamiento por sectores, carrotanques y prioridad a consumo humano)."
    if estado == "SEQUIA" or (h is not None and 0 <= h <= 72):
        return f"Alerta amarilla ({texto_horas(h)}): preparar racionamiento, informar a la comunidad y promover ahorro."
    if ctx.get("presionCae") and nivel <= 40:
        return "Presion en descenso con reserva moderada: vigilar de cerca y revisar el plan de racionamiento."
    return "Condicion normal: mantener monitoreo rutinario."


PROMPT_SISTEMA = (
    "Eres un asistente de gestion del riesgo hidrico para una alcaldia de la "
    "Sabana Centro (Colombia) durante el fenomeno de El Nino. Recibes datos ya "
    "calculados por una estacion IoT. Redacta UNA recomendacion operativa para "
    "la autoridad local, en espanol, de maximo 2 frases y 220 caracteres. "
    "Usa solo los datos entregados, no inventes cifras ni causas. Relaciona la "
    "accion con las fases del plan de contingencia: normal (monitoreo), alerta "
    "amarilla (preparar racionamiento y comunicar), alerta roja (activar "
    "racionamiento y carrotanques), fuga o anomalia (inspeccion inmediata), "
    "desborde (maniobra de descarga), falla de sensor (visita tecnica)."
)


def recomendacion_gemini(ctx, api_key, modelo, timeout=15):
    """Llama a la API de Gemini. Devuelve el texto o lanza excepcion."""
    datos = {
        "estado": ctx["estado"],
        "nivel_pct": ctx.get("nivel"),
        "pronostico": texto_horas(ctx.get("horas_critico")),
        "pendiente_pct_h": ctx.get("pendiente"),
        "anomalia_descenso": bool(ctx.get("anomalia")),
        "temperatura_c": ctx.get("temp"),
        "humedad_pct": ctx.get("hum"),
        "indice_evaporacion_0_100": ctx.get("evap"),
        "presion_en_descenso": bool(ctx.get("presionCae")),
        "fallas_sensores": ctx.get("fallas_txt") or "ninguna",
    }
    cuerpo = {
        "systemInstruction": {"parts": [{"text": PROMPT_SISTEMA}]},
        "contents": [{"role": "user", "parts": [{"text": "Datos de la estacion: " + json.dumps(datos, ensure_ascii=False)}]}],
        "generationConfig": {"temperature": 0.2, "maxOutputTokens": 120},
    }
    url = f"https://generativelanguage.googleapis.com/v1beta/models/{modelo}:generateContent"
    req = urllib.request.Request(url, data=json.dumps(cuerpo).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "x-goog-api-key": api_key})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        resp = json.loads(r.read().decode())
    texto = resp["candidates"][0]["content"]["parts"][0]["text"].strip()
    texto = " ".join(texto.split())
    if not texto:
        raise ValueError("respuesta vacia")
    return texto[:300]


NOMBRES_FALLA = {1: "DHT22", 2: "BMP180", 4: "UV", 8: "ultrasonico", 16: "OLED", 32: "WiFi", 64: "Gateway"}


def fallas_a_texto(mascara):
    return ", ".join(n for b, n in NOMBRES_FALLA.items() if (mascara or 0) & b)


class ModuloIA:
    """Orquesta pronostico + anomalia + recomendacion a partir de la base de datos."""

    def __init__(self, cfg, ruta_db):
        self.cfg = cfg
        self.ruta_db = ruta_db
        self.ultima_llamada_gemini = 0.0
        self.ultimo_estado_gemini = None
        self.ultima_reco = None

    def evaluar(self, estacion):
        # Conexion propia de solo lectura: la llamada a Gemini no bloquea las escrituras del Gateway.
        conn = sqlite3.connect(f"file:{self.ruta_db}?mode=ro", uri=True, timeout=5)
        try:
            return self._evaluar(conn, estacion)
        finally:
            conn.close()

    def _evaluar(self, conn, estacion):
        c = self.cfg
        ahora = time.time()
        filas = conn.execute(
            "SELECT ts, nivel, tasa, estado, temp, hum, evap, presion_cae, fallas FROM mediciones "
            "WHERE estacion=? AND ts>=? ORDER BY ts",
            (estacion, ahora - c["ventana_h"] * 3600)).fetchall()
        if not filas:
            return None
        ultima = filas[-1]
        serie = [(f[0], f[1]) for f in filas if f[1] is not None and f[3] != "ERROR_SENSOR"]
        pron = pronostico_nivel(serie, c["nivel_critico"], c["min_puntos"], c["min_span_s"])

        # historial de tasas (24 h); se toma un valor por cambio para no repetir la misma ventana
        tasas_filas = conn.execute(
            "SELECT tasa FROM mediciones WHERE estacion=? AND valida=1 AND ts>=? AND tasa IS NOT NULL ORDER BY ts",
            (estacion, ahora - 24 * 3600)).fetchall()
        tasas, previo = [], None
        for (t,) in tasas_filas:
            if t != previo and t != 0:
                tasas.append(t)
            previo = t
        anom = detectar_anomalia(tasas, c["umbral_z"])

        ctx = {
            "estado": ultima[3], "nivel": pron["nivel"] if pron["nivel"] is not None else ultima[1],
            "horas_critico": pron["horas_critico"], "pendiente": pron["pendiente"],
            "anomalia": anom["anomalia"], "temp": ultima[4], "hum": ultima[5], "evap": ultima[6],
            "presionCae": bool(ultima[7]), "fallas_txt": fallas_a_texto(ultima[8]),
        }
        texto, fuente = self._recomendar(ctx, ahora)
        return {**pron, **anom, "estado": ultima[3], "recomendacion": texto, "fuente": fuente, "ctx": ctx}

    def _recomendar(self, ctx, ahora):
        c = self.cfg
        cambio_estado = ctx["estado"] != self.ultimo_estado_gemini
        puede_llamar = (c["gemini_key"] and
                        (cambio_estado or ahora - self.ultima_llamada_gemini >= c["gemini_min_interval_s"]))
        if puede_llamar:
            try:
                texto = recomendacion_gemini(ctx, c["gemini_key"], c["gemini_model"])
                self.ultima_llamada_gemini, self.ultimo_estado_gemini = ahora, ctx["estado"]
                self.ultima_reco = (texto, "gemini")
                return texto, "gemini"
            except (urllib.error.URLError, KeyError, ValueError, TimeoutError, OSError) as e:
                log.warning("Gemini no disponible (%s); uso reglas", e)
                self.ultima_llamada_gemini = ahora           # evita reintentos en bucle
        elif c["gemini_key"] and self.ultima_reco and not cambio_estado:
            return self.ultima_reco                          # reutiliza mientras no toque llamar
        return recomendacion_reglas(ctx), "reglas"
