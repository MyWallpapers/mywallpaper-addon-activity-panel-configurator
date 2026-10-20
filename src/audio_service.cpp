#include "audio_service.hpp"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace activity {
namespace {
constexpr size_t jsonLimit = 65536, inventoryLimit = 16777216;
void Require(bool test, const char *message) {
    if (!test)
        throw std::runtime_error(message);
}
std::string S(const Json &j, const char *key) { return j.at(key).get<std::string>(); }
Json ReceiptHash(const char *domain, const std::string &value) {
    return value.empty()
               ? Json(nullptr)
               : Json(Sha256(std::string("a24-receipt-v1:") + domain + ":" + value));
}
const char *ClosureClass(const Json &reason) noexcept {
    if (!reason.is_string())
        return "unspecified";
    const auto &text = reason.get_ref<const std::string &>();
    if (text == "Native service endpoint revoked" || text == "Service endpoint revoked")
        return "revoked";
    if (text == "Invalid or revoked native service generation")
        return "invalid-or-revoked";
    if (text == "Service graph replaced")
        return "graph-replaced";
    return "unspecified"; // Never serialize an arbitrary host/broker reason.
}
void ErrorText(const Json &e) {
    if (e.is_null())
        return;
    const auto text = S(e, "message");
    size_t units = 0;
    for (unsigned char c : text)
        if ((c & 0xc0) != 0x80)
            units += c >= 0xf0 ? 2 : 1;
    Require(units <= 512, "Diagnostic A18 trop long");
}
} // namespace
AudioService::AudioService(Send send) : send_(std::move(send)) {}
std::string AudioService::Id() { return "a24:" + std::to_string(++serial_); }
void AudioService::Observe(const char *event, const std::string &operation, const char *topic,
                           int64_t sequence, int64_t revision, const char *closureClass,
                           const Json *closureReason) noexcept {
    // Per AudioService lifetime: at most 32 receipts + one limit marker, each <= 768 bytes.
    // The production Panel owns one instance. Logging failures never change service handling.
    if (diagnosticRecords_ > 32)
        return;
    const bool limited = diagnosticRecords_++ == 32;
    try {
        Json receipt{{"a24ServiceReceipt", 1},
                     {"ordinal", diagnosticRecords_},
                     {"event", limited ? "observation.limit" : event},
                     {"transportHash", ReceiptHash("transport", transport_)},
                     {"connectionHash", ReceiptHash("connection", connection_)},
                     {"providerGenerationHash", ReceiptHash("provider", generation_)}};
        if (!limited) {
            if (!operation.empty())
                receipt["operationHash"] = ReceiptHash("operation", operation);
            if (topic)
                receipt["topic"] = topic;
            if (sequence >= 0)
                receipt["brokerSequence"] = sequence;
            if (revision >= 0)
                receipt["topicRevision"] = revision;
            if (closureClass)
                receipt["closureClass"] = closureClass;
            // Correlate a host reason without copying its possibly private text.
            if (closureReason && closureReason->is_string())
                receipt["closureReasonHash"] =
                    ReceiptHash("closure-reason", closureReason->get_ref<const std::string &>());
            if (std::string(event) == "snapshot.rpc.received")
                receipt["revisions"] = {{"audio", topics_.at("audio").state.at("revision")},
                                        {"media", topics_.at("media").state.at("revision")}};
        }
        const auto line = receipt.dump() + "\n";
        if (line.size() <= 768)
            std::cerr.write(line.data(), static_cast<std::streamsize>(line.size()));
    } catch (...) {
        // A diagnostic is best effort; stdout framing, ACKs and service state remain authoritative.
    }
}
void AudioService::Envelope(const Json &envelope) {
    Require(Json{{"type", "message"}, {"v", 5}, {"target", "broadcast"}, {"payload", envelope}}
                    .dump()
                    .size() <= jsonLimit,
            "Enveloppe native A18 trop grande");
    send_(envelope);
}
void AudioService::Packet(const Json &packet) {
    if (!transport_.empty())
        Envelope({{"type", "mywallpaper.services"},
                  {"v", 1},
                  {"generation", transport_},
                  {"kind", "packet"},
                  {"packet", packet}});
}
bool AudioService::BusyCommand() const {
    for (const auto &[id, p] : pending_)
        if (p.method == "command")
            return true;
    return false;
}
void AudioService::CancelReads() {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.method == "command" || it->second.method == "connect") {
            ++it;
            continue;
        }
        Packet({{"type", "cancel"}, {"id", it->first}});
        it = pending_.erase(it);
    }
    inventory_.reset();
    for (auto &[name, topic] : topics_) {
        if (!topic.subscription.empty())
            Packet({{"type", "unsubscribe"}, {"id", topic.subscription}});
        topic = Topic{};
    }
}
void AudioService::Reset(bool disconnect) {
    if (BusyCommand()) {
        commandOutcome_ = "unknown";
        notice_ = "Résultat inconnu · connexion interrompue, commande non rejouée";
    }
    for (const auto &[id, p] : pending_)
        Packet({{"type", "cancel"}, {"id", id}});
    pending_.clear();
    inventory_.reset();
    for (auto &[name, topic] : topics_) {
        if (!topic.subscription.empty())
            Packet({{"type", "unsubscribe"}, {"id", topic.subscription}});
        topic = Topic{};
    }
    if (disconnect && !connection_.empty())
        Packet({{"type", "disconnect"}, {"connection", connection_}});
    connection_.clear();
    generation_.clear();
    recoveryUsed_ = false;
}
void AudioService::Close() {
    if (!transport_.empty() || !connection_.empty())
        Observe("consumer.closed");
    Reset(true);
    transport_.clear();
}
void AudioService::Enable(bool enabled, Clock::time_point now) {
    if (enabled_ == enabled)
        return;
    enabled_ = enabled;
    if (!enabled) {
        Observe("consumer.disabled");
        Reset(true);
    } else if (!transport_.empty())
        Connect(now);
}
void AudioService::Connect(Clock::time_point now) {
    if (!enabled_ || transport_.empty() || !connection_.empty() || !pending_.empty())
        return;
    auto id = Id();
    pending_.emplace(id, Pending{"connect", "", now + std::chrono::seconds(5)});
    notice_ = "Connexion au fournisseur A18 sélectionné…";
    Packet({{"type", "connect"}, {"id", id}, {"timeoutMs", 5000}, {"alias", "audio"}});
    Observe("connect.sent", id);
}
void AudioService::Call(const std::string &method, Json request, Clock::time_point now,
                        Clock::time_point deadline) {
    Require(!connection_.empty() && deadline > now, "Lecture A18 expirée");
    const auto id = Id();
    const auto requestId = S(request, "requestId");
    // Validate before dispatch: an oversized request is certainly not sent.
    Json packet{
        {"type", "call"},
        {"id", id},
        {"connection", connection_},
        {"method", method},
        {"payload", request},
        {"timeoutMs",
         std::max<int64_t>(
             1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count())}};
    Packet(packet);
    pending_.emplace(id, Pending{method, requestId, deadline});
    if (method == "snapshot")
        Observe("snapshot.rpc.sent", id);
}
void AudioService::Read(Clock::time_point now) {
    CancelReads();
    if (connection_.empty())
        return;
    notice_ = commandOutcome_ == "unknown" ? "Résultat inconnu · relecture de l’état, sans rejeu"
                                           : "Lecture initiale A18…";
    Call("snapshot", {{"kind", "audio-system.snapshot.request"}, {"requestId", Id()}}, now,
         now + std::chrono::seconds(5));
}
void AudioService::ReadAgain(Clock::time_point now) {
    if (!enabled_ || BusyCommand())
        return;
    recoveryUsed_ = false;
    if (connection_.empty())
        Connect(now);
    else
        Read(now);
}
void AudioService::Resync(const std::string &reason, Clock::time_point now) {
    const bool canRead = !recoveryUsed_ && !connection_.empty();
    CancelReads();
    notice_ = reason + " · Relire l’état pour continuer";
    recoveryUsed_ = true;
    if (canRead) {
        Call("snapshot", {{"kind", "audio-system.snapshot.request"}, {"requestId", Id()}}, now,
             now + std::chrono::seconds(5));
    }
}
const Json &AudioService::State(const std::string &topic) const { return topics_.at(topic).value; }
void AudioService::ValidateValue(const std::string &topic, const Json &state,
                                 const Json &value) const {
    if (value.is_null()) {
        Require(state.at("availability") == "absent" || state.at("availability") == "unavailable",
                "Inventaire A18 nul pour un état disponible");
        return;
    }
    Require(AudioContract(topic == "audio" ? "audioValue" : "mediaValue", value),
            "Valeur A18 hors contrat");
    std::set<std::string> ids;
    for (const auto &entry : value.at("sessions"))
        Require(ids.insert(S(entry, "id")).second, "Sessions A18 dupliquées");
    if (topic == "media") {
        const auto &selected = value.at("selected");
        Require((state.at("availability") == "ready") == !selected.is_null(),
                "Sélection média incohérente");
        bool found = false;
        for (const auto &entry : value.at("sessions")) {
            if (!selected.is_null() && entry.at("id") == selected.at("id")) {
                Require(entry.at("capabilities") == selected.at("capabilities"),
                        "Capacités média incohérentes");
                found = true;
            } else
                Require(entry.at("capabilities").is_null(),
                        "Capacités d’une session non sélectionnée");
        }
        if (!selected.is_null()) {
            Require(found, "Sélection média absente de l’inventaire");
            Require(selected.at("seekStartMs").get<int64_t>() <=
                        selected.at("seekEndMs").get<int64_t>(),
                    "Plage média inversée");
        }
    } else {
        std::set<std::string> endpoints;
        for (const auto &entry : value.at("endpoints")) {
            Require(endpoints.insert(S(entry, "id")).second, "Périphériques A18 dupliqués");
        }
        for (const auto &entry : value.at("sessions")) {
            Require(endpoints.contains(S(entry, "endpointId")), "Session audio orpheline");
            auto endpoint =
                std::find_if(value.at("endpoints").begin(), value.at("endpoints").end(),
                             [&](const Json &e) { return e.at("id") == entry.at("endpointId"); });
            if (endpoint->at("capture").get<bool>())
                Require(!entry.at("capabilities").at("volume").get<bool>() &&
                            !entry.at("capabilities").at("mute").get<bool>(),
                        "Capacité de capture interdite");
        }
        bool empty = endpoints.empty() && ids.empty();
        Require((state.at("availability") == "empty") == empty, "Disponibilité audio incohérente");
        const bool complete = value.at("complete").get<bool>();
        Require(complete == value.at("errors").empty(), "Complétude audio incohérente");
        Require(complete || !empty, "Observation audio absente mais partielle");
    }
}
void AudioService::AcceptState(const Json &state, bool snapshot) {
    const auto name = S(state, "topic");
    Require(name == "audio" || name == "media", "Topic A18 inconnu");
    Require(AudioContract(name == "audio" ? "audioState" : "mediaState", state),
            "État A18 hors contrat");
    ErrorText(state.at("error"));
    Require(state.at("generation") == generation_, "Génération A18 périmée");
    auto &topic = topics_.at(name);
    const auto revision = state.at("revision").get<int64_t>();
    if (!topic.state.is_null()) {
        const auto old = topic.state.at("revision").get<int64_t>();
        if (snapshot && revision == old && topic.state == state)
            return;
        Require(revision == old + 1, "Révision A18 discontinue");
    }
    if (inventory_ && inventory_->topic == name) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.method != "inventory") {
                ++it;
                continue;
            }
            Packet({{"type", "cancel"}, {"id", it->first}});
            it = pending_.erase(it);
        }
        inventory_.reset();
    }
    const auto &body = state.at("body");
    if (body.at("mode") == "inline")
        ValidateValue(name, state, body.at("value"));
    topic.state = state;
    topic.current = body.at("mode") == "inline";
    topic.value = topic.current ? body.at("value") : Json(nullptr);
    size_t retained = 0;
    for (const auto &[key, t] : topics_) {
        if (t.state.is_null())
            continue;
        if (t.state.at("body").at("mode") == "paged")
            retained += t.state.at("body").at("totalBytes").get<size_t>();
        else if (!t.value.is_null())
            retained += t.value.dump().size();
    }
    // Count both current paged references before allocating either assembly.
    // A later inline update must also fit beside the other topic's reservation.
    Require(retained <= inventoryLimit, "Budget des états A18 dépassé");
}
void AudioService::ContinueInventory(Clock::time_point now) {
    if (!inventory_) {
        for (auto &[name, topic] : topics_)
            if (!topic.current && !topic.state.is_null() &&
                topic.state.at("body").at("mode") == "paged") {
                const auto reference = topic.state.at("body");
                size_t retained = 0;
                for (const auto &[key, t] : topics_)
                    if (!t.value.is_null())
                        retained += t.value.dump().size();
                Require(retained + reference.at("totalBytes").get<size_t>() <= inventoryLimit,
                        "Budget d’inventaire A18 dépassé");
                inventory_ = Inventory{name,
                                       {},
                                       reference,
                                       topic.state.at("revision").get<int64_t>(),
                                       now + std::chrono::seconds(5)};
                inventory_->bytes.reserve(reference.at("totalBytes").get<size_t>());
                break;
            }
    }
    if (!inventory_)
        return;
    for (const auto &[id, p] : pending_)
        if (p.method == "inventory")
            return;
    const auto &in = *inventory_;
    Call("inventory",
         {{"kind", "audio-system.inventory.request"},
          {"requestId", Id()},
          {"generation", generation_},
          {"topic", in.topic},
          {"revision", in.revision},
          {"inventoryId", in.reference.at("inventoryId")},
          {"offset", in.bytes.size()}},
         now, in.deadline);
}
void AudioService::InventoryPage(const Json &page, Clock::time_point now) {
    Require(inventory_.has_value(), "Page A18 sans lecture");
    auto &in = *inventory_;
    Require(now < in.deadline && AudioContract("inventoryResult", page),
            "Page A18 invalide ou expirée");
    Require(page.at("generation") == generation_ && page.at("topic") == in.topic &&
                page.at("revision") == in.revision &&
                page.at("inventoryId") == in.reference.at("inventoryId") &&
                page.at("totalBytes") == in.reference.at("totalBytes") &&
                page.at("sha256") == in.reference.at("sha256") &&
                page.at("offset") == in.bytes.size(),
            "Identité de page A18 incohérente");
    const auto decoded = DecodeBase64(S(page, "data"));
    const auto total = in.reference.at("totalBytes").get<size_t>();
    Require(decoded.size() == std::min<size_t>(32768, total - in.bytes.size()),
            "Taille de page A18 incohérente");
    in.bytes += decoded;
    if (in.bytes.size() < total) {
        Require(page.at("nextOffset") == in.bytes.size(), "Continuation A18 invalide");
        ContinueInventory(now);
        return;
    }
    Require(page.at("nextOffset").is_null() && Sha256(in.bytes) == S(in.reference, "sha256"),
            "Digest d’inventaire A18 invalide");
    // Parse only complete, hash-verified UTF-8. Depth is bounded before allocation.
    auto value = Json::parse(in.bytes, [](int depth, Json::parse_event_t, Json &) {
        if (depth > 32)
            throw std::runtime_error("Profondeur d’inventaire A18 invalide");
        return true;
    });
    auto &topic = topics_.at(in.topic);
    ValidateValue(in.topic, topic.state, value);
    topic.value = std::move(value);
    topic.current = true;
    inventory_.reset();
    ContinueInventory(now);
}
void AudioService::Handle(const Json &packet, Clock::time_point now) {
    const auto type = S(packet, "type");
    if (type == "reply") {
        const auto id = S(packet, "id");
        auto it = pending_.find(id);
        if (it == pending_.end())
            return; // cancelled, expired or old generation
        const auto pending = it->second;
        pending_.erase(it);
        if (pending.method == "command")
            commandOutcome_ = "unknown";
        Require(packet.at("ok").is_boolean(), "Réponse broker invalide");
        if (!packet.at("ok").get<bool>()) {
            if (pending.method == "command") {
                commandOutcome_ = "unknown";
                notice_ = "Résultat inconnu · erreur de transport, commande non rejouée";
                Resync(notice_, now);
            } else if (pending.method == "connect")
                notice_ = "Fournisseur A18 absent, refusé ou indisponible · Reconnecter";
            else
                Resync("Lecture A18 indisponible", now);
            return;
        }
        const auto &value = packet.at("value");
        if (pending.method == "connect") {
            Require(AudioContract("token", value), "Connexion broker invalide");
            connection_ = value.get<std::string>();
            Observe("connect.received", id);
            Read(now);
            return;
        }
        Require(value.is_object() && value.at("requestId") == pending.request,
                "Corrélation A18 invalide");
        if (value.value("kind", "") == "audio-system.read.error") {
            Require(AudioContract("readError", value) && value.at("generation") == generation_,
                    "Erreur de lecture A18 invalide");
            ErrorText(value.at("error"));
            Resync("Lecture A18 refusée : " + S(value.at("error"), "code"), now);
            return;
        }
        if (pending.method == "snapshot") {
            Require(AudioContract("snapshotResult", value), "Snapshot A18 hors contrat");
            const auto generation = S(value, "generation");
            Require(generation_.empty() || generation_ == generation, "Génération A18 remplacée");
            generation_ = generation;
            AcceptState(value.at("audio"), true);
            AcceptState(value.at("media"), true);
            Observe("snapshot.rpc.received", id);
            for (auto &[name, topic] : topics_) {
                topic.subscription = Id();
                topic.first = true;
                topic.stateObserved = false;
                topic.sequence = -1;
                Packet({{"type", "subscribe"},
                        {"id", topic.subscription},
                        {"connection", connection_},
                        {"topic", name}});
                Observe("subscription.sent", topic.subscription, name.c_str());
            }
            notice_ = commandOutcome_ == "unknown"
                          ? "Résultat inconnu · état relu, commande non rejouée"
                          : "État A18 reçu";
            ContinueInventory(now);
        } else if (pending.method == "inventory") {
            InventoryPage(value, now);
        } else if (pending.method == "command") {
            Require(AudioContract("commandResult", value) && value.at("generation") == generation_,
                    "Résultat métier A18 invalide");
            ErrorText(value.at("error"));
            commandOutcome_ = S(value, "outcome");
            if (value.at("ok").get<bool>())
                notice_ = "Commande acceptée · seul l’état reçu décrit le résultat observé";
            else {
                notice_ = commandOutcome_ == "unknown" ? "Résultat inconnu · commande non rejouée"
                                                       : "Commande refusée";
                notice_ += " : " + S(value.at("error"), "code");
                Resync(notice_, now);
            }
        }
    } else if (type == "subscription") {
        const auto id = S(packet, "id");
        // Broker backpressure credit is separate from native delivery credit.
        Packet({{"type", "subscription.ack"}, {"id", id}});
        for (auto &[name, topic] : topics_)
            if (topic.subscription == id) {
                const auto &event = packet.at("event");
                Require(event.is_object() && event.size() == 3 && event.contains("value"),
                        "Événement A18 invalide");
                const auto kind = S(event, "kind");
                Require(kind == (topic.first ? "snapshot" : "state"),
                        "Ordre des événements A18 invalide");
                const auto &sequence = event.at("sequence");
                Require(sequence.is_number_integer() && sequence.get<double>() >= 0 &&
                            sequence.get<double>() <= 9007199254740991.0,
                        "Séquence broker invalide");
                const auto next = sequence.get<int64_t>();
                Require(topic.first || next == topic.sequence + 1, "Séquence broker discontinue");
                Require(event.at("value").at("topic") == name, "Topic A18 croisé");
                AcceptState(event.at("value"), topic.first);
                topic.sequence = next;
                if (topic.first || !topic.stateObserved)
                    Observe(topic.first ? "subscription.snapshot.received"
                                        : "subscription.state.received",
                            id, name.c_str(), next, event.at("value").at("revision").get<int64_t>());
                if (!topic.first)
                    topic.stateObserved = true;
                topic.first = false;
                ContinueInventory(now);
                break;
            }
    } else if (type == "subscription.error") {
        const auto id = S(packet, "id");
        for (const auto &[name, topic] : topics_)
            if (topic.subscription == id) {
                Observe("subscription.error.received", id, name.c_str());
                Resync("Abonnement A18 interrompu", now);
                break;
            }
    } else if (type == "connection.closed" && packet.at("id") == connection_) {
        Observe("connection.closed.received", {}, nullptr, -1, -1,
                packet.contains("reason") ? ClosureClass(packet.at("reason")) : "unspecified",
                packet.contains("reason") ? &packet.at("reason") : nullptr);
        const bool command = BusyCommand();
        Reset(false);
        if (!command)
            notice_ = "Fournisseur A18 déconnecté · Reconnecter";
    }
}
bool AudioService::Receive(const Json &frame, Clock::time_point now) {
    if (!frame.is_object() || !frame.contains("payload") || !frame.at("payload").is_object() ||
        frame.at("payload").value("type", "") != "mywallpaper.services")
        return false;
    Tick(now);
    try {
        const auto &e = frame.at("payload");
        Require(frame.at("type") == "message" && frame.at("v") == 5 &&
                    frame.at("surface") == "service" &&
                    frame.at("instanceId") == e.at("generation") && frame.at("displayIndex") == 0 &&
                    frame.dump().size() <= jsonLimit,
                "Route native A18 invalide");
        Require(e.at("v") == 1 && AudioContract("token", e.at("generation")) &&
                    e.at("generation") != "__proto__" && e.at("generation") != "constructor" &&
                    e.at("generation") != "prototype",
                "Enveloppe native A18 invalide");
        const auto epoch = S(e, "generation"), kind = S(e, "kind");
        std::set<std::string> keys{"type", "v", "generation", "kind"};
        if (kind == "packet") {
            keys.insert("packet");
            keys.insert("deliveryId");
        } else if (kind == "closed")
            keys.insert("reason");
        else
            Require(kind == "open", "Type d’enveloppe native inattendu");
        for (auto it = e.begin(); it != e.end(); ++it)
            Require(keys.contains(it.key()), "Champ d’enveloppe native inconnu");
        if (kind == "open") {
            if (epoch != transport_) {
                if (!transport_.empty())
                    Observe("transport.replaced");
                Reset(true);
                transport_ = epoch;
                Observe("transport.open.received");
                if (enabled_)
                    Connect(now);
            }
            return true;
        }
        if (epoch != transport_)
            return true;
        if (kind == "closed") {
            Require(e.at("reason").is_string(), "Fermeture native invalide");
            ErrorText({{"message", e.at("reason")}});
            Observe("transport.closed.received", {}, nullptr, -1, -1, ClosureClass(e.at("reason")),
                    &e.at("reason"));
            const bool command = BusyCommand();
            Reset(false);
            transport_.clear();
            if (!command)
                notice_ = "Service A18 indisponible · connexion fermée";
        } else {
            Require(AudioContract("token", e.at("deliveryId")) && e.at("packet").is_object(),
                    "Livraison native A18 invalide");
            // Release host reservation even if business validation subsequently fails.
            Envelope({{"type", "mywallpaper.services"},
                      {"v", 1},
                      {"generation", transport_},
                      {"kind", "ack"},
                      {"deliveryId", e.at("deliveryId")}});
            Handle(e.at("packet"), now);
        }
    } catch (const std::exception &e) {
        if (BusyCommand()) {
            commandOutcome_ = "unknown";
            notice_ = "Résultat inconnu · réponse invalide, commande non rejouée";
        }
        // Remove any in-flight command after an invalid business result. No replay.
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.method == "command") {
                Packet({{"type", "cancel"}, {"id", it->first}});
                it = pending_.erase(it);
            } else
                ++it;
        }
        Resync((commandOutcome_ == "unknown" ? std::string("Résultat inconnu · ") : std::string{}) +
                   "Contrat A18 refusé : " + e.what(),
               now);
    }
    return true;
}
void AudioService::Tick(Clock::time_point now) {
    bool expired = false, command = false, connecting = false;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now < it->second.deadline) {
            ++it;
            continue;
        }
        command |= it->second.method == "command";
        connecting |= it->second.method == "connect";
        Packet({{"type", "cancel"}, {"id", it->first}});
        it = pending_.erase(it);
        expired = true;
    }
    if (!expired)
        return;
    if (command)
        commandOutcome_ = "unknown";
    if (connecting)
        notice_ = "Fournisseur A18 absent ou délai de connexion dépassé · Reconnecter";
    else
        Resync(command ? "Résultat inconnu · délai dépassé, commande non rejouée"
                       : "Délai de lecture A18 dépassé",
               now);
}
std::vector<ServiceControl> AudioService::Controls() const {
    std::vector<ServiceControl> result;
    if (!enabled_ || connection_.empty() || generation_.empty() || BusyCommand())
        return result;
    auto add = [&](const std::string &label, Json command, const std::string &topic,
                   Json target = nullptr) {
        const auto &t = topics_.at(topic);
        result.push_back({label, std::move(command), connection_, generation_,
                          t.state.at("revision").get<int64_t>(), std::move(target)});
    };
    const auto &media = topics_.at("media");
    if (media.current && !media.value.is_null()) {
        const auto &selected = media.value.at("selected");
        if (!selected.is_null()) {
            const auto &caps = selected.at("capabilities");
            for (const auto &[action, label] : std::vector<std::pair<std::string, std::string>>{
                     {"previous", "Précédent"},
                     {"playPause",
                      selected.at("playing").get<bool>() ? "Pause média" : "Lire média"},
                     {"next", "Suivant"}})
                if (caps.at(action).get<bool>())
                    add(label, {{"kind", "media.command"}, {"action", action}}, "media",
                        selected.at("id"));
            if (caps.at("shuffle").get<bool>())
                add("Aléatoire",
                    {{"kind", "media.command"},
                     {"action", "shuffle"},
                     {"value", !selected.at("shuffle").get<bool>()}},
                    "media", selected.at("id"));
            if (caps.at("repeat").get<bool>())
                add("Répétition",
                    {{"kind", "media.command"},
                     {"action", "repeat"},
                     {"value", selected.at("repeat") == "none"   ? "list"
                               : selected.at("repeat") == "list" ? "track"
                                                                 : "none"}},
                    "media", selected.at("id"));
            if (caps.at("seek").get<bool>()) {
                for (int delta : {-10000, 10000}) {
                    auto position = std::clamp(selected.at("positionMs").get<int64_t>() + delta,
                                               selected.at("seekStartMs").get<int64_t>(),
                                               selected.at("seekEndMs").get<int64_t>());
                    add(delta < 0 ? "−10 s" : "+10 s",
                        {{"kind", "media.command"}, {"action", "seek"}, {"value", position}},
                        "media", selected.at("id"));
                }
            }
        }
        const auto &sessions = media.value.at("sessions");
        if (!sessions.empty() && (sessions.size() > 1 || selected.is_null())) {
            size_t next = 0;
            for (size_t i = 0; i < sessions.size(); ++i)
                if (!selected.is_null() && sessions[i].at("id") == selected.at("id"))
                    next = (i + 1) % sessions.size();
            add("Choisir : " + S(sessions[next], "label"),
                {{"kind", "media.command"},
                 {"action", "session"},
                 {"value", sessions[next].at("id")}},
                "media");
        }
    }
    const auto &audio = topics_.at("audio");
    if (audio.current && !audio.value.is_null()) {
        for (const auto &endpoint : audio.value.at("endpoints"))
            if (endpoint.at("default").get<bool>() && !endpoint.at("capture").get<bool>()) {
                const auto &caps = endpoint.at("capabilities");
                if (caps.at("volume").get<bool>())
                    for (int delta : {-5, 5})
                        add(delta < 0 ? "Volume −" : "Volume +",
                            {{"kind", "audio.command"},
                             {"action", "endpointVolume"},
                             {"id", endpoint.at("id")},
                             {"volume",
                              std::clamp(endpoint.at("volume").get<double>() + delta, 0.0, 100.0)}},
                            "audio");
                if (caps.at("mute").get<bool>())
                    add(endpoint.at("muted").get<bool>() ? "Rétablir son" : "Couper son",
                        {{"kind", "audio.command"},
                         {"action", "endpointMute"},
                         {"id", endpoint.at("id")},
                         {"muted", !endpoint.at("muted").get<bool>()}},
                        "audio");
                break;
            }
    }
    return result;
}
bool AudioService::Execute(const ServiceControl &control, Clock::time_point now) {
    Tick(now);
    const auto controls = Controls();
    if (std::find(controls.begin(), controls.end(), control) == controls.end()) {
        notice_ = "Commande non envoyée · cible, révision ou capacité modifiée";
        return false;
    }
    Json request{{"kind", "audio-system.command.request"},
                 {"requestId", Id()},
                 {"generation", generation_},
                 {"revision", control.revision},
                 {"timeoutMs", 3000},
                 {"expectedSessionId", control.target},
                 {"command", control.command}};
    if (!AudioContract("commandRequest", request)) {
        notice_ = "Commande A18 hors contrat · non envoyée";
        return false;
    }
    try {
        Call("command", request, now, now + std::chrono::seconds(3));
    } catch (const std::exception &) {
        commandOutcome_ = "unknown";
        notice_ = "Résultat inconnu · transport interrompu, commande non rejouée";
        for (auto &[name, topic] : topics_)
            topic = Topic{};
        pending_.clear();
        inventory_.reset();
        connection_.clear();
        generation_.clear();
        return false;
    }
    commandOutcome_ = "pending";
    notice_ = "Commande envoyée · résultat métier en attente";
    return true;
}
std::string AudioService::Detail() const {
    const auto &t = topics_.at("media");
    if (!enabled_)
        return "Média désactivé";
    if (connection_.empty())
        return notice_;
    if (!t.current || t.state.is_null())
        return "Média indisponible · lecture A18 en cours ou interrompue";
    const auto availability = S(t.state, "availability");
    if (availability == "absent")
        return "Aucune source média A18";
    if (availability == "unavailable")
        return "Média indisponible : " + S(t.state.at("error"), "code");
    if (t.value.at("selected").is_null())
        return "Aucun média sélectionné";
    const auto &s = t.value.at("selected");
    return (s.at("playing").get<bool>() ? "Lecture · " : "Pause · ") + S(s, "title") + " · " +
           S(s, "artist");
}
Json AudioService::Status() const {
    Json states = Json::object();
    for (const auto &[name, t] : topics_)
        states[name] = {
            {"current", t.current},
            {"availability", t.state.is_null() ? Json("unavailable") : t.state.at("availability")},
            {"revision", t.state.is_null() ? Json(nullptr) : t.state.at("revision")}};
    const auto &media = topics_.at("media");
    return {{"state", media.current && !media.state.is_null() ? media.state.at("availability")
                                                              : Json("unavailable")},
            {"reason", connection_.empty() ? "provider-not-connected"
                       : media.current     ? "state-received"
                                           : "state-not-current"},
            {"connected", !connection_.empty()},
            {"generation", generation_},
            {"topics", states},
            {"commandOutcome", commandOutcome_},
            {"notice", notice_},
            {"inventoryPending", inventory_.has_value()},
            {"pending", pending_.size()}};
}
} // namespace activity
