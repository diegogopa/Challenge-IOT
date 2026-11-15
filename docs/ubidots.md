# Guía de configuración de Ubidots (tablero global)

El Gateway publica en Ubidots por MQTT. Las variables se crean solas la primera vez que llegan datos; esta guía explica cómo armar el tablero, las notificaciones y el control para silenciar la alarma.

## 1. Cuenta, token y dispositivo

1. Crear una cuenta (el plan **STEM** es gratuito para uso educativo; revisar sus límites de dispositivos, variables y puntos por día).
2. En el menú del usuario → **API Credentials**, copiar el **Default token** (empieza por `BBUS-`). Va en `token` de `gateway.ini`. **No subirlo a GitHub.**
3. El dispositivo se crea solo con el nombre de `device_label` (por defecto `aquamonitor-est01`) al primer envío. Iniciar el Gateway y revisar en **Devices** que aparezca.
4. Broker: `industrial.api.ubidots.com`, puerto **8883 con TLS** (o 1883 sin cifrar), **también para cuentas STEM** (el antiguo `things.ubidots.com` está en retiro). Usuario = token, contraseña vacía.

### Cuenta STEM (gratuita)

STEM limita a **3 dispositivos, 10 variables por dispositivo, 4000 puntos por día, 1 envío por segundo y 1 tablero de 10 widgets**. Si la cuenta es STEM (la dirección del navegador empieza por `stem.ubidots.com`), en la sección `[ubidots]` de `gateway.ini` poner:

```ini
token = (el Default token de stem.ubidots.com → API Credentials)
intervalo_s = 300
variables = nivel,temp,hum,presion,uv,evap
device_label_control = aquamonitor-est01-control
min_s_entre_envios = 1.2
```

Así quedan **dos dispositivos**: `aquamonitor-est01` con 8 variables de medición (promedio cada 5 min: 8 × 288 = 2304 puntos/día) y `aquamonitor-est01-control` con `silenciar`, `esp32_online`, `evento`, `ia_horas_critico`, `ia_anomalia` e `ia_alerta`. En el tablero, el interruptor y los widgets de IA, eventos y conexión se toman de este segundo dispositivo. `tasa` y `perdida` siguen guardándose en SQLite, pero no se suben.

## 2. Variables que publica el Gateway

| Variable | Valores | Uso |
|---|---|---|
| `nivel` | 0–100 % | Valor actual e histórico |
| `temp`, `hum`, `presion`, `uv`, `evap` | unidades físicas / índice | Clima |
| `tasa`, `perdida` | %/h | Análisis de fuga (solo si no es STEM; con STEM se quedan en SQLite) |
| `estado` | 0 OK · 1 DESBORDE · 2 FUGA · 3 SEQUÍA · 4 ERROR_SENSOR | Indicador y notificaciones |
| `fallas` | máscara: 1 DHT22 · 2 BMP180 · 4 UV · 8 ultrasónico · 16 OLED · 32 WiFi · 64 Gateway | Notificación de falla de sensor |
| `evento` | código del evento; el nombre va en el *context* | Registro de eventos |
| `esp32_online` | 1 / 0 | Notificar si el equipo de campo se desconecta |
| `ia_horas_critico` | horas (−1 estable, −2 calculando); la recomendación va en el *context* | Pronóstico |
| `ia_anomalia` | 1 / 0 | Descenso anormal |
| `ia_alerta` | 1 / 0 | 1 si el nivel crítico se pronostica en `horas_alerta` (72 h) o menos: base de la notificación |
| `silenciar` | interruptor 0/1 | Desactivar la alarma física |

Las mediciones llegan **promediadas por intervalo** (`intervalo_s`: 60 s por defecto, 300 s con STEM) y **con su marca de tiempo original**, incluso las que se reenvían después de un corte de Internet.

## 3. Tablero

**Data → Dashboards → +**, nombre "AquaMonitor – Estación 01". Son los 10 widgets que permite el plan STEM (`est01` = `aquamonitor-est01`, `control` = `aquamonitor-est01-control`):

| Widget | Dispositivo → variable | Configuración |
|---|---|---|
| Tanque (*Tank*, o *Gauge*) | est01 → `nivel` | 0–100 %. Título "Nivel del tanque" |
| Métrica | est01 → `estado` | Título "Estado (0 OK · 1 Desborde · 2 Fuga · 3 Sequía · 4 Error)" |
| Gráfica de líneas | est01 → `nivel`, `evap` | Últimas 24 h |
| Gráfica de líneas | est01 → `temp`, `hum`, `presion` | Últimas 24 h; presión en un eje Y secundario |
| Métrica | est01 → `fallas` | Título "Fallas de sensores (0 = ninguna)" |
| Métrica | control → `ia_horas_critico` | Título "IA: horas a nivel crítico (−1 estable, −2 calculando)" |
| Indicador | control → `ia_anomalia` | 0 verde "Normal", 1 rojo "Anomalía" |
| Tabla de valores | control → `ia_horas_critico` y `evento` | Columnas Date, Value y **Context** (claves `recomendacion` y `evento`): recomendación de la IA y registro de eventos |
| Indicador | control → `esp32_online` | 0 rojo "Desconectado", 1 verde "Conectado" |
| **Interruptor (*Switch*)** | control → `silenciar` | "Silenciar alarma física". Al activarlo, el Gateway reenvía la orden al ESP32 y vuelve a poner el interruptor en 0 |

## 4. Notificaciones (*Events*)

**Data → Events → + Create Event**. El plan STEM permite **5 reglas** y 100 correos al mes, así que se agrupan. Acción: **Send Email**, sin repetición.

| Regla | Condición | Mensaje sugerido |
|---|---|---|
| Alarma del tanque | est01 → `estado` ≥ 1 | "AquaMonitor est01: [Variable name] = [Trigger value] a las [Trigger timestamp]. Estados: 1 Desborde · 2 Fuga · 3 Sequía · 4 Error." |
| Falla de sensor | est01 → `fallas` > 0 | "AquaMonitor est01: sensor con falla (código [Trigger value])." |
| Equipo desconectado | control → `esp32_online` = 0 | "AquaMonitor est01: equipo de campo desconectado." |
| Pronóstico crítico | control → `ia_alerta` = 1 | "AquaMonitor est01: la IA pronostica nivel crítico en menos de 72 h. Ver recomendación en el tablero." |
| Anomalía | control → `ia_anomalia` = 1 | "AquaMonitor est01: descenso anormal del nivel." |

Con un plan pago se pueden separar en una regla por estado y usar SMS o Telegram.

## 5. Acceso restringido

- El tablero **no se comparte con enlace público**.
- Cada autoridad entra con **usuario y contraseña** propios. Si el plan lo permite (*Users / Organizations*), crear usuarios con permiso de **solo lectura** y solo un rol con permiso para usar el interruptor. En el plan STEM, el acceso queda limitado a la cuenta del equipo, con contraseña robusta y verificación en dos pasos.
- El token solo está en la Raspberry Pi (`gateway.ini`, permisos 600). El ESP32 nunca lo conoce.

## 6. Prueba rápida

1. Con el Gateway corriendo, cambiar el estado en el banco de pruebas (o usar `simulador_esp32.py --escenario sequia`).
2. Verificar que el evento aparece de inmediato en la tabla de eventos y que, al cerrar el intervalo (hasta 5 min con STEM), `estado` cambia y llega el correo.
3. Activar el interruptor **Silenciar**: en el ESP32 debe apagarse el buzzer y aparecer el evento `SILENCIADA_NUBE`; en el Gateway, la línea *Orden 'silenciar' desde Ubidots reenviada al ESP32*.
