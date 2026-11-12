-- Base de datos local del Gateway AquaMonitor (SQLite)
-- Todo lo que llega del ESP32 se guarda aqui ANTES de enviarse a la nube.
-- La columna "enviado" permite reenviar lo pendiente si se cae Internet.

PRAGMA journal_mode = WAL;

CREATE TABLE IF NOT EXISTS mediciones (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          REAL    NOT NULL,          -- hora de llegada al Gateway (epoch, s)
    estacion    TEXT    NOT NULL,
    seq         INTEGER,                   -- contador del ESP32 (detecta mensajes perdidos)
    nivel       REAL, dist REAL, temp REAL, hum REAL,
    presion     REAL, pres_delta REAL, presion_cae INTEGER,
    uv          REAL, evap REAL,
    tasa        REAL, esperada REAL, perdida REAL, tendencia INTEGER,
    estado      TEXT, estado_cod INTEGER, fallas INTEGER, silenciada INTEGER, rssi INTEGER,
    valida      INTEGER DEFAULT 1,         -- 0 si el Gateway la descarto por rango
    enviado     INTEGER DEFAULT 0          -- 1 cuando ya se incluyo en un envio a la nube
);
CREATE INDEX IF NOT EXISTS ix_med_ts ON mediciones(estacion, ts);
CREATE INDEX IF NOT EXISTS ix_med_env ON mediciones(enviado);

CREATE TABLE IF NOT EXISTS eventos (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        REAL NOT NULL,
    estacion  TEXT NOT NULL,
    evento    TEXT NOT NULL,               -- p. ej. SEQUIA, FALLA_BMP180, SILENCIADA_NUBE
    codigo    INTEGER,
    enviado   INTEGER DEFAULT 0
);
CREATE INDEX IF NOT EXISTS ix_ev_env ON eventos(enviado);

CREATE TABLE IF NOT EXISTS conexion (      -- online/offline del ESP32 (LWT)
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        REAL NOT NULL,
    estacion  TEXT NOT NULL,
    estado    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS ordenes (       -- ordenes que bajan desde la nube
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    ts        REAL NOT NULL,
    estacion  TEXT NOT NULL,
    origen    TEXT NOT NULL,               -- ubidots / local
    orden     TEXT NOT NULL,               -- silenciar
    reenviada INTEGER DEFAULT 0            -- 1 cuando se publico al ESP32
);

CREATE TABLE IF NOT EXISTS predicciones (  -- resultados del modulo de IA
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    ts              REAL NOT NULL,
    estacion        TEXT NOT NULL,
    nivel           REAL,
    pendiente       REAL,                  -- %/h estimado por la regresion
    r2              REAL,
    horas_critico   REAL,                  -- -1 = estable o subiendo
    anomalia        INTEGER,
    z_anomalia      REAL,
    recomendacion   TEXT,
    fuente          TEXT,                  -- gemini / reglas
    enviado         INTEGER DEFAULT 0
);
