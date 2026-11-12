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

Así quedan **dos dispositivos**: `aquamonitor-est01` con 8 variables de medición (promedio cada 5 min: 8 × 288 = 2304 puntos/día) y `aquamonitor-est01-control` con `silenciar`, `esp32_online`, `evento`, `ia_horas_critico` e `ia_anomalia`. En el tablero, el interruptor y los widgets de IA, eventos y conexión se toman de este segundo dispositivo. `tasa` y `perdida` siguen guardándose en SQLite, pero no se suben.

## 2. Variables que publica el Gateway

| Variable | Valores | Uso |
|---|---|---|
| `nivel` | 0–100 % | Valor actual e histórico |
| `temp`, `hum`, `presion`, `uv`, `evap` | unidades físicas / índice | Clima |
| `tasa`, `perdida` | %/h | Análisis de fuga |
| `estado` | 0 OK · 1 DESBORDE · 2 FUGA · 3 SEQUÍA · 4 ERROR_SENSOR | Indicador y notificaciones |
| `fallas` | máscara: 1 DHT22 · 2 BMP180 · 4 UV · 8 ultrasónico · 16 OLED · 32 WiFi · 64 Gateway | Notificación de falla de sensor |
| `evento` | código del evento; el nombre va en el *context* | Registro de eventos |
| `esp32_online` | 1 / 0 | Notificar si el equipo de campo se desconecta |
| `ia_horas_critico` | horas (−1 estable, −2 calculando); la recomendación va en el *context* | Pronóstico |
| `ia_anomalia` | 1 / 0 | Descenso anormal |
| `ia_alerta` | 1 / 0 | 1 si el nivel crítico se pronostica en `horas_alerta` (72 h) o menos: base de la notificación |
| `silenciar` | interruptor 0/1 | Desactivar la alarma física |

Las mediciones llegan **promediadas por intervalo** (`intervalo_s`, 60 s por defecto) y **con su marca de tiempo original**, incluso las que se reenvían después de un corte de Internet.

## 3. Tablero

**Data → Dashboards → +**, nombre "AquaMonitor – Estación 01". Widgets sugeridos:

| Widget | Variable | Configuración |
|---|---|---|
| Tanque o medidor (*Tank / Gauge*) | `nivel` | 0–100 %, colores: rojo < 20, amarillo < 35 |
| Indicador (*Indicator*) | `estado` | 0 verde "OK", 1 rojo "DESBORDE", 2 morado "FUGA", 3 naranja "SEQUÍA", 4 gris "ERROR" |
| Métrica | `ia_horas_critico` | Título "Horas a nivel crítico" |
| Indicador | `ia_anomalia` | 0 "Normal", 1 "Anomalía" |
| Gráfica de líneas | `nivel` y `evap` | Últimas 24 h (histórico) |
| Gráfica de líneas | `temp`, `hum`, `presion` | Últimas 24 h |
| Tabla de valores (*Values table*) | `ia_horas_critico` | Muestra el *context* con la **recomendación de la IA** |
| Tabla de valores | `evento` | Registro de eventos con su nombre |
| Indicador | `esp32_online` | Conexión del equipo de campo |
| **Interruptor (*Switch*)** | `silenciar` | Etiqueta "Silenciar alarma física". Al activarlo, el Gateway reenvía la orden al ESP32 y vuelve a poner el interruptor en 0 |

## 4. Notificaciones (*Events*)

**Data → Events → +**. Crear una regla por condición; acción: correo, SMS o Telegram (según lo que permita el plan) a las autoridades autorizadas.

| Regla | Condición | Mensaje sugerido |
|---|---|---|
| Sequía | `estado` = 3 | "AquaMonitor est01: SEQUÍA. Nivel {{nivel}} %. Revisar el tablero." |
| Fuga | `estado` = 2 | "AquaMonitor est01: posible FUGA. Inspeccionar hoy." |
| Desborde | `estado` = 1 | "AquaMonitor est01: DESBORDE." |
| Falla del sensor de nivel | `estado` = 4 | "AquaMonitor est01: falla del sensor de nivel." |
| Falla de otro sensor | `fallas` > 0 | "AquaMonitor est01: sensor con falla (código {{value}})." |
| Equipo desconectado | `esp32_online` = 0 durante 5 min | "AquaMonitor est01: sin datos del equipo de campo." |
| Pronóstico | `ia_alerta` igual a 1 | "AquaMonitor est01: la IA pronostica nivel crítico en menos de 72 h. Ver recomendación en el tablero." |

## 5. Acceso restringido

- El tablero **no se comparte con enlace público**.
- Cada autoridad entra con **usuario y contraseña** propios. Si el plan lo permite (*Users / Organizations*), crear usuarios con permiso de **solo lectura** y solo un rol con permiso para usar el interruptor. En el plan STEM, el acceso queda limitado a la cuenta del equipo, con contraseña robusta y verificación en dos pasos.
- El token solo está en la Raspberry Pi (`gateway.ini`, permisos 600). El ESP32 nunca lo conoce.

## 6. Prueba rápida

1. Con el Gateway corriendo, cambiar el estado en el banco de pruebas (o usar `simulador_esp32.py --escenario sequia`).
2. Verificar que `estado` cambia en el tablero y que llega la notificación.
3. Activar el interruptor **Silenciar**: en el ESP32 debe apagarse el buzzer y aparecer el evento `SILENCIADA_NUBE`; en el Gateway, la línea *Orden 'silenciar' desde Ubidots reenviada al ESP32*.
