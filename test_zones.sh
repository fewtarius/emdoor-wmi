#!/bin/bash
# Identify the four keyboard zones and the two chassis RGB bars.
#
# Walks each zone in turn: turns it on with a unique colour and the
# other zones off. Watch the keyboard - the zone that lights up is
# the one the script is currently identifying. Press enter to move on.

set -eu

KBD_LEDS=/sys/class/leds

if ! ls "$KBD_LEDS"/emdoor:multicolor:zone1 >/dev/null 2>&1; then
    echo "error: emdoor:multicolor:* not found; is emdoor-wmi loaded?" >&2
    exit 1
fi

ZONES=(zone1 zone2 zone3 zone4 bar-left bar-right)
BASELINE=(255 255 255)

set_color() {
    printf '%s %s %s\n' "$1" "$2" "$3" | sudo tee \
        "$KBD_LEDS/emdoor:multicolor:$4/multi_intensity" >/dev/null
}

heading() {
    printf '\n=== %s ===\n' "$1"
}

heading "Baseline (all zones white)"
for z in "${ZONES[@]}"; do
    set_color "${BASELINE[@]}" "$z"
done
sleep 2

heading "Identifying keyboard zones"

# Keyboard zones (left to right on the keyboard):
#   zone1 = keyboard left quadrant
#   zone2 = keyboard left-of-center
#   zone3 = keyboard right-of-center
#   zone4 = keyboard right quadrant
KEYBOARD=(zone1 zone2 zone3 zone4)
COLOURS=(
    "255 0 0   red"
    "0 255 0   green"
    "0 0 255   blue"
    "255 255 0 yellow"
)
LABELS=(
    "left quad"
    "left-of-center"
    "right-of-center"
    "right quad"
)
for i in 0 1 2 3; do
    zone=${KEYBOARD[$i]}
    colour=${COLOURS[$i]}
    name=${colour##* }
    rgb=${colour%% *}
    label=${LABELS[$i]}
    # Turn the rest off
    for z in "${KEYBOARD[@]}"; do
        [ "$z" = "$zone" ] || set_color "0" "0" "0" "$z"
    done
    # Light this zone
    set_color ${rgb} "$zone"
    printf 'keyboard %s (%s) - press enter when identified.\n' "$label" "$name"
    read -r
    # Restore baseline on this zone
    set_color "${BASELINE[@]}" "$zone"
done

# Chassis bars (FLAB bits 4-5 of FA00, separate from keyboard zones)
heading "Identifying chassis bars"
for z in "${KEYBOARD[@]}"; do
    set_color "0" "0" "0" "$z"
done
set_color "255 0 255" bar-left
set_color "0 255 255" bar-right
printf 'bar-left is magenta, bar-right is cyan - press enter to restore.\n'
read -r

heading "Restore baseline"
for z in "${ZONES[@]}"; do
    set_color "${BASELINE[@]}" "$z"
done
printf 'done\n'
