# FLASH : conception de la V1

FLASH (Fast Latency Audio-video Stream Handler) envoie plusieurs vidéos et plusieurs sons
séparés d'un PC à un autre, avec une latence très faible et une horloge commune. Usages :
double PC de stream, ou un groupe de streamers qui s'échangent leurs flux. Il remplace NDI.
La V1 vise le réseau local sous Windows ; Internet vient ensuite.

Deux pièces :

- l'appli FLASH (`flash.exe`) : envoie (écrans, fenêtres, son par application) et reçoit
  (visionneuse) ;
- le plugin OBS (`flash-obs.dll`) : reçoit dans OBS sous forme de sources « FLASH vidéo »
  et « FLASH audio ».

## 1. Objectifs chiffrés

Mesurés en 1080p60, réseau filaire ou boucle locale, latence SRT réglée à 20 ms.

| Critère | Objectif |
|---|---|
| Latence image-à-image (capture chez l'émetteur jusqu'à l'image composée dans OBS) | médiane ≤ 40 ms, 99 % ≤ 60 ms |
| Écart son/image dans OBS | ≤ 10 ms en moyenne, jamais plus de 20 ms |
| Dérive son/image sur 4 h | ≤ 5 ms |
| Émetteur (1 vidéo 1080p60 + 3 sons) | CPU ≤ 3 % du 5900X, NVENC ≤ 20 %, 3D ≤ 2 %, RAM ≤ 80 Mo hors interface |
| Récepteur OBS, par flux 1080p60 | CPU ≤ 2 %, décodage sur le GPU |
| Débit par défaut | H.264 25 Mb/s en 1080p60 (réglable de 6 à 80), Opus 192 kb/s stéréo par son |
| Reconnexion (émetteur relancé, câble débranché puis rebranché) | image revenue en moins de 2 s |
| Tenue | 8 h sans plantage ni fuite mémoire |
| Simplicité | premier appairage en moins d'une minute, ensuite plus rien à faire |

Budget de latence prévu : capture 0 à 2 ms, conversion et encodage 3 à 4 ms, SRT 20 ms,
démultiplexage et décodage 3 à 4 ms, attente du prochain rendu d'OBS 0 à 17 ms (8 en moyenne).

## 2. Architecture

```
PC émetteur : appli FLASH                         PC récepteur : OBS + plugin FLASH

Écran 1 -> WGC -> NV12 -> NVENC --+                         +-> décodage GPU -> FLASH vidéo « Écran 1 »
Fenêtre -> WGC -> NV12 -> NVENC --+                         +-> décodage GPU -> FLASH vidéo « Fenêtre »
Jeu     -> loopback -> Opus ------+-> MPEG-TS -> SRT ==LAN==> SRT -> démux -+-> Opus -> FLASH audio « Jeu »
Discord -> loopback -> Opus ------+   (une horloge : QPC)   +-> Opus -> FLASH audio « Discord »
Micro   -> WASAPI   -> Opus ------+                         +-> Opus -> FLASH audio « Micro »
```

Un moteur C++ commun (`core/`) sert à l'appli et au plugin : horloge, MPEG-TS, SRT,
appairage, découverte, codecs. Tout ce qui touche au temps réel est natif, sans ramasse-miettes.

### 2.1 Émetteur

- Capture vidéo : Windows.Graphics.Capture (C++/WinRT), écrans et fenêtres, sans bordure
  jaune, curseur au choix, cadence plafonnée par un seau à jetons (§ 12). Pas d'injection dans les
  jeux : rien que l'anti-triche puisse reprocher.
- Conversion : `ID3D11VideoProcessor` (BGRA vers NV12 BT.709, mise à l'échelle), sur le GPU,
  quel que soit le fabricant.
- Encodage : NVENC appelé directement (en-têtes MIT), texture D3D11 en entrée sans copie.
  H.264 par défaut, HEVC et AV1 en option (l'AV1 seulement si l'encodeur et tous les
  récepteurs le gèrent, § 16). Réglages : preset P1 à P3, tuning ultra-low-latency,
  aucune B-frame, CBR avec un tampon d'une image, intra-refresh sur 1 s, IDR à la demande
  (nouveau récepteur, perte signalée). Repli : encodeur matériel Media Foundation (AMD, Intel),
  puis logiciel Media Foundation en dernier recours.
- Son par application : WASAPI en loopback de processus
  (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, arbre de processus inclus). Une appli cochée
  = une piste. Les applis sont suivies par nom d'exécutable : si Discord redémarre, la piste se
  raccroche seule. Pistes en plus : micro, son du système.
- Opus : libopus, 48 kHz, trames de 10 ms, mode `RESTRICTED_LOWDELAY`.
- Horloge : tout est horodaté en QPC. Vidéo : `SystemRelativeTime` de la trame WGC.
  Son : position QPC rendue par `IAudioCaptureClient::GetBuffer`, recalée en continu pour
  suivre la dérive de la carte son.
- Un encodage, N récepteurs : un seul flux TS est produit, puis écrit sur chaque connexion
  SRT. Un récepteur lent ne freine pas les autres (envoi non bloquant par socket). Sans
  récepteur, rien n'est encodé.

### 2.2 Format et transport

- MPEG-TS, écrit par un petit multiplexeur maison : un PID par piste (vidéo 0x100+n,
  son 0x200+n), un PID de PCR dédié (toutes les 20 ms, pour qu'un envoi sans vidéo marche
  aussi), un PID de métadonnées (noms des pistes, version). Le flux reste du TS standard : VLC
  (déjà installé) l'ouvre pour déboguer.
- Chaque image vidéo se termine par un paquet TS marqué (données privées du champ
  d'adaptation). Le démultiplexeur FLASH livre l'image dès ce paquet, sans attendre la suivante.
  VLC et FFmpeg ignorent ce marqueur.
- SRT en mode live, une connexion par couple émetteur/récepteur, toutes les pistes dedans.
  L'émetteur écoute (port UDP 9710), les récepteurs appellent. Latence SRT : 20 ms en filaire,
  60 ms en Wi-Fi, choisie seule (mode Auto) d'après le RTT et les pertes. Paquets limités à
  1316 octets, envoyés dès la fin d'une image ou d'une trame son.
- Voie de retour sur la même connexion SRT (bidirectionnelle) : demande d'IDR, pistes voulues
  par le récepteur (les autres ne lui sont pas envoyées), formats vidéo qu'il sait décoder,
  statistiques.
- AV1 : pas de stream_type normalisé dans le TS ; PES privé avec un descripteur
  d'enregistrement « AV01 », une unité temporelle par PES (§ 16).

### 2.3 Horloge commune et synchro

Le récepteur tient, par émetteur, un seul modèle `t_OBS = t_émetteur + décalage`, avec
correction de dérive. Le décalage vient du minimum glissant de (heure d'arrivée - PTS), la
dérive d'une régression sur plusieurs minutes. On y ajoute une marge fixe (latence de lecture).
Toutes les pistes d'un émetteur passent par ce même modèle : elles restent synchronisées entre
elles, quoi que fasse le réseau.

- Vidéo : l'image décodée est remise à OBS à son heure exacte, par un minuteur haute
  résolution, dans une source asynchrone non tamponnée. OBS l'affiche au rendu suivant.
- Son : remis à OBS avec un horodatage absolu dans l'horloge d'OBS. OBS reconnaît ces
  horodatages comme directs quand ils sont à moins de 2 s de son horloge
  (`source_output_audio_data` dans `libobs/obs-source.c`), donc des sources son et image
  séparées restent alignées.
- Dérive entre deux PC (quelques dizaines de ppm) : le son est rééchantillonné très
  finement (± 200 ppm) au lieu de laisser OBS combler par des sauts.
- En boucle locale, émetteur et OBS partagent la même horloge QPC : la latence se mesure
  directement, à la microseconde.

### 2.4 Réception dans OBS

- Une connexion par émetteur, partagée par toutes les sources FLASH qui le visent.
- Démultiplexage maison, décodage par le FFmpeg déjà livré avec OBS (FFmpeg 8.1.2, d3d11va,
  donc NVDEC, AMD ou Intel), copie NV12 puis `obs_source_output_video`. L'AV1 passe par le
  décodeur `av1` de FFmpeg, uniquement sur la carte graphique. Son : Opus décodé par
  libopus puis `obs_source_output_audio`.
- Sources : « FLASH vidéo » (émetteur, piste, son associé facultatif) et « FLASH audio »
  (émetteur, piste). Les listes se remplissent avec les émetteurs détectés et leurs pistes.
  Ajout manuel par adresse IP possible.
- Pas de dock Qt en V1 : tout passe par les propriétés des sources (pas de dépendance Qt).
- Perte de paquet : marquée par le compteur de continuité TS, image passée au décodeur quand
  même, demande d'IDR à l'émetteur (une par seconde au plus). L'intra-refresh nettoie l'image
  en moins d'une seconde dans tous les cas.

### 2.5 Réception dans l'appli

Une visionneuse : fenêtre D3D11 (plein écran possible), son vers la sortie choisie (utile pour
envoyer le son d'un ami dans un câble virtuel). Décodage par Media Foundation, intégré à
Windows, pour garder l'appli légère. H.264 marche partout ; HEVC demande l'extension HEVC de
Windows, AV1 l'extension AV1 et une carte graphique qui le décode.

### 2.6 Découverte, appairage, sécurité

- Découverte : le récepteur envoie une question en diffusion et en multicast (UDP 9711),
  chaque émetteur répond en unicast (identifiant, nom du PC, ports, pistes). Windows laisse
  passer une réponse unicast à une diffusion pendant 3 s : OBS n'a besoin d'aucune règle de
  pare-feu. Un émetteur dont l'adresse IP change est retrouvé par son identifiant.
- Appairage : l'appli affiche un code d'invitation de 8 caractères (ex. `7KQ4-M9XP`). Le
  récepteur le saisit une fois. Échange CPace sur X25519 (Monocypher) par TCP 9712 : le code ne
  circule pas et ne peut pas être cassé hors ligne à partir d'une capture réseau. Résultat :
  une clé de 256 bits propre à ce récepteur, gardée des deux côtés, chiffrée par DPAPI.
  Essais limités (5 par minute). « Nouveau code » invalide l'ancien pour les futurs
  appairages ; chaque récepteur autorisé peut être retiré de la liste.
- Chiffrement : SRT en AES-256, avec comme phrase secrète la clé de ce récepteur (choisie
  dans le rappel d'écoute SRT selon l'identifiant du récepteur). Un pair non appairé ne peut
  même pas se connecter. En réseau local la connexion est directe : le chiffrement est de bout
  en bout.
- Limite connue : le SRT livré avec OBS (1.5.2, mbedTLS) chiffre en AES-CTR, sans contrôle
  d'intégrité. Le contrôle d'intégrité arrive avec la couche AEAD prévue pour Internet (§ 7).

### 2.7 Interface

- Appli Win32 en C++ avec une interface WebView2 (comme NovaPanel), en français, icône dans la
  zone de notification. Fenêtre fermée : WebView2 libéré, seul le moteur tourne.
- Pages : **Envoyer** (sources vidéo avec aperçu, applis audio à cocher, micro, son système),
  **Recevoir** (émetteurs détectés, bouton Regarder), **Appareils** (code d'invitation,
  récepteurs autorisés), **Réglages** (qualité Économie / Haute / Très haute, réseau
  Auto / Filaire / Wi-Fi, démarrage avec Windows).
- Réglages par défaut prévus pour marcher sans y toucher.

## 3. Ce qu'on reprend de l'existant

| Projet | Repris | Écarté |
|---|---|---|
| Beam (C#, NativeAOT, MIT) | horodatage OBS, délai fixe pour aligner plusieurs sources | images compressées une à une (brut, LZ4, QOI, JPEG) : débit énorme, pas d'Internet ; TCP ; NativeAOT exige MSVC de toute façon |
| obs-teleport (Go, GPL-2.0) | découverte multicast, aucun réglage à faire | JPEG image par image, 1 Gb/s conseillé, CPU ; pas de son par appli ; pas de chiffrement |
| DistroAV / NDI | découverte par nom de PC, plusieurs flux par machine | runtime NDI propriétaire à installer à part ; NDI plein débit très gourmand, NDI HX plus lent ; instable à l'usage |
| Sunshine / Moonlight (C++, GPL-3.0) | réglages NVENC basse latence (P1, ULL, sans B-frames, intra-refresh), IDR sur perte, FEC pour Internet | une vidéo et un son seulement, pensé pour le jeu à distance ; appairage par PIN à 4 chiffres attaquable hors ligne |
| Sortie SRT d'OBS + Source média | MPEG-TS sur SRT, plusieurs pistes son, phrase secrète | une seule vidéo (le programme) ; réception par FFmpeg avec sondage et tampon réseau : de 200 ms à plus d'une seconde |

## 4. Écarts avec l'architecture de départ

1. Repli d'encodage par Media Foundation plutôt qu'AMF et QSV appelés directement : ce PC
   n'a ni carte AMD ni Intel pour tester ces deux voies, Media Foundation passe par les mêmes
   puces et ne demande aucune DLL. AMF direct si les essais sur une carte AMD le justifient.
2. Clé SRT issue d'un échange CPace, et non dérivée du code : dérivée directement, elle se
   casserait hors ligne à partir d'une poignée de main SRT capturée.
3. Démultiplexeur TS maison côté réception : celui de FFmpeg attend le début de l'image
   suivante pour clore une image vidéo, soit une image de latence en plus. Le flux reste du TS
   standard.
4. Découverte par question/réponse UDP plutôt que mDNS : aucune règle de pare-feu côté OBS,
   pas de Bonjour.
5. Visionneuse de l'appli décodée par Media Foundation : pas de FFmpeg de 45 Mo dans l'appli.
6. Instance OBS portable dédiée aux tests : on la lance et on l'arrête à volonté sans
   jamais toucher à l'OBS de stream.

## 5. Étapes

Chaque étape se termine par une mesure dont les résultats sont notés.

**(a) 1 vidéo + 1 son de bout en bout, en boucle locale (127.0.0.1) vers OBS**

- Squelette CMake, moteur, plugin chargé dans l'OBS de test.
- Mire générée par l'appli : horodatage QPC et numéro d'image en code-barres, flash blanc
  chaque seconde, bip de 1 kHz aligné à l'échantillon près sur le flash.
- NVENC, TS, SRT ; plugin : SRT, démux, décodage, remise à l'heure.
- Mesure automatique (§ 6). Fin d'étape : objectifs du § 1 tenus en boucle locale, flux lisible
  dans VLC.

**(b) Plusieurs sources et son par application**

- WGC écrans et fenêtres, mise à l'échelle, plusieurs sessions NVENC, repli Media Foundation.
- Loopback par processus, micro, son système, suivi des applis par nom.
- Plugin : connexion partagée, plusieurs sources, IDR à la demande, pistes à la carte,
  rattrapage de dérive. Essai de 4 h.

**(c) Interface, découverte, appairage**

- Interface WebView2, zone de notification, découverte, CPace, clés par récepteur,
  reconnexion automatique, propriétés des sources OBS, visionneuse.
- Premiers essais réseau réel (deuxième PC si disponible).

**(d) Installeur unique**

- Inno Setup : appli dans `Program Files\FLASH`, plugin dans
  `%ProgramData%\obs-studio\plugins\flash` (survit à une réinstallation d'OBS), règle de
  pare-feu pour `flash.exe`, désinstallation propre, avertissement si OBS est ouvert.
- Vérification qu'aucun chemin de la machine de compilation ne reste dans les binaires ni
  dans l'installeur.

## 6. Méthode de mesure

- Latence : le plugin, en mode mesure, lit le code-barres de chaque image décodée et note
  l'heure de capture, l'heure de remise à OBS et l'heure du rendu OBS qui l'affiche
  (`video_tick`). En boucle locale, tout est dans la même horloge QPC.
- Synchro : instant du flash affiché comparé à l'instant du bip remis à OBS ; puis
  vérification sur un enregistrement OBS, analysé par un petit outil (FFmpeg d'obs-deps) qui
  retrouve flashs et bips.
- Charge : compteurs CPU et GPU (NVENC, 3D) pendant 10 minutes.
- Pilotage de l'OBS de test par obs-websocket (intégré à OBS) depuis un script Node.
- Les essais lourds se font quand le PC est libre.

## 7. Après la V1 : Internet

- Fonctionne déjà à travers un VPN maillé (Netbird, Tailscale) en ajoutant l'émetteur par IP.
- Relais UDP aveugle : il fait suivre les paquets SRT sans jamais avoir la clé, le
  chiffrement reste de bout en bout.
- Couche AEAD interne (AES-256-GCM par paquet, clé de groupe distribuée aux récepteurs
  appairés) : intégrité, et diffusion par le relais (l'émetteur n'envoie qu'une copie).
- Traversée de NAT (mode rendezvous SRT), FEC SRT, débit adaptatif.
- Envoi depuis OBS (filtre ou sortie FLASH) si le besoin se confirme.

## 8. Outils et construction

- Visual Studio 2022 Build Tools (MSVC v143, SDK Windows 11, CMake, Ninja). C++20.
- obs-deps `2026-07-15` (celles d'OBS 32.2.2) : FFmpeg 8.1.2, SRT 1.5.2, mbedTLS 3.6.4,
  Opus 1.5.2, en-têtes NVENC et AMF, WIL. OBS 33 (RC du 2026-10-09) garde le même FFmpeg et le
  même SRT : le plugin devrait passer sans recompilation, à vérifier à sa sortie.
- En-têtes libobs tirés des sources OBS 32.2.2, bibliothèque d'import générée depuis `obs.dll`.
- SDK WebView2 (NuGet), Monocypher (source intégrée).
- Compilation depuis un lecteur `subst`, Release sans PDB (`/DEBUG:NONE`), `/Brepro`, puis
  recherche dans les octets des binaires de tout chemin local.

Organisation du dépôt :

```
core/          moteur commun
app/           flash.exe
obs/           flash-obs.dll
tools/         mire et mesure
installer/     script Inno Setup
third_party/   Monocypher
.deps/         obs-deps, sources OBS, SDK WebView2 (hors dépôt)
.test-obs/     OBS portable de test (hors dépôt)
```

## 9. Licences

| Composant | Licence | Conséquence |
|---|---|---|
| libobs | GPL-2.0 ou ultérieure | le plugin doit être sous licence compatible GPL |
| FFmpeg d'OBS | GPL-3.0 (compilé avec `--enable-gpl --enable-version3`) | le plugin, qui s'y lie, est diffusé sous GPL-3.0 |
| libsrt | MPL-2.0 | sources de libsrt à indiquer |
| mbedTLS | Apache-2.0 | compatible GPL-3.0 (pas GPL-2.0 seule) |
| Opus | BSD-3 | mention de licence |
| Monocypher | BSD-2 ou CC0 | mention de licence |
| En-têtes NVENC (nv-codec-headers), AMF, WIL, C++/WinRT | MIT | mention de licence |
| SDK WebView2 | licence Microsoft | redistribution du chargeur autorisée |

Choix envisagé : tout FLASH sous GPL-3.0 ou ultérieure. Diffuser les binaires implique alors de
publier les sources. Autre possibilité : l'appli sous une autre licence si elle ne contient
aucun code GPL, le plugin restant sous GPL-3.0. À trancher avant la première diffusion.

## 10. Risques connus

- Minuteries Windows à 15,6 ms par défaut : minuteries haute résolution et `timeBeginPeriod(1)`
  dans l'appli et le plugin.
- Nombre de sessions NVENC simultanées limité sur GeForce, à partager avec les encodages d'OBS.
- WGC : contenus protégés (DRM) capturés en noir.
- Loopback par processus : Windows 10 2004 au minimum.
- Wi-Fi : latence SRT relevée automatiquement, objectifs du § 1 valables en filaire.
- Binaires non signés : avertissement SmartScreen au premier lancement chez les autres.
- Les DLL de SRT et de FFmpeg livrées avec OBS dépendent du redistribuable Visual C++ :
  l'installeur devra l'inclure, ou l'appli compilera libsrt elle-même en CRT statique.

## 11. Mesures de l'étape (a)

Boucle locale (127.0.0.1), mire 1080p60 + bip, NVENC H.264 25 Mb/s, Opus 10 ms (libopus compilé
dans l'appli : un exe d'environ 580 Ko plus `srt.dll`), latence SRT 20 ms, OBS 32.2.2 portable. Mesures
du plugin (`tools/essai-boucle.ps1`, `tools/analyse_mesure.py`), essai de 5 minutes :

| Critère | Mesure |
|---|---|
| Images | 17 992 reçues, aucune perdue en route, 2 non affichées au démarrage |
| Capture -> réception (encodage + SRT) | 23,3 ms (99 % sous 25,2 ms) |
| Réception -> image décodée et copiée | 1,8 ms (99 % sous 2,2 ms) |
| Décalage d'horloge (heure de présentation) | 37,1 ms, stable |
| Latence capture -> image composée par OBS | 37,7 à 52,7 ms selon les sessions, constante dans une session |
| Écart son/image | -0,4 ms en moyenne, écart-type 0,1 ms, dérive non mesurable sur 5 min |
| Charge | envoi 0,2 % du CPU, OBS de test entier 1,2 % ; GPU : encodeur 7 %, décodeur 6 % |

Vérification indépendante sur un enregistrement d'OBS (`tools/analyse_enregistrement.py`) :
-0,26 ms avec x264, conforme au plugin.

Enseignements :

- Le décalage d'horloge est fixé par le son : trame de 10 ms + retard d'Opus 2,5 ms + SRT 20 ms.
  La vidéo est prête 12 ms plus tôt et attend. Leviers pour la médiane de 40 ms : latence SRT à
  15 ms en filaire (réglage des deux côtés : SRT retient la plus grande des deux), trames son
  plus courtes si la capture le permet.
- Une image est montrée au premier rendu d'OBS qui suit son heure de présentation : la latence
  varie donc d'une session à l'autre de 0 à 16,7 ms selon le calage entre les images de
  l'émetteur et les rendus d'OBS. Viser le rendu le plus proche fait perdre des images quand
  OBS encode lui-même en NVENC (le GPU partagé retarde une image sur trois).
- Aucune écriture de fichier dans un fil temps réel : un journal écrit au mauvais moment a
  bloqué l'émetteur 80 ms. Les journaux passent par un fil à part.
- Avec un encodeur GPU (NVENC), OBS horodate chaque image avec l'heure du rendu précédent
  (`output_gpu_encoders` dans `libobs/obs-video.c`) : dans un enregistrement ou un stream
  NVENC, l'image de toute source paraît en avance d'une image sur le son (-16,7 ms à 60 i/s).
  FLASH suit l'horloge d'OBS comme ses autres sources ; une compensation optionnelle pourra
  être ajoutée.

## 12. Étape (b) : sources réelles

Fait :

- Capture WGC d'écrans et de fenêtres : fenêtre retrouvée par exécutable ou par titre, rouverte
  si elle disparaît, redimensionnement suivi ; conversion BGRA vers NV12 et mise à l'échelle par
  le processeur vidéo du GPU.
- Son par application (processus racine retrouvé par son nom, suivi s'il redémarre), son du
  système (tout sauf FLASH), micro.
- Plusieurs pistes par envoi, un encodeur par piste. Le récepteur ne demande que les pistes
  utilisées par ses sources, et l'émetteur n'encode que celles-là.
- Repli d'encodage Media Foundation : encodeur matériel de la carte, puis encodeur logiciel de
  Windows. Pas d'intra-refresh générique : une image clé toutes les 2 s.
- En attendant l'interface, ligne de commande : `flash --liste`, puis par exemple
  `flash --ecran 1 --appli discord.exe --micro`.
- Banc d'essai (`flash --banc`) : une fenêtre et un son qui se comportent comme un jeu, pour
  mesurer la vraie chaîne de capture (`tools/essai-boucle.ps1 -Banc`).

Mesures (boucle locale, 1080p60, SRT 20 ms) :

| Essai | Résultat |
|---|---|
| Banc : fenêtre par WGC et son de l'appli par loopback | toutes les images livrées par Windows encodées et affichées ; synchro rapportée aux horodatages de capture : -6 à -13 ms selon les sessions, stable dans une session |
| 3 vidéos (mire, fenêtre, écran) et 3 sons | synchro de la piste principale -0,2 ms ; envoi 0,85 % du CPU, OBS de test 2,4 % ; GPU : encodeur 21 %, décodeur 17 % |
| HEVC | latence 37,5 ms, synchro -1,4 ms |
| Media Foundation (encodeur NVIDIA de Windows) | encodage 2,7 ms (NVENC direct : 2,1 ms), synchro -0,4 ms |
| Encodeur logiciel de Windows | 3,9 % du CPU sur la mire, synchro -0,6 ms |
| Micro | capté et encodé |

Enseignements :

- Windows date les images capturées sur la cadence de composition de l'écran principal
  (165 Hz ici) : une source à 60 i/s arrive avec quelques millisecondes de gigue. Côté
  émetteur, la cadence est limitée par un seau à jetons (une grille stricte jetait des
  images) ; côté récepteur, les horodatages sont lissés sur la période réelle de la source,
  sinon deux images tombent sur le même rendu d'OBS (un quart des images perdues avant ce
  lissage).
- L'horodatage WGC correspond à l'affichage de l'image à l'écran, environ 25 ms après son
  rendu sur un écran à 60 Hz. Le son du loopback est daté autrement : l'écart de 6 à 13 ms
  vient de Windows, pas du transport (la mire, aux horodatages exacts, reste sous 1 ms). Seule
  une mesure physique (filmer l'écran et enregistrer le son en même temps) dira laquelle des
  deux heures est la bonne ; en attendant, pas de correction à l'aveugle. L'écart se règle au
  besoin dans OBS, par source (décalage de synchronisation).
- Avec plusieurs vidéos, le son s'aligne sur la phase de la piste vidéo principale (la
  première).
- Une fenêtre qui affiche sans se caler sur le balayage de l'écran perd des images à la
  composition (comme beaucoup de jeux en fenêtre sans V-Sync) : FLASH ne peut transmettre que
  ce que Windows compose.
- Sans récepteur, la capture et la conversion continuent (l'encodage, lui, s'arrête) : à
  couper aussi plus tard pour ne rien consommer au repos.

Reste pour cette étape : un essai d'endurance de 4 h, quand le PC est libre assez longtemps.

## 13. Étape (c) : interface, découverte, appairage

Fait :

- Appli fenêtrée (WebView2) avec une icône dans la zone de notification : fermer la fenêtre
  n'arrête pas l'envoi, une seule instance à la fois, démarrage avec Windows au choix. On y
  coche les écrans, fenêtres, applis, le son du système et les micros ; on y voit le code
  d'invitation, les PC autorisés et les réglages (qualité, format, réseau, connexion).
- Identité, sources et réglages dans `%APPDATA%\FLASH\reglages.json` ; clés des récepteurs
  chiffrées par DPAPI.
- Découverte (UDP 9711) : question en diffusion, en multicast et à 127.0.0.1, réponse directe
  avec le nom du PC, les ports et les pistes.
- Appairage CPace sur X25519 (Monocypher), TCP 9712 : code de 8 caractères (base 32 de
  Crockford, saisie tolérante : minuscules, sans tiret, O pour 0), 5 essais par minute, clé de
  256 bits par couple émetteur/récepteur.
- SRT : l'émetteur choisit la phrase secrète d'après l'identifiant du récepteur et refuse les
  inconnus ; flux en AES-256. Un mode « ouvert » (`--libre`) sert aux essais.
- Plugin : émetteurs détectés et appairés dans une liste, adresse à saisir en secours, bouton
  Appairer, piste choisie par son nom, réglage de connexion. L'émetteur est retrouvé par son
  identifiant même s'il change d'adresse.
- Choix du rendu OBS avec une hystérésis de 1,5 ms : une heure visée à la frontière entre deux
  rendus ne fait plus alterner les images (latence bimodale vue avant ce correctif).

Vérifié en boucle locale : découverte ; mauvais code refusé ; appairage réussi, y compris avec
le code tapé en minuscules sans tiret ; connexion SRT refusée sans clé ; flux chiffré de bout en
bout jusqu'à l'OBS de test, pistes choisies par nom (latence 39 à 45 ms selon la session,
synchro -0,4 à -0,7 ms) ; affichage de l'interface et logique de la page.

### Visionneuse de l'appli

- Section « Regarder » de l'interface : les PC du réseau où FLASH est ouvert, ce PC compris
  (aperçu de ce qu'il envoie, sans le son par défaut). Pour un PC pas encore appairé : son
  code, puis « Appairer ». Ensuite on choisit l'image, le son (un, tous ou aucun) et la sortie
  son, puis « Regarder ». Un PC hors de portée de la découverte s'ajoute par son adresse IP.
- Ligne de commande : `flash --voir émetteur [--piste nom] [--son nom|tous] [--sortie nom]
  [--plein-ecran] [--ecran-vue N]`, avec ou sans envoi dans le même processus (`moi` : ce PC).
  `--appairer adresse code` enregistre maintenant l'appairage dans les réglages.
- Une fenêtre par émetteur, plein écran au double-clic, avec F11 ou Alt+Entrée, curseur
  masqué au bout de 2 s. Sans image, la fenêtre dit pourquoi (connexion, piste absente,
  décodeur manquant).
- Décodage par Media Foundation, sur la carte graphique (DXVA), copie dans une réserve de
  textures NV12, conversion vers l'écran par le processeur vidéo du GPU. HEVC demande
  l'extension HEVC de Windows ; sinon la fenêtre le dit.
- Même horloge que dans OBS : chaque image est présentée au milieu de l'intervalle qui
  précède le premier balayage de l'écran à partir de son heure, avec la même hystérésis. Un
  fil suit les balayages de l'écran où se trouve la fenêtre (`WaitForVBlank`) ; l'écart moyen
  avec l'heure d'affichage que donne Windows (`GetFrameStatistics`) corrige l'estimation.
- Son : chaque piste est décodée (libopus) et rangée sur l'horloge de l'émetteur dans un
  anneau commun, les pistes s'y additionnent. La sortie (WASAPI, plus petite période permise
  par Windows) lit l'anneau à l'heure où ses échantillons seront entendus, calée sur l'image
  réellement affichée, avec un rattrapage de vitesse de 0,5 % au plus.
- Le son n'est lu qu'une latence de sortie avant d'être entendu : l'horloge commune en tient
  compte, l'image attend donc d'autant. Sans son, l'image n'attend que le réseau et le
  décodage.
- Aperçu de ce PC : la visionneuse se connecte en local avec une clé tirée pour la session ;
  elle ne compte pas dans « PC qui reçoivent ».
- Plusieurs FLASH peuvent écrire `reglages.json` à la fois (interface, ligne de commande) :
  chaque écriture relit le fichier et ne remplace que ses propres parties, sous un verrou
  commun. Sans ce verrou, un appairage fait pendant un envoi en ligne de commande se perdait.
- Plugin : une source réglée par adresse IP prend la clé de l'émetteur appairé qui répond à
  cette adresse. Avant, l'émetteur refusait la connexion (il exige le chiffrement pour un
  récepteur appairé).

Mesures en boucle locale (mire 1080p60, SRT 20 ms, fenêtre sur un écran à 60 Hz, heure
d'affichage estimée comme ci-dessus) :

| Essai | Latence capture -> écran | Synchro son/image | Images perdues |
|---|---|---|---|
| Image seule, 30 s | 41,3 ms (99 % sous 43,1 ms) | | 0 |
| Image et son vers « Steam Streaming Speakers », 30 s | 106,3 ms | -0,08 ms (écart-type 0,11) | 0 |
| Plugin OBS après ces changements, 30 s | 41,3 ms | -0,40 ms | 0 |

La sortie virtuelle de Steam met 59 ms entre l'écriture d'un échantillon et son écoute
(période de 10 ms, dont 20 ms gardés d'avance par FLASH) : c'est elle qui porte la latence de
l'essai avec son. Une vraie sortie (casque, enceintes) reste à mesurer.

Reste pour cette étape :

- les essais en réseau réel : au premier lancement en « Réseau local », Windows demandera
  l'autorisation du pare-feu (l'installeur ajoutera la règle) ;
- un passage à la main dans l'interface (Regarder) et dans les propriétés des sources FLASH
  d'OBS ;
- la latence du son sur une vraie sortie, et une mesure physique de la synchro (filmer
  l'écran et enregistrer le son).

## 14. Étape (d) : installeur

Fait :

- `build.ps1 -Installeur` compile l'appli puis l'installeur (Inno Setup 6.7.3, dans
  `.deps\innosetup`) depuis le lecteur virtuel : `dist\FLASH-<version>-installation.exe`,
  3 Mo. La version vient de `CMakeLists.txt`. Script : `installer\flash.iss` (UTF-8 avec BOM,
  sans quoi Inno Setup lit les accents en ANSI).
- Ce qui est installé :
  - l'appli dans `Program Files\FLASH` : `flash.exe`, `srt.dll` et le runtime Visual C++ dont
    `srt.dll` a besoin (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`, copiés du
    dossier de redistribution des Build Tools), rien à installer à part ;
  - les licences des composants livrés dans `licences\` (SRT, mbedTLS, Opus, Monocypher,
    nlohmann/json, en-têtes NVENC de NVIDIA, WebView2) ;
  - le plugin dans `%ProgramData%\obs-studio\plugins\flash-obs` (`bin\64bit` et `data`),
    comme les autres plugins tiers : il survit à une réinstallation d'OBS. Composant à part,
    qu'on peut décocher ;
  - un raccourci dans le menu Démarrer, une icône sur le bureau au choix.
- Pare-feu : une tâche cochée par défaut, et visible, ajoute une règle entrante « FLASH »
  pour `flash.exe` sur les réseaux privés seulement. Elle est retirée à la désinstallation.
- Mise à jour : si FLASH tourne, l'installeur lui envoie le message « FLASH-quitter » (FLASH
  quitte comme avec son menu Quitter) et attend la fin de son verrou d'instance unique. Jamais
  d'arrêt forcé. Si OBS est ouvert (verrou `OBSStudioCore`), l'installeur demande de le fermer
  et attend ; il ne le ferme jamais lui-même.
- Désinstallation : mêmes précautions, retrait du lancement avec Windows, puis question : effacer
  aussi les réglages et appairages (FLASH et carnet du plugin) ? Non par défaut.
- Windows 10 2004 au minimum. Le son par application et le son du système sans FLASH
  demandent Windows 11 (capture par processus) ; sous Windows 10, ces sources restent muettes.
- Vérifié : compilation sans avertissement ; une copie sans compression ne contient aucun chemin
  de la machine ; propriétés du fichier (FLASH 0.1.0, « Installation de FLASH ») ; fermeture de
  FLASH par le message de l'installeur (FLASH caché dans la zone de notification, sorti en moins
  d'une seconde, code 0).
- Pas encore vérifié : l'installation elle-même (droits d'administrateur, règle de pare-feu),
  à lancer à la main.

Limites connues :

- L'installeur n'est pas signé : au premier lancement, SmartScreen affiche « Windows a protégé
  votre ordinateur » (« Informations complémentaires », puis « Exécuter quand même »). Un
  certificat de signature de code règlerait ça avant une diffusion publique.
- La licence de FLASH lui-même reste à choisir avant toute diffusion.

## 15. Nouvelle interface et envoi depuis OBS (conception)

### Interface

- Maquette retenue : en-tête (éclair, FLASH, nom du PC, latence et débit en direct,
  pastille d'état), navigation latérale (Envoyer, Regarder, Appareils, Réglages), colonne
  latérale (code d'invitation, réglages rapides), couleur d'accent ambre `#EF9F27`, thème
  clair ou sombre de Windows. Icônes dessinées dans la page (SVG) : aucune ressource en ligne.
- Envoyer : filtres en pastilles (Tout, Scènes OBS, Sources OBS, Écrans, Fenêtres), sources en
  tuiles avec aperçu et interrupteur, tuile « Ajouter » (choix d'une fenêtre ouverte), son
  par application en lignes (icône de l'appli, vumètre, volume envoyé, interrupteur).
- Tout s'applique tout de suite, sans Appliquer ni Annuler. L'émetteur ajoute et retire une
  piste en marche (nouvelle table de pistes, version de PMT incrémentée), sans couper les
  autres pistes ni les récepteurs. Les réglages qui refont un encodeur (qualité, format) sont
  temporisés (400 ms après le dernier mouvement) et ne refont que les encodeurs vidéo.
- Volume envoyé : gain appliqué avant Opus, changé sans rien redémarrer. Vumètres : niveau de
  crête de chaque appli lu dans les sessions audio de Windows (aucune capture en plus), celui
  de la piste envoyée pour les sources actives ; 15 mises à jour par seconde, seulement quand
  la fenêtre est visible.
- Aperçus : vignettes de 320×180 réduites par le processeur vidéo du GPU, en JPEG (WIC). Source
  envoyée : sa dernière image, 4 fois par seconde au plus. Écran ou fenêtre pas envoyé : une
  capture ponctuelle par seconde. Scènes et sources OBS : rendues par le plugin à la même
  taille. Rien quand la fenêtre est réduite, cachée ou sur une autre page.
- Latence de l'en-tête, sans horloge commune entre les PC : temps d'encodage (émetteur) +
  latence SRT négociée + temps de l'arrivée à l'affichage, que chaque récepteur mesure sur son
  horloge et renvoie chaque seconde par la voie de retour (message 0x03). On affiche le pire
  récepteur.
- Format : H.264 et HEVC. AV1 n'est proposé que si l'encodeur de ce PC et les décodeurs des
  récepteurs le gèrent ; il demande d'abord un transport AV1 dans le TS et l'échange des
  capacités de décodage par la voie de retour. Pas dans ce chantier : bouton grisé (fait
  ensuite, § 16).
- Responsive : aucune barre de défilement horizontale ; sous 960 px la colonne latérale passe
  sous le contenu, sous 760 px la navigation ne garde que ses icônes. Fenêtre de 720×560 au
  minimum ; au premier lancement, 1100×760 au plus, réduite pour tenir dans l'écran.

### Envoi des scènes et sources OBS

- Le plugin, sur le PC qui envoie, sert l'appli par un tube nommé local (`\\.\pipe\flash-obs`,
  accès réservé à l'utilisateur) : liste des scènes et sources (mise à jour quand elle change),
  aperçus, images et son à la demande. Personne connecté : le plugin ne fait rien.
- Image : pour une scène ou une source choisie, le plugin crée une vue OBS (`obs_view`, qui
  active ses sources comme dans un projecteur) et la rend, à la taille d'envoi, directement
  dans une texture D3D11 partagée à verrou à clé (trois en anneau) : aucune copie. OBS rend,
  libère la clé 1 ; l'appli prend la clé 1, convertit en NV12 sur le GPU (comme pour une
  capture d'écran), rend la clé 0. Si l'appli n'a pas fini, OBS saute l'image plutôt que
  d'attendre. Le programme d'OBS est copié depuis la texture principale, sans second rendu.
- Heure de chaque image : celle du rendu d'OBS (`obs_get_video_frame_time`, QPC) : même
  horloge que tout FLASH, la synchro avec le son est gardée.
- Son : les sources audio d'OBS (micro, média, son du bureau) et le mix d'OBS (piste 1)
  apparaissent dans la liste Son. Le plugin les reçoit par `obs_source_add_audio_capture_callback`
  (ou le mix), les ramène en 48 kHz stéréo et les passe par le tube avec leur heure.
- L'appli garde encodage, transport, appairage et synchro : un seul émetteur, une seule
  identité, un seul port. Écarté : faire envoyer le plugin lui-même (deux émetteurs à gérer,
  interface coupée en deux) ; recevoir les images d'OBS en mémoire (copies GPU -> CPU -> GPU).
- OBS fermé : les tuiles OBS le disent ; une source OBS choisie reprend seule quand OBS revient.
- À mesurer : coût GPU du rendu d'une scène qui n'est pas au programme, latence ajoutée entre
  le rendu d'OBS et l'encodage dans l'appli.

### Fait (version 0.2.0)

- Interface refaite d'après la maquette : quatre pages, tuiles avec aperçu et interrupteur,
  son par application avec icône, vumètre, volume et interrupteur, colonne avec le code et les
  réglages rapides en curseurs. Tout s'applique tout de suite.
- Émetteur : pistes ajoutées et retirées en marche (le serveur SRT écoute dès le démarrage,
  même sans source) ; débit, format et encodeur refont seulement les encodeurs vidéo.
- Volume envoyé par source, gardé dans les réglages ; vumètres lus dans Windows.
- Aperçus : 4 par seconde pour les sources envoyées (dernière image), une capture ponctuelle
  toutes les 400 ms à tour de rôle pour les autres écrans et fenêtres, une scène ou source
  d'OBS tous les 250 ms rendue par le plugin. Rien quand la fenêtre est cachée ou ailleurs.
- Latence de l'en-tête : chaque récepteur (plugin, visionneuse) renvoie chaque seconde sa
  latence SRT et son temps de l'arrivée à l'affichage ; l'émetteur y ajoute son temps de la
  capture à l'envoi.
- Envoi depuis OBS : tuiles « Programme OBS », scènes et sources ; son des sources et mix
  d'OBS. En ligne de commande : `--obs-scene`, `--obs-source`, `--obs-programme`,
  `--obs-son`. Les textures partagées sont au format typé `GS_BGRA_UNORM` : le format
  `GS_BGRA` d'OBS est créé « typeless », que le processeur vidéo refuse en entrée.
- Plugin : quand l'émetteur change le format d'une piste en marche, le décodeur est refait.

Vérifié :

- Responsive, en chargeant la page dans un navigateur à toutes les largeurs de 720 à 1920 px
  (pas de 20), sur deux hauteurs et les quatre pages : aucun débordement horizontal (488 cas).
  Captures nettes par Chrome sans fenêtre, avec facteur d'échelle 1, 1,25 et 1,5, et sur un
  écran vertical 1080×1920.
- Vraie appli (WebView2) : aperçus réels des écrans et des scènes OBS, icônes des applis,
  vumètres ; allumer une scène OBS et un écran crée deux pistes, en éteindre un ne retire que
  la sienne ; qualité et volume appliqués et enregistrés.
- Non-régression en boucle locale (mire, son vers « Steam Streaming Speakers ») : 107 ms,
  synchro +0,04 ms, aucune image perdue, comme avant.

Mesures de l'envoi depuis OBS (OBS de test, scène « Banc » hors programme avec la capture de
la fenêtre d'essai et son son, l'appli l'envoie et se regarde, 40 s) :

| Essai | Latence rendu -> écran | Images | GPU 3D | `render_main_texture` d'OBS (médiane) |
|---|---|---|---|---|
| OBS et fenêtre d'essai seuls | | | 13 % | 0,023 ms |
| Scène OBS envoyée par l'appli | 158 ms | 60 i/s, aucune perdue | 21 % | 0,183 ms |
| Même fenêtre capturée par l'appli | 141 ms | 53 à 56 i/s | 22 % | 0,027 ms |

- Le lien plugin -> appli n'ajoute presque rien : de l'heure de rendu d'OBS à la réception,
  23,5 ms, comme pour la mire générée par l'appli (encodage et 20 ms de latence SRT).
- Les 17 ms de plus viennent d'OBS lui-même : sa capture de fenêtre livre l'image 47 ms après
  son rendu par la fenêtre d'essai, contre 28 ms pour la capture de l'appli.
- Rendre une scène qui n'est pas au programme coûte environ 0,16 ms par image au fil graphique
  d'OBS ; au GPU, autant qu'une capture directe.
- Synchro rapportée aux horodatages : -11,8 ms (écart-type 0,9), comme la capture directe
  (-12,7 ms). L'écart brut de +42 ms est celui d'OBS entre sa capture de fenêtre et sa capture
  du son de l'appli.
- Les 107 à 158 ms de bout en bout comptent les 59 ms de la sortie son virtuelle de Steam :
  sans le son, une image arrive en 41 ms.

Limites :

- L'aperçu d'une scène qui n'est pas au programme ni envoyée peut rester noir : OBS ne fait
  tourner une capture de fenêtre ou de jeu que si sa scène est active. Une fois envoyée, la
  tuile montre sa vraie image.
- AV1 : pas dans ce chantier (fait ensuite, § 16).

## 16. AV1

### Ce qui est fait

- Encodage : NVENC en AV1 (puces depuis les RTX 40), mêmes réglages que pour H.264 et HEVC
  (P2, ultra-low-latency, aucune B-frame, CBR sur une image, intra-refresh d'une seconde si la
  puce le gère). Sortie en OBU avec leur taille (pas d'Annex B), en-tête de séquence répété à
  chaque image clé : un récepteur prend le flux en route. Repli Media Foundation : l'encodeur AV1
  matériel que le pilote fournit à Windows (NVIDIA, AMD, Intel) ; Windows n'a pas d'encodeur
  AV1 logiciel.
- Transport : l'AV1 n'a pas de stream_type dans la norme MPEG-TS. FLASH le met en PES privé
  (stream_type 0x06, stream_id 0xBD) avec un descripteur d'enregistrement « AV01 », comme
  l'Opus avec « Opus ». Une unité temporelle par PES, telle que l'encodeur la sort, close par
  le même marqueur de fin d'image que les autres vidéos.
- Tables sur plusieurs paquets : avec beaucoup de pistes (le descripteur de l'AV1 ajoute
  6 octets par vidéo), la table des pistes peut dépasser un paquet de 188 octets. Elle est
  maintenant découpée et recollée à la réception ; avant, elle était tronquée et refusée.
- Échange des formats : chaque récepteur annonce dès sa connexion, par la voie de retour
  (message 0x04 suivi d'un octet), les formats qu'il décode : 1 = H.264, 2 = HEVC, 4 = AV1.
  - Plugin OBS : H.264 et HEVC toujours ; AV1 si le FFmpeg d'OBS a son décodeur `av1` et que
    la carte graphique décode l'AV1 (profil DXVA, sortie NV12). Le seul décodeur AV1 logiciel
    de ce FFmpeg (libaom) ne tient pas 60 images par seconde.
  - Visionneuse : les formats dont Windows a le décodeur ; l'AV1 en plus seulement si la carte
    le décode.
  - Un récepteur d'avant cet échange n'annonce rien : au bout de 0,5 s, il compte pour
    H.264 et HEVC.
- Choix du format par l'émetteur : le format choisi part s'il est dans les formats de son
  encodeur et dans ceux de tous les récepteurs connectés ; sinon H.264. Quand un récepteur
  arrive ou part, l'émetteur refait ses encodeurs vidéo si le format possible change (vérifié
  toutes les 50 ms). La nouvelle table des pistes part avant la première image du nouveau
  format : un récepteur ne passe jamais une image d'un format à un décodeur de l'autre. Le
  format choisi reprend tout seul dès qu'il redevient possible.
- Décodage : plugin par le décodeur `av1` de FFmpeg en d3d11va ; visionneuse par Media
  Foundation (extension AV1 de Windows, sur la carte graphique).
- Interface : le bouton AV1 (et HEVC, par la même règle) n'est actif que si le format est
  possible ; une bulle dit pourquoi sinon (« l'encodeur de ce PC ne le produit pas » ou « un PC
  qui reçoit ne sait pas le décoder »). Si le format choisi ne l'est plus, il reste coché et la
  colonne indique « envoi en H.264 ». Ligne de commande : `--av1`.
- Message clair côté réception : l'émetteur met le format choisi dans ses métadonnées. Une
  visionneuse qui ne décode pas l'AV1 l'affiche dans « Regarder » : extension AV1 de Windows
  à installer (Microsoft Store), ou carte qui ne décode pas l'AV1, et l'image arrive en
  H.264 en attendant. Le plugin l'écrit dans le journal d'OBS.
- Réglages : `reglages.json` garde le format dans `format` (« h264 », « hevc », « av1 ») ;
  l'ancienne clé `hevc` est encore lue.
- VLC ou FFmpeg branchés sur l'émetteur pour déboguer n'annoncent rien : au bout de 0,5 s ils
  comptent comme d'anciens récepteurs et tout l'envoi passe en H.264, qu'ils savent lire.
  L'AV1 de FLASH ne sort donc que vers des récepteurs FLASH.
- Visionneuse : un décodeur qui refuse une image, ou qui ne sort plus rien pendant 1 s alors
  que les images arrivent, est refait et l'image repart de l'image clé suivante. Le décodeur
  AV1 de Windows ne se remet pas seul d'une image de référence perdue (vu avec l'encodeur AV1
  de Media Foundation : plus d'image du tout, malgré les images clés suivantes).
- Ligne de commande : le relevé de l'envoi donne le format en cours et le nombre d'images clés.

### Mesures

Boucle locale, mire 1080p60, NVENC à 25 Mb/s, latence SRT 20 ms, RTX 4070 SUPER, le
2026-10-17. Le FLASH installé tournait à côté : essais sur le port 9730, en ligne de commande.

Plugin OBS (OBS de test, mire et bip, 60 s chacun) :

| | H.264 | AV1 |
|---|---|---|
| Latence capture -> image composée par OBS | 47,4 ms | 50,6 ms |
| dont décalage d'horloge | 38,0 ms | 42,5 ms |
| Encodage | 2,1 ms | 2,5 ms |
| Capture -> réception (encodage + SRT) | 23,5 ms | 23,9 ms |
| Réception -> image décodée et copiée | 1,8 ms | 1,3 ms |
| Écart son/image | -0,56 ms (écart-type 0,18) | -0,61 ms (écart-type 0,34) |
| Images | 3 575 reçues, aucune perdue en route | 3 564 reçues, aucune perdue en route |
| Charge | envoi 0,08 % CPU, OBS 0,98 % ; GPU 3D 12 %, encodeur 7 %, décodeur 6 % | envoi 0,19 % CPU, OBS 1,02 % ; GPU 3D 13 %, encodeur 7 %, décodeur 3 % |
| Débit de la mire | 0,3 Mb/s | 0,1 Mb/s |

Visionneuse de l'appli (mire sans son, fenêtre sur un écran à 60 Hz, 40 s chacun) :

| | H.264 | AV1 |
|---|---|---|
| Latence capture -> écran | 36,5 ms | 36,5 ms, puis 44,0 ms après une image sautée |
| Encodage | 2,12 ms | 2,10 ms |
| Capture -> réception | 23,5 ms | 23,6 ms |
| Réception -> image décodée | 0,29 ms | 0,24 ms |
| Décodeur | Microsoft H264 Video Decoder (carte graphique) | AV1VideoExtension (carte graphique) |

- AV1 par Media Foundation (« NVIDIA AV1 Encoder MFT ») : encodage 2,3 ms, latence 36,6 ms,
  une image clé toutes les 2 s. Au premier essai, une image perdue a figé la visionneuse
  (décodeur AV1 de Windows bloqué malgré les images clés suivantes) : d'où le décodeur refait
  plus haut. Pas reproduit depuis, le correctif n'a donc pas encore été vu à l'œuvre.
- Repli vérifié : envoi en AV1 vers la visionneuse, puis `ffprobe` (qui n'annonce rien) se
  connecte : passage en H.264 0,5 s plus tard, retour à l'AV1 dès son départ, la visionneuse
  suit les deux changements (quelques images sautées à chaque fois).

Enseignements :

- L'AV1 ne coûte rien en latence dans la chaîne de FLASH : encodage du même ordre, décodage
  plus court sur la carte, aucune image perdue. Les 3 ms d'écart du plugin sont dans le
  décalage d'horloge, que fixe le son et qui varie d'une session à l'autre (37,7 à 52,7 ms en
  H.264 seul, § 11). L'écart de la visionneuse vient du calage sur le balayage de l'écran, qui
  a basculé d'un demi-intervalle après une image sautée (même effet qu'au § 11).
- Le décodeur de la carte travaille deux fois moins (3 % contre 6 %).
- La mire, très simple, se compresse trois fois mieux en AV1. Sur une vraie image, l'encodeur
  tient le débit choisi (CBR) : le gain se lit en qualité à débit égal, ou en débit à qualité
  égale (curseur Qualité plus bas). Pas mesuré ici.

## 17. Endurance

### Protocole

Boucle locale sur le PC de jeu, le 2026-10-17 de 04:50 à 08:45 (3,9 h), version 0a7b637 (H.264,
latence SRT 20 ms, 25 Mb/s par vidéo). L'appli tourne en ligne de commande (`--libre`, port
9720) et envoie six pistes :

- la mire (1080p60, heure de capture et numéro inscrits dans l'image, flash et bip à chaque
  seconde), seule piste mesurée image par image ;
- une scène d'un premier OBS portable (OBS-A), hors programme : une source navigateur animée
  avec un son continu faible ;
- l'écran 4, où tourne la même animation dans un navigateur en plein écran ;
- trois sons : le bip de la mire, le son de la scène d'OBS-A, le son du système.

Un second OBS portable (OBS-B) reçoit les six pistes et écrit le journal de mesure. Relevé
toutes les 30 s, par processus : processeur, mémoire privée et de travail, handles, fils,
objets GDI et USER, mémoire et moteurs du GPU (compteurs PDH), plus la machine entière
(nvidia-smi). Le long segment sans coupure vient d'abord (3,6 h) ; les coupures occupent le
dernier quart d'heure : OBS-B fermé puis relancé deux fois, l'appli tuée puis relancée trois
fois (dont une fois 30 s plus tard), OBS-A fermé puis relancé.

### Résultats

| Critère (§ 1) | Mesuré | Objectif |
|---|---|---|
| Latence capture -> image composée par OBS | 51,7 ms (médiane), 51,9 ms (99 %), jamais plus de 60 ms sur 781 579 images | médiane ≤ 40 ms : non tenu (voir plus bas) |
| Images | aucune perdue ; 220 reçues deux fois (répétées par l'émetteur) | |
| Écart son/image | -0,59 ms en moyenne, écart-type 0,20, pire 16,3 ms (une seule fois) | tenu |
| Dérive son/image | +0,004 ms par heure, soit +0,01 ms sur 4 h | tenu |
| Émetteur (3 vidéos 1080p60 + 3 sons) | CPU 0,64 %, encodeur 25 %, 3D 6,7 %, mémoire privée 275 Mo stable | voir plus bas |
| Récepteur OBS-B (3 flux, OBS entier) | CPU 3,2 %, soit 1,1 % par flux ; décodeur du GPU 18 % | tenu |
| Reconnexion (appli tuée puis relancée) | image revenue 1,19 s, 1,46 s, 1,94 s après la relance | tenu |
| OBS-B relancé | première image 2,3 à 2,6 s après le lancement d'OBS | |
| OBS-A relancé (scène envoyée) | scène reprise 2,1 s après | |
| Tenue | aucun plantage ; mémoire, handles, fils, GDI et USER plats sur 3,6 h | tenu sur 3,9 h (8 h pas encore faites) |

- La scène d'OBS-A est passée à 60 i/s sans faiblir, l'écran 4 à 57 i/s en moyenne (capture de
  Windows sur un écran à 60 Hz quand l'écran principal tourne à 165 Hz, § 12).
- Le compteur de mémoire GPU « dédiée » d'OBS-A (source navigateur) monte par paliers d'environ
  12 Go jusqu'à des valeurs impossibles, alors que la mémoire de la carte reste à 3,5 Go : un
  défaut du compteur, pas une fuite.

Latence, dans le découpage du journal : 23,5 ms de la capture à la réception (encodage 2,3 ms,
latence SRT 20 ms, mise en paquets et envoi 1,2 ms), 1,9 ms de décodage, 18,2 ms d'attente
avant la remise à OBS, 8,0 ms de la remise à l'image composée, soit 51,6 ms. Ce qui la fixe :
l'heure de présentation tombe 38 ms après la capture, et c'est le son qui l'impose (une trame
Opus de 10 ms part 10 ms après son horodatage, puis encodage, SRT et 2 ms de marge) ; l'image
partage ce décalage pour rester synchrone. L'image passe ensuite au premier rendu d'OBS qui
suit cette heure, 0 à 16,7 ms plus tard selon la phase entre la source et OBS : 13,6 ms ici
(38 + 13,6 = 51,6 ms), environ 7 ms après la relance d'OBS-B (45 ms de latence), 8 ms en
moyenne. La remise se fait au milieu de l'intervalle qui précède ce rendu, d'où les 8,0 ms de
la dernière étape. Le budget du § 1 oubliait la trame de son. Pistes de gain : latence SRT de
10 ms en filaire (-10 ms) et trames Opus de 5 ms (-5 ms), soit une trentaine de millisecondes
en moyenne.

Mémoire de l'émetteur : sur 241 Mo privés engagés, 141 Mo sont de la mémoire « write-combined »
du pilote NVIDIA (tas d'envoi vers le GPU, sessions NVENC : trois ici), 55 Mo trois gros blocs
encore à identifier (tampons de SRT laissés aux valeurs par défaut ?), 46 Mo de petits tas.
Aucune interface chargée. L'objectif de 80 Mo ne tient pas compte de la part du pilote.

### Trouvé pendant l'essai

- OBS-A ne s'est pas fermé seul (arrêté au bout de 30 s) alors que l'appli envoyait sa scène :
  le plugin gardait la scène (et la source navigateur qu'elle contient) jusqu'au déchargement des
  modules, après qu'OBS a vidé ses scènes. Fermé une fois l'appli partie, OBS-A s'arrêtait
  normalement. Corrigé : le plugin rend la vue et la source au signal `source_remove` de libobs
  (fermeture, changement de collection, suppression) et prévient l'appli, qui redemande la scène
  quand la liste change. Vérifié sur un OBS de test qui envoie une scène à source navigateur et
  son son : fermé en 0,4 s (bloqué avec le plugin d'avant), aucune source orpheline, scène
  reprise 2,0 s après la relance d'OBS et 0,8 s après un aller-retour de collection.

### Correction : redescente rapide après un changement de pistes

Trouvé en vérifiant l'AV1 (§ 16) : quand l'émetteur change de format, il refait ses encodeurs
vidéo et ses pistes s'arrêtent quelques dizaines de millisecondes. Le récepteur relevait
aussitôt son décalage d'horloge (+43 ms dans l'essai) puis ne le redescendait qu'à 0,1 ms par
seconde, la vitesse prévue pour suivre la dérive entre deux PC : la latence passait de 42 à
92 ms et y restait plusieurs minutes. Désormais, quand la table des pistes change (format,
piste ajoutée ou retirée), plugin et visionneuse autorisent pendant 30 s la descente rapide du
démarrage (2 ms par seconde).

Vérifié (mire en AV1 vers l'OBS de test, 80 s, ffprobe connecté à 20 s) : passage en H.264 puis
retour à l'AV1, décalage monté à 43 ms puis revenu à 36,5 ms dans les 5 s suivantes, latence
restée à 53,5 ms (quelques images à 70 ms au moment du changement), contre 92 ms pendant
plusieurs minutes avant.

Mémoire de l'émetteur avec une seule vidéo (mire) et un son : 202 Mo privés, contre 275 Mo avec
trois vidéos : environ 36 Mo par session NVENC, le reste est commun.
