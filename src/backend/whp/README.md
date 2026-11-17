# Backend WHP

Bibliothèque statique (`VirtualiseurWHPBackend.lib`) qui enveloppe l'API Windows Hypervisor Platform. Elle ne requiert aucun pilote noyau développé par le projet. Le VMM ([`src/vmm`](../../vmm/README.md)) s'appuie sur elle.

## Contenu

- `WindowsHypervisorBackend` :
  - `QueryHostCapabilities()` vérifie la disponibilité de WHP.
  - `Partition` gère la partition, ses vCPU et l'émulation xAPIC de l'hyperviseur.
  - Mémoire : `AllocateAndMapGuestMemory()`, avec le suivi optionnel des pages sales, et `QueryDirtyPages()`.
  - Registres : `Get/SetVirtualProcessorRegisters()`.
  - Exécution : `RunVirtualProcessor()` et `CancelRunVirtualProcessor()` (thread-safe).
  - Interruptions : `RequestInterrupt()`, un `WHvRequestInterrupt` thread-safe.
  - `TranslateGuestVirtualAddress()`.
- `VirtualDisk` : disque brut *sparse*, ou chaîne de couches au-dessus d'un tel disque (seule la couche du dessus est écrite). Les E/S positionnelles (`OVERLAPPED`) permettent des accès concurrents depuis plusieurs threads.
- `DiskLayer` (`LayerFile`) : une couche de disque. En-tête de 4 Kio (magique `VRTLAYR1`, taille virtuelle, nom du parent dans le même dossier), puis une **table plate** de `uint64_t` (un offset par bloc de 64 Kio, 0 = bloc absent, lu dans le parent), chargée en mémoire à l'ouverture, puis les blocs ajoutés en fin de fichier. Une première écriture partielle dans un bloc absent lit les 64 Kio dans la chaîne parente, applique la modification et écrit le bloc entier (données, puis entrée de table). À l'ouverture, une entrée mal alignée ou dont le bloc dépasse la fin du fichier (écriture interrompue par un plantage) est ignorée.
- `DiskSnapshots` (`SnapshotStore`) : instantanés de disque d'une VM (sans la mémoire), pris machine éteinte ou allumée (`BeginLive` + `VirtualDisk::PushLayer`, qui bascule la couche du dessus sous un verrou lecteur/écrivain après `FlushFileBuffers`), dans `<disque>.snapshots.ini` à côté de l'image de base (réécrit de façon atomique) : chaque instantané nomme une couche figée ou l'image de base, la VM écrit dans une couche « actuelle » au-dessus. Prendre ou restaurer crée toujours une nouvelle couche actuelle vide (les couches existantes ne sont jamais réécrites). Supprimer fusionne : l'unique enfant est recopié dans la couche de l'instantané (qui prend sa place), ou, avec plusieurs enfants, les blocs de la couche sont recopiés dans chacun là où ils manquent ; chaque couche restante garde exactement la même vue et les références ne changent qu'à la fin. Refus si la VM a le disque ouvert. Utilisé par le VMM (`--snapshot`, ouverture de la couche actuelle) et compilé aussi dans le gestionnaire.
- `Iso9660Image` : lecteur ISO 9660 (fichiers, blocs logiques de 2048 octets, libellé du volume), utilisé pour le CD-ROM virtuel et l'extraction du noyau.

Le protocole de démarrage Linux et le bus PCI sont dans le VMM (`src/vmm/boot`, `src/vmm/devices`).

Le programme consommateur doit lier `WinHvPlatform.lib`, et `WinHvEmulation.lib` s'il utilise l'émulateur d'instructions.
