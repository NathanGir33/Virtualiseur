# Tester la V1 WHP

Cette démo vérifie l’état d’entrée 32 bits protégé utilisé par le protocole de démarrage Linux x86, plus une sortie E/S traitée par le VMM. Elle ne lance pas le noyau Linux : le programme invité écrit `B32` sur le port virtuel `0xE9`, lit le nombre d’entrées E820 via `ESI` et s’arrête avec `HLT`.

## Lancer

Exécutable Debug :

`src/tools/WhpV1Demo/out/x64/Debug/VirtualiseurWhpV1Demo.exe`

Depuis PowerShell, à la racine du dépôt :

```powershell
.\src\tools\WhpV1Demo\out\x64\Debug\VirtualiseurWhpV1Demo.exe
```

Si WHP n’est pas disponible, active la fonctionnalité facultative Windows **Windows Hypervisor Platform**, puis relance. Le programme ne demande pas de désactiver Hyper-V, VBS ou l’intégrité de la mémoire.

## Résultat attendu

```text
Sortie console invite (port 0xE9): B32
Succes: le vCPU a execute du code 32 bits en mode protege via WHP.
E820 entries lues depuis ESI (boot_params): 3.
Ce test valide l'etat d'entree 32 bits du protocole Linux; il ne demarre pas encore le noyau.
```

Un message d’erreur `HRESULT` signifie généralement que la fonctionnalité WHP n’est pas activée ou accessible sur cette installation de Windows. Envoie-moi le texte exact affiché, y compris le code HRESULT.

Cette démonstration n’émule pas encore de disque, d’UEFI, d’écran, de clavier ou de souris. Le prochain jalon sera un démarrage d’invité plus substantiel ; booter une distribution Linux depuis une ISO demande encore firmware et périphériques.
