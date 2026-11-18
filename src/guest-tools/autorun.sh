#!/bin/sh
# Desktop autorun (GNOME, KDE...): when the CD is inserted, the desktop
# offers to run this file; it opens a terminal that runs install.sh (which
# asks for the administrator password). Pure POSIX sh.
directory=$(cd "$(dirname "$0")" && pwd)
command="sh '$directory/install.sh'; printf '\nAppuyez sur Entrée pour fermer cette fenêtre.'; read answer"
for terminal in x-terminal-emulator ptyxis kgx gnome-terminal konsole xfce4-terminal mate-terminal \
    lxterminal qterminal tilix alacritty kitty foot xterm; do
    command -v "$terminal" >/dev/null 2>&1 || continue
    case $terminal in
    ptyxis | kgx | gnome-terminal | tilix) exec "$terminal" -- sh -c "$command" ;;
    xfce4-terminal | mate-terminal) exec "$terminal" -x sh -c "$command" ;;
    lxterminal | qterminal) exec "$terminal" -e "sh -c \"$command\"" ;;
    *) exec "$terminal" -e sh -c "$command" ;;
    esac
done
# No terminal: graphical password prompt (polkit), output not shown.
exec pkexec sh "$directory/install.sh"
