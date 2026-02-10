#!/bin/bash
LOG=/var/log/bt-tether.log
IFACE=bnep0
log() { echo "$(date): $1" >> $LOG; }
log "BT auto-connect started"

MAC=$(bluetoothctl devices Paired 2>/dev/null | head -1 | awk '{print $2}')

while [ -z "$MAC" ]; do
    log "Waiting for paired device..."
    sleep 10
    MAC=$(bluetoothctl devices Paired 2>/dev/null | head -1 | awk '{print $2}')
done

log "Connecting to $MAC"

while true; do
    if ip link show $IFACE 2>/dev/null | grep -q UP && ping -c1 -W2 8.8.8.8 >/dev/null 2>&1; then
        sleep 30
        continue
    fi

    log "Connecting..."
    bt-network -c $MAC nap &
    PID=$!
    sleep 5

    if ip link show $IFACE >/dev/null 2>&1; then
        ip link set $IFACE up
        dhclient $IFACE 2>/dev/null
        sleep 2

        if ping -c1 -W3 8.8.8.8 >/dev/null 2>&1; then
            log "CONNECTED!"
            echo $(date +%s) > /tmp/.bt_internet_connected
            while ping -c1 -W5 8.8.8.8 >/dev/null 2>&1; do sleep 20; done
            log "Lost connection"
            rm -f /tmp/.bt_internet_connected
        fi
    fi

    kill $PID 2>/dev/null
    sleep 10
done
