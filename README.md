# 💧 AquaMonitor — Ecosistema IoT de monitoreo y alerta hídrica

![ESP32](https://img.shields.io/badge/dispositivo-ESP32-blue)
![Gateway](https://img.shields.io/badge/gateway-Raspberry%20Pi-c51a4a)
![MQTT](https://img.shields.io/badge/protocolo-MQTT-660066)
![Ubidots](https://img.shields.io/badge/nube-Ubidots-00a650)

Ecosistema IoT de bajo costo para monitorear tanques y reservorios en la región Sabana Centro (Cundinamarca) durante el fenómeno de El Niño 2026. Mide nivel de agua y variables meteorológicas, las combina mediante **lógica de fusión**, alerta **en sitio** (OLED y buzzer) y en tableros **local** y **global**, y usa **IA** para pronosticar cuándo se llegará al nivel crítico y recomendar acciones.

📖 **[Wiki del proyecto](../../wiki)** · 🎥 Video Challenge #3: `[ENLACE]`

## Arquitectura (Challenge #3)

```
 ┌──────────────────────┐  MQTT   ┌──────────────────────────────┐  MQTT/TLS  ┌──────────────┐
 │ ESP32 · v4.0         │ ──────► │ Raspberry Pi · Gateway IoT   │ ─────────► │ Ubidots      │
 │ sensores + fusión    │ ◄────── │ Mosquitto · SQLite · Python  │ ◄───────── │ tablero      │
 │ OLED + buzzer        │ órdenes │ IA: pronóstico, anomalías,   │  silenciar │ global +     │
 │ tablero local (WLAN) │   e IA  │ recomendación (Gemini)       │            │ notificación │
 └──────────────────────┘         └──────────────────────────────┘            └──────────────┘
```

## Estructura del repositorio

```
Challenge-IOT/
├── firmware/
│   └── AquaMonitor_v4_0/AquaMonitor_v4_0.ino   # ESP32 (Challenge #3): medición + MQTT
├── AquaMonitor_v3_2.ino                         # ESP32 (Challenge #2)
├── gateway/                                     # Raspberry Pi
│   ├── aquamonitor_gateway.py                   # MQTT → SQLite → procesamiento → Ubidots
│   ├── ia.py                                    # pronóstico, anomalías, recomendación
│   ├── schema.sql                               # base de datos local
│   ├── gateway.example.ini                      # configuración (copiar como gateway.ini)
│   ├── mosquitto/                               # broker con usuarios y lista de acceso
│   ├── systemd/                                 # arranque automático
│   ├── instalar_gateway.sh                      # instalación en la Raspberry Pi
│   ├── simulador_esp32.py                       # pruebas sin hardware
│   └── reporte_pruebas.py                       # persistencia, eventos, IA (y CSV)
└── docs/
    └── ubidots.md                               # tablero, notificaciones, interruptor y usuarios
```

## Equipo

Universidad de La Sabana — Internet de las Cosas 2026-2 · Profesor: Andrés Felipe Beltrán

| Integrante | Capa a cargo (Challenge #3) |
|---|---|
| Federico Valdez Muñoz | Dispositivo IoT (firmware v4.0) e IA predictiva |
| Victor Andrés Luna | Gateway IoT (Raspberry Pi, Mosquitto, SQLite) |
| Diego Alejandro Gómez | Nube (Ubidots), IA generativa y documentación |

## Puesta en marcha

### 1. Gateway (Raspberry Pi 5)

```bash
cd gateway
sudo bash instalar_gateway.sh            # Mosquitto, Python, usuarios MQTT y servicio
sudo nano /opt/aquamonitor/gateway/gateway.ini   # token de Ubidots, clave de Gemini, claves MQTT
sudo systemctl start aquamonitor-gateway
journalctl -u aquamonitor-gateway -f
```

Detalles en [`gateway/README.md`](gateway/README.md).

### 2. ESP32

1. Arduino IDE con el núcleo **esp32 by Espressif** (verificado con 2.0.17) y la placa **ESP32 Dev Module**.
2. Librerías: `DHT sensor library`, `Adafruit Unified Sensor`, `Adafruit GFX Library`, `Adafruit SH110X`, `Adafruit BMP085 Library`, `ESPAsyncWebServer`, `AsyncTCP`, **`PubSubClient`**.
3. En `AquaMonitor_v4_0.ino`, completar `WIFI_SSID`, `WIFI_PASS`, `WEB_USER`, `WEB_PASS`, `MQTT_USER` y `MQTT_PASS`. `MQTT_HOST` viene como `aquamonitor.local` (el ESP32 encuentra la Raspberry por su nombre aunque cambie su IP); también acepta una IP fija.
4. Cargar el código y abrir el monitor serie a 115200 baudios.

### 3. Ubidots

Seguir [`docs/ubidots.md`](docs/ubidots.md): token, tablero, notificaciones e interruptor "Silenciar".

### Probar sin hardware

```bash
python3 gateway/simulador_esp32.py --host <IP_RPI> --user esp32_est01 --password <CLAVE> --escenario sequia --acelerado 120
```

## Lógica de fusión (dispositivo)

| Estado | Regla |
|---|---|
| `ERROR_SENSOR` | 5 ciclos sin eco válido del HC-SR04 |
| `DESBORDE` | nivel ≥ 95 % (sale con < 90 %) |
| `FUGA` | el nivel baja y la pérdida real supera la esperada por evaporación en ≥ 2,5 %/h |
| `SEQUIA` | nivel ≤ 20 % (sale con > 25 %), o nivel ≤ 35 % con evaporación ≥ 70 (≥ 55 si la presión cae) |
| `OK` | ninguna de las anteriores |

## Seguridad

- Tablero local: solo la subred de la WLAN y usuario/contraseña.
- Mosquitto: sin acceso anónimo; cada cliente con usuario y lista de temas permitidos.
- Gateway ↔ Ubidots por TLS; el token y la clave de Gemini solo viven en `gateway.ini` en la Raspberry Pi (**ignorado por git**).

---

Proyecto académico — Universidad de La Sabana, 2026-2.
