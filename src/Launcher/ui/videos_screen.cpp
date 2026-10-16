#include "videos_screen.h"
#include <SDL3/SDL_misc.h>
#include <rex/filesystem.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include "hub_model.h"
#include "keyboard_panel.h"
#include "src/Launcher/launcher_style.h"
#include "src/Video/video_fetch.h"
#include "src/Video/video_job_state.h"
#include "src/Video/video_jobs.h"
#include "src/settings.h"
#include "widgets.h"

namespace band3::launcher::ui {

namespace fs = std::filesystem;
using State = VideoLibrary::State;

namespace {

constexpr float kLeft = 40, kRight = 890, kTop = 140, kBottom = 652;
constexpr float kBandHeight = 34, kRowHeight = 40, kRowPitch = 41, kGap = 6;
constexpr float kBodyLeft = 932, kBodyWidth = 298;

constexpr const char* kFilterNames[] = {"Every song", "Without a video", "With a video",
                                        "To review",  "Failed",          "Files named for no song"};

// a music video's size, in MB, for a few minutes at each height: for the
// batch's estimate only
int MegabytesAt(int height) {
    return height <= 480 ? 20 : height <= 720 ? 45 : height <= 1080 ? 90 : height <= 1440 ? 180 : 350;
}

std::string Letter(std::string_view title) {
    for (const char c : title) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            return std::string(1, char(std::toupper(static_cast<unsigned char>(c))));
        }
        if (std::isdigit(static_cast<unsigned char>(c))) return "1 2 3";
        if (c != ' ' && c != '(' && c != '\'' && c != '"') break;
    }
    return "#";
}

const char* StateText(State state) {
    switch (state) {
    case State::kSynced: return "Synced";
    case State::kNotSure: return "Not sure";
    case State::kSetByHand: return "Set by hand";
    case State::kNotSynced: return "Not synced";
    case State::kFinding: return "Finding";
    case State::kWaiting: return "Waiting";
    case State::kToReview: return "To review";
    case State::kFailed: return "Failed";
    case State::kNoneFound: return "None found";
    case State::kNoVideo: return "No video";
    }
    return "";
}

ImU32 StateColor(State state) {
    switch (state) {
    case State::kSynced: return color::kGood;
    case State::kNotSure:
    case State::kNotSynced:
    case State::kToReview: return color::kWarn;
    case State::kSetByHand: return color::kSoft;
    case State::kFinding: return color::kInfo;
    case State::kFailed: return color::kBad;
    default: return IM_COL32(142, 147, 163, 255);
    }
}


void OpenFile(const fs::path& file) {
    SDL_OpenURL(("file:///" + rex::path_to_utf8(file.generic_wstring())).c_str());
}

}

VideosScreen::VideosScreen(Skin& skin, VideosHost host) : skin_(skin), host_(std::move(host)) {}

void VideosScreen::Show() {
    input_.Show();
    box_.reset();
    typing_link_.reset();
    // the cursor starts on the first row that can do something now
    selected_ = 0;
    int pick = 0;
    for (const Item& item : Items()) {
        if (!Pickable(item.kind)) continue;
        if (!item.blocked) {
            selected_ = pick;
            break;
        }
        pick++;
    }
}

void VideosScreen::ShowBox(Box box) {
    box_ = std::move(box);
    box_frame_ = ImGui::GetFrameCount();
}

std::vector<VideosScreen::Item> VideosScreen::Items() const {
    std::vector<Item> out;
    const video::VideoJobsProgress p = video::VideoJobsNow();
    if (p.running || p.waiting > 0) {
        out.push_back({Item::kFindAll, std::format("Finding videos: {} of {}", p.done, p.total), -1,
                       p.paused ? "Paused" : ""});
    } else {
        // what it waits on, before it can run
        const video::YtDlpState yt = video::YtDlpNow(false);
        const char* waits = library_.Songs().empty()           ? "Start the game once first"
                            : !yt.checking && yt.version.empty() ? "Needs yt-dlp"
                                                                 : "";
        out.push_back({Item::kFindAll, "Find videos for every song", -1, waits, *waits != 0});
    }
    out.push_back({Item::kSettings, "Video settings"});
    out.push_back({Item::kFolder, "Open the videos folder"});
    out.push_back({Item::kFilter, "Show"});
    const auto& songs = library_.Songs();
    const auto& videos = library_.Videos();
    if (filter_ == Filter::kUnmatched) {
        out.push_back({Item::kLetter, "Files named for no song", -1,
                       std::to_string(library_.Unmatched().size())});
        for (const int i : library_.Unmatched()) {
            out.push_back({Item::kUnmatched, rex::path_to_utf8(videos[size_t(i)].path.filename()), i});
        }
        if (library_.Unmatched().empty()) out.push_back({Item::kEmpty, "Every video is named for a song band3 knows"});
        return out;
    }
    std::string letter;
    // the letter band being counted: none before the first song
    size_t band = SIZE_MAX;
    int band_songs = 0, band_videos = 0;
    const auto close_band = [&] {
        if (band < out.size()) {
            out[band].right = std::format("{} {} - {} {}", band_songs, band_songs == 1 ? "song" : "songs",
                                          band_videos, band_videos == 1 ? "video" : "videos");
        }
    };
    for (int i = 0; i < int(songs.size()); i++) {
        const VideoLibrary::Song& s = songs[size_t(i)];
        const State state = library_.StateOf(s);
        const bool shown = filter_ == Filter::kAll || (filter_ == Filter::kWithout && s.video < 0) ||
                           (filter_ == Filter::kWith && s.video >= 0) ||
                           (filter_ == Filter::kToReview && state == State::kToReview) ||
                           (filter_ == Filter::kFailed && state == State::kFailed);
        if (!shown) continue;
        const std::string l = Letter(s.title);
        if (l != letter) {
            close_band();
            letter = l;
            band = out.size();
            band_songs = band_videos = 0;
            out.push_back({Item::kLetter, l});
        }
        band_songs++;
        band_videos += s.video >= 0;
        out.push_back({Item::kSong, s.title, i});
    }
    close_band();
    if (songs.empty()) {
        out.push_back({Item::kEmpty, "No songs yet: band3 lists them once the game has read them"});
    } else if (letter.empty()) {
        out.push_back({Item::kEmpty, "No song matches"});
    }
    return out;
}

bool VideosScreen::NeedsYtDlp() {
    const video::YtDlpState yt = video::YtDlpNow(false);
    if (yt.checking || !yt.version.empty()) return false;
    ShowBox(Box{.title = "band3 needs yt-dlp to find videos",
                .lines = {yt.problem.empty() ? "It isn't on this PC." : yt.problem + ".",
                          "Get it, then set its program in Settings, Music Videos, or put it on PATH."},
                .options = {"Get yt-dlp", "Look again", "Cancel"},
                .cancel = 2,
                .chosen = [](int choice) {
                    if (choice == 0) SDL_OpenURL("https://github.com/yt-dlp/yt-dlp#installation");
                    if (choice == 1) video::YtDlpNow(true);
                }});
    return true;
}

void VideosScreen::FindFor(int song, video::VideoJob job) {
    if (NeedsYtDlp()) return;
    const VideoLibrary::Song& s = library_.Songs()[size_t(song)];
    if (s.video < 0) {
        library_.Find(s, std::move(job));
        return;
    }
    // a song with a video asks first: the old one stays until the new one's in
    const std::string file = rex::path_to_utf8(library_.Videos()[size_t(s.video)].path.filename());
    ShowBox(Box{.title = "Replace its video?",
                .lines = {file + " stays until the new one's in."},
                .options = {"Replace it", "Keep the old one"},
                .cancel = 1,
                .chosen = [this, song, job = std::move(job)](int choice) mutable {
                    if (choice == 0) library_.Find(library_.Songs()[size_t(song)], std::move(job));
                }});
}

void VideosScreen::Options(int song) {
    const VideoLibrary::Song& s = library_.Songs()[size_t(song)];
    const auto status = video::SongVideoStatusOf(s.shortname);
    const bool results = status && !status->candidates.empty();
    std::vector<std::string> options;
    std::vector<std::function<void()>> actions;
    const auto add = [&](std::string label, std::function<void()> action) {
        options.push_back(std::move(label));
        actions.push_back(std::move(action));
    };
    if (s.video >= 0) {
        const VideoLibrary::Video& v = library_.Videos()[size_t(s.video)];
        const fs::path path = v.path;
        add("Play it", [path] { OpenFile(path); });
        add("Sync it again", [this, song] {
            const auto& sg = library_.Songs()[size_t(song)];
            if (sg.video >= 0) library_.Sync(library_.Videos()[size_t(sg.video)]);
        });
        if (v.result && !v.result->confident) {
            add("Use the guess", [this, song] {
                const auto& sg = library_.Songs()[size_t(song)];
                if (sg.video >= 0) {
                    library_.UseGuess(const_cast<VideoLibrary::Video&>(library_.Videos()[size_t(sg.video)]));
                }
            });
        }
        if (!v.segments.empty()) {
            add("Use one offset throughout", [this, song] {
                const auto& sg = library_.Songs()[size_t(song)];
                if (sg.video >= 0) {
                    library_.MergeParts(const_cast<VideoLibrary::Video&>(library_.Videos()[size_t(sg.video)]));
                }
            });
        }
        add("Search for another", [this, song] { FindFor(song, {}); });
    } else {
        add("Search for its video", [this, song] { FindFor(song, {}); });
    }
    if (results) add("Choose a result", [this, song] { ChooseResult(song); });
    add("Paste a link", [this, song] {
        typing_link_ = song;
        keyboard_.Open("");
        ShowSteamKeyboard();
    });
    add("Choose a file", [this, song] { library_.PickFile(library_.Songs()[size_t(song)]); });
    if (s.video >= 0) {
        const VideoLibrary::Video& v = library_.Videos()[size_t(s.video)];
        if (!v.source.empty()) {
            const std::string url = v.source;
            add("Open its page", [url] { SDL_OpenURL(url.c_str()); });
        }
        add("Delete it", [this, song] {
            const auto& sg = library_.Songs()[size_t(song)];
            if (sg.video < 0) return;
            const std::string file = rex::path_to_utf8(library_.Videos()[size_t(sg.video)].path.filename());
            ShowBox(Box{.title = "Delete its video?",
                        .lines = {file + " and its offset go."},
                        .options = {"Delete it", "Keep it"},
                        .selected = 1,
                        .cancel = 1,
                        .chosen = [this, song](int choice) {
                            const auto& sg2 = library_.Songs()[size_t(song)];
                            if (choice == 0 && sg2.video >= 0) {
                                library_.Delete(library_.Videos()[size_t(sg2.video)], sg2.shortname);
                            }
                        }});
        });
    }
    options.push_back("Cancel");
    const int cancel = int(options.size()) - 1;
    ShowBox(Box{.title = s.title,
                .options = std::move(options),
                .cancel = cancel,
                .chosen = [actions = std::move(actions)](int choice) {
                    if (choice < int(actions.size())) actions[size_t(choice)]();
                }});
}

void VideosScreen::ChooseResult(int song) {
    const VideoLibrary::Song& s = library_.Songs()[size_t(song)];
    const auto status = video::SongVideoStatusOf(s.shortname);
    if (!status || status->candidates.empty()) return;
    std::vector<std::string> options;
    std::vector<std::string> urls;
    for (const video::TriedCandidate& t : status->candidates) {
        if (options.size() >= 6) break;
        const video::YtCandidate& c = t.candidate.video;
        std::string label = c.title;
        if (label.size() > 60) label = label.substr(0, 57) + "...";
        options.push_back(std::move(label));
        urls.push_back(c.url);
    }
    options.push_back("Cancel");
    const int cancel = int(options.size()) - 1;
    ShowBox(Box{.title = "Choose its video",
                .lines = {"The search's likeliest results. The one chosen is downloaded and synced."},
                .options = std::move(options),
                .cancel = cancel,
                .chosen = [this, song, urls](int choice) {
                    if (choice >= int(urls.size())) return;
                    video::VideoJob job;
                    job.kind = video::VideoJob::Kind::kLink;
                    job.link = urls[size_t(choice)];
                    FindFor(song, std::move(job));
                }});
}

void VideosScreen::Jump(int direction) {
    // to the next (or this, then the previous) letter band's first song
    const std::vector<Item> items = Items();
    std::vector<int> pickable;
    for (int i = 0; i < int(items.size()); i++) {
        if (Pickable(items[size_t(i)].kind)) pickable.push_back(i);
    }
    if (pickable.empty()) return;
    const int at = pickable[size_t(std::clamp(selected_, 0, int(pickable.size()) - 1))];
    int band = -1;
    if (direction > 0) {
        for (int i = at + 1; i < int(items.size()); i++) {
            if (items[size_t(i)].kind == Item::kLetter) {
                band = i;
                break;
            }
        }
    } else {
        int seen = 0;
        for (int i = at; i >= 0; i--) {
            if (items[size_t(i)].kind != Item::kLetter) continue;
            // the band this song is under first, unless it's its first song
            if (++seen == 2 || (seen == 1 && i + 1 != at)) {
                band = i;
                break;
            }
        }
    }
    if (band < 0) return;
    for (int k = 0; k < int(pickable.size()); k++) {
        if (pickable[size_t(k)] > band) {
            selected_ = k;
            reveal_ = true;
            return;
        }
    }
}

void VideosScreen::Activate(const Item& item) {
    switch (item.kind) {
    case Item::kFindAll: {
        const video::VideoJobsProgress p = video::VideoJobsNow();
        if (p.running || p.waiting > 0) {
            video::PauseVideoJobs(!p.paused);
            return;
        }
        if (library_.Songs().empty()) {
            ShowBox(Box{.title = "No songs yet",
                        .lines = {"band3 lists your songs once the game has read them: start the game once, then come back."},
                        .options = {"OK"},
                        .cancel = 0});
            return;
        }
        if (NeedsYtDlp()) return;
        const size_t count = library_.BatchSongs().size();
        if (count == 0) {
            ShowBox(Box{.title = "Every song has a video",
                        .lines = {"Or a search found none, or left one for you to review."},
                        .options = {"OK"},
                        .cancel = 0});
            return;
        }
        const int height = REXCVAR_GET(music_video_max_height);
        const double gb = double(count) * MegabytesAt(height) / 1000.0;
        std::vector<std::string> lines = {
            std::format("At {}p that's about {:.1f} GB, a song at a time. It goes on while you play.", height, gb)};
        const auto folders = host_.folders ? host_.folders() : std::vector<fs::path>{};
        if (!folders.empty()) {
            std::error_code ec;
            const fs::space_info space = fs::space(folders.front().root_path(), ec);
            if (!ec) lines.push_back(std::format("{:.0f} GB free there now.", double(space.available) / 1e9));
        }
        ShowBox(Box{.title = std::format("Find videos for {} songs?", Thousands(count)),
                    .lines = std::move(lines),
                    .options = {"Find them", "Cancel"},
                    .cancel = 1,
                    .chosen = [this](int choice) {
                        if (choice == 0) library_.FindBatch();
                    }});
        return;
    }
    case Item::kSettings:
        if (host_.settings) host_.settings();
        return;
    case Item::kFolder: {
        const auto folders = host_.folders ? host_.folders() : std::vector<fs::path>{};
        if (!folders.empty()) {
            std::error_code ec;
            fs::create_directories(folders.front(), ec);
            OpenFile(folders.front());
        }
        return;
    }
    case Item::kFilter: Step(item, 1); return;
    case Item::kSong: Options(item.index); return;
    case Item::kUnmatched: OpenFile(library_.Videos()[size_t(item.index)].path); return;
    default: return;
    }
}

void VideosScreen::Step(const Item& item, int delta) {
    if (item.kind == Item::kFilter) {
        const int count = int(std::size(kFilterNames));
        filter_ = Filter(((int(filter_) + delta) % count + count) % count);
        return;
    }
    if (item.kind == Item::kFindAll) {
        const video::VideoJobsProgress p = video::VideoJobsNow();
        // left stops the batch
        if (delta < 0 && (p.running || p.waiting > 0)) video::StopVideoJobs();
        return;
    }
    // a song's offset, 10 ms a step
    if (item.kind != Item::kSong) return;
    const VideoLibrary::Song& s = library_.Songs()[size_t(item.index)];
    if (s.video < 0) return;
    auto& v = const_cast<VideoLibrary::Video&>(library_.Videos()[size_t(s.video)]);
    const int ms = int(std::lround(v.offset.value_or(0.0) * 1000.0)) + delta * 10;
    library_.SetOffset(v, ms);
}

void VideosScreen::Handle(Action action) {
    if (box_) {
        if (const auto picked = StepBox(*box_, action)) {
            const auto chosen = std::move(box_->chosen);
            box_.reset();
            if (chosen) chosen(*picked);
        }
        return;
    }
    if (typing_link_) {
        const OnScreenKeyboard::Result r = keyboard_.Handle(action);
        if (r == OnScreenKeyboard::Result::kCancelled) typing_link_.reset();
        if (r == OnScreenKeyboard::Result::kDone) {
            const int song = *typing_link_;
            typing_link_.reset();
            if (!keyboard_.Text().empty()) {
                video::VideoJob job;
                job.kind = video::VideoJob::Kind::kLink;
                job.link = keyboard_.Text();
                FindFor(song, std::move(job));
            }
        }
        return;
    }
    const std::vector<Item> items = Items();
    std::vector<const Item*> pickable;
    for (const Item& i : items) {
        if (Pickable(i.kind)) pickable.push_back(&i);
    }
    const int count = int(pickable.size());
    const Item* item = selected_ < count ? pickable[size_t(selected_)] : nullptr;
    switch (action) {
    case Action::kUp:
    case Action::kDown:
        selected_ = MoveSelection(selected_, action == Action::kUp ? -1 : 1, count, false);
        reveal_ = true;
        break;
    case Action::kLeft:
    case Action::kRight:
        if (item) Step(*item, action == Action::kLeft ? -1 : 1);
        break;
    case Action::kSelect:
        if (item) Activate(*item);
        break;
    case Action::kSecondary:
        if (item && item->kind == Item::kSong) FindFor(item->index, {});
        break;
    case Action::kReset:
        // yt-dlp updated: YouTube changes often, and an old one stops finding videos
        if (item && item->kind == Item::kFindAll && !NeedsYtDlp()) video::UpdateYtDlp();
        if (item && item->kind == Item::kSong) {
            const auto& s = library_.Songs()[size_t(item->index)];
            if (s.video >= 0) library_.Sync(library_.Videos()[size_t(s.video)]);
        }
        break;
    case Action::kPreviousCategory: Jump(-1); break;
    case Action::kNextCategory: Jump(1); break;
    case Action::kBack:
        if (host_.back) host_.back();
        break;
    case Action::kPlay:
        if (host_.play) host_.play();
        break;
    }
}

void VideosScreen::Draw(ImGuiIO& io, std::span<const NavPad> pads, bool over_game) {
    over_game_ = over_game;
    library_.Refresh(host_.folders ? host_.folders() : std::vector<fs::path>{});
    if (const auto message = library_.TakeSyncMessage()) {
        note_ = *message;
        note_color_ = message->starts_with("Synced") ? color::kGood : color::kWarn;
        note_until_ = NowSeconds() + 5;
    }
    if (typing_link_) {
        bool edited = false;
        const OnScreenKeyboard::Result r = TypeIntoKeyboard(keyboard_, io, edited);
        if (r == OnScreenKeyboard::Result::kDone) Handle(Action::kPlay);
        if (r == OnScreenKeyboard::Result::kCancelled) typing_link_.reset();
        // the keys type: only the pads act
        std::vector<Action> actions = input_.Read(pads);
        for (const Action a : actions) {
            const bool key_typed = a == Action::kSecondary || a == Action::kReset || a == Action::kSelect ||
                                   a == Action::kBack;
            if (key_typed && input_.Keyboard()) continue;
            Handle(a);
        }
    } else {
        for (const Action a : input_.Read(pads)) Handle(a);
    }

    const std::vector<Item> items = Items();
    std::vector<int> pickable;
    for (int i = 0; i < int(items.size()); i++) {
        if (Pickable(items[size_t(i)].kind)) pickable.push_back(i);
    }
    selected_ = std::clamp(selected_, 0, std::max(0, int(pickable.size()) - 1));
    const Item* current = pickable.empty() ? nullptr : &items[size_t(pickable[size_t(selected_)])];
    const bool free = !box_ && !typing_link_;

    const Page page = PageFor(io);
    ImDrawList* dl = BeginScreen("band3##videos", io, skin_, over_game);
    DrawTitle(dl, page.At(70, 14), "Music Videos");
    ui::DrawStrip(dl, page.At(0, 96), page.At(890, 132), skin_);
    const size_t songs = library_.Songs().size();
    PaintText(dl, page.At(40, 100), kCaps(text::kStrip), color::kStripText,
              std::format("{} songs - {} with videos", Thousands(songs), Thousands(size_t(library_.WithVideos()))));
    {
        std::string right;
        ImU32 right_color = color::kInfo;
        const video::VideoJobsProgress p = video::VideoJobsNow();
        if (!note_.empty() && NowSeconds() < note_until_) {
            right = note_;
            right_color = note_color_;
        } else if (p.running && !p.paused) {
            right = std::format("Finding {}: {}", p.song, video::FetchStageText(p.stage));
        } else if (p.paused && p.waiting > 0) {
            right = p.paused_why.empty() ? "Finding paused" : "Paused: " + p.paused_why;
            right_color = color::kWarn;
        } else if (p.waiting_for_song) {
            right = "Waiting for the song being played";
        }
        if (!right.empty()) {
            const float w = std::min(TextWidth(kCaps(text::kHelp), right), Px(420));
            PaintText(dl, ImVec2(page.At(880, 0).x - w, page.At(0, 103).y), kCaps(text::kHelp), right_color, right,
                      Px(420));
        }
    }

    // the list: what's in sight of it
    std::vector<float> ys;
    float y = 0;
    for (size_t i = 0; i < items.size(); i++) {
        const Item::Kind kind = items[i].kind;
        if (kind == Item::kLetter) {
            y += kGap;
            ys.push_back(y);
            y += kBandHeight;
        } else if (kind == Item::kFindAll || kind == Item::kSettings || kind == Item::kFolder) {
            ys.push_back(y);
            y += kBandHeight + 2;
        } else {
            ys.push_back(y);
            y += kRowPitch;
        }
    }
    const float area = kBottom - kTop;
    if (reveal_ && current) {
        const size_t at = size_t(pickable[size_t(selected_)]);
        float top = ys[at];
        if (at > 0 && items[at - 1].kind == Item::kLetter) top = ys[at - 1];
        if (top < scroll_) scroll_ = top;
        if (ys[at] + kRowHeight > scroll_ + area) scroll_ = ys[at] + kRowHeight - area;
        reveal_ = false;
    }
    const ImVec2 amin = page.At(kLeft, kTop), amax = page.At(kRight, kBottom);
    if (free && ImGui::IsMouseHoveringRect(amin, amax) && io.MouseWheel != 0) {
        scroll_ -= io.MouseWheel * kRowPitch * 3;
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, y - area));
    dl->PushClipRect(amin, amax, true);
    int pick = -1;
    for (size_t i = 0; i < items.size(); i++) {
        const Item& item = items[i];
        if (Pickable(item.kind)) pick++;
        const float top = kTop + ys[i] - scroll_;
        if (top > kBottom || top + kBandHeight + 8 < kTop) continue;
        const bool selected = Pickable(item.kind) && pick == selected_ && free;
        if (item.kind == Item::kLetter) {
            DrawBand(dl, page.At(kLeft, top), page.At(kRight, top + kBandHeight), item.label, BandKind::kSection, skin_,
                     false, item.right);
            continue;
        }
        ImVec2 min = page.At(kLeft, top), max = page.At(kRight, top + kRowHeight);
        if (item.kind == Item::kFindAll || item.kind == Item::kSettings || item.kind == Item::kFolder) {
            max = page.At(kRight, top + kBandHeight);
            DrawBand(dl, min, max, item.label, BandKind::kAction, skin_, selected, item.right, item.blocked);
        } else if (item.kind == Item::kFilter) {
            RowLook look;
            look.label = "Show";
            look.value = kFilterNames[int(filter_)];
            look.selected = selected;
            look.arrows = selected;
            DrawRow(dl, min, max, look, skin_);
        } else if (item.kind == Item::kEmpty) {
            PaintText(dl, page.At(kLeft + 20, top + 9), kPlain(text::kBody), color::kMuted, item.label);
        } else if (item.kind == Item::kUnmatched) {
            RowLook look;
            look.label = item.label;
            look.value = "Open";
            look.value_caps = true;
            look.action = true;
            look.selected = selected;
            DrawRow(dl, min, max, look, skin_);
        } else {
            const VideoLibrary::Song& s = library_.Songs()[size_t(item.index)];
            const State state = library_.StateOf(s);
            std::string status = StateText(state);
            if (const auto share = library_.FindingShare(s); share && *share > 0) {
                status += std::format(" {:.0f}%", *share * 100);
            }
            RowLook look;
            look.label = "";
            look.value = status;
            look.value_caps = true;
            look.selected = selected;
            look.arrows = selected && s.video >= 0;
            look.value_color = StateColor(state);
            DrawRow(dl, min, max, look, skin_);
            // the title and the artist, left of the status
            const float vw = TextWidth(kCaps(text::kValue), status);
            const float right = max.x - Px(look.arrows ? 40 : 22);
            const float room = (right - vw - Px(24)) - (min.x + Px(20));
            const float tw = PaintText(dl, ImVec2(min.x + Px(20), min.y + Px(7)), kPlain(text::kRow),
                                       selected ? color::kOnYellow : color::kText, s.title, room * 0.7f);
            PaintText(dl, ImVec2(min.x + Px(20) + tw + Px(12), min.y + Px(10)), kPlain(20),
                      selected ? IM_COL32(61, 58, 7, 255) : IM_COL32(198, 201, 212, 255), s.artist,
                      room - tw - Px(12));
        }
        if (free && Pickable(item.kind) && ImGui::IsMouseHoveringRect(min, max) &&
            ImGui::IsMouseHoveringRect(amin, amax)) {
            if (input_.MouseMoved()) selected_ = pick;
            if (input_.Clicked()) {
                selected_ = pick;
                Activate(item);
            }
        }
    }
    dl->PopClipRect();
    if (y > area) {
        dl->AddRectFilled(page.At(893, kTop), page.At(897, kBottom), IM_COL32(255, 255, 255, 20));
        const float h = std::max(area * area / y, 30.0f);
        const float t = kTop + (area - h) * (scroll_ / (y - area));
        dl->AddRectFilled(page.At(893, t), page.At(897, t + h), IM_COL32(255, 255, 255, 120), Px(2));
    }

    DrawPanel(dl, page, current);

    std::vector<HelpItem> help;
    const std::string back = host_.back_label ? host_.back_label() : std::string("Back");
    if (typing_link_) {
        help = {{HelpButton::kA, "Type"}, {HelpButton::kB, "Cancel"}, {HelpButton::kX, "Delete"},
                {HelpButton::kY, "Space"}};
    } else if (box_) {
        help = {{HelpButton::kA, "Select"}, {HelpButton::kB, "Back"}};
    } else if (current && current->kind == Item::kSong) {
        help = {{HelpButton::kA, "Options"}, {HelpButton::kB, back}, {HelpButton::kX, "Find this video"},
                {HelpButton::kY, "Sync again"}};
    } else if (current && current->kind == Item::kFindAll) {
        const video::YtDlpState yt = video::YtDlpNow(false);
        help = {{HelpButton::kA, "Select"}, {HelpButton::kB, back},
                {HelpButton::kY, !yt.checking && yt.version.empty() ? "Get yt-dlp" : "Update yt-dlp"}};
    } else {
        help = {{HelpButton::kA, "Select"}, {HelpButton::kB, back}, {HelpButton::kBumpers, "Letter"}};
    }
    const bool play = bool(host_.play) && !typing_link_ && !box_;
    const int hovered = DrawHelpBar(dl, ImVec2(0, page.At(0, 664).y), io.DisplaySize, help, input_.Keyboard(), play,
                                    skin_);
    if (box_) {
        const int option = DrawBox(dl, ImVec2(0, 0), io.DisplaySize, *box_, skin_);
        if (option >= 0 && input_.MouseMoved()) box_->selected = option;
        if (option >= 0 && input_.Clicked() && ImGui::GetFrameCount() != box_frame_) {
            box_->selected = option;
            Handle(Action::kSelect);
        }
    } else if (hovered >= 0 && input_.Clicked()) {
        if (hovered == kHelpPlay) {
            Handle(Action::kPlay);
        } else {
            switch (help[size_t(hovered)].button) {
            case HelpButton::kA: Handle(Action::kSelect); break;
            case HelpButton::kB: Handle(Action::kBack); break;
            case HelpButton::kX: Handle(Action::kSecondary); break;
            case HelpButton::kY: Handle(Action::kReset); break;
            case HelpButton::kBumpers: Handle(Action::kNextCategory); break;
            case HelpButton::kStart: Handle(Action::kPlay); break;
            }
        }
    }
    EndScreen();
}

void VideosScreen::DrawPanel(ImDrawList* dl, const Page& page, const Item* item) {
    // over the game, nothing of it shows through the panel's text
    if (over_game_) dl->AddRectFilled(page.At(906, 30), page.At(1256, 650), IM_COL32(3, 6, 20, 235));
    ui::DrawPanel(dl, page.At(906, 30), page.At(1256, 650), skin_);
    const float x = kBodyLeft, width = kBodyWidth;
    float y = 56;
    if (typing_link_) {
        const OnScreenKeyboard::Result r = DrawKeyboardPanel(
            dl, page, keyboard_,
            KeyboardPanel{.heading = "Paste a link",
                          .hint = "A video's page, as its address: band3 downloads it and syncs it with the song. "
                                  "A keyboard plugged in types too; Ctrl+V pastes.",
                          .x = x,
                          .y = y,
                          .width = width},
            skin_, input_.Clicked());
        if (r == OnScreenKeyboard::Result::kDone) Handle(Action::kPlay);
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            if (const char* clip = ImGui::GetClipboardText()) keyboard_.Type(clip);
        }
        return;
    }
    const ImU32 body = color::kBody;
    const auto heading = [&](std::string_view text, bool caps = true) {
        const TextStyle style = caps ? kCaps(text::kPanelHeading) : kPlain(text::kPanelHeading);
        const auto lines = WrapText(style, text, Px(width));
        for (size_t i = 0; i < lines.size() && i < 3; i++) {
            PaintText(dl, page.At(x, y), style, color::kText, lines[i], Px(width));
            y += text::kPanelHeading + 4;
        }
    };
    const auto rule = [&] {
        DrawRule(dl, page.At(x, y + 6), page.At(x + width, y + 10), skin_);
        y += 22;
    };
    const auto para = [&](std::string_view text, ImU32 color) {
        for (const std::string& line : WrapText(kPlain(text::kBody), text, Px(width))) {
            PaintText(dl, page.At(x, y), kPlain(text::kBody), color, line);
            y += text::kBodyLine;
        }
    };
    const auto kv = [&](std::string_view key, std::string_view value, ImU32 value_color) {
        PaintText(dl, page.At(x, y + 5), kCaps(20), color::kMuted, key);
        const float vw = std::min(TextWidth(kPlain(20), value), Px(width * 0.62f));
        PaintText(dl, ImVec2(page.At(x + width, 0).x - vw, page.At(0, y + 5).y), kPlain(20), value_color, value,
                  Px(width * 0.62f));
        dl->AddLine(page.At(x, y + 30), page.At(x + width, y + 30), color::kHairline);
        y += 30;
    };
    if (!item) return;
    switch (item->kind) {
    case Item::kFindAll: {
        heading("Find videos for every song");
        rule();
        const video::VideoJobsProgress p = video::VideoJobsNow();
        if (p.running || p.waiting > 0) {
            para(std::format("{} of {} songs done. Select it to pause or go on; Left stops it.", p.done, p.total), body);
            y += 8;
            if (p.total > 0) {
                dl->AddRectFilled(page.At(x, y), page.At(x + width, y + 6), IM_COL32(255, 255, 255, 31), Px(3));
                dl->AddRectFilled(page.At(x, y), page.At(x + width * float(p.done) / float(p.total), y + 6),
                                  color::kYellow, Px(3));
                y += 18;
            }
            kv("Found", std::to_string(p.synced), color::kGood);
            kv("To check", std::to_string(p.guessed), color::kText);
            kv("To review", std::to_string(p.review), color::kWarn);
            kv("None found", std::to_string(p.none), color::kText);
            kv("Failed", std::to_string(p.failed), color::kBad);
        } else if (item->blocked) {
            // what it needs first, as steps
            const video::YtDlpState yt = video::YtDlpNow(false);
            const size_t songs = library_.Songs().size();
            const auto step = [&](int number, std::string_view title, bool done, std::string_view detail) {
                const ImVec2 c = page.At(x + 13, y + 13);
                if (done) {
                    dl->AddCircleFilled(c, Px(13), color::kGood);
                } else {
                    dl->AddCircle(c, Px(12), color::kSoft, 0, Px(2));
                }
                const std::string n = std::to_string(number);
                PaintText(dl, ImVec2(c.x - TextWidth(kCaps(20), n) * 0.5f, c.y - Px(10)), kCaps(20),
                          done ? color::kOnYellow : color::kText, n);
                PaintText(dl, page.At(x + 38, y + 3), kCaps(20), done ? color::kMuted : color::kText, title,
                          Px(width - 38));
                y += 30;
                for (const std::string& line : WrapText(kPlain(text::kBody), detail, Px(width - 38))) {
                    PaintText(dl, page.At(x + 38, y), kPlain(text::kBody), done ? color::kMuted : body, line);
                    y += text::kBodyLine;
                }
                y += 12;
            };
            para("Two things first:", body);
            y += 10;
            step(1, "Start the game once", songs > 0,
                 songs > 0 ? Thousands(songs) + " songs listed."
                           : "band3 lists your songs once the game has read them.");
            step(2, "Get yt-dlp", !yt.version.empty(),
                 !yt.version.empty() ? "Found: " + yt.version
                                     : "The program that downloads the videos. Get yt-dlp, in the help bar, shows "
                                       "how to install it.");
        } else {
            const size_t count = library_.BatchSongs().size();
            para(std::format("band3 looks up a music video for each of the {} songs without one, downloads it, and "
                             "lines it up with the song. It keeps going while you play.",
                             Thousands(count)),
                 body);
            y += 10;
            const int height = REXCVAR_GET(music_video_max_height);
            kv("Download size", std::format("About {:.0f} GB", double(count) * MegabytesAt(height) / 1000.0),
               color::kText);
        }
        const video::YtDlpState yt = video::YtDlpNow(false);
        // the steps above already say whether it's there
        if (!item->blocked) {
            kv("yt-dlp", yt.checking ? "Looking..." : yt.updating ? "Updating..." : yt.version.empty() ? "Not found" : yt.version,
               yt.version.empty() && !yt.checking ? color::kBad : color::kText);
        }
        if (!yt.checking && !yt.version.empty() && !yt.js_runtime) {
            y += 8;
            para("yt-dlp needs a JavaScript runtime to fetch from YouTube: install Deno or Node.js, then start "
                 "band3 again.",
                 color::kWarn);
        }
        break;
    }
    case Item::kSettings:
        heading("Video settings");
        rule();
        para("Where videos are kept, how they fit the screen, and how big a picture to download. Opens Settings at "
             "Music Videos.",
             body);
        break;
    case Item::kFolder:
        heading("Open the videos folder");
        rule();
        para("Opens the folder music videos are kept in. Each is named after its song's shortname, with its offset in an .ini beside it.", body);
        break;
    case Item::kFilter:
        heading("Show");
        rule();
        para("Which songs the list shows. Left and right change it.", body);
        break;
    case Item::kEmpty: break;
    case Item::kLetter: break;
    case Item::kUnmatched: {
        const VideoLibrary::Video& v = library_.Videos()[size_t(item->index)];
        heading(rex::path_to_utf8(v.path.filename()), false);
        rule();
        para("No song band3 knows has the shortname " + v.stem +
                 ". A video is named after its song's shortname, which the log gives as the song starts: rename this "
                 "one to play it with its song.",
             body);
        break;
    }
    case Item::kSong: {
        const VideoLibrary::Song& s = library_.Songs()[size_t(item->index)];
        heading(s.title, false);
        if (!s.artist.empty()) {
            PaintText(dl, page.At(x, y), kPlain(22), IM_COL32(191, 195, 207, 255), s.artist, Px(width));
            y += 28;
        }
        rule();
        const State state = library_.StateOf(s);
        if (s.video >= 0) {
            const VideoLibrary::Video& v = library_.Videos()[size_t(s.video)];
            kv("Video", rex::path_to_utf8(v.path.filename()), color::kText);
            kv("Offset", v.offset ? std::format("{:+.0f} ms", *v.offset * 1000) : std::string("None yet"),
               color::kText);
            kv("Sync", library_.SyncingThis(v) ? "Syncing..." : StateText(state),
               library_.SyncingThis(v) ? color::kInfo : StateColor(state));
            if (!v.segments.empty()) kv("Parts", std::to_string(v.segments.size() + 1), color::kText);
            if (v.pinned) kv("Chosen", "By hand", color::kText);
            y += 10;
            para(input_.Keyboard() ? "Left and right move its offset 10 ms; R syncs it again."
                                   : "Left and right move its offset 10 ms; Y syncs it again.",
                 color::kMuted);
        } else {
            kv("Video", StateText(state), StateColor(state));
            y += 10;
            para("X looks for its video: a search's likeliest results have their sound matched with the song's, "
                 "and a sure one is taken.",
                 body);
        }
        if (const auto status = video::SongVideoStatusOf(s.shortname)) {
            // what a search came to, unless the video's been put there since
            const bool stale = s.video >= 0 && library_.Videos()[size_t(s.video)].written > status->when + 5;
            if (!stale) {
                y += 10;
                const ImU32 c = status->end == video::FetchEnd::kFailed ? color::kBad
                                : status->end == video::FetchEnd::kReview || status->end == video::FetchEnd::kGuessed
                                    ? color::kWarn
                                    : color::kMuted;
                std::string what = std::string(video::FetchEndText(status->end));
                if (!status->message.empty()) what += ": " + status->message;
                if (what.size() > 200) what = what.substr(0, 197) + "...";
                para(what, c);
                if (!status->candidates.empty()) {
                    y += 6;
                    para(std::format("{} results to choose from, in its options (A).", status->candidates.size()),
                         color::kMuted);
                }
            }
        }
        if (!s.in_package) {
            y += 10;
            para("One of the game's own songs: its sound is read from the game's archive where it's there; "
                 "otherwise its video syncs the first time the song plays.",
                 color::kMuted);
        }
        break;
    }
    }
}

}
