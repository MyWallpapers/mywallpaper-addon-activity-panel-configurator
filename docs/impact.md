# Revue d’impact et de code mort A24

Périmètre : sources, tests, manifeste, contrats vendus et documentation A24. Aucun fichier partagé,
add-on voisin, schéma, moteur ou Ledger n’est modifié. Codebase Memory a été
consulté pour le SDK/protocole courant et pour découvrir les propriétaires ;
le dépôt A24 neuf et A18 n’étaient pas indexés. Les références locales exactes
ont ensuite été suivies avec les fichiers déclarés et rg.

- `wmain` est le point d’entrée du compagnon déclaré par le manifeste. Les
  variantes `--self-test` et `--fixture-protocol` ne font apparaître aucune
  fenêtre et ne collectent pas l’état du bureau ; les cibles CTest les appellent.
- Le chemin réglage → `ParseConfig` → `Apply` → `Paint`/`Update` utilise les
  mêmes définitions embarquées que le manifeste. Les conditions de visibilité
  ne suppriment pas les valeurs. Les tests parcourent chaque définition, option,
  borne et type. La clé inconnue `profile` ne gouverne aucune valeur.
- `ReadThread` et `PipeBuffer` sont joints à l’arrêt ; la lecture interrompable
  couvre EOF, en-tête/corps incomplet et une fenêtre privée fermée pendant une
  lecture. Un seul message attend le thread UI, sans file non bornée.
- `Proc` est enregistré par `RegisterClassExW`. `Message` traite les trames,
  timers, clavier, souris, perte de capture, DPI, affichages, préférences et
  fermeture. Ce sont des registrations dynamiques : leur absence dans un
  graphe d’appels statique ne prouve pas qu’elles sont inutilisées.
- `AccessibleObject` est appelé par WM_GETOBJECT. Ses méthodes IAccessible sont
  des entrées COM externes, avec références comptées et état survivant protégé.
  Les actions sont adressées à la fenêtre possédée avec le jeton de son état,
  et aucune action ne peut atteindre une nouvelle génération de fenêtre.
- Les quatre tests Windows sont enregistrés par CMake et intégrés à
  `mw_native_development`; `activity_contracts` et `activity_audio_service` sont aussi portables. Le build
  Windows compile toujours le runtime, même si les contrats seuls passent.
- `tools/build-candidate.mjs` reste l’entrypoint DEV de `mywallpaper.dev.json`.
  `native/build.ps1 -Development` conserve ce chemin. La configuration par défaut
  appelle le build Release autonome sans import du checkout MyWallpaper voisin. Le réemploi d’un candidat requiert l’identité exacte du checkout,
  HEAD, sources, Ledger, outils partagés et fichiers produits.
- Seule la route exploratoire MinGW, abandonnée après rétablissement MSVC,
  a été retirée : son fichier CMake n’était référencé par aucun entrypoint de
  manifeste, preset, release ou test. Ses reçus d’échec restent des preuves
  locales ; aucun binaire issu de cette route n’est distribué.
- Le champ enum `Snapshot::media` n’avait qu’un lecteur, `Content`, et aucun
  export, registration dynamique ou contrat externe. Il est remplacé par
  `mediaDetail`, alimenté par `Panel::SyncService`; les états batterie et les
  valeurs utilisateur sont conservés. La chaîne média toujours indisponible
  sont remplacés, sans supprimer le fallback absent. Les clés diagnostiques
  `media.state` et `media.reason` sont conservées ; le statut est enrichi avec
  les topics, la connexion et le résultat métier.
- `AudioService` est possédé par Panel ; Frame, Apply, le timer local 3 et les
  actions l’appellent. Close/shutdown/désactivation libèrent requêtes,
  abonnements et inventaires. Ses callbacks sont synchrones sur le thread UI.
  Les méthodes snapshot/command/inventory, topics audio/media, enveloppes SDK
  et alias `audio` sont des contrats externes conservés sans renommage.
- `ServiceControl` lie un bouton à sa cible et ses capacités exactes. Les
  registrations COM IAccessible restent externes ; leurs messages portent en
  plus une révision d’actions. Aucun appel COM n’est supprimé au motif qu’un
  graphe statique ne le trouve pas. Les tests couvrent les actions périmées.
- CMake embarque le schéma figé et enregistre le nouveau test portable/Windows.
  `mywallpaper.dev.json` surveille aussi `contracts` pour reconstruire après
  une modification du contrat figé. Les exemples synthétiques servent uniquement
  aux fixtures explicites. Aucun
  point d’entrée de production ne déclenche leur scénario. Les petites chaînes
  adjacentes du schéma contournent la limite de littéral MSVC sans changer ses
  octets ou son contrat.
- La copie du contrat A18 est verrouillée et accompagnée de sa licence. Aucun
  provider, runner, backend, schéma partagé ou moteur n’est recopié. Le seul
  fichier de SDK compilé ayant changé dans le verrou existant est
  `dist/addon-schema/index.js` : ancien SHA256
  `b7f414f9b5d4e64d4a929da66f490fa734c5468feb2f9602d4f5292716a07098`,
  nouveau `36fb3187d61e5b16e759f6556cf9f5bb66afcce63e7dc6b7509ab712d2e53b1e`.
  La revue du validateur courant confirme les requires natifs et la sélection
  publique de release ; le manifeste A24 passe ce SDK exact avec 49 définitions.
  Le verrou échoue explicitement si ses fichiers changent à nouveau.

Sources publiques examinées : documentation MyWallpaper process-v2 v5, SDK
public exact verrouillé et API Windows officielles
[UpdateLayeredWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-updatelayeredwindow),
[opérations des pipes anonymes](https://learn.microsoft.com/en-us/windows/win32/ipc/anonymous-pipe-operations).
Aucune implémentation de rendu ou de fournisseur tiers n’a été copiée. Le
schéma, README et exemples A18 sont des artefacts de contrat MIT attribués.
