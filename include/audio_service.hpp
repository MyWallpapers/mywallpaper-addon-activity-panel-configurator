#pragma once
#include "model.hpp"
#include <functional>
#include <map>

namespace activity {
// This validator implements only the vocabulary of the pinned A18 schema. It is
// deliberately not a general JSON Schema engine or a second service contract.
bool AudioContract(const char *definition, const Json &value);
std::string Sha256(const std::string &bytes);
std::string DecodeBase64(const std::string &text);

struct ServiceControl {
    std::string label;
    Json command;
    std::string connection, generation;
    int64_t revision = 0;
    Json target = nullptr;
    bool operator==(const ServiceControl &) const = default;
};

class AudioService {
  public:
    using Clock = std::chrono::steady_clock;
    using Send = std::function<void(const Json &)>;
    explicit AudioService(Send send);
    bool Receive(const Json &frame, Clock::time_point now);
    void Enable(bool enabled, Clock::time_point now);
    void ReadAgain(Clock::time_point now);
    void Tick(Clock::time_point now);
    void Close();
    bool Execute(const ServiceControl &control, Clock::time_point now);
    std::vector<ServiceControl> Controls() const;
    std::string Detail() const;
    std::string Notice() const {
        return commandOutcome_ == "unknown" ? "Résultat inconnu · commande non rejouée. " + notice_
                                            : notice_;
    }
    Json Status() const;
    const Json &State(const std::string &topic) const;

  private:
    struct Topic {
        Json state = nullptr, value = nullptr;
        std::string subscription;
        int64_t sequence = -1;
        bool first = true, current = false, stateObserved = false;
    };
    struct Pending {
        std::string method, request;
        Clock::time_point deadline;
    };
    struct Inventory {
        std::string topic, bytes;
        Json reference;
        int64_t revision;
        Clock::time_point deadline;
    };
    Send send_;
    std::string transport_, connection_, generation_, notice_ = "Fournisseur A18 non connecté";
    std::string commandOutcome_ = "none";
    bool enabled_ = false, recoveryUsed_ = false;
    uint64_t serial_ = 0;
    unsigned diagnosticRecords_ = 0;
    std::map<std::string, Pending> pending_;
    std::map<std::string, Topic> topics_{{"audio", {}}, {"media", {}}};
    std::optional<Inventory> inventory_;
    std::string Id();
    void Observe(const char *event, const std::string &operation = {}, const char *topic = nullptr,
                 int64_t sequence = -1, int64_t revision = -1,
                 const char *closureClass = nullptr,
                 const Json *closureReason = nullptr) noexcept;
    void Envelope(const Json &envelope);
    void Packet(const Json &packet);
    void Connect(Clock::time_point now);
    void Reset(bool disconnect);
    void CancelReads();
    void Call(const std::string &method, Json request, Clock::time_point now,
              Clock::time_point deadline);
    void Read(Clock::time_point now);
    void Resync(const std::string &reason, Clock::time_point now);
    void Handle(const Json &packet, Clock::time_point now);
    void AcceptState(const Json &state, bool snapshot);
    void ValidateValue(const std::string &topic, const Json &state, const Json &value) const;
    void ContinueInventory(Clock::time_point now);
    void InventoryPage(const Json &page, Clock::time_point now);
    bool BusyCommand() const;
};
} // namespace activity
