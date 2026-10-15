#pragma once
#include <functional>
#include <memory>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

// A QR code for the web server's page (http_server.h) at the top of the
// Music Library, between its title and the profile's name, with the page's
// address beside it in the menu's own font (RB3's Pentatonic, read from the
// game's ui/resource/fonts/gen/pentatonic_display.milo_xbox, as /karaoke sets
// its lyrics). Shown while the song list is up (aspect.h's SongListShowing),
// the server listens where a phone can reach it, and http_qr_code is on.
// Where it all goes: song_pick_qr_model.h.

namespace band3::song_pick_qr {

class SongPickQrDialog : public rex::ui::ImGuiDialog {
public:
    using DrawerGetter = std::function<rex::ui::ImmediateDrawer*()>;
    // `drawer` makes the caption's textures
    SongPickQrDialog(rex::ui::ImGuiDrawer* imgui_drawer, DrawerGetter drawer);
    ~SongPickQrDialog() override;

    // hidden while the launcher is up
    void set_hidden(bool hidden) { hidden_ = hidden; }

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    struct State;
    DrawerGetter drawer_;
    bool hidden_ = false;
    std::unique_ptr<State> state_;
};

}  // namespace band3::song_pick_qr
