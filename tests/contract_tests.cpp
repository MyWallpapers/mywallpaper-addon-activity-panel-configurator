#include "model.hpp"
#include "wire.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace activity;
void Require(bool c, const std::string &msg) {
    if (!c)
        throw std::runtime_error(msg);
}
template <class F> void Reject(F f, const std::string &msg) {
    bool failed = false;
    try {
        f();
    } catch (const std::exception &) {
        failed = true;
    }
    Require(failed, msg);
}
void Header(std::ostream &out, uint32_t value) {
    for (int i = 0; i < 4; i++)
        out.put(static_cast<char>(value >> (i * 8)));
}
int main() {
    try {
        auto manifest = Manifest();
        auto defaults = ParseConfig(Json::object());
        Require(manifest.at("settings").size() <= 100, "Manifest cap");
        for (const auto &setting : manifest.at("settings")) {
            auto key = setting.at("id").get<std::string>(),
                 type = setting.at("type").get<std::string>();
            if (type == "section")
                continue;
            Require(defaults.values.at(key) == setting.at("default"), "Default " + key);
            if (type == "select")
                for (const auto &o : setting.at("options"))
                    Require(ParseConfig({{key, o.at("value")}}).values.at(key) == o.at("value"),
                            "Option " + key);
            if (type == "range") {
                for (const auto &endpoint : {"min", "max"})
                    Require(ParseConfig({{key, setting.at(endpoint)}}).values.at(key) ==
                                setting.at(endpoint),
                            "Endpoint " + key);
                Reject([&] { ParseConfig({{key, setting.at("min").get<int>() - 1}}); },
                       "Lower bound " + key);
                Reject([&] { ParseConfig({{key, 1.5}}); }, "Fraction " + key);
            }
            if (type == "boolean")
                Require(ParseConfig({{key, !setting.at("default").get<bool>()}}).values.at(key) !=
                            setting.at("default"),
                        "Toggle " + key);
            Reject([&] { ParseConfig({{key, nullptr}}); }, "Wrong type " + key);
        }
        auto original = defaults.values;
        original["fill"] = "solid";
        original["colorEnd"] = "#ABCDEF25";
        original["gradientAngle"] = 272;
        auto hidden = ParseConfig(original).values;
        hidden["fill"] = "gradient";
        Require(ParseConfig(hidden).text("colorEnd") == "#ABCDEF25" &&
                    ParseConfig(hidden).number("gradientAngle") == 272,
                "Hidden values retained");
        auto legacy = original;
        legacy["profile"] = "rose";
        Require(ParseConfig(legacy).text("colorStart") == defaults.text("colorStart"),
                "Unknown theme cannot override direct values");
        Reject([&] { ParseConfig({{"textColor", "#abc"}}); }, "Invalid color");
        Rect work{-1920, 100, 1920, 1040};
        auto r = Place(work, 420, 400, 20, "bottom-right");
        Require(r.x == -440 && r.y == 720, "Negative monitor placement");
        r = Place({0, 0, 160, 100}, 800, 800, 160, "top-center");
        Require(r.width == 160 && r.height == 100 && r.x == 0 && r.y == 0, "Small monitor clamp");
        Require(Motion(0, 180, false) == 0 && Motion(180, 180, false) == 1 &&
                    Motion(0, 180, true) == 1 && Motion(0, 0, false) == 1,
                "Reduced motion");
        auto now = Timer::Clock::time_point{};
        Timer timer;
        timer.Reset(1, now);
        timer.Toggle(now);
        Require(timer.Seconds(now + std::chrono::milliseconds(500)) == 60, "Ceiling timer");
        timer.Toggle(now + std::chrono::seconds(10));
        Require(!timer.Running() && timer.Seconds(now) == 50, "Pause preserves remaining");
        timer.Toggle(now + std::chrono::seconds(100));
        Require(!timer.Tick(now + std::chrono::seconds(149)), "Not finished early");
        Require(timer.Tick(now + std::chrono::seconds(150)) &&
                    !timer.Tick(now + std::chrono::seconds(151)),
                "Exactly one completion");
        Require(timer.Finished() && timer.Seconds(now) == 0, "Completed state");
        timer.Reset(25, now);
        Require(!timer.Finished() && !timer.Running(), "Reset");
        Snapshot snapshot;
        snapshot.battery = Availability::absent;
        Require(Content("battery", defaults, snapshot, timer, now).detail == "Absent",
                "No fabricated battery");
        snapshot.battery = Availability::denied;
        Require(Content("battery", defaults, snapshot, timer, now).detail == "Accès refusé",
                "Denied state");
        Require(Content("media", defaults, snapshot, timer, now).detail.find("non connecté") !=
                    std::string::npos,
                "Media unavailable explicit");
        snapshot.battery = Availability::available;
        snapshot.batteryPercent = 10;
        snapshot.ac = false;
        timer.Toggle(now);
        Require(CompactSource(defaults, snapshot, timer) == "battery", "Battery priority");
        snapshot.ac = true;
        Require(CompactSource(defaults, snapshot, timer) == "timer", "Timer priority");
        timer.Reset(1, now);
        snapshot.ac.reset();
        Require(Content("battery", defaults, snapshot, timer, now).detail.find("inconnue") !=
                    std::string::npos,
                "Unknown AC status remains explicit");
        Require(CompactSource(defaults, snapshot, timer) == "clock", "Idle priority");
        auto all = defaults.values;
        all["showMedia"] = true;
        Require(Rows(ParseConfig(all), snapshot, timer, now).size() == 5, "Five sources");
        Require(!ResolveVisibility(defaults, true, true).visible, "Fullscreen hide");
        all["fullscreen"] = "compact";
        Require(ResolveVisibility(ParseConfig(all), true, true).visible &&
                    !ResolveVisibility(ParseConfig(all), true, true).expanded,
                "Fullscreen compact");
        all["enabled"] = false;
        Require(!ResolveVisibility(ParseConfig(all), false, false).visible, "Disabled");
        Json big = {{"v", 5}, {"type", "message"}};
        // Chunk boundaries split UTF-8 bytes; JSON is decoded after reassembly.
        big["payload"] = std::string(wire::maxChunk * 2 + 17, 'x') + "é 😀";
        std::stringstream wire;
        wire::Write(wire, big);
        Require(wire::Read(wire) == big, "Chunked roundtrip");
        Require(!wire::Read(wire), "Clean EOF");
        for (uint32_t header : {0u, wire::maxChunk + 1, 0x80000001u, 0xc0000001u}) {
            std::stringstream bad;
            Header(bad, header);
            bad << 'x';
            Reject([&] { wire::Read(bad); }, "Bad chunk header");
        }
        {
            std::stringstream bad;
            Header(bad, 0x40000001);
            bad << 'x';
            Header(bad, 1);
            bad << 'x';
            Reject([&] { wire::Read(bad); }, "Nested single");
        }
        {
            std::stringstream bad;
            Header(bad, 0x40000001);
            bad << 'x';
            Reject([&] { wire::Read(bad); }, "Partial sequence");
        }
        {
            std::stringstream bad;
            bad << 'x';
            Reject([&] { wire::Read(bad); }, "Truncated header");
        }
        {
            std::stringstream bad;
            Header(bad, 20);
            bad << "{}";
            Reject([&] { wire::Read(bad); }, "Truncated body");
        }
        std::cout << "Settings, composition, content availability, timer, geometry, policy and "
                     "process-v2 contracts passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
