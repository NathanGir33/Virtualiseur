# VMM : VirtualiseurVM.exe

Moniteur de machine virtuelle de Virtualiseur. Il crée une partition WHP, y place la mémoire et les périphériques d'un PC simplifié, charge Linux et exécute les vCPU. Le gestionnaire de VM le lance avec un processus par VM.

## Modèle d'exécution

- **Un thread par vCPU** (`Vm::VcpuThread`) boucle sur `WHvRunVirtualProcessor` et traite les sorties :
  - **E/S port simples** : traitées sur un chemin rapide, avec une seule écriture des registres (RIP, et RAX en lecture).
  - **E/S chaînes (`rep ins/outs`) et MMIO** : déléguées à l'émulateur d'instructions WHP (`WinHvEmulation`).
  - **EOI APIC** : transmises à l'IOAPIC pour les interruptions en mode niveau (lignes ISA, virtio sans MSI-X).
  - **Triple faute** : provoque un reset de la VM.
- **APIC local émulé par l'hyperviseur** (mode xAPIC de WHP). Le timer LAPIC, les EOI et les IPI ne provoquent aucune sortie. Les AP sont parqués en *wait-for-SIPI* (`StartupSuspend`) ; le noyau les réveille par INIT/SIPI.
- **Interruptions** : les périphériques hérités pilotent une ligne vers l'**IOAPIC** (`devices/IoApic`), en front pour l'ISA et en niveau avec *Remote IRR* pour le PCI. Les fonctions virtio utilisent **MSI-X** dès que le pilote l'active : un message par file, en front, envoyé directement aux APIC locaux (pas de ligne partagée, pas de lecture de l'ISR, pas de sortie EOI). Tout passe par `WHvRequestInterrupt`, qui est thread-safe et réveille le vCPU ciblé. Les PIC 8259 ne sont qu'un modèle de registres : Linux les masque dès que la MADT annonce l'IOAPIC.
- **Horloge** : la partition expose les *enlightenments* Hyper-V gérés par WHP (`--hyperv on`, par défaut) : fréquences du TSC et du timer APIC lues dans des MSR (aucun étalonnage contre le PIT, qui échouait souvent), horloge `hyperv_clocksource_tsc_page` sans sortie VM, timers synthétiques en mode direct. Sans eux, l'étalonnage raté faisait passer Linux sur `acpi_pm`, et chaque lecture de l'heure devenait une sortie d'E/S.
- **Verrous** : chaque fonction virtio a son propre verrou (anneaux, MSI-X, état), pris par les bus pour les accès BAR et par les threads hôtes qui la servent. L'IOAPIC se verrouille seul. Les périphériques hérités (PIT, UART, PS/2, IDE, configuration PCI, ACPI) partagent le verrou de la VM. Ordre : verrou de la VM, puis d'un périphérique, puis de l'IOAPIC ; un thread de périphérique ne prend jamais le verrou de la VM. Le bus d'E/S se lit sans verrou (routes atomiques), le bus MMIO sous `shared_mutex`.
- **Pause, reprise, reset, arrêt** : les vCPU sont annulés par `WHvCancelRunVirtualProcessor`. En pause, ils attendent une variable de condition, sans consommer de CPU. Un reset reconstruit entièrement la VM ; la fenêtre reste ouverte.

## Carte mémoire invitée

| Plage | Contenu |
|---|---|
| `0x0` - `0x9EFFF` | RAM (boot_params, ligne de commande, trampoline SMP) |
| `0xA0000` - `0xFFFFF` | zone legacy : texte VGA (`0xB8000`), tables ACPI (`0xE0000`) |
| `0x100000` - min(RAM, 3 Gio) | RAM ; noyau à 16 Mio, initrd en haut |
| `0xE0000000` | framebuffer linéaire 32 bpp (suivi des pages sales, jusqu'à 8 bandes de lignes modifiées) |
| `0xFE000000` | BAR1 MMIO des fonctions virtio (MSI-X, doorbells), 64 Kio par slot |
| `0xFEC00000` | IOAPIC (MMIO émulé) |
| `0xFEE00000` | APIC local (géré par WHP) |
| ≥ 4 Gio | RAM au-delà de 3 Gio |

## Périphériques

| Périphérique | Ports / IRQ | Notes |
|---|---|---|
| PIT 8254 | 0x40-0x43, 0x61, IRQ 0 (GSI 2) | thread à timer haute résolution ; modes périodique et monocoup |
| RTC MC146818 | 0x70-0x71 | heure hôte en UTC (option `--rtc local` pour Windows) |
| UART 16550A | 0x3F8, IRQ 4 | FIFO, loopback, interruptions THRE/RDA ; console série |
| i8042 PS/2 | 0x60/0x64, IRQ 1 et 12 | clavier MF2 (scancodes set 1), souris IntelliMouse |
| ACPI PM | 0x600-0x60B, SCI GSI 9 | PM1 événement/contrôle, timer PM 32 bits, bouton power, S5 |
| Bus PCI | 0xCF8-0xCFF | mécanisme #1 ; reset système via 0xCF9 |
| 00:01.1 PIIX3 IDE | 0x1F0/0x170, IRQ 14/15, BMDMA | disque ATA LBA48 et CD-ROM ATAPI (toujours présent, média changeable à chaud), PIO + DMA multimot |
| 00:03.0 virtio-net | 1AF4:1041, GSI 11 sans MSI-X | MAC configurable (`--mac`) ; NAT, pont, réseau interne, privé hôte (`--net`) ; file d'attente RX |
| 00:04.0 virtio-blk | 1AF4:1042, GSI 10 sans MSI-X | thread d'E/S réveillé directement par le doorbell, flush, GET_ID |
| 00:05.0 virtio-gpu | BAR2 mém. = framebuffer, GSI 16 sans MSI-X | classe VGA ; 2D, plan curseur, redimensionnement à chaud |
| 00:06.0 virtio-input | GSI 17 sans MSI-X | tablette (ABS_X/Y 0-32767, 5 boutons, molettes) |
| 00:07.0 virtio-serial | GSI 18 sans MSI-X | multiport ; port 1 `com.redhat.spice.0` pour spice-vdagent |
| 00:08.0 virtio-sound | GSI 19 sans MSI-X | un flux de sortie S16, 1-2 canaux, 8-96 kHz ; `--audio off` le retire |
| 00:09.0 virtio-serial | GSI 20 sans MSI-X | port `org.qemu.guest_agent.0` pour qemu-guest-agent (heure de l'invité) |
| VGA | 0x3B0-0x3DF | registres du curseur en mode texte |

Toutes les fonctions virtio utilisent le transport moderne virtio 1.0 (`devices/VirtioModern`), avec pour le slot *n* :

- **BAR0**, 256 octets d'E/S à `0xC000 + n × 0x100` : configuration commune, ISR, notification sans doorbells, configuration du périphérique (capacités PCI vendeur) ;
- **BAR1**, 16 Kio de MMIO à `0xFE000000 + n × 64 Kio` : table MSI-X (`0x0000`, une entrée par file plus une pour la configuration), bits en attente (`0x0800`), adresses de notification (`0x1000 + 4 × file`).

**Notifications** : quand l'hôte les propose, chaque adresse de notification porte un **doorbell WHP** (`WHvCreateNotificationPort`, ou l'ancien `WHvRegisterPartitionDoorbellEvent`, résolus à l'exécution). L'écriture de l'invité signale un événement dans l'hyperviseur : le vCPU repart sans sortie vers le VMM. Un thread par périphérique attend ces événements et traite la file sous le verrou du périphérique. Sinon, la notification passe par le port d'E/S de BAR0, moins coûteux qu'une sortie MMIO émulée. Les doorbells suivent BAR1 si l'invité la déplace. Le journal indique le mode retenu.

**Réception réseau** : les backends livrent une rafale de trames dans l'anneau RX, puis signalent une seule fois (`GuestPort::flush`) : une interruption par passe de libslirp ou par lot de capture, et non par trame.

virtio-gpu est déclarée en classe VGA, avec un BAR mémoire posé sur le framebuffer de démarrage. Linux retire donc vesafb/simpledrm quand `virtio_gpu` se charge, comme avec une vraie carte. Les commandes sont exécutées par le thread de notifications du périphérique (ou le vCPU qui notifie, sans doorbells). La fenêtre copie sous verrou les lignes signalées par `RESOURCE_FLUSH`, puis les affiche par Direct2D. Le plan curseur devient un `HCURSOR` Windows quand la tablette est active ; sinon il est dessiné par-dessus l'image. Options : `--gpu virgl|virtio|none` et `--pointer ps2`.

**3D (virgl)** : `devices/Virgl` charge `libvirglrenderer-1.dll` à côté de l'exécutable. Un thread GPU dédié possède les contextes OpenGL (WGL, profil core, tous partagés avec le premier) et exécute tous les appels virglrenderer, qui n'est pas thread-safe. Le thread qui reçoit la notification copie chaque requête hors de l'anneau et la poste au thread GPU. Les commandes avec barrière ne sont rendues à l'invité qu'au rappel `write_fence`, que le thread GPU obtient en interrogeant le GPU toutes les 0,5 ms tant qu'une barrière est en vol. **Affichage sans relecture** : quand le pilote OpenGL de l'hôte propose `WGL_NV_DX_interop2`, chaque rectangle signalé par `RESOURCE_FLUSH` est copié GPU→GPU (`glBlitFramebuffer`) de la texture virgl vers une texture Direct3D 11 partagée (`core/SharedGpu`). La copie passe par un contexte GL dédié, qui partage ses objets avec le `ctx0` de virgl et restaure le contexte courant : l'état GL que virgl garde en cache n'est jamais touché. Les ressources marquées `VIRGL_RESOURCE_Y_0_TOP` (rendu Mesa) sont retournées au passage, comme le fait QEMU. La fenêtre dessine cette texture telle quelle. Sinon (extension absente, `--gpu-share off`, commande de tube `gpu-share off`, repli GDI), l'image est relue (`transfer_read_iov`) sur les mêmes rectangles puis téléversée comme en 2D. Mesuré avec `kmscube` (1280x800) : 480 images/s contre 180 à 300 en relecture, et 40 à 60 % de CPU en moins pour le VMM.

**Fenêtre** : Direct2D dessine dans une swap chain DXGI à basculement (`FLIP_DISCARD`), sur un périphérique D3D11 unique pour le processus et partagé avec le thread GPU. Le verrou `SharedD3DLock` encadre tout usage de ce périphérique par l'un ou l'autre thread. Après un reset du périphérique, un compteur de génération écarte les commandes encore en file.

**Réseau** (`net/`) : virtio-net envoie ses trames à un `NetworkBackend`, choisi par `--net` :

- `nat` / `hostonly` : `SlirpNetworkBackend` (libslirp, 10.0.2.0/24). `--nat-forward tcp:2222:22` ajoute une redirection depuis 127.0.0.1, sans alerte du pare-feu ni exposition au réseau local. `hostonly` active le mode `restricted` de libslirp : la machine est isolée, seules les redirections l'atteignent.
- `bridged` : `BridgeNetworkBackend` capture et injecte les trames avec Npcap (`System32\Npcap\wpcap.dll`, chargée dynamiquement), sur deux poignées (réception, émission). Sur Ethernet, la capture est en mode promiscuité, filtrée par BPF sur la MAC de l'invité et la diffusion. Sur Wi-Fi, un point d'accès n'accepte que la MAC associée : les trames sortent avec la MAC de l'hôte, l'ARP est réécrit, le DHCP demande une réponse en diffusion, et les trames entrantes pour l'IPv4 apprise de l'invité lui sont rendues ; l'IPv6 n'est pas transmise. Certains pilotes Wi-Fi refusent toute injection : le journal le signale.
- `internal` : `InternalNetworkBackend`. Chaque VMM rejoint un groupe multicast UDP sur l'interface de bouclage (port dérivé du nom du réseau). Chaque datagramme porte une étiquette et une trame Ethernet ; chacun garde les trames pour sa MAC et la diffusion.

Si un mode ne peut pas démarrer (Npcap absent, carte introuvable), la carte reste présente mais câble débranché. Le câble se branche et se débranche aussi pendant l'exécution (menu Périphériques, commande de tube `cable on|off`) : virtio-net change l'état du lien (`VIRTIO_NET_F_STATUS`) et envoie une interruption de changement de configuration, l'invité voit `NO-CARRIER`.

**Affichage** : `ui/DisplayWindow` demande à l'invité la taille de la fenêtre, après une pause de 250 ms pendant un glissement et de 60 ms sinon (agrandissement, ancrage, plein écran). Il le fait aussi dès que le pilote virtio-gpu prend l'écran. Le plein écran (Ctrl droit + F, `--fullscreen`) occupe le moniteur de la fenêtre ; une barre non activante en haut de l'écran donne le menu et la sortie.

**Heure de l'invité (qemu-guest-agent)** : la pause du VMM ne décale pas l'horloge (le temps de la partition WHP continue de courir, mesuré : écart inchangé après 60 s de pause). Pour les autres cas (veille de l'hôte, dérive, heure changée dans l'invité), `agent/QemuGuestAgent` parle le protocole JSON de qemu-guest-agent sur une seconde carte virtio-serial. Il se synchronise par `guest-sync-delimited` (précédé d'un octet 0xFF), compare `guest-get-time` à l'horloge de l'hôte et corrige par `guest-set-time` au-delà de 0,5 s. Les contrôles ont lieu à la connexion de l'agent, à la reprise après une pause, au réveil de l'hôte (`PowerRegisterSuspendResumeNotification`), toutes les minutes et sur la commande de tube `timesync`. Les statistiques donnent l'écart mesuré (`ecart_horloge_ms`). Debian démarre l'agent tout seul quand le port existe (règle udev) ; les outils invité l'installent avec l'installateur Debian.

**Son (virtio-sound)** : `devices/VirtioSound` annonce un flux de sortie (pas de prise jack ni de carte de canaux) au pilote Linux `virtio_snd`. Les requêtes de contrôle (`PCM_INFO`, `SET_PARAMS`, `PREPARE`, `START`, `STOP`, `RELEASE`) sont traitées sur le vCPU qui notifie. Chaque tampon de la file de transmission est copié hors de l'anneau, puis rendu à l'invité par le thread audio quand ses échantillons sont passés à Windows : l'invité est cadencé par l'horloge de la carte son de l'hôte, comme avec un vrai convertisseur. `RELEASE` rend tout ce qui reste en file, comme l'attend le pilote. `audio/WasapiOutput` joue en WASAPI partagé, piloté par événements, avec un tampon de 60 ms ; le moteur audio de Windows convertit la fréquence et le nombre de canaux (`AUTOCONVERTPCM`). Le thread (priorité MMCSS « Pro Audio ») suit le périphérique par défaut et, sans sortie utilisable, consomme les échantillons au rythme du flux pour ne jamais bloquer l'invité.

**Agent invité** (`agent/GuestAgent`, `ui/ClipboardBridge`) : côté hôte du protocole SPICE vdagent, découpé en blocs `VDIChunkHeader` de 2 Kio. L'hôte annonce `CLIPBOARD_BY_DEMAND`. Une copie dans Windows envoie un *grab* (texte UTF-8, PNG, BMP), et les données ne partent qu'à la demande de l'invité. Un *grab* de l'invité est récupéré tout de suite et posé dans le presse-papiers Windows, avec conversion des fins de ligne. Les images sont converties par WIC. Les fichiers déposés (`WM_DROPFILES`) partent l'un après l'autre sur un thread dédié, par blocs de 64 Kio, avec contrôle de flux sur le tampon du port.

Les tables ACPI générées (`firmware/AcpiTables`) sont : RSDP, RSDT, FADT (rev. 3, registre de reset), FACS, MADT (LAPIC par CPU, IOAPIC, redirections IRQ0→GSI2 et SCI en niveau) et un DSDT en AML. Le DSDT contient `\_S5` et le pont `PCI0` avec `_CRS` (fenêtres E/S, mémoire et bus) et `_PRT`.

## Démarrage

Le VMM charge directement le noyau Linux par le protocole de démarrage x86 32 bits (`boot/LinuxLoader`). Il n'y a ni BIOS ni UEFI. Le noyau et l'initrd viennent de trois sources possibles :

- **ISO** (`boot/BootSources`) : profils Debian (installateur texte ou GTK), Debian live, Ubuntu (casper), Arch, Fedora, openSUSE. Le libellé du volume ISO est injecté dans la ligne de commande quand la distribution l'exige.
- **Disque installé** : le VMM lit la table MBR (avec la chaîne EBR des partitions logiques) ou GPT. Il parcourt ensuite le système ext2/3/4 : extents, blocs indirects, liens symboliques. Il prend la première entrée de `grub.cfg` ou des fichiers BLS (`/boot/loader/entries`). À défaut, il utilise `/vmlinuz` et `/initrd.img` avec `root=UUID=`.
- **Noyau explicite** : `--kernel` et `--initrd`.

En mode `--boot auto`, le disque est prioritaire s'il contient un Linux amorçable. Sinon, le VMM démarre sur le CD. Après une installation, le redémarrage démarre donc sur le système installé.

## Options

`VirtualiseurVM.exe --help` liste toutes les options. Les plus utiles pour le développement :

```powershell
# Console série dans le terminal, sans fenêtre
VirtualiseurVM.exe --iso debian.iso --disk test.img --disk-size 20480 --display none
# Journal série et script d'entrée suivi en continu (tests automatisés)
VirtualiseurVM.exe ... --serial-log serial.log --serial-input input.txt --log vmm.log --verbose
# Installation sans surveillance
VirtualiseurVM.exe ... --preseed docs\preseed-debian.cfg
```

Le canal `\\.\pipe\Virtualiseur-<nom>` accepte `status`, `pause`, `resume`, `acpi-shutdown`, `poweroff`, `reset`, `stats`, `cable on|off` (et `cable` pour lire l'état), `cdrom` (image insérée ou `empty`), `cdrom-insert <chemin.iso>`, `cdrom-eject` et `timesync`.

**Changement de disque à chaud** : `IdeController::ChangeCdromMedia` remplace l'image puis signale le changement comme un vrai lecteur : la commande ATAPI suivante échoue en *UNIT ATTENTION* (clé 6, ASC 0x28) et `GET EVENT STATUS NOTIFICATION` rapporte *NewMedia* (2) ou *MediaRemoval* (3). Linux émet alors un uevent `DISK_MEDIA_CHANGE=1` (udisks/GNOME montent le disque). Le changement est conservé au redémarrage de l'invité (`Vm::CurrentConfig`), pas dans `vm.ini`.
