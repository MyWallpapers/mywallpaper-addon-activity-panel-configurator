# Panneau d’activité — A24

Compagnon Win32 x64 possédé par l’add-on : une île réduite ou un panneau
développé, configurés dans la sidebar habituelle de MyWallpaper. Aucune fenêtre
Windows native n’est remplacée. L’implémentation n’utilise ni injection, ni
symboles privés, ni hook ou raccourci global, ni runtime HTML/JavaScript.

Les 49 définitions du manifeste comprennent les sections. Tous les réglages
visuels s’appliquent directement ; aucun thème ne remplace les valeurs choisies.
Le fond uni ou dégradé, son opacité, le grain, le reflet, le contour, l’ombre et
les couleurs des textes se combinent. L’opacité du fond laisse les textes nets.
Le flou du bureau et les images ne sont pas proposés par cette version.

Le panneau affiche l’horloge locale, la batterie quand Windows la fournit,
l’utilisation mémoire et CPU et un minuteur personnel. Les données système
proviennent des API publiques Windows. Un état absent ou inconnu reste explicite.
En mode de priorité automatique, une batterie faible hors secteur passe avant
un minuteur actif ou terminé, puis vient le contenu de repos choisi.

Le média consomme le fournisseur facultatif A18 `a18/audio-system` v1 choisi
par le graphe de services. Activer « État du fournisseur média » ou choisir le
média comme contenu réduit affiche les métadonnées reçues. Les contrôles de
lecture, de lecteur et de volume apparaissent seulement avec un état courant
et les capacités correspondantes. Une absence, un refus ou un résultat inconnu
reste explicite. Les commandes exigent un clic et ne sont jamais rejouées.
Voir [l’adaptateur et son contrat public](docs/audio-consumer.md). Les notifications et activités globales
d’autres applications et la capture d’écran ne sont pas collectées.

Choisir un écran principal, celui du pointeur au démarrage, ou son numéro ;
une sélection indisponible retombe sur le principal avec un diagnostic. Les
dimensions et marges suivent le DPI de l’écran. Le panneau respecte la zone
de travail, les coordonnées négatives et les changements d’écran. Au plein
écran sur son écran, il peut se masquer, se réduire ou rester visible.

Le bouton +/− et le survol ouvrent le panneau. Alt+Tab donne accès au panneau lorsque cette option est activée.
Tab et les flèches choisissent
un bouton ; Entrée ou Espace l’activent, Échap réduit le panneau. Page précédente/suivante ou la molette font défiler
le contenu lorsque la zone de travail est petite ; les actions restent visibles. Les boutons
sont exposés par MSAA avec leur nom, leur rôle, leur position et leur action.
La réduction des animations, demandée ici ou dans Windows, neutralise le
mouvement en conservant les choix visuels. Le contraste élevé peut prendre
priorité sur la palette. La désactivation cache la fenêtre ; l’arrêt libère
la fenêtre, les timers et la lecture du pipe. Aucun état de shell n’est modifié
ou à restaurer. Le minuteur s’arrête quand son processus est arrêté ; il n’est
pas une alarme persistante et ne joue pas de son.

## Construire et vérifier

Le build Release autonome requiert Windows x64, Visual Studio 2022 avec les
outils C++ et le SDK Windows. Depuis la racine de ce dépôt :

```powershell
powershell.exe -NoLogo -NoProfile -NonInteractive -File native/build.ps1
```

Cette entrée vérifie les octets MIT embarqués, compile en Release, exécute les
tests CTest déclarés et ne copie les sorties dans `native/out/windows-x86_64/`
qu'après leur réussite. Aucun checkout MyWallpaper voisin n'est nécessaire.
Les tests Windows utilisent des fenêtres de fixture et des processus contenus
dans un Job Object. Leurs rendus ne sont pas des captures du bureau.

Les sources et fixtures ne constituent pas une qualification complète dans
MyWallpaper, ni une admission catalogue. Les observations Windows acquises
restent liées à leurs sources et artefacts exacts ; la reproductibilité centrale
et les comportements non observés demeurent à qualifier.

Le chemin DEV optionnel reste disponible via `mywallpaper.dev.json`,
`tools/build-candidate.mjs` et `native/build.ps1 -Development`. Il requiert le
tooling MyWallpaper voisin et le SDK public aux octets exacts du verrou.
Ces prérequis de développement ne sont pas ceux du build Release autonome.
La préparation d'un candidat DEV ne prouve pas son activation dans l'application.

## Sources publiques et build release

`mywallpaper.config.json` appelle `native/build.ps1` pour compiler et vérifier
les composants en Release sous Windows x64/MSVC, sans checkout MyWallpaper voisin.
`powershell.exe -NoProfile -File native/build.ps1` est la même entrée autonome.
Le mode DEV conserve `tools/build-candidate.mjs` et la configuration
`mywallpaper.dev.json`; `native/build.ps1 -Development` reste disponible.
Le SDK public verrouillé sert à la validation DEV du manifeste ; le CMake natif
compile les seuls contrats et dépendances MIT embarqués. Cette séparation ne
qualifie ni le rendu, ni la reproductibilité centrale, ni une release catalogue.
