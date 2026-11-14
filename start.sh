#!/bin/sh
# Pluto autostart script.
#
# Launch Pluto with this script as its startup command, e.g.:
#   pluto -s "$HOME/.config/pluto/start.sh"
# or from a display manager session:
#   Exec=pluto -s /home/you/.config/pluto/start.sh
#
# Apps are backgrounded so this script can exit; Pluto keeps running.

# Terminal
foot &

# Application launcher (optional)
# fuzzel &

# Status bar backend (uncomment when pluto-bar's dwl backend is wired up)
# pluto-status | pluto-bar &

# Notifications (optional)
# mako &

exit 0