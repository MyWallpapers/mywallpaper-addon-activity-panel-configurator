# a18/audio-system, version 1

Contrat métier adopté par A18 sous DEC-0012/0013. `schema.json` est le schéma
JSON Schema 2020-12 public ; ce texte précise ses invariants de protocole.
Identité exacte : `{ "contract": "a18/audio-system", "version": 1 }`.
Nom court : audio-system v1. Alias fournisseur prévu : `audio` ; alias du
consommateur libre. Une version de fournisseur n'est pas une version de contrat.
Le namespace A18 ne désigne aucune API audio officielle du core.
La version 1 est identifiée par ces artefacts exacts et leurs SHA-256 dans le
reçu de phase. Toute modification incompatible des formes ou invariants exige
une nouvelle version de contrat, distincte de la release du fournisseur.

Ce lot livre définition et conformité synthétique hors ligne. Le manifeste ne
déclare pas de fournisseur et aucun adaptateur broker n'est livré. Aucun wrapper
ESM, canal privé A24 ou pouvoir Windows supplémentaire. Le propriétaire commun
vérifie séparément la disponibilité de l'entrée fournisseur native directe.

## Identités et connexion

Le consommateur choisit explicitement un fournisseur compatible ; il conserve
l'identité exacte de connexion fournie par le broker (principal, fournisseur,
release si présente, génération). Ce contrat n'autorise aucune substitution ni
résolution par nom d'exécutable. Les identifiants endpoint/session restent opaques,
locaux à ce fournisseur et à cette génération, jamais transportables vers Music
Player ou une autre instance. Un PID, HWND ou label ne remplace pas un ID.

`generation` est un token opaque lié par l'adaptateur à l'identité et à la
génération du fournisseur établies par l'hôte. Il n'est ni un secret ni une preuve
d'autorisation. Les champs envoyés par le consommateur ne créent aucune identité.
Tout remplacement, redémarrage, retrait de consentement/lease ou fermeture de
connexion invalide demandes, IDs, inventaires et callbacks de cette génération.
Une nouvelle connexion doit relire snapshot et capacités ; aucun état ancien
n'autorise une commande. Les refus du broker restent observables même lorsqu'un
résultat métier ne peut plus être livré.

## Méthodes et topics

Les noms et `$defs` d'entrée/sortie figurent dans `schema.json/x-service`.

| Membre | Contrat |
| --- | --- |
| `snapshot` | Requête `snapshotRequest` ; `snapshotResult` contient les derniers états audio et média d'une même génération, sans nouvelle interrogation Windows. |
| `command` | Requête `commandRequest` ; un `commandResult` avec le même requestId et la génération d'exécution. |
| `inventory` | Lecture seulement, liée à une référence paginée ; `inventoryResult` ou `readError`. |
| topic `audio` | Valeur `audioState` retenue, snapshot initial puis états de remplacement. |
| topic `media` | Valeur `mediaState` retenue, snapshot initial puis états de remplacement. |

Le fournisseur doit publier les deux états initiaux, y compris `absent`, avant
de se déclarer prêt. Le premier événement retenu du broker est `snapshot`, les
suivants `state` ; son `sequence` reste distinct de notre `revision`. Chaque
topic incrémente strictement sa révision entière à chaque publication dans une
génération (0 pour l'état absent initial). Une révision identique n'est acceptable
que pour la même valeur initiale relue ; une régression ou un trou exige une
resynchronisation en lecture avant commande. Pas de mélange entre générations.
Le remplacement est celui d'un état complet, sans patch implicite ni événement
éphémère rejoué. Une erreur de subscription/backpressure impose de la fermer et
de relire ; aucune perte silencieuse n'est présentée comme un état complet.

`snapshot` est atomique pour les deux derniers états retenus du fournisseur,
pas une mesure simultanée des APIs Windows. Les résultats et états ne sont pas
ordonnés entre leurs canaux : un ack ne remplace pas l'observation suivante.

## Disponibilité, données et capacités

| availability | Sens |
| --- | --- |
| `absent` | Pas de snapshot produit pour cette fonction, notamment désactivée ; value null, aucune capacité présumée. |
| `empty` | Interrogation réussie : audio sans endpoint/session, ou média sans session sélectionnée ; inventaire explicite, éventuellement non vide pour les autres lecteurs. |
| `ready` | Audio avec endpoint/session, ou média sélectionné avec ses métadonnées et capacités observées. |
| `unavailable` | Lecture impossible ou dépassement observable ; value null et erreur explicite. Aucun ancien état présenté comme actuel. |

Un fournisseur absent/non résolu est d'abord un état de connexion du broker,
pas une liste vide. Audio `complete=false` avec erreurs décrit une énumération
partielle ; absence dans cette liste ne prouve pas disparition. Si aucun élément
n'a pu être observé et que l'énumération est partielle, utiliser unavailable,
jamais empty. Les sessions
exportées doivent référencer un endpoint présent : une entrée native orpheline
rend la projection indisponible, sans la supprimer silencieusement.

Volumes : nombres finis en pourcentage 0–100. Durées/positions : millisecondes
entières signées sûres JSON, en conservant la plage de seek réellement observée.
PID : uint32, HWND : chaîne décimale issue de la valeur native pour éviter toute
perte d'entier. Métadonnées/noms : Unicode intégral, aucune troncature. IDs :
1–4096 octets UTF-8 (`x-maxUtf8Bytes`), à vérifier en plus du schéma standard.
Les tableaux n'autorisent aucun ID dupliqué dans leur propre collection.

Chaque endpoint/session audio porte `capabilities.volume` et `.mute`, dérivées
des contrôles présents ET des permissions/configurations actuelles. Endpoint
capture : faux tant que allowMicrophoneChanges=false ; session capture : toujours
faux ; tout faux si A18 est désactivé ou si le contrôle est indisponible. Ne jamais
transformer une capacité déclarée en permission. Revalider à l'exécution.

Chaque session média porte `capabilities`: objet si les contrôles ont été
observés pour CETTE session, null sinon. Le code actuel ne connaît les capacités
que du lecteur sélectionné ; les autres restent null, jamais copiées du lecteur
courant. `selected.id` doit exister dans sessions et ses capacités doivent être
identiques à celles de cette entrée. L'ordre seekStartMs <= seekEndMs est requis ;
position et duration ne sont pas une capacité implicite. Les IDs égaux entre
inventaires audio et média n'établissent aucune correspondance.

## Commandes, corrélation et issue incertaine

La méthode `command` contient uniquement les actions déjà implémentées :

- audio : refresh, show, endpointVolume/sessionVolume (id, volume),
  endpointMute/sessionMute (id, muted) ;
- média : refresh, session (value=ID), playPause, previous, next,
  seek (value=ms absolues dans la plage), shuffle (bool), repeat (none/track/list).

Pas de changement de routage, de périphérique Windows par défaut, son arbitraire,
règle, hotkey, configuration ou action système ajouté par ce contrat. Les effets
conditionnels des commandes existantes (par exemple cue configuré sur mute)
restent soumis au consentement et aux réglages existants.

`requestId` est obligatoire, unique pendant la connexion/génération ; token
ASCII de 1–128 caractères. L'adaptateur injectera un ID natif propre à cette
connexion dans le payload companion et vérifiera le résultat corrélé. Deux
consommateurs peuvent choisir le même requestId sans partager une opération.
Un ID déjà employé est refusé avant dispatch (`invalid_request`), jamais exécuté
de nouveau ; pas de cache d'idempotence qui rejouerait l'opération.

`generation` et `revision` doivent correspondre au dernier topic concerné.
Avant dispatch : valider schéma, octets, permissions, génération/révision,
ID encore présent, capacité actuelle et plage. Rejeter tout état périmé.
`expectedSessionId` vaut null pour audio et media.refresh/session ; il vaut
l'ID sélectionné exact pour chaque contrôle de lecture. L'adaptateur doit
sérialiser la sélection, vérifier l'ID immédiatement avant l'appel et interdire
le fallback de `Media::Current` vers un autre lecteur pour ces commandes.
Sans cette garantie native, la commande doit être refusée : aucun simple relais
du payload actuel ne satisfait ce contrat. Aucun raccordement n'est livré ici.

`timeoutMs` : entier 1–30000, plafond déjà existant du broker. Le délai effectif
est le minimum de cette valeur et du budget restant de l'appel/AbortSignal.
L'attente native média conserve son plafond actuel de 1500 ms par opération
asynchrone ; les étapes séquentielles consomment le même budget total. Le futur
adaptateur doit porter ce budget et empêcher tout dispatch tardif. L'annulation
n'est pas une promesse d'annulation de l'effet Windows déjà commencé.

| Résultat | Sens |
| --- | --- |
| ok=true, acknowledged | Le companion a répondu ok pour cette demande. Pas de preuve que l'état/lecteur externe a déjà changé. |
| ok=false, rejected | Refus certain avant toute action native, par exemple capacité absente ou révision périmée. |
| ok=false, unknown | Action dispatchée, puis refus natif non typé, exception, timeout, annulation ou perte de génération : effet possible, même après Cancel(). |

Les anciens `audio.result`/`media.result` ne donnent pas de code structuré :
ok=false après dispatch se classe `native_failure`/`unknown`, jamais refus certain
à partir d'un message localisé. `error.code` est stable ; message est diagnostic,
au plus 512 caractères, ne sert pas au contrôle. Les codes sont ceux de `$defs/error`.
Une erreur de transport ou fermeture après dispatch signifie aussi issue inconnue,
même sans résultat métier livrable. Les réponses tardives sont écartées.

Aucune commande n'est répétée automatiquement, y compris refresh, show,
playPause ou une valeur absolue. Reconnexion, timeout et resynchronisation ne
rejouent rien. Après issue inconnue, relire l'état puis attendre un nouveau choix
explicite du consommateur et un nouveau requestId. Le signal d'annulation est
celui du broker ; pas de méthode cancel métier ni de nouveau pouvoir core.

## Inventaire au-delà de 64 KiB

Audio::Snapshot exporte un nombre non borné d'endpoints/sessions/erreurs et
Media::Snapshot des listes/métadonnées de longueur non bornée. Un inventaire
synthétique de 100 sessions avec labels Unicode suffit à dépasser 65536 octets.
La continuation est donc nécessaire pour les valeurs existantes, sans nouveau
plafond natif ou quota core. Le test reproduit ce cas sans interroger Windows.

Mesurer le JSON UTF-8 compact réellement émis, enveloppe métier incluse,
avant chaque publication/réponse. Maximum 65536 octets ; les refus et budgets
génériques du broker restent applicables (16 MiB retenus et, séparément, 16 MiB
en attente/en livraison par session, partagés avec les autres services).
Un état qui tient utilise `body.mode=inline`. S'il dépasse, utiliser `paged`.
Pour snapshot, si les deux états inline assemblés dépassent 65536, remplacer
le plus grand par une référence paginée, puis l'autre si nécessaire.

Une référence décrit exactement le JSON compact UTF-8 du seul audioValue ou
mediaValue immuable, longueur totale et SHA-256 inclus. `inventoryId` est opaque,
lié à la génération/topic/révision. Chaque appel inventory vérifie l'identité et
l'autorisation de sa propre connexion broker ; aucun droit ne se transporte
avec le token ni ne vient d'un champ principal du payload. Les topics partagés
peuvent porter la même référence publique, sans créer une copie par connexion ;
son utilisation reste vérifiée contre la connexion et les octets du topic actuel.

offset est un décalage en OCTETS, 0 puis le nextOffset exact reçu. Chaque page
contient au plus 32768 octets bruts encodés base64 standard (au plus 43692 caractères),
sans découpage destructif de chaîne Unicode : concaténer les octets avant décodage
UTF-8/JSON. Pages pleines sauf dernière ; nextOffset=offset+octets, null exactement
à totalBytes. Le résultat répète identité/génération/topic/révision/inventoryId,
totalBytes et digest. Rejeter page hors ordre, conflit de métadonnées, base64
invalide, saut/duplication d'offset ou checksum faux ; ne jamais exposer un état
partiel. Valider la valeur assemblée avec audioValue/mediaValue et invariants.

Une seule page en vol par consommateur. Le lecteur impose 5000 ms de budget
total d'assemblage (ou son budget plus court), au plus 512 pages et 16 MiB.
Il annule/libère les octets à expiration, révocation ou erreur. Le fournisseur
ne conserve que les deux dernières valeurs courantes immuables, somme <=16 MiB,
sans copie par consommateur ni historique de curseurs. Remplacement du topic
invalide immédiatement l'ancien inventaire (`inventory_expired`) : jeter
l'assemblage, relire snapshot, sans rejouer de commande. Les références restent
valides tant que leur topic/révision est courant et la connexion autorisée.
Le budget de 5 s borne le lecteur, pas la durée d'un snapshot retenu inchangé.

Si même une valeur ou les deux valeurs retenues dépassent le budget fournisseur,
publier `unavailable` avec `message_too_large`, sans corps ni troncature. En cas
de dépassement cumulé, la nouvelle valeur est celle refusée ; conserver l'autre
topic avec sa révision. Un refus broker/backpressure doit rester observable ;
fermer le fournisseur si son état d'erreur ne peut être publié. Ne jamais relever
un quota, masquer une session ou tronquer une métadonnée pour faire passer un état.

## Points natifs à raccorder plus tard

- Panel::Frame/Command/Send : routage broker, IDs de requêtes isolés par connexion,
  validation avant dispatch, génération, budget et résultats structurés.
- Panel::Tick et Audio::Snapshot : disponibilité/qualité, capacités dérivées de
  Config et contrôles présents, HWND en chaîne, publication audio et inventaire.
- Media::Snapshot : capacités connues du lecteur sélectionné, null pour les autres,
  état absent si collecte désactivée, empty distinct d'un échec GSMTC.
- Media::Current/Command/Await : cible épinglée sans fallback et budget total
  annulable. Le runtime actuel ne fournit pas ces garanties de service.
- Cleanup, EOF/shutdown, consentement/lease et callbacks broker : invalider
  générations, références, requêtes, assemblages et publication sans répétition.

## Conformité locale

`python3 tests/audio_system_contract_test.py` utilise jsonschema déjà installé
pour valider le schéma et les fixtures synthétiques. Le modèle de test vérifie
aussi corrélation, capacités, IDs périmés, sélection, issues inconnues et
continuation/budgets. Il n'est ni un fournisseur ni un adaptateur runtime.
Exécuter via `task.mjs run native --stage verify` pour un reçu lié aux sources.
Le plan `native --proof unit` reste la recette portable de référence ; cette
validation partielle n'exécute ni ancien CTest ni build de runtime inchangé.
