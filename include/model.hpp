#pragma once
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
namespace activity {
using Json = nlohmann::json;
struct Config {
    Json values;
    bool flag(const char *key) const { return values.at(key).get<bool>(); }
    int number(const char *key) const { return values.at(key).get<int>(); }
    std::string text(const char *key) const { return values.at(key).get<std::string>(); }
};
Config ParseConfig(const Json &input);
Json Manifest();
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
};
Rect Place(Rect work, int width, int height, int margin, const std::string &anchor);
double Motion(double elapsedMs, int durationMs, bool reduced);
enum class Availability { available, absent, denied, unavailable };
std::string AvailabilityText(Availability state);
struct Snapshot {
    std::string clock;
    std::optional<int> batteryPercent;
    bool batteryPresent = false;
    std::optional<bool> ac;
    Availability battery = Availability::unavailable;
    std::optional<int> memoryPercent, cpuPercent;
    std::string mediaDetail = "Indisponible · Fournisseur A18 non connecté";
};
class Timer {
  public:
    using Clock = std::chrono::steady_clock;
    void Reset(int minutes, Clock::time_point now);
    void Toggle(Clock::time_point now);
    bool Tick(Clock::time_point now);
    int Seconds(Clock::time_point now) const;
    bool Running() const { return running_; }
    bool Finished() const { return finished_; }

  private:
    int remaining_ = 25 * 60;
    Clock::time_point deadline_{};
    bool running_ = false, finished_ = false;
};
struct Row {
    std::string key, title, detail;
    double progress = -1;
};
Row Content(const std::string &source, const Config &config, const Snapshot &snapshot,
            const Timer &timer, Timer::Clock::time_point now);
std::string CompactSource(const Config &config, const Snapshot &snapshot, const Timer &timer);
std::vector<Row> Rows(const Config &config, const Snapshot &snapshot, const Timer &timer,
                      Timer::Clock::time_point now);
struct Visibility {
    bool visible, expanded;
};
Visibility ResolveVisibility(const Config &c, bool expanded, bool fullscreen);
} // namespace activity
