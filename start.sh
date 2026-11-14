#!/bin/sh
# Pluto autostart script.
#
# Launch Pluto with this script as its startup command, e.g.:
#   pluto -s "$HOME/.config/pluto/start.sh"
# or from a display manager session:
#   Exec=pluto -s /home/you/.config/pluto/start.sh
#
# Apps are backgrounded so this script can exit; Pluto keeps running.

# Add services below
foot --server &
mako &
swaybg -m fill -i "$(cat "$HOME/.local/state/wallpaper")" &
wl-paste --type text --watch cliphist store &
wl-paste --type image --watch cliphist store &
wlsunset -l 28.65 -L 77.20 &

exit 0
