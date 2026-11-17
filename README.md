# Virtualiseur

Hyperviseur de type 2 pour Windows 11, dans l'esprit de VirtualBox, construit sur **Windows Hypervisor Platform (WHP)**. Windows garde la main sur la virtualisation matérielle : Hyper-V, VBS et l'intégrité de la mémoire restent activés. Virtualiseur fournit le reste : le moniteur de machine virtuelle (VMM), les périphériques virtuels, l'affichage et le gestionnaire de VM.

## Ce qui fonctionne

- **Installation complète de Debian 13** depuis l'ISO netinst, en mode texte ou avec l'installateur graphique, puis démarrage du système installé depuis le disque virtuel, sans BIOS ni QEMU.
- **Multiprocesseur** : jusqu'à 64 vCPU, démarrés par le système invité via INIT/SIPI.
- **Carte graphique virtio-gpu** : le pilote Linux `virtio_gpu` reprend l'écran du framebuffer de démarrage. Le curseur de l'invité devient le curseur Windows (aucune latence). La résolution de l'invité suit la fenêtre en direct pendant qu'on l'étire, ainsi qu'en **plein écran** (Ctrl droit + F, avec une barre en haut de l'écran pour le menu et la sortie). Le rendu côté hôte passe par Direct2D (mise à l'échelle par le GPU), au rythme de l'écran.
- **Accélération 3D OpenGL (virgl)** : avec virglrenderer (MSYS2), le pilote Mesa `virgl` de l'invité dessine avec le GPU de l'hôte via OpenGL. GNOME et les applications OpenGL n'utilisent plus le rendu logiciel. Sans les DLL, la VM reste en 2D.
- **Affichage de démarrage** : framebuffer linéaire (vesafb/simpledrm), dont seules les lignes modifiées sont envoyées, grâce au suivi des pages sales de WHP.
- **Son (virtio-sound)** : le pilote Linux `virtio_snd` voit une carte son ; PipeWire/PulseAudio jouent dessus et le son sort sur le périphérique audio par défaut de Windows (WASAPI), au rythme de l'horloge audio de l'hôte. La sortie suit les changements de périphérique, la VM apparaît dans le mélangeur de volume de Windows, et le menu *Périphériques* coupe le son.
- **Souris absolue virtio-input (tablette)** : dès que le système invité a chargé son pilote, la souris suit le curseur Windows, sans capture.
- **Presse-papiers partagé et dépôt de fichiers** : avec l'agent standard `spice-vdagent` dans l'invité (port virtio-serial `com.redhat.spice.0`), le texte et les images passent dans les deux sens. Les fichiers déposés sur la fenêtre arrivent dans le dossier Téléchargements de l'invité. L'installateur Debian installe l'agent automatiquement avec le bureau (`--guest-tools`).
- **Capture du clavier** comme dans VirtualBox : fenêtre au premier plan, la touche Windows, Alt+Tab et les touches de volume et multimédia vont à l'invité, pas à Windows ; Ctrl droit seul rend le clavier à Windows.
- **Clavier et souris PS/2** : avant le pilote tablette, la souris (IntelliMouse avec molette) est capturée d'un clic et libérée avec Ctrl droit.
- **Disque virtio-blk** sur un thread d'E/S dédié, ou disque IDE. Le **CD-ROM ATAPI** transfère en DMA.
- **Réseau virtio-net**, avec les modes de VirtualBox et une adresse MAC propre à chaque VM :
  - **NAT** (libslirp) : DHCP, DNS et accès Internet, avec redirection de ports depuis 127.0.0.1, sans pilote ni droits administrateur ;
  - **accès par pont** avec Npcap : la VM est sur le réseau local. Sur Ethernet le pont est transparent ; sur Wi-Fi il traduit les adresses MAC. L'injection de trames dépend du pilote **et** du réseau : avec l'Intel AX201 testé, elle est refusée sur certains réseaux et fonctionne sur d'autres (par exemple un partage de connexion) ;
  - **réseau interne** entre VM d'un même nom, porté par du multicast local sur 127.0.0.1 ;
  - **réseau privé hôte** (machine isolée, joignable par redirections de ports) et **câble débranché**.
- **ACPI** : arrêt propre via le bouton d'alimentation, extinction S5, redémarrage, timer PM et routage PCI (`_PRT`).
- **Gestionnaire de VM** dans l'esprit de VirtualBox : barre d'outils, liste des machines avec leur état, panneau de détails par section, fenêtre de configuration par catégories (Général, Système, Affichage, Stockage, Audio, Réseau, Installation) avec curseurs pour la RAM, les vCPU et le disque, et zones vert/orange/rouge selon les ressources de l'hôte. Création guidée depuis une ISO (système détecté d'après son nom), agrandissement du disque, pause/reprise, arrêt ACPI, redémarrage, extinction forcée, journal, suppression d'une VM avec son disque.
- **Installation automatique** : un fichier preseed Debian est injecté dans l'initrd (`--preseed`).
- **Instantanés de disque** : fenêtre *Instantanés* du gestionnaire (prendre, restaurer, supprimer, en arbre) ; prise possible machine allumée, systèmes de fichiers gelés par qemu-guest-agent (sinon « cohérent après plantage ») ; format en couches de 64 Kio sans droits administrateur, le `disk.img` existant devient la base. Pas d'instantané de la mémoire (l'état 3D virgl ne se sauvegarde pas).
- **Dossiers partagés** (virtio-9p, 3 au plus), montés tout seuls sur `/media/<nom>` par qemu-guest-agent ; **micro** de l'hôte (désactivé par défaut) ; **heure** de l'invité tenue sur celle de l'hôte ; **ISO et câble réseau** changeables à chaud.

Mesures sur un i5-11300H, VM de 2 vCPU et 2 Gio :

| Mesure | Résultat |
|---|---|
| Démarrage du noyau jusqu'à `/init` | ~1,5 s |
| Installation complète de Debian 13 (ISO netinst, sans réseau) | ~4 min 30 |
| Sorties VM pour un démarrage complet du système installé | ~25 000 |
| CPU hôte, VM au repos à l'invite de connexion | ~0,6 % d'un cœur |
| CPU hôte, VM en pause | 0 |
| Disque virtio-blk, lecture séquentielle directe (1 Mio) | ~600 Mo/s |
| Disque virtio-blk, lecture 4 Kio QD1 | ~19 000 IOPS (~2 700 sorties VM pour 30 000 requêtes, grâce aux doorbells) |
| Réseau NAT, flux TCP hôte → invité (redirection de port) | ~50 Mo/s, ~300 sorties VM en 10 s |
| Disque IDE (DMA), lecture séquentielle directe | ~275 Mo/s |
| Téléchargement HTTP via le NAT | débit de la connexion hôte (11,3 Mo/s contre 10,1 Mo/s sur l'hôte) |

## Installateur

`.\build.ps1 -Installer` compile tout puis produit `installer\Output\Virtualiseur-Setup.exe` (Inno Setup 6 : `winget install JRSoftware.InnoSetup`). Il installe Virtualiseur dans `Program Files` avec le runtime Visual C++, la 3D (virglrenderer) et le NAT (libslirp). Il crée les raccourcis et active au besoin la Plateforme de l'hyperviseur Windows (un redémarrage est alors demandé). Le désinstallateur apparaît dans *Paramètres → Applications* et dans le menu Démarrer ; il propose de supprimer aussi `Documents\Virtualiseur\VMs`. Npcap, utile seulement pour l'accès par pont, n'est pas inclus : sa licence gratuite interdit de le redistribuer.

## Démarrage rapide

Prérequis : Windows 10 2004 ou plus récent avec la fonctionnalité facultative **Plateforme de l'hyperviseur Windows**, et Visual Studio 2026 avec la charge de travail C++. vcpkg n'est nécessaire que pour le réseau NAT (libslirp). L'accélération 3D demande [MSYS2](https://www.msys2.org) (`winget install MSYS2.MSYS2`) et le paquet `mingw-w64-ucrt-x86_64-virglrenderer`. `build.ps1` copie ses DLL dans `dist\`.

```powershell
.\build.ps1              # tout compiler et assembler dist\ (réseau NAT inclus)
.\build.ps1 -NoNetwork   # sans libslirp ni vcpkg
.\dist\VirtualiseurManager.exe
```

Dans le gestionnaire, clique sur **Nouvelle**, choisis l'ISO, règle la RAM, les vCPU et la taille du disque avec les curseurs, puis **Créer**. Le [guide d'utilisation](docs/run-linux-v1.md) détaille la suite.

En ligne de commande :

```powershell
VirtualiseurVM.exe --name Debian --memory 2048 --cpus 2 `
  --iso debian-13.7.0-amd64-netinst.iso --disk Debian.img --disk-size 20480 --net nat
```

## Architecture

```
src/
  backend/whp/   Bibliothèque WHP : partition, vCPU, mémoire, ISO 9660, disque brut
  vmm/           VirtualiseurVM.exe, le moniteur de machine virtuelle
    core/        bus E/S et MMIO, mémoire invitée, journal
    devices/     IOAPIC, PIT, RTC, UART 16550A, PS/2, ACPI PM, PCI, IDE/ATAPI,
                 virtio-blk, virtio-net, virtio-gpu (virgl), virtio-input,
                 virtio-serial, virtio-sound
    audio/       sortie WASAPI (son de l'invité vers Windows)
    firmware/    tables ACPI et DSDT généré en AML
    boot/        protocole de démarrage Linux, profils ISO, lecteur ext2/3/4 et grub.cfg
    net/         NAT libslirp
    ui/          fenêtre d'affichage
  app/VirtualiseurManager/   gestionnaire de VM (Win32) : bibliothèque (VmLibrary),
                             thème et dessin (Ui), fenêtres de configuration (Dialogs)
```

Le [README du VMM](src/vmm/README.md) décrit le modèle d'exécution, les interruptions et chaque périphérique.

## Licences

libslirp (BSD-3-Clause) et ses dépendances (glib : LGPL-2.1+) sont restaurées par vcpkg. virglrenderer et libepoxy (MIT), libwinpthread (MIT/BSD) et libgcc_s (GPL avec exception d'exécution) viennent de MSYS2. Il faut lire leurs notices avant toute redistribution publique. QEMU n'est ni inclus ni utilisé : Virtualiseur a son propre moteur.

## Feuille de route

1. Valider le NAT libslirp sur plusieurs versions de Windows (téléchargements apt, mises à jour).
2. EDID virtio-gpu (liste de résolutions et nom d'écran dans l'invité).
3. Firmware UEFI (OVMF) pour les ISO non Linux et les invités Windows.
4. Clones de machines (complet ou lié).
5. Signature et installateur pour une distribution grand public.

Pistes pour la 3D : afficher directement la texture de l'invité, sans relecture (partage WGL/Direct3D) ; Vulkan avec Venus. WHP ne permet pas de donner le GPU à la VM (GPU-P et DDA sont réservés à Hyper-V).
