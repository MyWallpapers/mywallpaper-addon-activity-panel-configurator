#include "audio_examples.hpp"
#include "audio_service.hpp"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace activity;
namespace {
const Json example = Json::parse(audioExamples);
void Check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string Base64(const std::string &bytes) {
    const std::string a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned n = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size())
            n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size())
            n |= static_cast<unsigned char>(bytes[i + 2]);
        out += a[(n >> 18) & 63];
        out += a[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? a[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? a[n & 63] : '=';
    }
    return out;
}
struct Fixture {
    std::vector<Json> sent;
    AudioService client{[this](const Json &e) { sent.push_back(e); }};
    AudioService::Clock::time_point now{};
    std::string epoch = "host-generation-1";
    int delivery = 0;
    void Envelope(Json e) {
        e["type"] = "mywallpaper.services";
        e["v"] = 1;
        e["generation"] = epoch;
        Check(client.Receive({{"type", "message"},
                              {"v", 5},
                              {"surface", "service"},
                              {"instanceId", epoch},
                              {"displayIndex", 0},
                              {"payload", e}},
                             now),
              "Recognized service route");
    }
    void Packet(Json p) {
        Envelope({{"kind", "packet"},
                  {"deliveryId", "delivery:" + std::to_string(++delivery)},
                  {"packet", p}});
    }
    std::vector<Json> Packets(const std::string &type) const {
        std::vector<Json> out;
        for (const auto &e : sent)
            if (e.at("kind") == "packet" && e.at("packet").at("type") == type)
                out.push_back(e.at("packet"));
        return out;
    }
    Json Last(const std::string &type) const {
        auto p = Packets(type);
        Check(!p.empty(), "Expected outgoing packet");
        return p.back();
    }
    void Reply(Json value) {
        auto call = Last("call");
        value["requestId"] = call.at("payload").at("requestId");
        Packet({{"type", "reply"}, {"id", call.at("id")}, {"ok", true}, {"value", value}});
    }
    void Open() {
        client.Enable(true, now);
        Envelope({{"kind", "open"}});
        Check(Last("connect").at("alias") == "audio", "Exact optional alias");
        Packet({{"type", "reply"},
                {"id", Last("connect").at("id")},
                {"ok", true},
                {"value", "opaque-connection:1"}});
    }
    void Start(Json snapshot = example.at("snapshotResult")) {
        Open();
        Reply(snapshot);
    }
    Json Subscription(const std::string &topic) const {
        for (const auto &p : Packets("subscribe"))
            if (p.at("topic") == topic)
                return p;
        throw std::runtime_error("Missing subscription");
    }
    void Event(const std::string &topic, Json state, int sequence, const char *kind = "snapshot") {
        Packet({{"type", "subscription"},
                {"id", Subscription(topic).at("id")},
                {"event", {{"kind", kind}, {"sequence", sequence}, {"value", state}}}});
    }
    ServiceControl Control(const std::string &action) const {
        for (const auto &c : client.Controls())
            if (c.command.at("action") == action)
                return c;
        throw std::runtime_error("Expected permitted control");
    }
};
struct StderrCapture {
    std::ostringstream bytes;
    std::streambuf *previous = std::cerr.rdbuf(bytes.rdbuf());
    ~StderrCapture() { std::cerr.rdbuf(previous); }
    std::vector<Json> Records() const {
        std::vector<Json> result;
        std::istringstream lines(bytes.str());
        std::string line;
        while (std::getline(lines, line)) {
            Check(line.size() + 1 <= 768, "Each receipt is bounded");
            result.push_back(Json::parse(line));
        }
        return result;
    }
};
void ReceiptDiagnostics() {
    StderrCapture capture;
    Fixture f;
    f.Start();
    f.Event("audio", example.at("audioState"), 8);
    f.Event("media", example.at("mediaState"), 12);
    auto next = example.at("mediaState");
    const auto revision = next.at("revision").get<int64_t>();
    for (int i = 1; i <= 100; ++i) {
        next["revision"] = revision + i;
        f.Event("media", next, 12 + i, "state");
    }
    const auto records = capture.Records();
    auto find = [&](const char *event, const char *topic = nullptr) -> Json {
        for (const auto &record : records)
            if (record.at("event") == event &&
                (!topic || record.value("topic", "") == topic))
                return record;
        throw std::runtime_error("Missing receipt");
    };
    const auto rpc = find("snapshot.rpc.received");
    const auto subscribe = find("subscription.sent", "media");
    const auto retained = find("subscription.snapshot.received", "media");
    const auto state = find("subscription.state.received", "media");
    Check(rpc.at("ordinal") < subscribe.at("ordinal") &&
              subscribe.at("ordinal") < retained.at("ordinal") &&
              retained.at("ordinal") < state.at("ordinal"),
          "RPC response, subscribe, retained snapshot and first state remain distinct");
    Check(!rpc.contains("brokerSequence") && rpc.at("revisions").at("media") == revision &&
              retained.at("brokerSequence") == 12 && retained.at("topicRevision") == revision &&
              state.at("brokerSequence") == 13 && state.at("topicRevision") == revision + 1,
          "Broker sequence is independent of topic revision");
    Check(rpc.at("transportHash") == Sha256("a24-receipt-v1:transport:" + f.epoch) &&
              rpc.at("connectionHash") == Sha256("a24-receipt-v1:connection:opaque-connection:1") &&
              rpc.at("providerGenerationHash") ==
                  Sha256("a24-receipt-v1:provider:" +
                         example.at("snapshotResult").at("generation").get<std::string>()),
          "Only domain-separated opaque identities are exposed");
    Check(find("snapshot.rpc.sent").at("operationHash") == rpc.at("operationHash") &&
              subscribe.at("operationHash") == retained.at("operationHash") &&
              retained.at("operationHash") == state.at("operationHash"),
          "RPC and subscription receipts correlate to their own operations");
    unsigned states = 0;
    for (const auto &record : records)
        states += record.at("event") == "subscription.state.received" ? 1 : 0;
    Check(states == 1, "Repeated states produce no periodic diagnostic noise");
    const auto bytes = capture.bytes.str();
    Check(bytes.find(f.epoch) == std::string::npos &&
              bytes.find("opaque-connection:1") == std::string::npos &&
              bytes.find(example.at("snapshotResult").at("generation").get<std::string>()) ==
                  std::string::npos &&
              bytes.find("Synthetic track") == std::string::npos &&
              bytes.find("delivery:") == std::string::npos,
          "No raw transport, connection, generation, delivery or media payload");
    for (const auto &endpoint : example.at("audioState").at("body").at("value").at("endpoints"))
        for (const auto *key : {"id", "name"})
            Check(bytes.find(endpoint.at(key).get<std::string>()) == std::string::npos,
                  "No endpoint identifiers or names");
    const auto &media = example.at("mediaState").at("body").at("value");
    for (const auto *key : {"id", "source", "title", "artist", "album"}) {
        const auto value = media.at("selected").at(key).get<std::string>();
        Check(value.empty() || bytes.find(value) == std::string::npos,
              "No media identifiers, names or metadata");
    }
    for (const auto &session : media.at("sessions"))
        for (const auto *key : {"id", "label"})
            Check(bytes.find(session.at(key).get<std::string>()) == std::string::npos,
                  "No media session identifiers or labels");
    f.Packet({{"type", "connection.closed"}, {"id", "opaque-connection:1"},
              {"reason", "Service endpoint revoked"}});
    const auto closed = capture.Records().back();
    Check(closed.at("event") == "connection.closed.received" &&
              closed.at("closureClass") == "revoked" &&
              closed.at("closureReasonHash") ==
                  Sha256("a24-receipt-v1:closure-reason:Service endpoint revoked") &&
              closed.at("connectionHash") == rpc.at("connectionHash") &&
              closed.at("providerGenerationHash") == rpc.at("providerGenerationHash"),
          "Revocation receipt retains the old identities before reset");
    f.Envelope({{"kind", "closed"}, {"reason", "private-unclassified-reason"}});
    Check(capture.Records().back().at("closureClass") == "unspecified" &&
              capture.Records().back().at("closureReasonHash") ==
                  Sha256("a24-receipt-v1:closure-reason:private-unclassified-reason") &&
              capture.bytes.str().find("private-unclassified-reason") == std::string::npos,
          "Arbitrary closure reasons are never copied");
}
void ReceiptDiagnosticLimit() {
    StderrCapture capture;
    Fixture f;
    for (int i = 0; i < 20; ++i) {
        f.Start();
        f.client.Enable(false, f.now);
    }
    const auto records = capture.Records();
    Check(records.size() == 33 && records.back().at("event") == "observation.limit" &&
              records.back().at("ordinal") == 33 && capture.bytes.str().size() <= 33 * 768,
          "Reconnects cannot reset or exceed the per-instance output budget");
    f.client.Close();
    Check(capture.Records().size() == 33, "Closure cannot bypass the diagnostic budget");
}
void ClosureReasonDiagnostics() {
    // Check actual emitted records: a size-only assertion could miss an omitted close line.
    size_t largestLine = 0;
    unsigned cases = 0;
    auto closed = [&](const StderrCapture &capture, size_t before, const char *event,
                      const Json &expectedHash, bool connected) {
        const auto records = capture.Records();
        Check(records.size() == before + 1 && records.back().at("event") == event,
              "The new closure receipt is emitted within the line budget");
        const auto &record = records.back();
        Check(record.at("closureReasonHash") == expectedHash,
              "Closure hash uses the exact domain and decoded reason bytes");
        Check(!record.at("transportHash").is_null() &&
                  record.at("connectionHash").is_null() == !connected &&
                  record.at("providerGenerationHash").is_null() == !connected,
              "Close receipt preserves identities before reset");
        largestLine = std::max(largestLine, record.dump().size() + 1);
        ++cases;
    };
    const std::string longestClass = "Invalid or revoked native service generation";
    // A18 dictionary S02: independent exact-vector anchor, not a hash of a prefix.
    const std::string dictionaryHash =
        "sha256:16ae60e44d2caa5f25f77edec9989ab726b9f5a3f484471490f06ca0647c9430";
    {
        StderrCapture capture;
        Fixture f;
        f.Envelope({{"kind", "open"}}); // Same deferred-enable order as the real attempt.
        f.client.Enable(true, f.now);
        const auto before = capture.Records().size();
        Check(before == 2 && f.Last("connect").at("id") == "a24:1",
              "First connect is pending after deferred enable");
        f.Envelope({{"kind", "closed"}, {"reason", longestClass}});
        closed(capture, before, "transport.closed.received", dictionaryHash, false);
        Check(capture.bytes.str().find(longestClass) == std::string::npos,
              "A classified reason remains opaque before connect reply");
    }
    std::string unicodeReason;
    for (int i = 0; i < 256; ++i)
        unicodeReason += "🚀"; // 512 UTF-16 units, 1024 UTF-8 bytes.
    const std::vector<std::string> reasons{
        longestClass, "synthetic-private-reason:\n\"\\", std::string(512, 's'), unicodeReason, ""};
    for (bool transport : {false, true})
        for (const auto &reason : reasons) {
            StderrCapture capture;
            Fixture f;
            f.Start(); // All three opaque identities are present, before any reset.
            const auto before = capture.Records().size();
            if (transport)
                f.Envelope({{"kind", "closed"}, {"reason", reason}});
            else
                f.Packet({{"type", "connection.closed"}, {"id", "opaque-connection:1"},
                          {"reason", reason}});
            const Json hash = reason.empty()
                                  ? Json(nullptr)
                                  : reason == longestClass
                                        ? Json(dictionaryHash)
                                        : Json(Sha256("a24-receipt-v1:closure-reason:" + reason));
            closed(capture, before,
                   transport ? "transport.closed.received" : "connection.closed.received", hash,
                   true);
            Check(reason.empty() || capture.bytes.str().find(reason) == std::string::npos,
                  "Neither arbitrary nor maximum-size reason text is exposed");
            Check(capture.bytes.str().find("synthetic-private-reason") == std::string::npos &&
                      capture.bytes.str().find("\\n\\\"") == std::string::npos,
                  "Escaped private reason fragments are not serialized either");
        }
    for (bool missing : {false, true}) {
        StderrCapture capture;
        Fixture f;
        f.Start();
        const auto before = capture.Records().size();
        Json packet{{"type", "connection.closed"}, {"id", "opaque-connection:1"}};
        if (!missing)
            packet["reason"] = {{"synthetic-private-reason", true}};
        f.Packet(packet);
        const auto records = capture.Records();
        Check(records.size() == before + 1 &&
                  records.back().at("event") == "connection.closed.received" &&
                  !records.back().contains("closureReasonHash") &&
                  capture.bytes.str().find("synthetic-private-reason") == std::string::npos,
              "Missing or non-string connection reasons preserve handling without disclosure");
        ++cases;
    }
    {
        StderrCapture capture;
        Fixture f;
        for (int i = 0; i < 12; ++i) {
            f.Envelope({{"kind", "open"}});
            f.client.Close();
        }
        f.Start();
        const auto before = capture.Records().size();
        Check(before == 31, "Exercise the last allowed receipt with every identity present");
        f.Packet({{"type", "connection.closed"}, {"id", "opaque-connection:1"},
                  {"reason", longestClass}});
        closed(capture, before, "connection.closed.received", dictionaryHash, true);
        Check(capture.Records().back().at("ordinal") == 32,
              "New field fits at the last allowed ordinal");
        f.epoch += ":next";
        f.Envelope({{"kind", "open"}});
        const auto records = capture.Records();
        Check(records.size() == 33 && records.back().at("event") == "observation.limit" &&
                  !records.back().contains("closureReasonHash"),
              "The reason field cannot bypass or enlarge the limit marker");
    }
    Check(largestLine <= 768, "Full-identity closure receipts fit the existing line budget");
    std::cout << "closureReasonHash diagnostic cases=" << cases
              << " maxLineBytes=" << largestLine << " limit=768\n";
}
void ContractChecks() {
    for (auto it = example.begin(); it != example.end(); ++it)
        Check(AudioContract(it.key() == "unknownResult" ? "commandResult" : it.key().c_str(),
                            it.value()),
              "Pinned A18 example conforms");
    auto bad = example.at("mediaState");
    bad["secret"] = true;
    Check(!AudioContract("mediaState", bad), "Reject extra keys");
    bad = example.at("mediaState");
    bad["body"]["value"]["selected"]["positionMs"] = 9007199254740992ULL;
    Check(!AudioContract("mediaState", bad), "Safe timeline integers");
    bad = example.at("commandRequest");
    bad["expectedSessionId"] = nullptr;
    bad["command"] = {{"kind", "media.command"}, {"action", "playPause"}};
    Check(!AudioContract("commandRequest", bad), "Pinned playback target required");
    Check(Sha256("") == "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA empty vector");
    Check(Sha256("abc") ==
              "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA abc vector");
    Check(Sha256(std::string(1000000, 'a')) ==
              "sha256:cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
          "SHA multi-block vector");
    Check(DecodeBase64("8J+agA==") == "🚀", "UTF8 base64 bytes");
    for (const auto *text : {"Zg=", "Zh==", "Zm9=", "=m9v", "Zm=v", "Zm9v====", "Zm9v\n"}) {
        bool threw = false;
        try {
            (void)DecodeBase64(text);
        } catch (...) {
            threw = true;
        }
        Check(threw, "Reject noncanonical base64");
    }
}
void Lifecycle() {
    Fixture f;
    Check(f.client.Status().at("state") == "unavailable" &&
              f.client.Status().at("reason") == "provider-not-connected",
          "Existing diagnostic fields retained");
    f.Start();
    Check(f.client.Detail().find("Synthetic track") != std::string::npos,
          "Real metadata enters content");
    Check(f.client.Controls().size() == 7,
          "Only current media and render capabilities produce controls");
    f.Event("audio", example.at("audioState"), 8);
    f.Event("media", example.at("mediaState"), 12);
    Check(f.Packets("subscription.ack").size() == 2, "Subscription backpressure credits");
    size_t nativeAcks = 0;
    for (const auto &e : f.sent)
        nativeAcks += e.at("kind") == "ack" ? 1 : 0;
    Check(nativeAcks == 4, "Native delivery credit is independent");
    auto pinned = f.Control("playPause");
    auto next = example.at("mediaState");
    next["revision"] = 12;
    next["body"]["value"]["selected"]["playing"] = true;
    f.Event("media", next, 13, "state");
    const auto calls = f.Packets("call").size();
    Check(!f.client.Execute(pinned, f.now) && f.Packets("call").size() == calls,
          "Stale revision at click is never retargeted");
    Check(f.client.Execute(f.Control("playPause"), f.now), "Current explicit playback dispatch");
    auto call = f.Last("call");
    Check(call.at("payload").at("expectedSessionId") == "media-owned",
          "Opaque selected target pinned");
    Check(call.at("payload").at("revision") == 12, "Current business revision pinned");
    Check(f.client.Status().at("commandOutcome") == "pending",
          "Delivery is not business acknowledgement");
    f.Reply(example.at("commandResult"));
    Check(f.client.Status().at("commandOutcome") == "acknowledged", "Correlated business result");
    Check(f.client.State("media").at("selected").at("playing") == true,
          "No optimistic mutation of observed state");
    auto old = f.Control("playPause");
    f.epoch = "host-generation-2";
    f.Envelope({{"kind", "open"}});
    Check(f.client.Controls().empty() && !f.client.Execute(old, f.now),
          "New host generation invalidates controls");
    Check(f.Last("connect").at("id") != call.at("id"), "Request IDs are not reused");
    f.client.Enable(false, f.now);
    Check(f.client.Controls().empty(), "Disable releases connection and controls");
}
void Failures() {
    Fixture f;
    f.Start();
    Check(f.client.Execute(f.Control("playPause"), f.now), "Dispatch before timeout");
    auto call = f.Last("call");
    f.now += std::chrono::seconds(4);
    f.client.Tick(f.now);
    Check(f.client.Status().at("commandOutcome") == "unknown", "Post dispatch timeout is unknown");
    Check(f.Last("call").at("method") == "snapshot", "Only read resync follows timeout");
    size_t commands = 0;
    for (const auto &p : f.Packets("call"))
        commands += p.at("method") == "command" ? 1 : 0;
    Check(commands == 1, "No implicit command replay");
    auto late = example.at("commandResult");
    late["requestId"] = call.at("payload").at("requestId");
    f.Packet({{"type", "reply"}, {"id", call.at("id")}, {"ok", true}, {"value", late}});
    Check(f.client.Status().at("commandOutcome") == "unknown",
          "Late result cannot repair unknown outcome");
    f.Reply(example.at("snapshotResult"));
    Check(!f.client.Controls().empty(), "Fresh read restores controls");
    Check(f.client.Execute(f.Control("playPause"), f.now), "New explicit user action");
    auto bad = example.at("commandResult");
    bad["requestId"] = "wrong";
    f.Packet({{"type", "reply"}, {"id", f.Last("call").at("id")}, {"ok", true}, {"value", bad}});
    Check(f.client.Status().at("commandOutcome") == "unknown",
          "Malformed business result cannot report success");
    Fixture gap;
    gap.Start();
    gap.Event("media", example.at("mediaState"), 50);
    auto state = example.at("mediaState");
    state["revision"] = 13;
    gap.Event("media", state, 52, "state");
    Check(gap.client.Controls().empty() && gap.Last("call").at("method") == "snapshot",
          "Gap invalidates capacities before resync");
    const auto count = gap.Packets("call").size();
    gap.now += std::chrono::seconds(6);
    gap.client.Tick(gap.now);
    Check(gap.Packets("call").size() == count, "Recovery retries are bounded");
    Fixture absent;
    absent.client.Enable(true, absent.now);
    absent.Envelope({{"kind", "open"}});
    absent.Packet({{"type", "reply"},
                   {"id", absent.Last("connect").at("id")},
                   {"ok", false},
                   {"error", "Service dependency is not bound"}});
    Check(absent.client.Controls().empty() &&
              absent.client.Detail().find("absent") != std::string::npos,
          "Optional missing provider remains visible");
    Fixture revoked;
    revoked.Start();
    revoked.client.Execute(revoked.Control("playPause"), revoked.now);
    revoked.Packet(
        {{"type", "connection.closed"}, {"id", "opaque-connection:1"}, {"reason", "revoked"}});
    Check(revoked.client.Controls().empty() &&
              revoked.client.Status().at("commandOutcome") == "unknown",
          "Revocation invalidates pending effect");
}
Json PagedSnapshot(const std::string &bytes) {
    auto snapshot = example.at("snapshotResult");
    snapshot["media"]["body"] = {{"mode", "paged"},
                                 {"inventoryId", "inventory:media"},
                                 {"totalBytes", bytes.size()},
                                 {"sha256", Sha256(bytes)}};
    return snapshot;
}
void SendPage(Fixture &f, const std::string &bytes, bool badHash = false) {
    auto call = f.Last("call");
    const auto offset = call.at("payload").at("offset").get<size_t>();
    const auto size = std::min<size_t>(32768, bytes.size() - offset);
    Json page{{"kind", "audio-system.inventory.result"},
              {"generation", "synthetic-generation-1"},
              {"topic", "media"},
              {"revision", 11},
              {"inventoryId", "inventory:media"},
              {"offset", offset},
              {"totalBytes", bytes.size()},
              {"sha256", badHash ? "sha256:" + std::string(64, '0') : Sha256(bytes)},
              {"encoding", "base64-utf8-json"},
              {"data", Base64(bytes.substr(offset, size))},
              {"nextOffset", offset + size == bytes.size() ? Json(nullptr) : Json(offset + size)}};
    f.Reply(page);
}
void SemanticRejection() {
    std::vector<Json> snapshots;
    auto invalid = example.at("snapshotResult");
    invalid["media"]["body"]["value"]["sessions"][0]["capabilities"] = nullptr;
    snapshots.push_back(invalid);
    invalid = example.at("snapshotResult");
    invalid["media"]["body"]["value"]["sessions"].push_back(
        invalid["media"]["body"]["value"]["sessions"][0]);
    snapshots.push_back(invalid);
    invalid = example.at("snapshotResult");
    invalid["audio"]["body"]["value"]["sessions"][0]["endpointId"] = "missing";
    snapshots.push_back(invalid);
    invalid = example.at("snapshotResult");
    invalid["media"]["body"]["value"]["selected"]["seekStartMs"] = 4000;
    snapshots.push_back(invalid);
    invalid = example.at("snapshotResult");
    invalid["media"]["availability"] = "empty";
    snapshots.push_back(invalid);
    invalid = example.at("snapshotResult");
    invalid["audio"]["body"]["value"]["complete"] = false;
    snapshots.push_back(invalid);
    for (const auto &snapshot : snapshots) {
        Fixture f;
        f.Start(snapshot);
        Check(f.client.Controls().empty(), "Semantic violation invalidates both topics");
        Check(f.Last("call").at("method") == "snapshot", "Semantic failure resync is read only");
    }
    for (const auto *availability : {"absent", "empty", "unavailable"}) {
        auto snapshot = example.at("snapshotResult");
        auto &media = snapshot["media"];
        media["availability"] = availability;
        if (std::string(availability) == "empty") {
            media["body"]["value"]["selected"] = nullptr;
            for (auto &session : media["body"]["value"]["sessions"])
                session["capabilities"] = nullptr;
        } else
            media["body"] = {{"mode", "inline"}, {"value", nullptr}};
        if (std::string(availability) == "unavailable")
            media["error"] = {{"code", "unavailable"}, {"message", "Synthetic unavailable"}};
        Fixture f;
        f.Start(snapshot);
        for (const auto &control : f.client.Controls())
            Check(control.command.at("kind") != "media.command" ||
                      (std::string(availability) == "empty" &&
                       control.command.at("action") == "session"),
                  "Playback is unavailable until a session is selected");
    }
    Fixture rejected;
    rejected.Start();
    rejected.client.Execute(rejected.Control("playPause"), rejected.now);
    auto result = example.at("unknownResult");
    result["outcome"] = "rejected";
    result["error"]["code"] = "stale_session";
    rejected.Reply(result);
    Check(rejected.client.Status().at("commandOutcome") == "rejected",
          "Typed business rejection is distinct from unknown");
    Fixture broken;
    broken.Start();
    broken.client.Execute(broken.Control("playPause"), broken.now);
    broken.Packet({{"type", "reply"}, {"id", broken.Last("call").at("id")}, {"value", nullptr}});
    Check(broken.client.Status().at("commandOutcome") == "unknown",
          "Malformed broker result cannot leave a command pending");
}
void Inventories() {
    auto value = example.at("mediaState").at("body").at("value");
    value["selected"]["title"] = "";
    const auto titleStart = value.dump().find("\"title\":\"\"") + 9;
    value["selected"]["title"] =
        std::string(32767 - titleStart, 'x') + "🚀" + std::string(40000, 'y');
    auto bytes = value.dump();
    Check(bytes.find("🚀") == 32767, "UTF8 character actually crosses the page boundary");
    Fixture f;
    f.Start(PagedSnapshot(bytes));
    Check(f.client.State("media").is_null(), "Paged state not partially exposed");
    const auto before = f.Packets("call").size();
    SendPage(f, bytes);
    Check(f.client.State("media").is_null() && f.Packets("call").size() == before + 1,
          "Single sequential page in flight");
    while (f.client.Status().at("inventoryPending").get<bool>())
        SendPage(f, bytes);
    Check(f.client.State("media") == value, "Complete UTF8 value verified and adopted");
    Fixture hash;
    hash.Start(PagedSnapshot(bytes));
    SendPage(hash, bytes, true);
    Check(hash.client.State("media").is_null() && hash.Last("call").at("method") == "snapshot",
          "Bad page digest fails closed");
    Fixture replaced;
    replaced.Start(PagedSnapshot(bytes));
    auto oldCall = replaced.Last("call");
    auto state = example.at("mediaState");
    state["revision"] = 12;
    replaced.Event("media", state, 20);
    Check(!replaced.client.Status().at("inventoryPending").get<bool>() &&
              replaced.client.State("media") == state.at("body").at("value"),
          "Replacement discards inventory lease");
    Fixture timeout;
    timeout.Start(PagedSnapshot(bytes));
    timeout.now += std::chrono::seconds(6);
    timeout.client.Tick(timeout.now);
    Check(!timeout.client.Status().at("inventoryPending").get<bool>(),
          "Inventory total deadline releases bytes");
    auto badValue = value;
    badValue["sessions"][0]["capabilities"] = nullptr;
    Fixture invalid;
    invalid.Start(PagedSnapshot(badValue.dump()));
    while (invalid.client.Status().at("inventoryPending").get<bool>())
        SendPage(invalid, badValue.dump());
    Check(invalid.client.State("media").is_null(),
          "Hash verification does not replace semantic validation");
    Fixture nullValue;
    nullValue.Start(PagedSnapshot("null"));
    SendPage(nullValue, "null");
    Check(nullValue.client.State("media").is_null() &&
              !nullValue.client.Status().at("topics").at("media").at("current").get<bool>(),
          "Paged null cannot become a current ready state");
    auto combined = PagedSnapshot(bytes);
    combined["audio"]["body"] = {{"mode", "paged"},
                                 {"inventoryId", "inventory:audio"},
                                 {"totalBytes", 10000000},
                                 {"sha256", Sha256("synthetic")}};
    combined["media"]["body"]["totalBytes"] = 10000000;
    Fixture totalBudget;
    totalBudget.Start(combined);
    Check(!totalBudget.client.Status().at("inventoryPending").get<bool>() &&
              totalBudget.client.Controls().empty(),
          "Combined references are bounded before allocation");
    auto big = PagedSnapshot(bytes);
    big["media"]["body"]["totalBytes"] = 16777217;
    Fixture budget;
    budget.Start(big);
    Check(budget.client.State("media").is_null(), "Inventory size bound enforced");
}
} // namespace
int main() {
    try {
        ContractChecks();
        Lifecycle();
        Failures();
        Inventories();
        SemanticRejection();
        ReceiptDiagnostics();
        ReceiptDiagnosticLimit();
        ClosureReasonDiagnostics();
        std::cout << "A18 consumer schema, lifecycle, pinned commands, credits, failures and "
                     "inventories passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
