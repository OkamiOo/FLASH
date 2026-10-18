# FLASH

FLASH envoie les écrans, les fenêtres, les scènes d'OBS et les sons d'un PC vers un autre PC ou
vers OBS Studio, avec très peu de latence, sur le réseau local. Image et son partent sur la même
horloge et arrivent synchronisés. Il remplace NDI dans un double setup de stream, ou entre amis
qui s'échangent leurs flux.

- Envoi : capture des écrans et fenêtres par Windows, son par application, scènes et sources
  d'OBS, encodage par la carte graphique (H.264, HEVC ou AV1).
- Réception : plugin pour OBS Studio (sources « FLASH vidéo » et « FLASH audio »), ou fenêtre de
  visionnage dans l'appli.
- Transport : SRT chiffré, appairage par code entre les PC.

## Deux éditions

**Portable** : `FLASH-x.y.z-portable.zip`. Dézipper, lancer `flash.exe`. Au premier lancement,
FLASH propose d'installer son plugin dans OBS (OBS fermé ; Windows peut demander
l'autorisation). Rien n'est écrit à côté de l'exe, à part les mises à jour. Réglages, « Tout
retirer » efface le plugin, le lancement avec Windows et les réglages.

**Installable** : `FLASH-x.y.z-installation.exe`. Installe FLASH pour l'utilisateur seul, sans
demander de droits d'administrateur, avec une entrée dans le menu Démarrer et un désinstalleur.

Dans les deux cas, au premier envoi sur le réseau, Windows demande s'il faut autoriser FLASH :
acceptez pour les réseaux privés.

## Mises à jour

FLASH vérifie chaque jour s'il existe une nouvelle version. Le manifeste de chaque version donne
l'empreinte SHA-256 de chaque fichier et il est signé (Ed25519) ; la clé publique est dans
FLASH. Une mise à jour dont la signature ou une empreinte ne correspond pas est refusée. Seuls
les fichiers qui ont changé sont téléchargés, et la nouvelle version est essayée avant d'être
gardée : si elle ne démarre pas, l'ancienne revient. Le plugin d'OBS se met à jour depuis les
réglages, OBS fermé.

## Avertissement de Windows

FLASH n'est pas signé avec un certificat de signature de code : un certificat classique coûte
plus de 100 euros par an. Au premier lancement, Windows peut donc afficher « Windows a protégé
votre ordinateur ». Pour lancer FLASH quand même : « Informations complémentaires », puis
« Exécuter quand même ».

Ce message vient de SmartScreen, qui ne connaît pas encore le fichier ; il ne signale aucun
problème trouvé dans FLASH. Pour vérifier que le téléchargement est bien celui publié, comparez
son empreinte SHA-256, donnée avec chaque version, à celle que calcule PowerShell :

```
Get-FileHash FLASH-x.y.z-portable.zip -Algorithm SHA256
```

Les mises à jour automatiques, elles, sont signées (voir plus haut), indépendamment de
Windows.

## Compiler

Windows 10 ou 11 x64, Visual Studio 2022 Build Tools, CMake et Ninja, dépendances dans `.deps`
(voir CONCEPTION.md, § 8).

```
powershell -ExecutionPolicy Bypass -File build.ps1              # appli et plugin
powershell -ExecutionPolicy Bypass -File build.ps1 -Portable    # + édition portable dans dist\
powershell -ExecutionPolicy Bypass -File build.ps1 -Installeur  # + installeur dans dist\
```

La conception, les mesures et les choix sont dans CONCEPTION.md.

## Licence

FLASH, l'appli comme le plugin pour OBS Studio, est un logiciel libre sous licence GPL-3.0 ou
plus récente : voir `LICENSE`. Les sources de chaque version sont celles de son tag dans ce
dépôt.

Composants : SRT (MPL-2.0, sources sur https://github.com/Haivision/srt), mbedTLS (Apache-2.0),
Opus (BSD-3), Monocypher (BSD-2 ou CC0), nlohmann json, en-têtes NVENC (MIT), chargeur WebView2
(BSD-3). Leurs licences sont dans le dossier `licences` de chaque édition.
