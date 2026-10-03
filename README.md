# 💧 AquaMonitor v3.2 — Sistema IoT de monitoreo de disponibilidad y escasez de agua

![Platform](https://img.shields.io/badge/platform-ESP32-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B%20(Arduino)-orange)
![RTOS](https://img.shields.io/badge/FreeRTOS-tarea%20de%20sensado-green)
![License](https://img.shields.io/badge/license-Academic-lightgrey)

Prototipo IoT de bajo costo para monitorear puntos críticos de almacenamiento de agua en la región Sabana Centro (Cundinamarca) durante el fenómeno de El Niño 2026. Mide nivel de agua y variables meteorológicas, las combina mediante **lógica de fusión** y emite alertas **in situ** (OLED + buzzer) y en un **tablero de control web embebido** en el ESP32, accesible solo desde la WLAN de la zona.


---

## 👥 Equipo

Universidad de La Sabana — Facultad de Ingeniería · Internet de las Cosas 2026-2

| Integrante | Rol |
|---|---|
| Victor Andrés Luna | Hardware e integración |
| Federico Valdez Muñoz | Software y pruebas |
| Diego Alejandro Gómez | Documentación e integración |
| **Profesor:** Andrés Felipe Beltrán | |

---

## 🧩 Sensores y actuadores

| Componente | Variable / función | Interfaz | Pin ESP32 |
|---|---|---|---|
| HC-SR04 | Distancia → nivel (%) | TRIG / ECHO | GPIO 5 / GPIO 18 |
| DHT22 | Temperatura y humedad | 1 hilo | GPIO 4 |
| GUVA-S12SD | Radiación UV → índice UV | ADC | GPIO 34 |
| BMP180 | Presión atmosférica | I2C 0x77 | SDA 21 / SCL 23 |
| OLED SH1106 128×64 | Visualización local | I2C 0x3C | SDA 21 / SCL 23 |
| Buzzer activo | Alarma física | Digital | GPIO 33 |

---

## 🧠 Lógica de fusión

- **Nivel:** mediana de 5 disparos + EMA (α = 0.5), velocidad del sonido compensada con la temperatura del DHT22.
- **Índice de evaporación (0–100):** 0.40·T + 0.30·UV + 0.30·(100 − HR), normalizados.
- **Tendencia de presión:** caída ≥ 2 hPa en 3 h.

| Estado | Regla (prioridad de arriba a abajo) |
|---|---|
| `ERROR_SENSOR` | 5 ciclos seguidos sin eco válido del HC-SR04 |
| `DESBORDE` | nivel ≥ 95 % (sale con < 90 %) |
| `FUGA` | el nivel baja y la pérdida real supera a la esperada por evaporación en ≥ 2.5 %/h |
| `SEQUIA` | nivel ≤ 20 % (sale con > 25 %), **o** nivel ≤ 35 % con evaporación ≥ 70 (≥ 55 si la presión está cayendo) |
| `OK` | ninguna de las anteriores |

Además, cada componente (DHT22, BMP180, GUVA, ultrasónico, OLED, WiFi) tiene detección de falla y recuperación automática.

---

## 🏗️ Arquitectura

- **Tarea FreeRTOS `tareaSensado`** (núcleo 1): medición, filtrado, fusión, eventos e histórico — independiente del hilo principal.
- **`loop()`**: buzzer, OLED, reconexión WiFi y verificación de la OLED.
- **Servidor web asíncrono**: `GET /`, `GET /data`, `GET /historico`, `POST /silenciar`.
- **Sincronización:** `mutexDatos` (datos compartidos) y `mutexI2C` (bus OLED + BMP180).
- **Acceso al tablero:** solo IPs de la subred de la WLAN (403 si no) + usuario y contraseña (401 si no). Sin MQTT ni nube.
- Si la WLAN cae, la medición y las alertas locales siguen funcionando.

---

## 📁 Estructura del repositorio

```
Challenge-IOT/
├── firmware/
│   └── AquaMonitor_v3_2/
│       └── AquaMonitor_v3_2.ino   # Firmware principal
├── docs/
│   ├── esquematico/               # Esquemático (PNG/PDF + fuente)
│   ├── actas/                     # Actas de reunión
│   ├── pruebas/                   # Datos crudos del banco de pruebas (CSV)
│   └── fotos/                     # Prototipo, OLED y tablero
└── README.md
```

---

## ⚙️ Compilación

1. Arduino IDE con el núcleo **esp32 by Espressif Systems**; placa **ESP32 Dev Module**.
2. Librerías: `DHT sensor library`, `Adafruit GFX Library`, `Adafruit SH110X`, `Adafruit BMP085 Library`, `ESPAsyncWebServer`, `AsyncTCP`.
3. Editar `WIFI_SSID`, `WIFI_PASS`, `WEB_USER` y `WEB_PASS` (el repositorio solo tiene valores de ejemplo).
4. Ajustar `DIST_TANQUE_LLENO` y `DIST_TANQUE_VACIO` a la geometría del tanque.
5. Cargar, abrir el monitor serie a 115200 baudios y entrar a la IP que muestra la OLED desde un dispositivo de la misma WLAN.

---

## 📄 Licencia

Proyecto académico — Universidad de La Sabana, 2026-2.
