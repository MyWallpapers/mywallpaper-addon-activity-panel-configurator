#include "audio_examples.hpp"
#include "wire.hpp"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <windows.h>
using namespace activity;
namespace {
void Require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
class Child {
    HANDLE write_ = nullptr, read_ = nullptr, process_ = nullptr, job_ = nullptr;

  public:
    explicit Child(const std::wstring &exe) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE input = nullptr, output = nullptr;
        Require(CreatePipe(&input, &write_, &sa, 0) != FALSE, "Input pipe");
        Require(CreatePipe(&read_, &output, &sa, 0) != FALSE, "Output pipe");
        SetHandleInformation(write_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);
        auto null = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa,
                                OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = input;
        si.hStdOutput = output;
        si.hStdError = null;
        PROCESS_INFORMATION pi{};
        std::wstring command = L"\"" + exe + L"\" --fixture-protocol";
        BOOL started =
            CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        CloseHandle(input);
        CloseHandle(output);
        CloseHandle(null);
        Require(started != FALSE, "Fixture process");
        process_ = pi.hProcess;
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_ ||
            !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info,
                                     sizeof(info)) ||
            !AssignProcessToJobObject(job_, process_)) {
            TerminateProcess(process_, 1);
            CloseHandle(pi.hThread);
            throw std::runtime_error("Fixture job");
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
    }
    ~Child() {
        if (job_)
            CloseHandle(job_);
        if (process_) {
            WaitForSingleObject(process_, 2000);
            CloseHandle(process_);
        }
        if (write_)
            CloseHandle(write_);
        if (read_)
            CloseHandle(read_);
    }
    DWORD Pid() const { return GetProcessId(process_); }
    void Send(const Json &frame) {
        std::ostringstream out;
        wire::Write(out, frame);
        Bytes(out.str());
    }
    void Bytes(const std::string &bytes) {
        size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            Require(WriteFile(write_, bytes.data() + offset,
                              static_cast<DWORD>(bytes.size() - offset), &written, nullptr) &&
                        written,
                    "Pipe write");
            offset += written;
        }
    }
    Json Read() {
        auto start = GetTickCount64();
        std::string bytes;
        uint32_t expected = 0;
        for (;;) {
            DWORD available = 0;
            Require(PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr) != FALSE,
                    "Pipe unexpectedly closed");
            if (available) {
                char block[4096];
                DWORD read = 0;
                DWORD wanted = std::min<DWORD>(
                    available, expected ? expected - static_cast<uint32_t>(bytes.size())
                                        : 4 - static_cast<uint32_t>(bytes.size()));
                wanted = std::min<DWORD>(wanted, sizeof(block));
                Require(ReadFile(read_, block, wanted, &read, nullptr) && read, "Pipe read");
                bytes.append(block, read);
                if (!expected && bytes.size() == 4) {
                    auto p = reinterpret_cast<const unsigned char *>(bytes.data());
                    expected = 4 + (uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                                    (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
                    Require(expected > 4 && expected < 1024 * 1024, "Expected small single output");
                }
                if (expected && bytes.size() == expected) {
                    std::istringstream in(bytes);
                    return *wire::Read(in);
                }
            }
            Require(GetTickCount64() - start < 5000, "Bounded response timeout");
            Sleep(2);
        }
    }
    void Exit(DWORD expected) {
        Require(WaitForSingleObject(process_, 3000) == WAIT_OBJECT_0, "Bounded fixture exit");
        DWORD code = 99;
        Require(GetExitCodeProcess(process_, &code) && code == expected, "Fixture exit status");
    }
    void CloseInput() {
        CloseHandle(write_);
        write_ = nullptr;
    }
};
Json Init(const std::filesystem::path &scratch) {
    return {{"type", "init"},
            {"v", 5},
            {"addonId", "A24-owned-fixture"},
            {"artifactDigest", "sha256:" + std::string(64, 'a')},
            {"version", "0.1.0"},
            {"targetArchitecture", "windows-x86_64"},
            {"scratchDirectory", scratch.string()},
            {"deviceSettings", Json::object()},
            {"layerSettings", Json::object()}};
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        Require(argc == 3, "Expected companion and isolated output directory");
        std::filesystem::path output = argv[2];
        std::filesystem::create_directories(output);
        {
            Child child(argv[1]);
            child.Send(Init(output));
            Require(child.Read().at("type") == "ready", "Exact v5 ready");
            auto status = child.Read().at("payload");
            Require(status.at("fixture") == true && status.at("visible") == false,
                    "Hidden provider fixture identity");
            Require(status.at("pid").get<DWORD>() == child.Pid(), "Exact process identity");
            HWND window = reinterpret_cast<HWND>(std::stoull(status.at("hwnd").get<std::string>()));
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            Require(pid == child.Pid() && !IsWindowVisible(window), "Owned hidden HWND");
            child.Send({{"type", "settings"},
                        {"v", 5},
                        {"layerSettings", Json::object()},
                        {"deviceSettings", {{"compactWidth", -1}}}});
            Require(child.Read().at("payload").at("kind") == "activity-panel.settings-rejected",
                    "Invalid settings observable");
            child.Read();
            child.Send(
                {{"type", "message"},
                 {"v", 5},
                 {"surface", "wallpaper"},
                 {"instanceId", "fixture"},
                 {"displayIndex", 0},
                 {"payload",
                  {{"kind", "unsupported"}, {"bytes", std::string(wire::maxChunk * 2 + 1, 'x')}}}});
            Require(child.Read().at("payload").at("kind") == "activity-panel.unsupported-message",
                    "Reassembled large record handled");
            // Leave an incomplete header, then close only this owned window. No input
            // simulation, desktop switching or active MyWallpaper surface is used.
            child.Bytes(std::string(1, 'x'));
            Require(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE, "Owned close message");
            child.Exit(0);
        }
        {
            Child child(argv[1]);
            child.Send(Init(output));
            child.Read();
            child.Read();
            child.Send({{"type", "shutdown"}, {"v", 5}});
            child.Exit(0);
        }
        {
            Child child(argv[1]);
            child.Send({{"type", "init"}, {"v", 4}});
            Require(child.Read().at("type") == "error", "Legacy version rejected");
            child.Exit(1);
        }
        {
            Child child(argv[1]);
            child.CloseInput();
            child.Exit(0);
        }
        {
            Child child(argv[1]);
            auto init = Init(output);
            init["deviceSettings"] = {
                {"showMedia", true}, {"mode", "expanded"}, {"reduceMotion", true}};
            child.Send(init);
            child.Read();
            child.Read();
            auto service = [&](Json envelope) {
                envelope["type"] = "mywallpaper.services";
                envelope["v"] = 1;
                envelope["generation"] = "fixture-host-epoch";
                child.Send({{"type", "message"},
                            {"v", 5},
                            {"surface", "service"},
                            {"instanceId", "fixture-host-epoch"},
                            {"displayIndex", 0},
                            {"payload", envelope}});
            };
            int delivery = 0;
            auto packet = [&](Json value) {
                service({{"kind", "packet"},
                         {"deliveryId", "fixture-delivery:" + std::to_string(++delivery)},
                         {"packet", value}});
                Require(child.Read().at("payload").at("kind") == "ack",
                        "Native host delivery acknowledged");
            };
            service({{"kind", "open"}});
            auto connect = child.Read().at("payload").at("packet");
            Require(connect.at("type") == "connect" && connect.at("alias") == "audio",
                    "Broker connect over real v5 pipes");
            packet({{"type", "reply"},
                    {"id", connect.at("id")},
                    {"ok", true},
                    {"value", "fixture-opaque-connection"}});
            auto call = child.Read().at("payload").at("packet");
            Require(call.at("method") == "snapshot", "Snapshot before retained subscriptions");
            auto examples = Json::parse(audioExamples);
            auto snapshot = examples.at("snapshotResult");
            snapshot["requestId"] = call.at("payload").at("requestId");
            packet({{"type", "reply"}, {"id", call.at("id")}, {"ok", true}, {"value", snapshot}});
            std::vector<Json> subscriptions;
            for (int i = 0; i < 2; ++i)
                subscriptions.push_back(child.Read().at("payload").at("packet"));
            for (const auto &sub : subscriptions) {
                Require(sub.at("type") == "subscribe",
                        "Retained topic registration over real pipes");
                auto state = examples.at(sub.at("topic") == "audio" ? "audioState" : "mediaState");
                packet({{"type", "subscription"},
                        {"id", sub.at("id")},
                        {"event", {{"kind", "snapshot"}, {"sequence", 1}, {"value", state}}}});
                Require(child.Read().at("payload").at("packet").at("type") == "subscription.ack",
                        "Independent subscription credit");
            }
            auto inspect = [&]() {
                child.Send({{"type", "message"},
                            {"v", 5},
                            {"payload", {{"kind", "activity-panel.inspect"}}}});
                return child.Read().at("payload");
            };
            auto status = inspect();
            Require(status.at("media").at("topics").at("media").at("current") == true &&
                        status.at("visible") == false,
                    "Panel consumed snapshot without activation");
            packet({{"type", "connection.closed"},
                    {"id", "fixture-opaque-connection"},
                    {"reason", "revoked"}});
            // On close A24 releases both subscriptions; it does not reconnect or replay.
            Require(child.Read().at("payload").at("packet").at("type") == "unsubscribe",
                    "Audio subscription released");
            Require(child.Read().at("payload").at("packet").at("type") == "unsubscribe",
                    "Media subscription released");
            status = inspect();
            Require(status.at("media").at("connected") == false,
                    "Revocation propagated to actual panel");
            child.Send({{"type", "shutdown"}, {"v", 5}});
            child.Exit(0);
        }
        std::cout << "Private Windows pipes, exact v5 handshake, chunking, rejection, partial-read "
                     "stop, shutdown and EOF passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
