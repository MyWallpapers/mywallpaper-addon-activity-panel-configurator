#include "manifest_data.hpp"
#include "model.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace activity {
Json Manifest() { return Json::parse(activity_manifest); }
Config ParseConfig(const Json &input) {
    if (!input.is_object())
        throw std::invalid_argument("Les réglages doivent être un objet");
    Config c{Json::object()};
    const auto manifest = Manifest();
    for (const auto &s : manifest.at("settings")) {
        if (s.at("type") == "section")
            continue;
        const auto key = s.at("id").get<std::string>();
        const auto v = input.contains(key) ? input.at(key) : s.at("default");
        const auto type = s.at("type").get<std::string>();
        bool valid = false;
        if (type == "boolean")
            valid = v.is_boolean();
        else if (type == "range")
            valid = v.is_number_integer() && v >= s.at("min") && v <= s.at("max");
        else if (type == "select") {
            for (const auto &option : s.at("options"))
                if (v == option.at("value"))
                    valid = true;
        } else if (type == "color" && v.is_string()) {
            auto color = v.get<std::string>();
            valid = (color.size() == 7 || color.size() == 9) && color[0] == '#' &&
                    std::all_of(color.begin() + 1, color.end(),
                                [](unsigned char ch) { return std::isxdigit(ch) != 0; });
        }
        if (!valid)
            throw std::invalid_argument("Réglage invalide : " + key);
        c.values[key] = v;
    }
    return c;
}
Rect Place(Rect work, int width, int height, int margin, const std::string &anchor) {
    width = std::clamp(width, 1, std::max(1, work.width));
    height = std::clamp(height, 1, std::max(1, work.height));
    int mx = std::min(margin, std::max(0, (work.width - width) / 2)),
        my = std::min(margin, std::max(0, (work.height - height) / 2));
    int x = work.x + (work.width - width) / 2;
    if (anchor.ends_with("left"))
        x = work.x + mx;
    if (anchor.ends_with("right"))
        x = work.x + work.width - width - mx;
    int y = anchor.starts_with("bottom") ? work.y + work.height - height - my : work.y + my;
    return {x, y, width, height};
}
double Motion(double elapsedMs, int durationMs, bool reduced) {
    if (reduced || durationMs <= 0)
        return 1;
    auto t = std::clamp(elapsedMs / durationMs, 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}
std::string AvailabilityText(Availability s) {
    switch (s) {
    case Availability::available:
        return "Disponible";
    case Availability::absent:
        return "Absent";
    case Availability::denied:
        return "Accès refusé";
    default:
        return "Indisponible";
    }
}
void Timer::Reset(int minutes, Clock::time_point) {
    remaining_ = std::clamp(minutes, 1, 180) * 60;
    running_ = finished_ = false;
}
void Timer::Toggle(Clock::time_point now) {
    Tick(now);
    if (running_) {
        remaining_ = Seconds(now);
        running_ = false;
    } else if (!finished_) {
        deadline_ = now + std::chrono::seconds(remaining_);
        running_ = true;
    }
}
bool Timer::Tick(Clock::time_point now) {
    if (running_ && now >= deadline_) {
        remaining_ = 0;
        running_ = false;
        finished_ = true;
        return true;
    }
    return false;
}
int Timer::Seconds(Clock::time_point now) const {
    if (!running_)
        return remaining_;
    return std::max(
        0, static_cast<int>(std::ceil(std::chrono::duration<double>(deadline_ - now).count())));
}
Row Content(const std::string &source, const Config &c, const Snapshot &s, const Timer &timer,
            Timer::Clock::time_point now) {
    if (source == "clock")
        return {source, "Horloge", s.clock};
    if (source == "battery") {
        if (s.battery != Availability::available)
            return {source, "Batterie", AvailabilityText(s.battery)};
        return {source, "Batterie",
                s.batteryPercent
                    ? std::to_string(*s.batteryPercent) + " %" +
                          (s.ac.has_value() ? (*s.ac ? " · Sur secteur" : " · Sur batterie")
                                            : " · Alimentation inconnue")
                    : "Charge inconnue",
                s.batteryPercent ? *s.batteryPercent / 100.0 : -1};
    }
    if (source == "timer") {
        int seconds = timer.Seconds(now);
        std::ostringstream text;
        text << seconds / 60 << ':' << std::setw(2) << std::setfill('0') << seconds % 60;
        text << (timer.Finished() ? " · Terminé" : timer.Running() ? " · En cours" : " · En pause");
        return {source, "Minuteur", text.str(),
                std::clamp(1.0 - seconds / (c.number("timerMinutes") * 60.0), 0.0, 1.0)};
    }
    if (source == "system")
        return {source, "Système",
                s.memoryPercent
                    ? "Mémoire " + std::to_string(*s.memoryPercent) + " % · CPU " +
                          (s.cpuPercent ? std::to_string(*s.cpuPercent) + " %" : "Mesure en cours")
                    : "Mesure indisponible",
                s.memoryPercent ? *s.memoryPercent / 100.0 : -1};
    return {"media", "Média", s.mediaDetail};
}
std::string CompactSource(const Config &c, const Snapshot &s, const Timer &t) {
    const auto selected = c.text("compactSource");
    if (selected != "auto")
        return selected;
    if (c.flag("showBattery") && s.battery == Availability::available && s.batteryPercent &&
        s.ac == false && *s.batteryPercent <= c.number("lowBattery"))
        return "battery";
    if (c.flag("showTimer") && (t.Running() || t.Finished()))
        return "timer";
    return c.text("idleContent");
}
std::vector<Row> Rows(const Config &c, const Snapshot &s, const Timer &t,
                      Timer::Clock::time_point now) {
    std::vector<Row> rows;
    for (const auto &pair :
         std::vector<std::pair<const char *, const char *>>{{"showClock", "clock"},
                                                            {"showBattery", "battery"},
                                                            {"showTimer", "timer"},
                                                            {"showSystem", "system"},
                                                            {"showMedia", "media"}})
        if (c.flag(pair.first))
            rows.push_back(Content(pair.second, c, s, t, now));
    return rows;
}
Visibility ResolveVisibility(const Config &c, bool expanded, bool fullscreen) {
    return {c.flag("enabled") && !(fullscreen && c.text("fullscreen") == "hide"),
            expanded && !(fullscreen && c.text("fullscreen") == "compact")};
}
} // namespace activity
