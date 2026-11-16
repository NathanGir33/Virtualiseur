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
- `VirtualDisk` : disque brut *sparse*. Les E/S positionnelles (`OVERLAPPED`) permettent des accès concurrents depuis plusieurs threads.
- `Iso9660Image` : lecteur ISO 9660 (fichiers, blocs logiques de 2048 octets, libellé du volume), utilisé pour le CD-ROM virtuel et l'extraction du noyau.
- `LinuxBootProtocol`, `PciConfigMechanism1` : versions historiques utilisées par les prototypes de `src/tools`. Le VMM a ses propres implémentations dans `src/vmm/boot` et `src/vmm/devices`.

Le programme consommateur doit lier `WinHvPlatform.lib`, et `WinHvEmulation.lib` s'il utilise l'émulateur d'instructions.
