# Gateway IoT AquaMonitor (Raspberry Pi)

Recibe por MQTT lo que publica el ESP32, lo guarda en **SQLite**, lo procesa localmente, ejecuta el **módulo de IA** y lo sube a **Ubidots** por MQTT. También baja al ESP32 la orden de **silenciar** que se da desde el tablero de Ubidots.

```
ESP32 ──MQTT──► Mosquitto (RPi) ──► aquamonitor_gateway.py ──► SQLite
                                         │  validación + promedio por intervalo
                                         │  IA: pronóstico, anomalías, recomendación
                                         ▼
                                   Ubidots (MQTT/TLS) ──► tablero global + notificaciones
Ubidots (interruptor "silenciar") ──► Gateway ──► aquamonitor/est01/cmd ──► ESP32
```

## Archivos

| Archivo | Qué es |
|---|---|
| `aquamonitor_gateway.py` | Servicio principal |
| `ia.py` | Módulo de IA: regresión (pronóstico), z robusto (anomalías), Gemini o reglas (recomendación) |
| `schema.sql` | Tablas de la base de datos |
| `gateway.example.ini` | Configuración de ejemplo (copiar como `gateway.ini`, **no subirlo a GitHub**) |
| `mosquitto/` | Configuración segura del broker y lista de acceso |
| `systemd/` | Servicio de arranque automático |
| `instalar_gateway.sh` | Instalación en la Raspberry Pi |
| `simulador_esp32.py` | Publica como si fuera el ESP32 (pruebas sin hardware) |
| `reporte_pruebas.py` | Persistencia, pendientes, eventos, órdenes e IA desde la base (y exporta CSV) |

## Instalación en la Raspberry Pi

1. Raspberry Pi OS con red y SSH, con hostname **`aquamonitor`**: el firmware la busca como `aquamonitor.local`, así que no hace falta IP fija (sirve también con el hotspot de un celular).
2. Copiar la carpeta `gateway/` del repositorio a la Raspberry Pi y ejecutar:
   ```bash
   cd gateway
   sudo bash instalar_gateway.sh
   ```
   El script instala Mosquitto y Python, pide las contraseñas MQTT de `gateway` y `esp32_est01`, y deja el servicio habilitado.
3. Editar `/opt/aquamonitor/gateway/gateway.ini`: contraseña del usuario `gateway`, **token de Ubidots**, `device_label` y (opcional) **clave de Gemini**.
4. Iniciar y ver el registro:
   ```bash
   sudo systemctl start aquamonitor-gateway
   journalctl -u aquamonitor-gateway -f
   ```

## Comprobaciones rápidas

```bash
# Ver lo que publica el ESP32 (usuario gateway)
mosquitto_sub -h localhost -u gateway -P 'CLAVE' -t 'aquamonitor/#' -v

# Probar sin ESP32
/opt/aquamonitor/venv/bin/python simulador_esp32.py --host localhost --user esp32_est01 --password 'CLAVE' --escenario sequia --acelerado 120

# Comprobar que se rechaza un cliente sin credenciales (debe fallar: "not authorised")
mosquitto_pub -h localhost -t aquamonitor/est01/telemetria -m prueba

# Reporte para la wiki
/opt/aquamonitor/venv/bin/python reporte_pruebas.py --db /opt/aquamonitor/data/aquamonitor.db --csv resultados
```

## Temas MQTT

| Tema | Dirección | QoS | Contenido |
|---|---|---|---|
| `aquamonitor/est01/telemetria` | ESP32 → Gateway | 0 | JSON cada 10 s (nivel, T, HR, P, UV, evaporación, tasa, estado, fallas, `seq`) |
| `aquamonitor/est01/eventos` | ESP32 → Gateway | 0 | Cambios de estado, fallas, recuperaciones, silenciado |
| `aquamonitor/est01/estado` | ESP32 → Gateway | 1, retenido | `online` / `offline` (testamento LWT) |
| `aquamonitor/est01/cmd` | Gateway → ESP32 | 1 | `{"orden":"silenciar","origen":"ubidots"}` |
| `aquamonitor/est01/ia` | Gateway → ESP32 | 1, retenido | Pronóstico, anomalía y recomendación |
| `/v1.6/devices/<device>` | Gateway → Ubidots | 1 | Variables con su marca de tiempo |
| `/v1.6/devices/<device>/silenciar/lv` | Ubidots → Gateway | 1 | Interruptor del tablero |

## Comportamiento ante fallas

| Falla | Qué pasa |
|---|---|
| Sin Internet / Ubidots caído | Todo sigue guardándose en SQLite con `enviado=0`; al volver se sube **con la hora original** |
| Gemini no responde o no hay clave | Recomendación por reglas (fases del plan de contingencia) |
| ESP32 desconectado | Mosquitto publica `offline` (LWT); el Gateway lo registra y lo sube como `esp32_online = 0` |
| Reinicio de la Raspberry Pi | systemd vuelve a iniciar Mosquitto y el Gateway; la base y los mensajes retenidos persisten |
