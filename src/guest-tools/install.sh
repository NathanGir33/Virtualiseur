#!/bin/sh
# Virtualiseur guest tools, for any Linux distribution.
#
#   sh install.sh               install (or update)
#   sh install.sh --uninstall   remove the shared-folder service
#
# Always start it through "sh": the CD may be mounted without the exec bit.
# Pure POSIX sh (dash, busybox ash, bash...). Installs, from the
# distribution's own repositories:
#   - qemu-guest-agent: guest clock kept on the host's, disks frozen for
#     live snapshots;
#   - spice-vdagent, only when a desktop or display manager is present:
#     shared clipboard, files dropped on the VM window;
# and a boot service (systemd or OpenRC) that mounts every virtio-9p shared
# folder of the VM on /mnt/<tag>.

VERSION=0.4.0
SERVICE=virtualiseur-shares
MOUNT_SCRIPT=/usr/local/sbin/virtualiseur-mount-shares

say() { printf '%s\n' "$*"; }
warn() { printf 'Attention : %s\n' "$*" >&2; }

# --- Privileges: re-run as root through sudo, doas or su -----------------------
if [ "$(id -u)" != 0 ]; then
    script=$0
    case $script in /*) ;; *) script=$(pwd)/$script ;; esac
    if command -v sudo >/dev/null 2>&1; then
        say "Droits administrateur demandés (sudo)."
        exec sudo sh "$script" "$@"
    elif command -v doas >/dev/null 2>&1; then
        say "Droits administrateur demandés (doas)."
        exec doas sh "$script" "$@"
    elif command -v su >/dev/null 2>&1; then
        say "Mot de passe de root demandé (su)."
        quoted=$(printf '%s' "$script" | sed "s/'/'\\\\''/g")
        arguments=
        for argument in "$@"; do
            arguments="$arguments '$(printf '%s' "$argument" | sed "s/'/'\\\\''/g")'"
        done
        exec su root -c "sh '$quoted'$arguments"
    fi
    say "Erreur : ni sudo, ni doas, ni su ; lancez ce script en root." >&2
    exit 1
fi

# --- Environment ----------------------------------------------------------------------
distribution=$( (. /etc/os-release 2>/dev/null; printf '%s' "${PRETTY_NAME:-${NAME:-Linux}}") )
[ -n "$distribution" ] || distribution=Linux

# "running": services are started now. "offline": installed system seen from
# a chroot (Debian installer's late command): enabled for the next boot only.
init=other
running=yes
if [ -d /run/systemd/system ]; then
    init=systemd
elif command -v openrc-run >/dev/null 2>&1 || command -v rc-service >/dev/null 2>&1; then
    init=openrc
elif command -v systemctl >/dev/null 2>&1 && [ -d /etc/systemd/system ]; then
    init=systemd
    running=no
fi

# A systemd unit file, wherever the distribution keeps them.
unit_exists() {
    for unit_directory in /etc/systemd/system /usr/lib/systemd/system /lib/systemd/system /usr/local/lib/systemd/system; do
        [ -e "$unit_directory/$1" ] && return 0
    done
    return 1
}

# A desktop: X11/Wayland sessions, or a display manager.
# (POSIX sh has no local variables: function variables get their own names.)
has_desktop() {
    for desktop_directory in /usr/share/xsessions /usr/share/wayland-sessions; do
        for desktop_session in "$desktop_directory"/*.desktop; do
            [ -e "$desktop_session" ] && return 0
        done
    done
    [ -e /etc/X11/default-display-manager ] && return 0
    [ -e /etc/systemd/system/display-manager.service ] && return 0
    for desktop_greeter in gdm gdm3 sddm lightdm lxdm xdm slim ly greetd cosmic-greeter; do
        command -v "$desktop_greeter" >/dev/null 2>&1 && return 0
        for desktop_directory in /usr/lib/systemd/system /lib/systemd/system /etc/init.d; do
            [ -e "$desktop_directory/$desktop_greeter.service" ] || [ -e "$desktop_directory/$desktop_greeter" ] && return 0
        done
    done
    return 1
}

# First existing unit (systemd) or init script (OpenRC) among the names
# given, enabled at boot and (re)started now. Fails if none exists.
start_service() {
    for service in "$@"; do
        case $init in
        systemd)
            unit=$service
            case $unit in *.service|*.socket) ;; *) unit=$unit.service ;; esac
            unit_exists "$unit" || continue
            # Units without [Install] (started by udev) cannot be enabled: not an error.
            systemctl enable "$unit" >/dev/null 2>&1
            if [ "$running" = no ]; then
                say "  service $unit prêt pour le prochain démarrage"
                return 0
            fi
            systemctl restart "$unit" >/dev/null 2>&1 && { say "  service $unit actif"; return 0; }
            warn "le service $unit ne démarre pas (journalctl -u $unit)."
            return 1
            ;;
        openrc)
            [ -e "/etc/init.d/$service" ] || continue
            rc-update add "$service" default >/dev/null 2>&1
            rc-service "$service" restart >/dev/null 2>&1 && { say "  service $service actif"; return 0; }
            warn "le service $service ne démarre pas."
            return 1
            ;;
        esac
    done
    return 1
}

# --- Uninstall --------------------------------------------------------------------------
if [ "${1:-}" = "--uninstall" ] || [ "${1:-}" = "--desinstaller" ]; then
    case $init in
    systemd)
        systemctl disable --now "$SERVICE.service" >/dev/null 2>&1
        rm -f "/etc/systemd/system/$SERVICE.service"
        [ "$running" = yes ] && systemctl daemon-reload
        ;;
    openrc)
        rc-update del "$SERVICE" default >/dev/null 2>&1
        rm -f "/etc/init.d/$SERVICE"
        ;;
    esac
    rm -f "$MOUNT_SCRIPT"
    say "Service des dossiers partagés retiré. Les paquets qemu-guest-agent et spice-vdagent restent installés"
    say "(désinstallez-les avec le gestionnaire de paquets de la distribution si besoin)."
    exit 0
fi

say "Outils invité Virtualiseur $VERSION pour $distribution"
say ""

# --- Packages ---------------------------------------------------------------------------
manager=
for candidate in apt-get dnf yum zypper pacman apk xbps-install emerge; do
    if command -v "$candidate" >/dev/null 2>&1; then
        manager=$candidate
        break
    fi
done

qga_package=qemu-guest-agent
vdagent_package=spice-vdagent
case $manager in
xbps-install) qga_package=qemu-ga ;;
emerge) qga_package=app-emulation/qemu-guest-agent; vdagent_package=app-emulation/spice-vdagent ;;
esac

desktop=no
has_desktop && desktop=yes

packages=
command -v qemu-ga >/dev/null 2>&1 || [ -x /usr/bin/qemu-ga ] || [ -x /usr/sbin/qemu-ga ] || packages=$qga_package
if [ "$desktop" = yes ]; then
    say "Bureau détecté : presse-papiers partagé et dépôt de fichiers (spice-vdagent)."
    command -v spice-vdagentd >/dev/null 2>&1 || [ -x /usr/sbin/spice-vdagentd ] || [ -x /usr/bin/spice-vdagentd ] ||
        packages="$packages $vdagent_package"
else
    say "Pas de bureau : spice-vdagent n'est pas installé (inutile sur un serveur)."
fi

install_packages() {
    case $manager in
    apt-get)
        # The installation CD often stays listed ("deb cdrom:", still active
        # during the Debian installer's late command): apt would then wait
        # for someone to insert it. A copy of the list without those lines,
        # and no terminal input: nothing can stop on a question.
        apt_list=/etc/apt/sources.list
        apt_copy=
        if grep -q '^[[:space:]]*deb[[:space:]]\{1,\}cdrom:' /etc/apt/sources.list 2>/dev/null; then
            apt_copy=$(mktemp 2>/dev/null) || apt_copy=/tmp/virtualiseur-sources.list
            grep -v '^[[:space:]]*deb[[:space:]]\{1,\}cdrom:' /etc/apt/sources.list > "$apt_copy"
            apt_list=$apt_copy
        fi
        # Right after boot a desktop updates itself: wait for its lock (5 min).
        # One unreachable source makes update fail while the others are
        # refreshed: install anyway; with no working source, install fails.
        DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 -o "Dir::Etc::SourceList=$apt_list" \
            update -q < /dev/null ||
            say "  (apt-get update a signalé une source inaccessible ; installation tentée quand même)"
        apt_status=0
        DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 -o "Dir::Etc::SourceList=$apt_list" \
            install -y -q "$@" < /dev/null || apt_status=$?
        [ -n "$apt_copy" ] && rm -f "$apt_copy"
        return $apt_status ;;
    dnf) dnf install -y "$@" ;;
    yum) yum install -y "$@" ;;
    zypper) ZYPP_LOCK_TIMEOUT=300 zypper --non-interactive --gpg-auto-import-keys install "$@" ;;
    pacman) pacman -Sy --needed --noconfirm "$@" ;;
    apk) apk add "$@" ;;
    xbps-install) xbps-install -Sy "$@" ;;
    emerge) emerge --noreplace "$@" ;;
    *) return 1 ;;
    esac
}

status=0
packages=$(printf '%s' "$packages" | sed 's/^ *//')
if [ -n "$packages" ]; then
    if [ -z "$manager" ]; then
        warn "gestionnaire de paquets inconnu : installez vous-même $packages."
        status=1
    else
        say "Installation avec $manager : $packages"
        # shellcheck disable=SC2086 # One word per package.
        if ! install_packages $packages; then
            warn "l'installation des paquets a échoué (la machine a-t-elle accès à Internet ?)."
            status=1
        fi
    fi
else
    say "Paquets déjà installés."
fi

# --- Agents -----------------------------------------------------------------------------
say ""
say "Agents :"
if command -v qemu-ga >/dev/null 2>&1 || [ -x /usr/bin/qemu-ga ] || [ -x /usr/sbin/qemu-ga ]; then
    start_service qemu-guest-agent qemu-ga || { warn "qemu-guest-agent installé mais pas démarré."; status=1; }
fi
if [ "$desktop" = yes ] && { command -v spice-vdagentd >/dev/null 2>&1 || [ -x /usr/sbin/spice-vdagentd ] || [ -x /usr/bin/spice-vdagentd ]; }; then
    # The socket (when there is one) starts the daemon for each session;
    # the daemon itself opens the VM port now. Names vary between distributions.
    [ "$init" = systemd ] && systemctl enable spice-vdagentd.socket >/dev/null 2>&1
    start_service spice-vdagentd spice-vdagent || { warn "spice-vdagent installé mais pas démarré."; status=1; }
fi

# --- Shared folders (virtio-9p) on /mnt/<tag> -----------------------------------------
say ""
say "Dossiers partagés :"
mkdir -p "${MOUNT_SCRIPT%/*}"
cat > "$MOUNT_SCRIPT" <<'SCRIPT'
#!/bin/sh
# Virtualiseur: mounts every virtio-9p shared folder of the VM on /mnt/<tag>.
# Installed by the guest tools; run at boot by the virtualiseur-shares
# service (systemd or OpenRC). Safe to run again: mounted tags are skipped.
base=/sys/bus/virtio/drivers/9pnet_virtio
modprobe 9pnet_virtio 2>/dev/null
modprobe 9p 2>/dev/null
# The devices bind to the driver a moment after the module loads.
tries=0
while [ ! -d "$base" ] && [ "$tries" -lt 5 ]; do
    sleep 1
    tries=$((tries + 1))
done
status=0
for file in "$base"/virtio*/mount_tag; do
    [ -r "$file" ] || continue
    tag=$(tr -d '\000\n' < "$file")
    case $tag in
    '' | . | .. | *[!A-Za-z0-9._-]*)
        echo "virtualiseur-shares: étiquette ignorée : $tag" >&2
        continue ;;
    esac
    directory=/mnt/$tag
    # A virtio-9p channel serves one mount at a time: already on /mnt/<tag>,
    # nothing to do; elsewhere (older automatic mount on /media/<tag>),
    # moved here unless it is in use.
    current=$(awk -v t="$tag" '$1 == t && $3 == "9p" { print $2; exit }' /proc/mounts)
    [ "$current" = "$directory" ] && continue
    if [ -n "$current" ]; then
        if ! umount "$current" 2>/dev/null; then
            echo "virtualiseur-shares: $tag est utilisé sur $current : laissé à cet endroit" >&2
            continue
        fi
        rmdir "$current" 2>/dev/null
    fi
    mkdir -p "$directory"
    if mount -t 9p -o trans=virtio,version=9p2000.L,rw,nofail,msize=524288 "$tag" "$directory" 2>/dev/null ||
        mount -t 9p -o trans=virtio,version=9p2000.L,rw,nofail "$tag" "$directory"; then
        echo "virtualiseur-shares: $tag monté sur $directory"
    else
        echo "virtualiseur-shares: échec du montage de $tag" >&2
        status=1
    fi
done
exit $status
SCRIPT
chmod 755 "$MOUNT_SCRIPT"

case $init in
systemd)
    cat > "/etc/systemd/system/$SERVICE.service" <<UNIT
[Unit]
Description=Virtualiseur : dossiers partagés (virtio-9p) sur /mnt/<nom>
After=systemd-modules-load.service local-fs.target
Wants=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$MOUNT_SCRIPT

[Install]
WantedBy=multi-user.target
UNIT
    [ "$running" = yes ] && systemctl daemon-reload
    start_service "$SERVICE" || status=1
    ;;
openrc)
    cat > "/etc/init.d/$SERVICE" <<INIT
#!/sbin/openrc-run
description="Virtualiseur : dossiers partagés (virtio-9p) sur /mnt/<nom>"
depend() {
    need localmount
}
start() {
    ebegin "Montage des dossiers partagés Virtualiseur"
    $MOUNT_SCRIPT
    eend \$?
}
INIT
    chmod 755 "/etc/init.d/$SERVICE"
    start_service "$SERVICE" || status=1
    ;;
*)
    "$MOUNT_SCRIPT"
    warn "système d'init non reconnu : ajoutez $MOUNT_SCRIPT au démarrage pour monter les dossiers partagés."
    ;;
esac
found=no
for file in /sys/bus/virtio/drivers/9pnet_virtio/virtio*/mount_tag; do
    [ -r "$file" ] || continue
    found=yes
    tag=$(tr -d '\000\n' < "$file")
    say "  $tag -> /mnt/$tag"
done
[ "$found" = yes ] || say "  aucun pour l'instant (montés au démarrage sur /mnt/<nom> dès qu'il y en aura)"

say ""
if [ "$status" -eq 0 ]; then
    say "Terminé."
    [ "$desktop" = yes ] && say "Fermez puis rouvrez la session graphique pour activer le presse-papiers partagé."
else
    say "Terminé avec des avertissements (voir ci-dessus)."
fi
exit $status
