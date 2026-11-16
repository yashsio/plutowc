#!/bin/sh
# Pluto autostart script.
#
# Launch Pluto with this script as its startup command, e.g.:
#   pluto -s "$HOME/.config/pluto/start.sh"
# or from a display manager session:
#   Exec=pluto -s /home/you/.config/pluto/start.sh
#
# Apps are backgrounded so this script can exit; Pluto keeps running.
# Each service is optional, so a minimal install still starts cleanly.

have() {
	command -v "$1" >/dev/null 2>&1
}

# Terminal server, if foot is installed.
have foot && foot --server &

# Notification daemon.
have mako && mako &

# Wallpaper.  The state file is optional.
wallpaper_file="${XDG_STATE_HOME:-$HOME/.local/state}/wallpaper"
if have swaybg && [ -r "$wallpaper_file" ]; then
	wallpaper=$(cat "$wallpaper_file")
	[ -n "$wallpaper" ] && swaybg -m fill -i "$wallpaper" &
fi

# Clipboard history.
if have wl-paste && have cliphist; then
	wl-paste --type text --watch cliphist store &
	wl-paste --type image --watch cliphist store &
fi

# Night light.
have wlsunset && wlsunset -l 28.65 -L 77.20 &

exit 0
