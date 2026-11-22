#!/vendor/bin/sh
#
# Copyright (C) 2026 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#
# Re-run Type-C detection when a fast charger is stuck at 5V.
#
# When the PMIC sees VBUS before the Type-C attach is settled (plugging in
# slowly, or a charger already attached at boot), qpnp-smb5 can flag the
# cable as legacy and never start PD. Charging then stays at 5V until the
# driver re-checks on its own, which can take minutes.
#
# After every plug-in, wait for PD to come up on its own. If a high current
# Type-C source is still without a PD contract, set the power role to none
# for 3 s (CC high-Z, the charger drops its contract) and back to dual, so
# the attach is detected again. At most two tries per plug-in. Nothing
# about charging current or charging mode is changed.

USB_ONLINE=/sys/class/power_supply/usb/online
TYPEC_MODE=/sys/class/qcom-battery/typec_mode

log() {
    echo "<6>pd_kick: $*" > /dev/kmsg
}

find_iio() {
    for d in /sys/bus/iio/devices/*; do
        if [ "$(cat "$d/name" 2>/dev/null)" = "qpnp-smb5" ]; then
            echo "$d"
            return
        fi
    done
}

IIO=$(find_iio)
if [ -z "$IIO" ]; then
    log "qpnp-smb5 iio device not found, exiting"
    exit 0
fi
PD_ACTIVE="$IIO/in_index_usb_pd_active_input"
POWER_ROLE="$IIO/in_index_usb_typec_power_role_input"

pd_stuck() {
    [ "$(cat $USB_ONLINE 2>/dev/null)" = "1" ] || return 1
    [ "$(cat $PD_ACTIVE 2>/dev/null)" = "0" ] || return 1
    case "$(cat $TYPEC_MODE 2>/dev/null)" in
        *"high current"*) return 0 ;;
    esac
    return 1
}

check_plug() {
    # Give the PD policy engine time to negotiate on its own first
    sleep 12
    tries=0
    while [ $tries -lt 2 ] && pd_stuck; do
        tries=$((tries + 1))
        log "no PD contract on a high current source, re-running Type-C detection ($tries)"
        echo 0 > $POWER_ROLE
        sleep 3
        echo 1 > $POWER_ROLE
        sleep 10
    done
    [ $tries -gt 0 ] && log "pd_active=$(cat $PD_ACTIVE 2>/dev/null) after $tries tries"
}

last=0
while true; do
    online=$(cat $USB_ONLINE 2>/dev/null)
    if [ "$online" = "1" ] && [ "$last" != "1" ]; then
        check_plug
    fi
    last=$online
    sleep 5
done
