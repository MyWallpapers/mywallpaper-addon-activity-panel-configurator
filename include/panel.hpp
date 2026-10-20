#pragma once
#include "accessibility.hpp"
#include "audio_service.hpp"
#include "model.hpp"
#include <filesystem>
#include <gdiplus.h>
#include <memory>
#include <mutex>
#include <objidl.h>
#include <windows.h>
namespace activity {
std::wstring Wide(const std::string &text);
class Panel {
  public:
    explicit Panel(bool fixture = false);
    ~Panel();
    int Run();
    int SelfTest(const std::filesystem::path &output);
    int WindowPolicyTest();

  private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static DWORD WINAPI ReadThread(void *);
    LRESULT Message(UINT, WPARAM, LPARAM);
    void Frame(const Json &frame);
    void Apply(const Config &config);
    void Sample();
    void Render();
    void Update(bool reposition = true);
    void Action(int action);
    void SyncService();
    int ServiceHeight() const;
    void Emit(const Json &payload);
    bool WantsExpanded() const {
        return expanded_ || (hover_ && !suppressHover_ && config_.text("mode") == "automatic");
    }
    Json Status() const;
    HMONITOR Monitor();
    bool Fullscreen(HMONITOR monitor) const;
    std::unique_ptr<Gdiplus::Bitmap> Paint(int width, int height, double scale);
    bool fixture_, protocol_, ready_ = false, expanded_ = false, hover_ = false, full_ = false,
                              reduced_ = false, contrast_ = false;
    bool stopping_ = false;
    bool suppressHover_ = false;
    HWND window_ = nullptr;
    HANDLE reader_ = nullptr, consumed_ = nullptr, inputStop_ = nullptr;
    std::mutex inputMutex_;
    std::optional<Json> pending_;
    std::string inputError_, notice_;
    Config config_;
    Snapshot snapshot_;
    AudioService audio_;
    struct ButtonAction {
        int local = -1;
        std::optional<ServiceControl> service;
        bool operator==(const ButtonAction &) const = default;
    };
    std::vector<ButtonAction> buttonActions_, accessibleActions_;
    std::optional<ButtonAction> pressedAction_;
    Timer timer_;
    std::chrono::steady_clock::time_point animationStart_{};
    Rect startRect_, target_, current_;
    double scale_ = 1;
    int focus_ = 0, hit_ = -1, pressed_ = -1;
    int scrollOffset_ = 0, scrollMax_ = 0;
    bool keyboardFocus_ = false;
    std::vector<Rect> buttons_;
    std::vector<std::string> buttonLabels_;
    std::shared_ptr<AccessibleState> accessible_ = std::make_shared<AccessibleState>();
    HMONITOR pointerMonitor_ = nullptr;
    uint64_t previousIdle_ = 0, previousKernel_ = 0, previousUser_ = 0;
};
} // namespace activity
