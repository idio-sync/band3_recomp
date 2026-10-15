#include "src/Net/song_pick_qr.h"
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "src/Hooks/aspect.h"
#include "src/Net/http_game.h"
#include "src/Net/http_server.h"
#include "src/Net/local_address.h"
#include "src/Net/milo.h"
#include "src/Net/song_pick_qr_model.h"
#include "src/Render/renderer_switch.h"
#include "src/settings.h"

namespace band3::song_pick_qr {

namespace {

// Pentatonic's display cut: the menus' face, its glyphs drawn at 73 px, so
// they stay sharp shrunk to the caption at any window size
constexpr char kFontPath[] = "ui/resource/fonts/gen/pentatonic_display.milo_xbox";
constexpr size_t kMaxFontFile = 4 * 1024 * 1024;
// the game may be too busy to read it for a few seconds as the library loads
constexpr int kFontReadTries = 5;
// how often the page's address is looked up again (a new network, a new lease)
constexpr auto kAddressCheck = std::chrono::seconds(5);

// The font, read once on a thread of its own, since the game thread reads the
// file and the UI thread can't wait on it
struct FontLoad {
    enum class Status { kLoading, kReady, kFailed };
    std::atomic<Status> status{Status::kLoading};
    // written before status turns kReady
    milo::Font font;
    http::Image texture;
};

std::shared_ptr<FontLoad> StartFontLoad() {
    auto load = std::make_shared<FontLoad>();
    std::thread([load] {
        std::optional<std::string> file;
        bool read = false;
        for (int i = 0; i < kFontReadTries && !read; i++) {
            read = http::RunOnGameThreadAndWait([&file](PPCContext& ctx, uint8_t* base) {
                file = http::game::GameFile(ctx, base, kFontPath, kMaxFontFile);
            });
        }
        const auto raw = file ? milo::Decompress(*file) : std::nullopt;
        auto font = raw ? milo::FindFont(*raw) : std::nullopt;
        auto texture = raw ? milo::FindBitmap(*raw) : std::nullopt;
        if (font && texture) {
            load->font = std::move(*font);
            load->texture = std::move(*texture);
            load->status = FontLoad::Status::kReady;
        } else {
            REXLOG_WARN("Song list QR code: couldn't read {}{}, the address is in ImGui's font",
                        kFontPath, read ? "" : " (the game was busy)");
            load->status = FontLoad::Status::kFailed;
        }
    }).detach();
    return load;
}

// the window's 16:9 frame letterboxed: the picture letterboxed, or the
// native renderer's cameras filling the window, which keeps the menus to its
// middle 16:9 (mouse_hover.cpp's PictureFor, aspect_model.h)
bool FrameLetterboxed() {
    const bool sdk_letterbox =
        !rex::cvar::GetFlagInfo("present_letterbox") || rex::cvar::Query<bool>("present_letterbox");
    if (render::ShowsNativePicture()) return REXCVAR_GET(native_fill_window) || sdk_letterbox;
    return sdk_letterbox;
}

ImU32 Grey(uint8_t level) { return IM_COL32(level, level, level, 255); }

}  // namespace

struct SongPickQrDialog::State {
    std::shared_ptr<FontLoad> font;
    // the page's address and its code, looked up every kAddressCheck
    std::string url;
    std::optional<Modules> code;
    std::chrono::steady_clock::time_point checked{};
    bool checked_once = false;
    // the caption's lines, white in their alpha, made for this address at
    // these heights with this drawer
    struct LineTexture {
        std::unique_ptr<rex::ui::ImmediateTexture> texture;
        int width = 0, height = 0;
    };
    LineTexture title, address;
    std::string lines_url;
    int title_h = 0, address_h = 0;
    rex::ui::ImmediateDrawer* lines_drawer = nullptr;
};

SongPickQrDialog::SongPickQrDialog(rex::ui::ImGuiDrawer* imgui_drawer, DrawerGetter drawer)
    : rex::ui::ImGuiDialog(imgui_drawer), drawer_(std::move(drawer)), state_(std::make_unique<State>()) {}

SongPickQrDialog::~SongPickQrDialog() = default;

void SongPickQrDialog::OnDraw(ImGuiIO& io) {
    if (hidden_ || !REXCVAR_GET(http_qr_code) || !aspect::SongListShowing()) return;
    State& s = *state_;

    const auto now = std::chrono::steady_clock::now();
    if (!s.checked_once || now - s.checked >= kAddressCheck) {
        s.checked_once = true;
        s.checked = now;
        std::string url;
        if (const auto listening = http::ListeningOn()) {
            const std::string local =
                listening->address == "0.0.0.0" ? net::LocalAddress() : std::string();
            url = PageUrl(listening->address, local, listening->port);
        }
        if (url != s.url) {
            s.url = url;
            s.code = url.empty() ? std::nullopt : Encode(url, kQuiet);
        }
    }
    if (!s.code || io.DisplaySize.x < 1 || io.DisplaySize.y < 1) return;
    if (!s.font) s.font = StartFontLoad();

    const Rect frame =
        GameFrame(uint32_t(io.DisplaySize.x), uint32_t(io.DisplaySize.y), FrameLetterboxed());
    const float scale = frame.h / kFrameHeight;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    // the code, whole pixels a module, light behind it all
    const Modules& code = *s.code;
    const int module = ModulePixels(code.size, scale);
    const float side = float(module * code.size);
    const float left = std::round(frame.x + kCodeLeft * scale);
    const float bottom = std::round(frame.y + kCodeBottom * scale);
    const float top = bottom - side;
    draw->AddRectFilled(ImVec2(left, top), ImVec2(left + side, bottom), IM_COL32_WHITE);
    for (int y = 0; y < code.size; y++) {
        // each row's dark runs as one rectangle
        for (int x = 0; x < code.size;) {
            if (!code.Dark(x, y)) {
                x++;
                continue;
            }
            const int start = x;
            while (x < code.size && code.Dark(x, y)) x++;
            draw->AddRectFilled(ImVec2(left + float(start * module), top + float(y * module)),
                                ImVec2(left + float(x * module), top + float((y + 1) * module)),
                                IM_COL32_BLACK);
        }
    }

    // the caption: two lines beside the code, the second's bottom level with it
    const float caption_left = left + side + std::round(kCaptionGap * scale);
    const int title_h = std::max(1, int(std::lround(kTitlePx * scale)));
    const int address_h = std::max(1, int(std::lround(kAddressPx * scale)));
    const float line_gap = std::max(1.0f, std::round(kLineGap * scale));
    const float address_top = bottom - float(address_h);
    const float title_top = address_top - line_gap - float(title_h);
    const std::string address = Caption(s.url);

    rex::ui::ImmediateDrawer* drawer = drawer_ ? drawer_() : nullptr;
    const bool font_ready = s.font->status == FontLoad::Status::kReady;
    if (font_ready && drawer &&
        (s.lines_url != s.url || s.title_h != title_h || s.address_h != address_h ||
         s.lines_drawer != drawer)) {
        auto make = [&](std::string_view text, int height) {
            State::LineTexture out;
            const FontLoad& f = *s.font;
            const Mask mask = Rasterize(Layout(f.font, text, float(height), f.texture.width, f.texture.height),
                                        f.texture, height);
            if (mask.width <= 0) return out;
            std::vector<uint8_t> rgba(size_t(mask.width) * size_t(mask.height) * 4, 255);
            for (size_t i = 0; i < mask.alpha.size(); i++) rgba[i * 4 + 3] = mask.alpha[i];
            out.texture = drawer->CreateTexture(uint32_t(mask.width), uint32_t(mask.height),
                                                rex::ui::ImmediateTextureFilter::kNearest, false,
                                                rgba.data());
            out.width = mask.width;
            out.height = mask.height;
            return out;
        };
        s.title = make(kTitle, title_h);
        s.address = make(address, address_h);
        s.lines_url = s.url;
        s.title_h = title_h;
        s.address_h = address_h;
        s.lines_drawer = drawer;
    }
    auto put = [&](const State::LineTexture& line, float y, uint8_t grey) {
        const ImTextureRef texture(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(line.texture.get())));
        draw->AddImage(texture, ImVec2(caption_left, y),
                       ImVec2(caption_left + float(line.width), y + float(line.height)), ImVec2(0, 0),
                       ImVec2(1, 1), Grey(grey));
    };
    if (font_ready && s.lines_drawer == drawer && s.title.texture && s.address.texture) {
        put(s.title, title_top, kTitleGrey);
        put(s.address, address_top, kAddressGrey);
    } else if (s.font->status == FontLoad::Status::kFailed || !drawer) {
        // without the game's font, ImGui's at the same heights
        draw->AddText(ImGui::GetFont(), float(title_h), ImVec2(caption_left, title_top), Grey(kTitleGrey),
                      kTitle);
        draw->AddText(ImGui::GetFont(), float(address_h), ImVec2(caption_left, address_top),
                      Grey(kAddressGrey), address.c_str());
    }
}

}  // namespace band3::song_pick_qr
