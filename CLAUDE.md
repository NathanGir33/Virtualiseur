# Virtualiseur

## clangd / compile_commands.json

- Chaque fois qu'un fichier .cpp est ajouté ou supprimé d'un .vcxproj, exécute le script de génération dans `tools\` pour mettre à jour `build-clangd\compile_commands.json` :

  ```powershell
  .\tools\gen-compile-commands.ps1
  ```

  Le script ne lit que les projets listés dans `$projects` (`tools\gen-compile-commands.ps1`) : si un nouveau .vcxproj est créé, ajoute-le à cette liste avant de relancer le script.
