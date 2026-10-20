#include "audio_examples.hpp"
#include "panel.hpp"
#include "wire.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <oleacc.h>
#include <shellscalingapi.h>
#include <stdexcept>
#include <windowsx.h>
namespace activity {
namespace {
constexpr UINT inputMessage = WM_APP + 1;
// Read only bytes already available in the inherited pipe. The stop event
// interrupts idle/partial records without racing a synchronous read cancellation.
class PipeBuffer final : public std::streambuf {
    HANDLE pipe_, stop_;
    char buffer_[4096];

  public:
    PipeBuffer(HANDLE pipe, HANDLE stop) : pipe_(pipe), stop_(stop) {}

  protected:
    int_type underflow() override {
        if (gptr() != egptr())
            return traits_type::to_int_type(*gptr());
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr))
                return traits_type::eof();
            if (!available) {
                WaitForSingleObject(stop_, 20);
                continue;
            }
            DWORD read = 0;
            if (!ReadFile(pipe_, buffer_, std::min<DWORD>(available, sizeof(buffer_)), &read,
                          nullptr) ||
                !read)
                return traits_type::eof();
            setg(buffer_, buffer_, buffer_ + read);
            return traits_type::to_int_type(*gptr());
        }
        return traits_type::eof();
    }
};
uint64_t Number(FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
Gdiplus::Color ParseColor(const std::string &text, double opacity = 1) {
    auto rgb = std::stoul(text.substr(1, 6), nullptr, 16);
    auto alpha = text.size() == 9 ? std::stoul(text.substr(7, 2), nullptr, 16) : 255;
    return Gdiplus::Color(static_cast<BYTE>(alpha * opacity), static_cast<BYTE>(rgb >> 16),
                          static_cast<BYTE>(rgb >> 8), static_cast<BYTE>(rgb));
}
Gdiplus::Color SystemColor(int id) {
    auto c = GetSysColor(id);
    return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
}
void Round(Gdiplus::GraphicsPath &path, float x, float y, float w, float h, float radius) {
    radius = std::clamp(radius, 0.0f, std::min(w, h) / 2);
    float d = radius * 2;
    if (d < 1) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}
void Text(Gdiplus::Graphics &g, const std::string &text, float x, float y, float width,
          float height, float size, Gdiplus::Color color, bool bold = false) {
    Gdiplus::Font font(L"Segoe UI", size, bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::SolidBrush brush(color);
    Gdiplus::StringFormat format;
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    auto w = Wide(text);
    Gdiplus::RectF box(x, y, width, height);
    g.DrawString(w.c_str(), static_cast<int>(w.size()), &font, box, &format, &brush);
}
bool Contains(Rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}
CLSID PngEncoder() {
    UINT count = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    std::vector<BYTE> bytes(size);
    auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo *>(bytes.data());
    Gdiplus::GetImageEncoders(count, size, codecs);
    for (UINT i = 0; i < count; i++)
        if (std::wcscmp(codecs[i].MimeType, L"image/png") == 0)
            return codecs[i].Clsid;
    throw std::runtime_error("Encodeur PNG absent");
}
} // namespace
std::wstring Wide(const std::string &s) {
    if (s.empty())
        return {};
    auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                     static_cast<int>(s.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error("Texte UTF-8 invalide");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        result.data(), count);
    return result;
}
Panel::Panel(bool fixture)
    : fixture_(fixture), protocol_(!fixture), config_(ParseConfig(Json::object())),
      audio_([this](const Json &envelope) { Emit(envelope); }) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = L"MyWallpaper.ActivityPanel.A24";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Classe de fenêtre indisponible");
    consumed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    inputStop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!consumed_ || !inputStop_) {
        if (consumed_)
            CloseHandle(consumed_);
        if (inputStop_)
            CloseHandle(inputStop_);
        throw std::runtime_error("Événement de lecture indisponible");
    }
    window_ =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Panneau d’activité",
                        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
    if (!window_) {
        CloseHandle(consumed_);
        CloseHandle(inputStop_);
        consumed_ = inputStop_ = nullptr;
        throw std::runtime_error("Fenêtre compagnon indisponible");
    }
    if (!fixture_) {
        POINT p{};
        GetCursorPos(&p);
        pointerMonitor_ = MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
    }
    timer_.Reset(config_.number("timerMinutes"), Timer::Clock::now());
}
Panel::~Panel() {
    {
        std::lock_guard lock(inputMutex_);
        stopping_ = true;
    }
    if (reader_) {
        SetEvent(inputStop_);
        SetEvent(consumed_);
        WaitForSingleObject(reader_, INFINITE);
        CloseHandle(reader_);
    }
    if (accessible_) {
        std::lock_guard lock(accessible_->mutex);
        accessible_->window = nullptr;
    }
    if (window_) {
        KillTimer(window_, 1);
        KillTimer(window_, 2);
        KillTimer(window_, 3);
        DestroyWindow(window_);
    }
    if (consumed_)
        CloseHandle(consumed_);
    if (inputStop_)
        CloseHandle(inputStop_);
}
DWORD WINAPI Panel::ReadThread(void *value) {
    auto self = static_cast<Panel *>(value);
    PipeBuffer buffer(GetStdHandle(STD_INPUT_HANDLE), self->inputStop_);
    std::istream input(&buffer);
    for (;;) {
        std::optional<Json> frame;
        std::string error;
        try {
            frame = wire::Read(input);
        } catch (const std::exception &e) {
            error = e.what();
        }
        {
            std::lock_guard lock(self->inputMutex_);
            if (self->stopping_)
                break;
            self->pending_ = std::move(frame);
            self->inputError_ = error;
        }
        if (!PostMessageW(self->window_, inputMessage, 0, 0))
            break;
        if (WaitForSingleObject(self->consumed_, INFINITE) != WAIT_OBJECT_0)
            break;
        {
            std::lock_guard lock(self->inputMutex_);
            if (self->stopping_)
                break;
        }
    }
    return 0;
}
int Panel::Run() {
    protocol_ = true;
    reader_ = CreateThread(nullptr, 0, ReadThread, this, 0, nullptr);
    if (!reader_)
        throw std::runtime_error("Lecture du host indisponible");
    MSG msg{};
    int state;
    while ((state = static_cast<int>(GetMessageW(&msg, nullptr, 0, 0))) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return state < 0 ? 1 : static_cast<int>(msg.wParam);
}
void Panel::Emit(const Json &payload) {
    if (protocol_)
        wire::Write(std::cout,
                    {{"type", "message"}, {"v", 5}, {"target", "broadcast"}, {"payload", payload}});
}
Json Panel::Status() const {
    return {{"kind", "activity-panel.status"},
            {"version", 1},
            {"pid", GetCurrentProcessId()},
            {"hwnd", std::to_string(reinterpret_cast<uintptr_t>(window_))},
            {"fixture", fixture_},
            {"visible", !fixture_ && IsWindowVisible(window_) != FALSE},
            {"expanded", expanded_},
            {"media", audio_.Status()},
            {"battery", AvailabilityText(snapshot_.battery)},
            {"notice", notice_},
            {"globalHooks", false},
            {"globalHotkeys", false},
            {"screen",
             {{"x", current_.x},
              {"y", current_.y},
              {"width", current_.width},
              {"height", current_.height}}}};
}
void Panel::Frame(const Json &frame) {
    if (!frame.is_object() || frame.value("v", 0) != 5)
        throw std::runtime_error("Version process-v2 attendue : 5");
    auto type = frame.value("type", std::string{});
    if (!ready_) {
        if (type != "init")
            throw std::runtime_error("Initialisation attendue");
        auto digest = frame.at("artifactDigest").get<std::string>();
        if (frame.at("addonId").get<std::string>().empty() ||
            frame.at("version").get<std::string>().empty() ||
            frame.at("targetArchitecture") != "windows-x86_64" || digest.size() != 71 ||
            !digest.starts_with("sha256:") ||
            !std::all_of(digest.begin() + 7, digest.end(),
                         [](unsigned char c) { return std::isxdigit(c) != 0; }) ||
            !frame.at("layerSettings").is_object())
            throw std::runtime_error("Identité du host invalide");
        auto scratch = std::filesystem::path(Wide(frame.at("scratchDirectory").get<std::string>()));
        if (!scratch.is_absolute() || !std::filesystem::is_directory(scratch))
            throw std::runtime_error("Dossier temporaire du host invalide");
        auto parsed = ParseConfig(frame.at("deviceSettings"));
        ready_ = true;
        if (protocol_)
            wire::Write(std::cout, {{"type", "ready"}, {"v", 5}});
        Apply(parsed);
        if (!fixture_)
            SetTimer(window_, 1, 1000, nullptr);
        Emit(Status());
        return;
    }
    if (type == "shutdown") {
        audio_.Close();
        PostQuitMessage(0);
        return;
    }
    if (type == "settings") {
        if (!frame.at("layerSettings").is_object())
            throw std::runtime_error("Réglages de couche invalides");
        try {
            auto parsed = ParseConfig(frame.at("deviceSettings"));
            notice_.clear();
            Apply(parsed);
        } catch (const std::exception &e) {
            notice_ = e.what();
            Emit({{"kind", "activity-panel.settings-rejected"}, {"reason", notice_}});
        }
        Emit(Status());
        return;
    }
    if (type == "message") {
        if (audio_.Receive(frame, Timer::Clock::now())) {
            SyncService();
            Update();
            return;
        }
        if (frame.at("payload").value("kind", std::string{}) == "activity-panel.inspect")
            Emit(Status());
        else
            Emit({{"kind", "activity-panel.unsupported-message"},
                  {"reason", "Message non pris en charge par le panneau"}});
        return;
    }
    throw std::runtime_error("Type de trame inconnu");
}
void Panel::Apply(const Config &next) {
    bool first = !config_.values.is_object();
    bool modeChanged = first || config_.text("mode") != next.text("mode");
    int previousMinutes = config_.number("timerMinutes");
    config_ = next;
    auto style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    style &= ~(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW);
    SetWindowLongPtrW(window_, GWL_EXSTYLE,
                      style | (config_.flag("taskSwitcher") ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW));
    if (modeChanged || !ready_)
        expanded_ = config_.text("mode") == "expanded";
    // Init uses the persisted mode too, even when it equals the defaults.
    if (current_.width == 0)
        expanded_ = config_.text("mode") == "expanded";
    if (previousMinutes != config_.number("timerMinutes"))
        timer_.Reset(config_.number("timerMinutes"), Timer::Clock::now());
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    reduced_ = config_.flag("reduceMotion") || !animations;
    HIGHCONTRASTW hc{sizeof(hc)};
    contrast_ = config_.flag("respectContrast") &&
                SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
                (hc.dwFlags & HCF_HIGHCONTRASTON);
    audio_.Enable(config_.flag("enabled") &&
                      (config_.flag("showMedia") || config_.text("compactSource") == "media"),
                  Timer::Clock::now());
    if (config_.flag("enabled") &&
        (config_.flag("showMedia") || config_.text("compactSource") == "media"))
        SetTimer(window_, 3, 100, nullptr);
    else
        KillTimer(window_, 3);
    SyncService();
    Sample();
    Update();
}
void Panel::SyncService() { snapshot_.mediaDetail = audio_.Detail(); }
int Panel::ServiceHeight() const {
    if (!config_.flag("showMedia") && config_.text("compactSource") != "media")
        return 0;
    int count = config_.flag("quickActions") ? static_cast<int>(audio_.Controls().size()) + 1 : 0;
    return 48 + ((count + 2) / 3) * 42;
}
HMONITOR Panel::Monitor() {
    if (config_.text("monitor") == "pointer" && pointerMonitor_) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(pointerMonitor_, &mi))
            return pointerMonitor_;
    }
    if (config_.text("monitor") == "index") {
        std::vector<HMONITOR> monitors;
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR m, HDC, LPRECT, LPARAM data) -> BOOL {
                reinterpret_cast<std::vector<HMONITOR> *>(data)->push_back(m);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&monitors));
        int index = config_.number("monitorIndex") - 1;
        if (index < static_cast<int>(monitors.size()))
            return monitors[index];
        notice_ = "Écran absent ; repli sur le principal";
    }
    return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
}
bool Panel::Fullscreen(HMONITOR monitor) const {
    if (fixture_)
        return false;
    auto foreground = GetForegroundWindow();
    if (!foreground || foreground == window_ || !IsWindowVisible(foreground))
        return false;
    wchar_t cls[128]{};
    GetClassNameW(foreground, cls, 128);
    if (std::wcscmp(cls, L"Progman") == 0 || std::wcscmp(cls, L"WorkerW") == 0 ||
        std::wcscmp(cls, L"Shell_TrayWnd") == 0)
        return false;
    if (MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) != monitor)
        return false;
    MONITORINFO mi{sizeof(mi)};
    RECT r{};
    if (!GetMonitorInfoW(monitor, &mi) || !GetWindowRect(foreground, &r))
        return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}
void Panel::Sample() {
    if (fixture_)
        return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char clock[64]{};
    int hour = config_.text("clockFormat") == "12" ? ((time.wHour + 11) % 12 + 1) : time.wHour;
    if (config_.flag("clockSeconds"))
        std::snprintf(clock, sizeof(clock), "%02d:%02d:%02d", hour, time.wMinute, time.wSecond);
    else
        std::snprintf(clock, sizeof(clock), "%02d:%02d", hour, time.wMinute);
    snapshot_.clock = clock;
    if (config_.text("clockFormat") == "12")
        snapshot_.clock += time.wHour < 12 ? " AM" : " PM";
    SYSTEM_POWER_STATUS power{};
    snapshot_.batteryPercent.reset();
    snapshot_.battery = Availability::unavailable;
    if (GetSystemPowerStatus(&power)) {
        snapshot_.batteryPresent = power.BatteryFlag != 255 && !(power.BatteryFlag & 128);
        snapshot_.battery = power.BatteryFlag == 255   ? Availability::unavailable
                            : snapshot_.batteryPresent ? Availability::available
                                                       : Availability::absent;
        snapshot_.ac.reset();
        if (power.ACLineStatus <= 1)
            snapshot_.ac = power.ACLineStatus == 1;
        if (snapshot_.batteryPresent && power.BatteryLifePercent <= 100)
            snapshot_.batteryPercent = power.BatteryLifePercent;
    }
    MEMORYSTATUSEX memory{sizeof(memory)};
    snapshot_.memoryPercent.reset();
    if (GlobalMemoryStatusEx(&memory))
        snapshot_.memoryPercent = static_cast<int>(memory.dwMemoryLoad);
    FILETIME idle{}, kernel{}, user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        auto i = Number(idle), k = Number(kernel), u = Number(user);
        if (previousKernel_ && k >= previousKernel_ && u >= previousUser_ && i >= previousIdle_) {
            auto total = k - previousKernel_ + u - previousUser_, inactive = i - previousIdle_;
            if (total && inactive <= total)
                snapshot_.cpuPercent = static_cast<int>((total - inactive) * 100 / total);
        }
        previousIdle_ = i;
        previousKernel_ = k;
        previousUser_ = u;
    }
}
void Panel::Update(bool reposition) {
    if (!ready_)
        return;
    auto monitor = Monitor();
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(monitor, &mi))
        throw std::runtime_error("Géométrie écran indisponible");
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy)))
        dx = 96;
    scale_ = dx / 96.0;
    full_ = Fullscreen(monitor);
    auto visibility = ResolveVisibility(config_, WantsExpanded(), full_);
    // Limit the order change to this window, including when restoring a hidden
    // panel. A successful API return alone does not establish the topmost bit.
    const bool topmost = visibility.visible && config_.flag("topmost");
    if (!SetWindowPos(window_, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
        throw std::runtime_error("Ordre du panneau indisponible (Win32 " +
                                 std::to_string(GetLastError()) + ")");
    if (((GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != topmost)
        throw std::runtime_error("Windows n’a pas appliqué l’ordre demandé au panneau");
    if (!visibility.visible) {
        KillTimer(window_, 2);
        ShowWindow(window_, SW_HIDE);
        return;
    }
    int rows = static_cast<int>(Rows(config_, snapshot_, timer_, Timer::Clock::now()).size());
    int logicalHeight = visibility.expanded ? 64 + rows * 58 + ServiceHeight() +
                                                  (config_.flag("quickActions") ? 48 : 0) +
                                                  (config_.flag("showDiagnostics") ? 32 : 0)
                                            : config_.number("compactHeight");
    int pad = static_cast<int>(std::ceil((config_.number("shadow") + 2) * scale_));
    int w = static_cast<int>((visibility.expanded ? config_.number("expandedWidth")
                                                  : config_.number("compactWidth")) *
                             scale_) +
            pad * 2;
    int h = static_cast<int>(logicalHeight * scale_) + pad * 2;
    Rect next = Place({mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left,
                       mi.rcWork.bottom - mi.rcWork.top},
                      w, h, static_cast<int>(config_.number("edgeMargin") * scale_),
                      config_.text("anchor"));
    if (reposition && (target_.x != next.x || target_.y != next.y || target_.width != next.width ||
                       target_.height != next.height)) {
        startRect_ = current_.width ? current_ : next;
        target_ = next;
        animationStart_ = Timer::Clock::now();
        if (!reduced_ && config_.number("animationMs") > 0 && !fixture_)
            SetTimer(window_, 2, 16, nullptr);
        else
            current_ = target_;
    }
    if (current_.width == 0)
        current_ = next;
    Render();
    if (!fixture_)
        ShowWindow(window_, SW_SHOWNOACTIVATE);
}
std::unique_ptr<Gdiplus::Bitmap> Panel::Paint(int width, int height, double scale) {
    using namespace Gdiplus;
    auto bitmap = std::make_unique<Bitmap>(width, height, PixelFormat32bppPARGB);
    Graphics g(bitmap.get());
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.ScaleTransform(static_cast<float>(scale), static_cast<float>(scale));
    float pad = static_cast<float>(config_.number("shadow") + 2),
          w = static_cast<float>(width / scale), h = static_cast<float>(height / scale);
    float x = pad, y = pad, pw = std::max(1.0f, w - pad * 2), ph = std::max(1.0f, h - pad * 2),
          radius = static_cast<float>(config_.number("cornerRadius"));
    if (!contrast_)
        for (int i = config_.number("shadow"); i > 0; i -= 2) {
            GraphicsPath shadow;
            Round(shadow, x - i / 2.0f, y - i / 2.0f + 2, pw + i, ph + i, radius + i / 2.0f);
            SolidBrush b(Color(6, 0, 0, 0));
            g.FillPath(&b, &shadow);
        }
    GraphicsPath shape;
    Round(shape, x, y, pw, ph, radius);
    auto a = contrast_ ? SystemColor(COLOR_WINDOW)
                       : ParseColor(config_.text("colorStart"),
                                    config_.number("backgroundOpacity") / 100.0);
    auto b = contrast_ ? a
                       : ParseColor(config_.text("colorEnd"),
                                    config_.number("backgroundOpacity") / 100.0);
    if (config_.text("fill") == "gradient" && !contrast_) {
        LinearGradientBrush fill(RectF(x, y, pw, ph), a, b,
                                 static_cast<float>(config_.number("gradientAngle")));
        g.FillPath(&fill, &shape);
    } else {
        SolidBrush fill(a);
        g.FillPath(&fill, &shape);
    }
    g.SetClip(&shape);
    if (!contrast_) {
        LinearGradientBrush reflection(
            PointF(x, y), PointF(x, y + ph / 2),
            Color(static_cast<BYTE>(config_.number("reflection") * 255 / 100), 255, 255, 255),
            Color(0, 255, 255, 255));
        g.FillRectangle(&reflection, x, y, pw, ph / 2);
        if (config_.number("grain") > 0) {
            SolidBrush grain(
                Color(static_cast<BYTE>(config_.number("grain") * 255 / 100), 255, 255, 255));
            uint32_t seed = 17;
            for (int i = 0; i < static_cast<int>(pw * ph / 90); i++) {
                seed = seed * 1664525 + 1013904223;
                float gx = x + (seed % 10000) / 10000.0f * pw;
                seed = seed * 1664525 + 1013904223;
                float gy = y + (seed % 10000) / 10000.0f * ph;
                g.FillRectangle(&grain, gx, gy, 1.0f, 1.0f);
            }
        }
    }
    g.ResetClip();
    auto border =
        contrast_ ? SystemColor(COLOR_WINDOWTEXT) : ParseColor(config_.text("borderColor"));
    Pen pen(border, static_cast<float>(contrast_ ? 2 : config_.number("borderWidth")));
    if (contrast_ || config_.number("borderWidth"))
        g.DrawPath(&pen, &shape);
    auto text = contrast_ ? SystemColor(COLOR_WINDOWTEXT) : ParseColor(config_.text("textColor"));
    auto muted = contrast_ ? text : ParseColor(config_.text("mutedColor"));
    auto accent =
        contrast_ ? SystemColor(COLOR_HIGHLIGHT) : ParseColor(config_.text("accentColor"));
    const float fs = static_cast<float>(config_.number("fontSize"));
    auto visibility = ResolveVisibility(config_, WantsExpanded(), full_);
    buttons_.clear();
    buttonLabels_.clear();
    buttonActions_.clear();
    auto button = [&](int local, const std::string &label, float bx, float by, float bw, float bh,
                      std::optional<ServiceControl> service = std::nullopt) {
        int id = static_cast<int>(buttons_.size());
        buttonActions_.push_back({local, std::move(service)});
        buttons_.push_back({static_cast<int>(bx * scale), static_cast<int>(by * scale),
                            static_cast<int>(bw * scale), static_cast<int>(bh * scale)});
        buttonLabels_.push_back(label);
        GraphicsPath p;
        Round(p, bx, by, bw, bh, 10);
        SolidBrush bg(Color(static_cast<BYTE>(id == hit_ ? 65 : 26), accent.GetR(), accent.GetG(),
                            accent.GetB()));
        g.FillPath(&bg, &p);
        if (keyboardFocus_ && focus_ == id) {
            Pen outline(accent, 2);
            g.DrawPath(&outline, &p);
        }
        Text(g, label, bx + 10, by + 8, bw - 16, bh - 8, fs - 2, text);
    };
    if (!visibility.expanded) {
        auto row = Content(CompactSource(config_, snapshot_, timer_), config_, snapshot_, timer_,
                           Timer::Clock::now());
        Text(g, row.detail, x + 18, y + (ph - fs - 5) / 2, pw - 70, fs + 8, fs, text, true);
        button(0, "+", x + pw - 44, y + (ph - 32) / 2, 32, 32);
        if (row.progress >= 0) {
            Pen progress(accent, 2);
            g.DrawLine(&progress, x + 18, y + ph - 9,
                       x + 18 + static_cast<float>((pw - 78) * row.progress), y + ph - 9);
        }
    } else {
        Text(g, "Activité", x + 20, y + 18, pw - 90, 30, fs + 2, text, true);
        button(0, "−", x + pw - 44, y + 12, 32, 32);
        auto rows = Rows(config_, snapshot_, timer_, Timer::Clock::now());
        float footer =
            static_cast<float>(ServiceHeight() + (config_.flag("quickActions") ? 48 : 0) +
                               (config_.flag("showDiagnostics") ? 32 : 0));
        float viewport = std::max(0.0f, ph - 62 - footer);
        scrollMax_ = std::max(0, static_cast<int>(std::ceil(rows.size() * 58 - viewport)));
        scrollOffset_ = std::clamp(scrollOffset_, 0, scrollMax_);
        if (scrollMax_)
            Text(g, "↕", x + pw - 76, y + 20, 24, 24, fs - 2, muted);
        g.SetClip(RectF(x + 10, y + 62, pw - 20, viewport));
        float ry = y + 62 - scrollOffset_;
        for (auto &row : rows) {
            Text(g, row.title, x + 20, ry, pw - 40, 24, fs - 2, muted);
            Text(g, row.detail, x + 20, ry + 21, pw - 40, 28, fs, text, true);
            if (row.progress >= 0) {
                Pen progress(accent, 2);
                g.DrawLine(&progress, x + 20, ry + 51,
                           x + 20 + static_cast<float>((pw - 40) * row.progress), ry + 51);
            }
            ry += 58;
        }
        g.ResetClip();
        ry = y + ph - footer;
        if (config_.flag("quickActions")) {
            button(1,
                   timer_.Finished()  ? "Recommencer"
                   : timer_.Running() ? "Pause"
                                      : "Démarrer",
                   x + 18, ry, (pw - 44) / 2, 36);
            button(2, "Remise à zéro", x + 26 + (pw - 44) / 2, ry, (pw - 44) / 2, 36);
            ry += 48;
        }
        if (ServiceHeight()) {
            Text(g, audio_.Notice(), x + 20, ry, pw - 40, 42, fs - 3, muted);
            ry += 48;
            if (config_.flag("quickActions")) {
                auto controls = audio_.Controls();
                const float bw = (pw - 44) / 3;
                int i = 0;
                button(3, "Relire l’état", x + 18, ry, bw, 36);
                for (const auto &control : controls) {
                    ++i;
                    button(-1, control.label, x + 18 + (i % 3) * (bw + 4), ry + (i / 3) * 42, bw,
                           36, control);
                }
                ry += ((static_cast<int>(controls.size()) + 3) / 3) * 42;
            }
        }
        if (config_.flag("showDiagnostics"))
            Text(g, notice_.empty() ? "Sources locales · " + audio_.Notice() : notice_, x + 20, ry,
                 pw - 40, 24, fs - 3, muted);
    }
    return bitmap;
}
void Panel::Render() {
    if (current_.width <= 0 || current_.height <= 0)
        return;
    auto bitmap = Paint(current_.width, current_.height, scale_);
    focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(buttons_.size()) - 1));
    auto dc = GetDC(nullptr);
    auto memory = CreateCompatibleDC(dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = current_.width;
    info.bmiHeader.biHeight = -current_.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    auto dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !memory || !dib) {
        if (dib)
            DeleteObject(dib);
        if (memory)
            DeleteDC(memory);
        if (dc)
            ReleaseDC(nullptr, dc);
        throw std::runtime_error("Surface de rendu indisponible");
    }
    Gdiplus::BitmapData data{};
    Gdiplus::Rect bounds(0, 0, current_.width, current_.height);
    auto locked =
        bitmap->LockBits(&bounds, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data);
    if (locked != Gdiplus::Ok) {
        DeleteObject(dib);
        DeleteDC(memory);
        ReleaseDC(nullptr, dc);
        throw std::runtime_error("Pixels de rendu indisponibles");
    }
    for (int row = 0; row < current_.height; row++)
        std::memcpy(static_cast<BYTE *>(bits) + row * current_.width * 4,
                    static_cast<BYTE *>(data.Scan0) + row * data.Stride, current_.width * 4);
    bitmap->UnlockBits(&data);
    auto original = SelectObject(memory, dib);
    POINT position{current_.x, current_.y}, source{};
    SIZE size{current_.width, current_.height};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL ok =
        UpdateLayeredWindow(window_, dc, &position, &size, memory, &source, 0, &blend, ULW_ALPHA);
    SelectObject(memory, original);
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, dc);
    if (!ok)
        throw std::runtime_error("Présentation du panneau indisponible");
    auto row = Content(CompactSource(config_, snapshot_, timer_), config_, snapshot_, timer_,
                       Timer::Clock::now());
    const auto name = "Panneau d’activité · " + row.detail;
    {
        std::lock_guard lock(accessible_->mutex);
        if (accessibleActions_ != buttonActions_) {
            ++accessible_->revision;
            accessibleActions_ = buttonActions_;
        }
        accessible_->window = window_;
        accessible_->name = name;
        accessible_->locations = buttons_;
        accessible_->labels = buttonLabels_;
        if (!accessible_->labels.empty())
            accessible_->labels[0] = ResolveVisibility(config_, WantsExpanded(), full_).expanded
                                         ? "Réduire le panneau"
                                         : "Développer le panneau";
        accessible_->focus = keyboardFocus_ ? focus_ : -1;
    }
    SetWindowTextW(window_,
                   Wide(name + " · Tab : boutons, Entrée : action, Échap : réduire").c_str());
}
void Panel::Action(int id) {
    if (id < 0 || id >= static_cast<int>(buttons_.size()))
        return;
    const auto action = buttonActions_.at(id);
    auto now = Timer::Clock::now();
    if (action.service)
        audio_.Execute(*action.service, now);
    id = action.local;
    if (id == 3)
        audio_.ReadAgain(now);
    if (id == 0) {
        expanded_ = !ResolveVisibility(config_, WantsExpanded(), full_).expanded;
        suppressHover_ = !expanded_;
    }
    if (id == 1) {
        if (timer_.Finished())
            timer_.Reset(config_.number("timerMinutes"), now);
        timer_.Toggle(now);
    }
    if (id == 2)
        timer_.Reset(config_.number("timerMinutes"), now);
    SyncService();
    Update();
    Emit(Status());
}
LRESULT CALLBACK Panel::Proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<Panel *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Panel *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self)
        try {
            return self->Message(msg, w, l);
        } catch (const std::exception &e) {
            if (self->protocol_)
                try {
                    wire::Write(std::cout, {{"type", "error"},
                                            {"v", 5},
                                            {"code", "activity-panel-failure"},
                                            {"message", e.what()}});
                } catch (...) {
                }
            PostQuitMessage(1);
            return 0;
        }
    return DefWindowProcW(window, msg, w, l);
}
LRESULT Panel::Message(UINT msg, WPARAM w, LPARAM l) {
    if (msg == inputMessage) {
        std::optional<Json> frame;
        std::string error;
        {
            std::lock_guard lock(inputMutex_);
            frame = std::move(pending_);
            pending_.reset();
            error = std::move(inputError_);
        }
        if (!error.empty())
            throw std::runtime_error(error);
        if (!frame) {
            PostQuitMessage(0);
            return 0;
        }
        Frame(*frame);
        SetEvent(consumed_);
        return 0;
    }
    if (msg == WM_TIMER && w == 3) {
        const auto before = audio_.Status();
        audio_.Tick(Timer::Clock::now());
        if (before != audio_.Status()) {
            SyncService();
            Update();
        }
        return 0;
    }
    if (msg == WM_TIMER && w == 1) {
        Sample();
        if (timer_.Tick(Timer::Clock::now())) {
            if (config_.flag("autoExpandTimer"))
                expanded_ = true;
            Emit({{"kind", "activity-panel.timer-finished"}});
        }
        Update();
        return 0;
    }
    if (msg == WM_TIMER && w == 2) {
        double elapsed =
            std::chrono::duration<double, std::milli>(Timer::Clock::now() - animationStart_)
                .count();
        double t = Motion(elapsed, config_.number("animationMs"), reduced_);
        auto blend = [&](int a, int b) { return static_cast<int>(std::round(a + (b - a) * t)); };
        current_ = {blend(startRect_.x, target_.x), blend(startRect_.y, target_.y),
                    blend(startRect_.width, target_.width),
                    blend(startRect_.height, target_.height)};
        if (t >= 1)
            KillTimer(window_, 2);
        Render();
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        bool changed = !hover_;
        hover_ = true;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
        TrackMouseEvent(&tracking);
        int hit = -1;
        for (size_t i = 0; i < buttons_.size(); i++)
            if (Contains(buttons_[i], GET_X_LPARAM(l), GET_Y_LPARAM(l)))
                hit = static_cast<int>(i);
        changed = changed || hit_ != hit;
        hit_ = hit;
        if (changed)
            Update();
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        hover_ = false;
        suppressHover_ = false;
        hit_ = -1;
        Update();
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        pressed_ = -1;
        for (size_t i = 0; i < buttons_.size(); i++)
            if (Contains(buttons_[i], GET_X_LPARAM(l), GET_Y_LPARAM(l)))
                pressed_ = static_cast<int>(i);
        pressedAction_ =
            pressed_ >= 0 ? std::optional<ButtonAction>(buttonActions_.at(pressed_)) : std::nullopt;
        if (pressed_ >= 0)
            SetCapture(window_);
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        int pressed = pressed_;
        auto pinned = pressedAction_;
        pressed_ = -1;
        ReleaseCapture();
        if (pressed >= 0 && pressed < static_cast<int>(buttons_.size()) && pinned &&
            *pinned == buttonActions_.at(pressed) &&
            Contains(buttons_[pressed], GET_X_LPARAM(l), GET_Y_LPARAM(l)))
            Action(pressed);
        return 0;
    }
    if (msg == WM_CAPTURECHANGED) {
        pressedAction_.reset();
        pressed_ = -1;
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        scrollOffset_ =
            std::clamp(scrollOffset_ - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 58, 0, scrollMax_);
        Render();
        return 0;
    }
    if (msg == WM_KEYDOWN) {
        if (w == VK_ESCAPE) {
            expanded_ = hover_ = false;
            suppressHover_ = true;
            keyboardFocus_ = false;
            focus_ = 0;
            Update();
        } else if (w == VK_TAB || w == VK_RIGHT || w == VK_LEFT) {
            int count = static_cast<int>(buttons_.size());
            if (count)
                focus_ =
                    (focus_ + ((w == VK_LEFT || (w == VK_TAB && (GetKeyState(VK_SHIFT) & 0x8000)))
                                   ? count - 1
                                   : 1)) %
                    count;
            keyboardFocus_ = true;
            Render();
        } else if (w == VK_RETURN || w == VK_SPACE)
            Action(focus_);
        else if (w == VK_NEXT || w == VK_PRIOR) {
            scrollOffset_ = std::clamp(scrollOffset_ + (w == VK_NEXT ? 116 : -116), 0, scrollMax_);
            Render();
        }
        return 0;
    }
    if (msg == WM_SETFOCUS) {
        keyboardFocus_ = true;
        Render();
        return 0;
    }
    if (msg == WM_KILLFOCUS) {
        keyboardFocus_ = false;
        Render();
        return 0;
    }
    if (msg == WM_GETDLGCODE)
        return DLGC_WANTALLKEYS;
    if (msg == WM_GETOBJECT && static_cast<LONG>(l) == OBJID_CLIENT)
        return AccessibleObject(w, accessible_);
    if ((msg == accessibleAction || msg == accessibleFocus) &&
        l == reinterpret_cast<LPARAM>(accessible_.get())) {
        if (static_cast<uint32_t>(w >> 32) != accessible_->revision)
            return 0;
        const auto index = static_cast<uint32_t>(w);
        if (msg == accessibleAction)
            Action(static_cast<int>(index));
        else if (!fixture_ && index < buttons_.size()) {
            focus_ = static_cast<int>(index);
            SetFocus(window_);
            keyboardFocus_ = true;
            Render();
        }
        return 0;
    }
    if (msg == WM_DISPLAYCHANGE || msg == WM_DPICHANGED) {
        Update();
        return 0;
    }
    if (msg == WM_SETTINGCHANGE) {
        Apply(config_);
        return 0;
    }
    if (msg == WM_CLOSE) {
        audio_.Close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, msg, w, l);
}
int Panel::WindowPolicyTest() {
    if (!fixture_ || IsWindowVisible(window_))
        throw std::runtime_error("Le contrat de fenêtre exige une fixture cachée");
    const auto original = config_.values;
    ready_ = true;
    auto next = original;
    next["mode"] = "expanded";
    next["showMedia"] = false;
    next["reduceMotion"] = true;
    next["taskSwitcher"] = false;
    int cases = 0;
    auto requireOrder = [&](bool topmost) {
        if (IsWindowVisible(window_) ||
            ((GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) != topmost)
            throw std::runtime_error("Ordre ou visibilité du HWND de contrat incorrect");
        ++cases;
    };
    // Repeated transitions rule out an initial creation/order coincidence.
    for (int cycle = 0; cycle < 3; ++cycle) {
        next["enabled"] = true;
        next["topmost"] = false;
        Apply(ParseConfig(next));
        requireOrder(false);
        next["topmost"] = true;
        Apply(ParseConfig(next));
        requireOrder(true);
        Message(WM_TIMER, 1, 0); // Real periodic Update/Render; no focus or input.
        requireOrder(true);
        next["taskSwitcher"] = !next["taskSwitcher"].get<bool>();
        Apply(ParseConfig(next));
        requireOrder(true);
        next["enabled"] = false;
        Apply(ParseConfig(next));
        requireOrder(false);
        next["topmost"] = false;
        Apply(ParseConfig(next));
        requireOrder(false);
        next["enabled"] = true;
        Apply(ParseConfig(next));
        requireOrder(false);
    }
    Apply(ParseConfig(original));
    requireOrder(original.at("enabled").get<bool>() && original.at("topmost").get<bool>());
    std::cout << Json{{"kind", "activity-panel.window-policy-contract"},
                      {"cases", cases}, {"allWindowsHidden", true},
                      {"topmostTransitions", "false-true-tick-style-disable-restore"},
                      {"settingsRestored", config_.values == original}}
                     .dump()
              << '\n';
    return 0;
}
int Panel::SelfTest(const std::filesystem::path &output) {
    auto require = [](bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    };
    snapshot_.clock = "09:41";
    snapshot_.ac = false;
    snapshot_.battery = Availability::available;
    snapshot_.batteryPresent = true;
    snapshot_.batteryPercent = 42;
    snapshot_.memoryPercent = 37;
    snapshot_.cpuPercent = 8;
    ready_ = true;
    auto next = config_.values;
    next["mode"] = "expanded";
    next["showMedia"] = true;
    next["reduceMotion"] = true;
    Apply(ParseConfig(next));
    require(!IsWindowVisible(window_), "La fixture ne doit jamais devenir visible");
    std::filesystem::create_directories(output);
    auto encoder = PngEncoder();
    auto bitmap = Paint(current_.width, current_.height, scale_);
    require(bitmap->Save((output / L"owned-expanded.png").c_str(), &encoder, nullptr) ==
                Gdiplus::Ok,
            "PNG fixture");
    Action(1);
    require(timer_.Running(), "Démarrage minuteur");
    Action(1);
    require(!timer_.Running(), "Pause minuteur");
    Action(2);
    require(timer_.Seconds(Timer::Clock::now()) == 25 * 60, "Réinitialisation minuteur");
    next["enabled"] = false;
    Apply(ParseConfig(next));
    require(!IsWindowVisible(window_), "Désactivation");
    next["enabled"] = true;
    next["mode"] = "compact";
    Apply(ParseConfig(next));
    require(!expanded_, "Mode réduit");
    bitmap = Paint(current_.width, current_.height, scale_);
    require(bitmap->Save((output / L"owned-compact.png").c_str(), &encoder, nullptr) == Gdiplus::Ok,
            "PNG réduit");
    // Paint at explicit zoom factors without changing the user's screen DPI.
    for (int dpi : {144, 192}) {
        auto scaled = Paint(448 * dpi / 96, 440 * dpi / 96, dpi / 96.0);
        require(scaled->Save((output / (L"owned-dpi-" + std::to_wstring(dpi) + L".png")).c_str(),
                             &encoder, nullptr) == Gdiplus::Ok,
                "PNG DPI");
    }
    next["mode"] = "expanded";
    Apply(ParseConfig(next));
    IAccessible *object = nullptr;
    require(
        SUCCEEDED(AccessibleObjectFromWindow(window_, static_cast<DWORD>(OBJID_CLIENT),
                                             IID_IAccessible, reinterpret_cast<void **>(&object))),
        "Objet accessible");
    long count = 0;
    require(SUCCEEDED(object->get_accChildCount(&count)) && count == 4,
            "Actions locales et relecture A18 accessibles");
    VARIANT child;
    VariantInit(&child);
    child.vt = VT_I4;
    child.lVal = 2;
    BSTR name = nullptr;
    require(SUCCEEDED(object->get_accName(child, &name)) && name != nullptr, "Libellé accessible");
    SysFreeString(name);
    object->Release();
    SendMessageW(window_, WM_KEYDOWN, VK_TAB, 0);
    require(focus_ == 1, "Navigation clavier locale");
    SendMessageW(window_, WM_KEYDOWN, VK_ESCAPE, 0);
    require(!WantsExpanded(), "Échap réduit même en ouverture automatique");
    next["mode"] = "automatic";
    Apply(ParseConfig(next));
    hover_ = true;
    suppressHover_ = false;
    Update();
    Action(0);
    require(!WantsExpanded(), "Réduire un panneau ouvert par survol");
    hover_ = false;
    next["backgroundOpacity"] = 0;
    next["shadow"] = 0;
    next["reflection"] = 0;
    next["grain"] = 0;
    next["borderWidth"] = 0;
    next["mode"] = "expanded";
    Apply(ParseConfig(next));
    auto transparent = Paint(current_.width, current_.height, scale_);
    bool opaqueText = false;
    for (UINT py = 0; py < transparent->GetHeight(); py++)
        for (UINT px = 0; px < transparent->GetWidth(); px++) {
            Gdiplus::Color pixel;
            transparent->GetPixel(px, py, &pixel);
            if (pixel.GetA() == 255)
                opaqueText = true;
        }
    require(opaqueText, "Le texte reste opaque avec un fond transparent");
    auto smallCanvas = Paint(448, 240, 1);
    require(scrollMax_ > 0, "Défilement sur petite zone de travail");
    scrollOffset_ = scrollMax_;
    smallCanvas = Paint(448, 240, 1);
    require(buttons_.size() == 4 && buttons_[3].y + buttons_[3].height < 240,
            "Actions accessibles malgré le défilement");
    // Exercise the real panel adapter and controls with an isolated broker
    // fixture. It never connects to a running A18 or invokes Windows media/audio.
    std::vector<Json> deliveries;
    audio_ = AudioService([&](const Json &e) { deliveries.push_back(e); });
    audio_.Enable(true, Timer::Clock::now());
    auto serviceFrame = [&](Json e) {
        e["type"] = "mywallpaper.services";
        e["v"] = 1;
        e["generation"] = "owned-ui-generation";
        Frame({{"type", "message"},
               {"v", 5},
               {"surface", "service"},
               {"instanceId", "owned-ui-generation"},
               {"displayIndex", 0},
               {"payload", e}});
    };
    int delivery = 0;
    auto servicePacket = [&](Json packet) {
        serviceFrame({{"kind", "packet"},
                      {"deliveryId", "owned-ui:" + std::to_string(++delivery)},
                      {"packet", packet}});
    };
    auto lastPacket = [&](const std::string &type) {
        for (auto it = deliveries.rbegin(); it != deliveries.rend(); ++it)
            if (it->at("kind") == "packet" && it->at("packet").at("type") == type)
                return it->at("packet");
        throw std::runtime_error("Paquet de fixture A18 manquant");
    };
    serviceFrame({{"kind", "open"}});
    servicePacket({{"type", "reply"},
                   {"id", lastPacket("connect").at("id")},
                   {"ok", true},
                   {"value", "owned-ui-connection"}});
    const auto examples = Json::parse(audioExamples);
    auto source = examples.at("snapshotResult");
    source["requestId"] = lastPacket("call").at("payload").at("requestId");
    servicePacket(
        {{"type", "reply"}, {"id", lastPacket("call").at("id")}, {"ok", true}, {"value", source}});
    require(snapshot_.mediaDetail.find("Synthetic track") != std::string::npos,
            "Métadonnées A18 affichées");
    auto findMedia = [&]() {
        for (size_t i = 0; i < buttonActions_.size(); ++i)
            if (buttonActions_[i].service &&
                buttonActions_[i].service->command.at("action") == "playPause")
                return i;
        throw std::runtime_error("Bouton de lecture A18 manquant");
    };
    const auto play = findMedia();
    const WPARAM oldAccessible =
        (static_cast<WPARAM>(accessible_->revision) << 32) | static_cast<uint32_t>(play);
    Action(static_cast<int>(play));
    auto command = lastPacket("call");
    require(command.at("method") == "command" &&
                command.at("payload").at("expectedSessionId") == "media-owned",
            "Action UI vers cible opaque A18");
    auto result = examples.at("commandResult");
    result["requestId"] = command.at("payload").at("requestId");
    servicePacket({{"type", "reply"}, {"id", command.at("id")}, {"ok", true}, {"value", result}});
    require(audio_.Status().at("commandOutcome") == "acknowledged" &&
                !audio_.State("media").at("selected").at("playing").get<bool>(),
            "Résultat métier sans état optimiste");
    bitmap = Paint(current_.width, current_.height, scale_);
    require(bitmap->Save((output / L"owned-media.png").c_str(), &encoder, nullptr) == Gdiplus::Ok,
            "Rendu A18 synthétique");
    auto sub = lastPacket("subscribe");
    auto media = examples.at("mediaState");
    media["revision"] = 12;
    auto &caps = media["body"]["value"]["selected"]["capabilities"];
    for (auto it = caps.begin(); it != caps.end(); ++it)
        it.value() = false;
    media["body"]["value"]["sessions"][0]["capabilities"] = caps;
    servicePacket({{"type", "subscription"},
                   {"id", sub.at("id")},
                   {"event", {{"kind", "snapshot"}, {"sequence", 20}, {"value", media}}}});
    const auto beforeAction = deliveries.size();
    SendMessageW(window_, accessibleAction, oldAccessible,
                 reinterpret_cast<LPARAM>(accessible_.get()));
    require(deliveries.size() == beforeAction, "Action accessible périmée annulée");
    for (const auto &action : buttonActions_)
        require(!action.service || action.service->command.at("action") != "playPause",
                "Capacité retirée du panneau");
    audio_.Close();
    // The callback references this scope; detach it before leaving the fixture.
    audio_ = AudioService([](const Json &) {});
    require(!IsWindowVisible(window_), "Aucune activation fixture");
    std::ofstream report(output / L"fixture.json");
    report << Json{{"proof", "owned-hidden-window"}, {"visible", false},
                   {"desktopCapture", false},        {"media", "synthetic-A18-service"},
                   {"pid", GetCurrentProcessId()},   {"status", Status()}}
                  .dump(2);
    std::cerr << "Owned hidden-window fixture passed; no desktop/application activation\n";
    return 0;
}
} // namespace activity
