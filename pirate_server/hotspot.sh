#!/bin/bash
# Helper script to control and monitor the Maker Faire Pirate Hotspot

ACTION="${1:-status}"

case "$ACTION" in
  start)
    echo "[PIRATE] Activating PirateHat Wi-Fi hotspot on wlan0..."
    sudo nmcli connection up PirateHotspot
    echo "[PIRATE] Restarting pirate-server on port 80..."
    sudo systemctl restart pirate-server
    ;;
  stop)
    echo "[PIRATE] Deactivating hotspot and reconnecting to home Wi-Fi..."
    sudo nmcli connection up netplan-wlan0-benhill6
    ;;
  status)
    echo "============================================================"
    echo "PIRATE HOTSPOT STATUS"
    echo "============================================================"
    nmcli -p device status
    echo ""
    echo "--- Web Server Service ---"
    systemctl is-active pirate-server >/dev/null && echo "pirate-server: RUNNING (port 80)" || echo "pirate-server: STOPPED"
    echo ""
    echo "--- Connected Clients (DHCP Leases) ---"
    LEASE_FILE="/var/lib/NetworkManager/dnsmasq-wlan0.leases"
    if [ -f "$LEASE_FILE" ] && [ -s "$LEASE_FILE" ]; then
      awk '{print "IP: " $3 "\tMAC: " $2 "\tDevice: " $4}' "$LEASE_FILE"
    else
      echo "No visitors currently aboard."
    fi
    echo "============================================================"
    ;;
  logs)
    sudo journalctl -u pirate-server -f -n 50
    ;;
  *)
    echo "Usage: $0 {start|stop|status|logs}"
    exit 1
    ;;
esac
