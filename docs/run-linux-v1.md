# Guide d'utilisation

## 1. Préparer Windows

1. Active la fonctionnalité facultative **Plateforme de l'hyperviseur Windows** dans « Activer ou désactiver des fonctionnalités Windows », puis redémarre. Hyper-V, VBS et l'intégrité de la mémoire peuvent rester activés.
2. Compile avec `.\build.ps1`. Si vcpkg n'est pas disponible, utilise `.\build.ps1 -NoNetwork`.
3. Télécharge une ISO Linux x86-64, par exemple `debian-13.x.x-amd64-netinst.iso`.

## 2. Créer une VM dans le gestionnaire

Lance `dist\VirtualiseurManager.exe`. La fenêtre reprend l'organisation de VirtualBox : la barre d'outils en haut, la liste des machines à gauche (avec leur état), et le détail de la machine sélectionnée à droite.

Clique sur **Nouvelle** (Ctrl+N) :

- **Nom** : il donne aussi son nom au dossier `Documents\Virtualiseur\VMs\<nom>`. Choisir l'ISO le propose automatiquement (« Debian 13 »).
- **Image ISO** : l'image d'installation ; le système est reconnu d'après le nom du fichier.
- **Mémoire vive**, **Processeurs**, **Taille du disque** : curseurs reliés à un champ numérique. La bande colorée sous chaque curseur indique la zone confortable (vert), la zone qui charge l'hôte (orange) et celle qui risque de le ralentir (rouge) : plus de la moitié ou des trois quarts de la RAM, plus de vCPU que de cœurs physiques, ou un disque plus grand que l'espace libre.
- **Options** : accélération 3D, son, réseau NAT, installateur Debian graphique.

Le disque virtuel est un fichier brut *sparse*, créé au premier démarrage : il n'occupe sur l'hôte que l'espace réellement écrit. **Créer** enregistre la machine et la démarre (case « Démarrer la machine après sa création »).

## 3. Configurer une VM

**Configuration** (Ctrl+S), ou un clic sur le titre d'une section du panneau de détails, ouvre la fenêtre de configuration à la page correspondante (la machine doit être éteinte) :

| Page | Réglages |
|---|---|
| Général | nom, libellé du système invité, dossier |
| Système | mémoire vive et processeurs (curseurs), ordre de démarrage, horloge UTC ou locale |
| Affichage | carte graphique (virtio-gpu ou framebuffer simple), résolution initiale, accélération 3D |
| Stockage | fichier et taille du disque, contrôleur virtio-blk ou IDE, image ISO du lecteur CD (**Éjecter**) |
| Audio | activation de la carte son |
| Réseau | carte réseau et mode d'accès (NAT, accès par pont, réseau interne, réseau privé hôte, câble débranché), carte de l'hôte ou nom du réseau, adresse MAC, redirection de ports |
| Installation | installateur Debian texte ou graphique, fichier preseed, outils invité |

Pour un disque existant, le curseur ne descend pas sous sa taille actuelle : l'augmenter agrandit le fichier à l'enregistrement. Il faut ensuite agrandir la partition dans l'invité (GParted, `growpart` puis `resize2fs`).

Le fichier preseed facultatif, par exemple [`preseed-debian.cfg`](preseed-debian.cfg), déroule l'installation sans aucune question.

Un double-clic sur une machine, ou **Démarrer** (Entrée), l'ouvre dans sa propre fenêtre. Un clic droit dans la liste donne toutes les commandes, dont **Afficher le journal**.

## 4. Pendant l'exécution

- **Son** : la carte virtio-sound joue sur le périphérique audio par défaut de Windows et suit ses changements (casque branché, autre sortie choisie). La machine apparaît dans le mélangeur de volume de Windows sous « Virtualiseur — <nom> ». *Périphériques → Couper le son* la rend muette.
- **Capture du clavier** : quand la fenêtre de la machine est au premier plan, toutes les touches vont à l'invité, y compris celles que Windows garde d'habitude : touche Windows, Alt+Tab, Alt+Échap, Ctrl+Échap, Impr. écran, touches de volume et multimédia. Un appui seul sur **Ctrl droit** rend le clavier à Windows (le titre de la fenêtre l'indique) ; un clic dans la fenêtre, ou le retour sur elle, le reprend. Ctrl+Alt+Suppr reste réservé à Windows : utilise *Machine → Envoyer Ctrl+Alt+Suppr*. *Périphériques → Capturer le clavier* désactive cette capture.
- **Clavier** : les touches physiques sont transmises. Si l'hôte est en AZERTY, choisis le clavier « French » dans l'installateur pour que les touches correspondent.
- **Souris** : une fois le système démarré, elle suit directement le curseur Windows (tablette virtio). Avant cela (installateur, démarrage), clique dans l'écran pour la capturer et appuie sur **Ctrl droit** pour la libérer.
- **Fenêtre** : avec la carte virtio-gpu, la résolution de l'invité suit la fenêtre en direct pendant qu'on l'étire, et quand on l'agrandit (*Affichage → Résolution de l'invité adaptée à la fenêtre*, par défaut). La largeur de la fenêtre avance alors par pas de 8 pixels : Linux n'accepte que des largeurs d'écran multiples de 8, et l'image reste ainsi au pixel près, sans bandes noires. *Résolution fixe, image mise à l'échelle* garde la résolution de l'invité et agrandit ou réduit l'image ; dans ce mode, *Fenêtre à la taille de l'écran invité* remet la fenêtre à l'échelle 1:1.
- **Plein écran** : **Ctrl droit + F** ou *Affichage → Plein écran*. L'invité passe à la résolution de l'écran. En amenant la souris en haut de l'écran, une barre propose le menu et « Quitter le plein écran ». La case « Démarrer en plein écran » de la page Affichage ouvre directement la machine ainsi.
- **Menu Machine** : pause et reprise, Ctrl+Alt+Suppr, arrêt ACPI, redémarrage, extinction forcée.
- **Fermer la fenêtre** : elle propose un arrêt propre (ACPI) ou une extinction immédiate.

Le gestionnaire affiche l'état de chaque VM (en marche, en pause, éteinte) et offre les mêmes commandes. Une VM éteinte peut être supprimée avec **Supprimer** : son dossier et son disque sont effacés après confirmation.

**Arrêt ACPI et GNOME** : sur l'écran de connexion (et selon les réglages, dans la session), GNOME réagit au bouton d'alimentation par une **mise en veille** et non par un arrêt : l'écran devient noir. Une touche ou un nouvel arrêt ACPI réveille la machine. Pour que le bouton éteigne vraiment : *Paramètres → Énergie → Bouton d'alimentation → Éteindre* dans l'invité.

## 5. Après l'installation

À la fin, l'installateur Debian éjecte le CD et redémarre. Virtualiseur détecte alors le système installé : il lit le `grub.cfg` de la partition ext4 et démarre directement dessus. L'ISO peut rester attachée. Pour redémarrer sur l'ISO malgré un disque installé, lance `VirtualiseurVM.exe` avec `--boot cdrom`.

## Modes réseau

| Mode | Ce que voit la machine | Remarques |
|---|---|---|
| NAT | Internet via l'hôte, adresse 10.0.2.15 | Redirections de ports : 127.0.0.1:2222 → 22 permet `ssh -p 2222 utilisateur@localhost` |
| Accès par pont | Le réseau local, adresse donnée par la box | Demande [Npcap](https://npcap.com). Fiable en Ethernet. En Wi-Fi, l'injection de trames dépend du pilote et du réseau (l'Intel AX201 la refuse sur certains réseaux, l'accepte sur d'autres) : en cas de refus, `vmm.log` l'indique, utilise alors le NAT |
| Réseau interne | Les autres machines du même nom de réseau | Pas de DHCP : adresses fixes, par exemple 192.168.100.1/24, .2… |
| Réseau privé hôte | Rien (isolée) | L'hôte la joint seulement par les redirections de ports |
| Câble débranché | Une carte sans lien | |

Chaque machine a sa propre adresse MAC (bouton **Générer** pour en changer). Deux machines ne doivent pas partager la même MAC sur un réseau interne ou un pont.

## 6. Presse-papiers et fichiers

Ces fonctions passent par l'agent `spice-vdagent` du système invité.

- **Nouvelle VM Debian** : rien à faire. Virtualiseur ajoute une ligne à la configuration de l'installateur, qui installe spice-vdagent à la fin si un bureau a été choisi. Les questions de l'installateur ne changent pas. Il faut le réseau pendant l'installation, comme pour le bureau. `--guest-tools off` désactive cet ajout.
- **Ubuntu, Fedora…** : leurs images de bureau contiennent déjà l'agent.
- **VM déjà installée sans l'agent** : `sudo apt install spice-vdagent`, puis ferme et rouvre la session.

- **Copier-coller** : texte et images, dans les deux sens.
- **Glisser-déposer** : dépose des fichiers de l'Explorateur Windows sur la fenêtre de la VM. Ils arrivent dans le dossier Téléchargements de l'invité. spice-vdagent ne permet pas le sens invité → Windows.

`vmm.log` affiche « Agent invité (spice-vdagent) connecté » quand l'agent est actif.

## 7. Diagnostic

Chaque VM écrit dans son dossier :

- `serial.log` : la console série `ttyS0`, avec les messages du noyau ;
- `vmm.log` : le journal du VMM (source de démarrage, ligne de commande, statistiques de sorties VM). Pour le son, il indique « virtio-snd : le pilote invité a trouvé la sortie audio », puis la sortie Windows utilisée et le format joué.

Pour une console série interactive, lance le VMM dans un terminal avec `--display none`.

## Limites connues

- Seuls les systèmes Linux avec noyau et initrd sur ext2/3/4 démarrent depuis le disque. Btrfs, XFS, ou `/boot` sur LVM ou chiffré ne sont pas encore lus.
- Pas encore de firmware UEFI : les ISO non Linux et Windows ne démarrent pas.
- Sans pilote `virtio_gpu` dans l'invité, la résolution reste celle choisie au démarrage. Avec lui, redimensionner la fenêtre change la résolution.
- La souris doit être capturée tant que l'invité n'a pas chargé `virtio_input` (installateur, démarrage).
- L'installateur Debian graphique (GTK) repasse en mode texte (« Framebuffer not available »).
- Son : lecture seulement (pas de micro), en 16 bits stéréo. Il faut un noyau invité avec `virtio_snd` (Linux 5.13 ou plus récent, présent dans Debian 12/13, Ubuntu, Fedora).
- Accélération 3D : OpenGL seulement (virgl), pas encore de Vulkan ni de décodage vidéo matériel. Sans les DLL virglrenderer à côté de `VirtualiseurVM.exe`, le bureau est rendu par les vCPU (llvmpipe). `vmm.log` indique « accélération 3D virgl active » quand elle fonctionne.
