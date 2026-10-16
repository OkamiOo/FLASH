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
  jaune, curseur au choix, cadence plafonnée par `MinUpdateInterval`. Pas d'injection dans les
  jeux : rien que l'anti-triche puisse reprocher.
- Conversion : `ID3D11VideoProcessor` (BGRA vers NV12 BT.709, mise à l'échelle), sur le GPU,
  quel que soit le fabricant.
- Encodage : NVENC appelé directement (en-têtes MIT), texture D3D11 en entrée sans copie.
  H.264 par défaut, HEVC en option. Réglages : preset P1 à P3, tuning ultra-low-latency,
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
  par le récepteur (les autres ne lui sont pas envoyées), statistiques.

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
  donc NVDEC, AMD ou Intel), copie NV12 puis `obs_source_output_video`. Son : Opus décodé par
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
Windows.

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

Boucle locale (127.0.0.1), mire 1080p60 + bip, NVENC H.264 25 Mb/s, Opus 10 ms, latence SRT
20 ms, OBS 32.2.2 portable. Mesures du plugin (`tools/essai-boucle.ps1`, `tools/analyse_mesure.py`),
essai de 5 minutes :

| Critère | Mesure |
|---|---|
| Images | 17 992 reçues, aucune perdue en route, 2 non affichées au démarrage |
| Capture -> réception (encodage + SRT) | 23,3 ms (99 % sous 25,2 ms) |
| Réception -> image décodée et copiée | 1,8 ms (99 % sous 2,2 ms) |
| Décalage d'horloge (heure de présentation) | 37,1 ms, stable |
| Latence capture -> image composée par OBS | 37,7 à 49,5 ms selon les sessions, constante dans une session |
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
