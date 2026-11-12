#!/usr/bin/env bash
# Respalda la configuracion y la base de datos del Gateway en un solo archivo .tgz.
# Uso (en la Raspberry Pi):   sudo bash respaldar.sh
# El archivo tiene el token de Ubidots y las claves MQTT: NO subirlo a GitHub.
set -euo pipefail
USUARIO=${SUDO_USER:-pi}
CASA=$(getent passwd "$USUARIO" | cut -d: -f6)
SALIDA=${1:-$CASA/respaldo_aquamonitor_$(date +%Y%m%d_%H%M).tgz}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cp /opt/aquamonitor/gateway/gateway.ini "$TMP/gateway.ini"
cp /etc/mosquitto/passwd "$TMP/mosquitto_passwd"
# Copia consistente de la base aunque el Gateway este corriendo
sqlite3 /opt/aquamonitor/data/aquamonitor.db ".backup '$TMP/aquamonitor.db'"
# Redes Wi-Fi guardadas (referencia; al restaurar se configuran con Raspberry Pi Imager)
mkdir -p "$TMP/redes"
cp /etc/netplan/*.yaml "$TMP/redes/" 2>/dev/null || true
cp /etc/NetworkManager/system-connections/* "$TMP/redes/" 2>/dev/null || true

tar czf "$SALIDA" -C "$TMP" .
chown "$USUARIO": "$SALIDA"
chmod 600 "$SALIDA"
echo "Respaldo creado: $SALIDA"
sqlite3 "$TMP/aquamonitor.db" "SELECT 'mediciones: '||count(*) FROM mediciones; SELECT 'eventos: '||count(*) FROM eventos; SELECT 'predicciones: '||count(*) FROM predicciones;"
