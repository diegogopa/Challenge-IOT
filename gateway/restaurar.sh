#!/usr/bin/env bash
# Restaura el Gateway en una Raspberry Pi recien instalada a partir de un respaldo de respaldar.sh.
# Uso (en la Raspberry Pi, desde la carpeta gateway/):   sudo bash restaurar.sh ~/respaldo_aquamonitor_XXXX.tgz
set -euo pipefail
RESPALDO=${1:?Indique el archivo .tgz del respaldo}
cd "$(dirname "$0")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
tar xzf "$RESPALDO" -C "$TMP"

# Se dejan en su lugar ANTES del instalador: asi no pide claves ni sobrescribe gateway.ini
mkdir -p /etc/mosquitto /opt/aquamonitor/gateway /opt/aquamonitor/data
cp "$TMP/mosquitto_passwd" /etc/mosquitto/passwd
cp "$TMP/gateway.ini" /opt/aquamonitor/gateway/gateway.ini
[ -f "$TMP/aquamonitor.db" ] && cp "$TMP/aquamonitor.db" /opt/aquamonitor/data/aquamonitor.db

bash instalar_gateway.sh
systemctl start aquamonitor-gateway
echo
echo "Restaurado. Ver el registro con:  journalctl -u aquamonitor-gateway -f"
