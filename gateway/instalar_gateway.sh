#!/usr/bin/env bash
# Instalacion del Gateway AquaMonitor en una Raspberry Pi (Raspberry Pi OS / Debian).
# Ejecutar desde la carpeta gateway/ del repositorio:   sudo bash instalar_gateway.sh
set -euo pipefail

DEST=/opt/aquamonitor
USUARIO=aquamonitor

echo "== 1. Paquetes del sistema"
apt-get update
apt-get install -y mosquitto mosquitto-clients python3-venv sqlite3

echo "== 2. Usuario de servicio y carpetas"
id -u "$USUARIO" >/dev/null 2>&1 || useradd --system --home "$DEST" --shell /usr/sbin/nologin "$USUARIO"
mkdir -p "$DEST/gateway" "$DEST/data"
cp aquamonitor_gateway.py ia.py schema.sql reporte_pruebas.py simulador_esp32.py requirements.txt "$DEST/gateway/"
[ -f "$DEST/gateway/gateway.ini" ] || cp gateway.example.ini "$DEST/gateway/gateway.ini"
chown -R "$USUARIO":"$USUARIO" "$DEST"
chmod 600 "$DEST/gateway/gateway.ini"

echo "== 3. Entorno de Python"
python3 -m venv "$DEST/venv"
"$DEST/venv/bin/pip" install -r "$DEST/gateway/requirements.txt"

echo "== 4. Mosquitto con usuarios y lista de acceso"
cp mosquitto/aquamonitor.conf /etc/mosquitto/conf.d/aquamonitor.conf
cp mosquitto/aquamonitor.acl /etc/mosquitto/aquamonitor.acl
if [ ! -f /etc/mosquitto/passwd ]; then
  echo "Contrasena para el usuario MQTT 'gateway':"
  mosquitto_passwd -c /etc/mosquitto/passwd gateway
  echo "Contrasena para el usuario MQTT 'esp32_est01' (la misma que MQTT_PASS en el firmware):"
  mosquitto_passwd /etc/mosquitto/passwd esp32_est01
fi
chown root:mosquitto /etc/mosquitto/passwd /etc/mosquitto/aquamonitor.acl
chmod 640 /etc/mosquitto/passwd /etc/mosquitto/aquamonitor.acl
systemctl enable mosquitto
systemctl restart mosquitto

echo "== 5. Servicio del Gateway (arranque automatico)"
cp systemd/aquamonitor-gateway.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable aquamonitor-gateway

echo
echo "Listo. Pasos que faltan:"
echo "  1) Editar $DEST/gateway/gateway.ini (clave MQTT del gateway, token de Ubidots, clave de Gemini, path de la base)."
echo "  2) sudo systemctl start aquamonitor-gateway"
echo "  3) journalctl -u aquamonitor-gateway -f      # ver el registro en vivo"
