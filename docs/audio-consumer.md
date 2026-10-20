# Adaptateur A18 du panneau

`AudioService` est une machine d’état portable appelée uniquement par le thread
UI. Sa sortie est l’enveloppe native du SDK ; `Panel::Frame` lui remet les
messages de la surface service. Aucun client réseau ni accès GSMTC/Core Audio
n’est ajouté à A24. Les observations et mutations Windows appartiennent à A18.

La génération du transport host et celle du fournisseur sont distinctes. Une
nouvelle génération host invalide connexions, états, capacités, abonnements,
requêtes et inventaires. Le snapshot corrélé fixe la génération métier ; les
deux états doivent lui correspondre. La connexion opaque provient du broker,
qui a résolu le fournisseur choisi dans le graphe. Le texte des diagnostics
n’est jamais interprété comme une permission ou une réponse de contrôle.

Les événements commencent par snapshot, puis state ; les séquences broker et
les révisions métier sont vérifiées séparément. Une même révision initiale
n’est admise qu’avec un état identique. Trou, régression, abonnement fermé ou
contrat invalide retire les capacités et relit snapshot une seule fois par
cycle de connexion/relecture explicite. La lecture ne relance aucun effet.

Au plus une commande et une lecture d’inventaire sont en cours. Les IDs ne
sont jamais réutilisés pendant la vie du processus. Le délai d’une commande
est de 3 s, celui d’une lecture de 5 s ; les délais utilisent l’horloge monotone.
Une réponse annulée/tardive n’altère plus l’état. `acknowledged` signifie
acceptation A18, et seul un nouvel état change les données affichées. Rejet
métier, erreur de transport et résultat inconnu restent distincts. Une action
nouvelle nécessite un nouveau clic après resynchronisation.

Un bouton transporte la connexion, la génération, la révision, l’ID opaque et
la commande construite depuis les capacités courantes. Le relâchement souris
vérifie que l’action correspond encore à l’appui. Les messages MSAA comportent
une révision des actions afin qu’un ancien numéro de bouton ne commande pas
un nouveau contrôle. Le dispatch compare de nouveau la totalité du contrôle.
Aucun nom, PID ou index de tableau ne remplace un ID périmé.

Les inventaires audio/media passent par une seule continuation à la fois.
Chaque page doit correspondre à l’inventaire, au topic, à la génération, à la
révision, à l’offset, au nombre total d’octets et au digest annoncés. Le base64
est canonique, une page comporte au plus 32768 octets, et les offsets doivent
avancer exactement. Les limites impliquent au plus 512 pages et 16 MiB pour
les valeurs conservées et l’assemblage en cours. Ce budget porte sur les octets
JSON, pas sur toute la mémoire du parseur, des objets C++ et du rendu.
Le délai de 5 s couvre toute la continuation, et non chaque page séparément.
Le remplacement d’un état annule son assemblage. Rien n’est présenté avant
l’assemblage complet, le SHA256, le décodage UTF-8 et la validation sémantique.
Le parseur borne aussi la profondeur. Une valeur paginée null pour ready/empty
est refusée, même avec un digest valide.

Le validateur n’est pas un moteur JSON Schema général : il interprète seulement
le vocabulaire du schéma A18 figé, vérifié par `validate-manifest.mjs`.
Les invariants supplémentaires du README couvrent les IDs UTF-8 de 4096 octets,
les IDs uniques, les sessions orphelines, la complétude audio, les capacités du
lecteur sélectionné, les entiers sûrs, la plage seek et les diagnostics UTF-16.
Le SHA256 portable est testé avec les vecteurs vide, abc et un million de a.

Les fixtures vérifient le consommateur et le panneau caché sans fournisseur
réel, son audio, fenêtre visible, capture du bureau ou commande système. La
preuve interop A18/A24 et le comportement Windows réel restent à l’intégrateur.
