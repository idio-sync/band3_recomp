#include "videos_tab.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_misc.h>
#include <imgui.h>
#include <rex/filesystem.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>

#include "launcher_style.h"
#include "src/Content/game_song_list.h"
#include "src/Content/package_songs.h"
#include "src/Video/auto_sync.h"
#include "src/Video/music_video.h"
#include "src/Video/sync_cache.h"
#include "src/Video/video_files.h"
#include "src/Video/video_jobs.h"
#include "src/settings.h"

namespace band3::launcher {

struct VideosTab::Job {
    std::mutex mutex;
    std::filesystem::path video;
    bool done = false;
    std::optional<video::SyncResult> result;
    std::string error;
};

struct VideosTab::FilePick {
    std::mutex mutex;
    bool done = false;
    std::string song;  // the shortname it's for
    std::optional<std::string> file;
    std::string error;
};

namespace {

namespace fs = std::filesystem;
using video::FetchEnd;

// how often the folder is looked at again while the tab shows, and the songs
constexpr auto kVideosEvery = std::chrono::seconds(2);
constexpr auto kSongsEvery = std::chrono::seconds(10);

// a music video's size, in MB, for a few minutes at each height: for the
// batch's estimate only
int MegabytesAt(int height) {
    return height <= 480 ? 20 : height <= 720 ? 45 : height <= 1080 ? 90 : height <= 1440 ? 180 : 350;
}

constexpr const char* kFilters[] = {"All", "No video", "With a video", "To review", "Failed"};
// the filter after kFilters': the videos named after no song, listed instead
constexpr int kUnmatched = int(std::size(kFilters));
constexpr const char* kScopes[] = {"songs without a video", "songs to review", "songs that failed",
                                   "the songs shown, without a video"};

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool IsVideo(const fs::path& p) {
    const std::string ext = Lower(p.extension().string());
    return std::any_of(std::begin(video::kVideoExtensions), std::end(video::kVideoExtensions),
                       [&](std::string_view v) { return v == ext; });
}

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// `video`'s offsets as just saved: the playing video moves too, if it's this
// one (in game, behind the settings)
void ApplyIfPlaying(const fs::path& video) {
    uint64_t song = 0;
    const std::optional<video::VideoFile> playing = video::CurrentMusicVideo(song);
    std::error_code ec;
    if (!playing || !fs::equivalent(playing->path, video, ec)) return;
    const std::string ini = video::ReadIni(video);
    video::SetMusicVideoSegments(song, video::FindStartTime(ini).value_or(0.0),
                                 video::ParseSegments(video::FindIniValue(ini, "video_segments").value_or("")));
}

void Muted(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void Wrapped(const ImVec4& colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

int ResizeText(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(size_t(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

// a text field over a std::string; true when Enter is pressed in it
bool InputString(const char* id, const char* hint, std::string& text, float width) {
    ImGui::SetNextItemWidth(width);
    return ImGui::InputTextWithHint(id, hint, text.data(), text.capacity() + 1,
                                    ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_EnterReturnsTrue,
                                    ResizeText, &text);
}

std::string Minutes(double seconds) {
    const int s = int(std::lround(seconds));
    return std::format("{}:{:02}", s / 60, s % 60);
}

void OpenUrl(const std::string& url) { SDL_OpenURL(url.c_str()); }

void OpenFolder(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    OpenUrl("file:///" + rex::path_to_utf8(folder.generic_wstring()));
}

void SDLCALL OnFilePicked(void* userdata, const char* const* files, int) {
    auto* holder = static_cast<std::shared_ptr<VideosTab::FilePick>*>(userdata);
    {
        VideosTab::FilePick& pick = **holder;
        std::lock_guard lock(pick.mutex);
        pick.done = true;
        if (!files) pick.error = SDL_GetError();
        else if (files[0]) pick.file = files[0];
    }
    delete holder;
}

}

VideosTab::VideosTab() = default;
VideosTab::~VideosTab() = default;

void VideosTab::RefreshSongs() {
    songs_read_ = std::chrono::steady_clock::now();
    // the packages' songs as last read: those of the last run before the game boots
    std::vector<Song> songs;
    std::set<std::string> seen;
    for (const content::PackageSongs& package : content::PackageSongsNow(std::chrono::milliseconds(0))) {
        for (const content::DtaSong& s : package.songs) {
            // the first of a shortname is the one a video's named for
            if (s.shortname.empty() || !seen.insert(Lower(s.shortname)).second) continue;
            Song song;
            song.shortname = s.shortname;
            song.title = s.title.empty() ? s.shortname : s.title;
            song.artist = s.artist;
            song.length_ms = s.song_length;
            song.lower = Lower(song.title + " " + song.artist + " " + song.shortname);
            songs.push_back(std::move(song));
        }
    }
    // the game's own songs in none of the packages (the disc's, exports), as
    // it last listed them
    for (const content::ListedSong& s : content::LastGameSongs()) {
        if (!seen.insert(Lower(s.shortname)).second) continue;
        Song song;
        song.shortname = s.shortname;
        song.title = s.title.empty() ? s.shortname : s.title;
        song.artist = s.artist;
        song.length_ms = s.length_ms;
        song.origin = content::OriginName(s.origin);
        song.in_package = false;
        song.lower = Lower(song.title + " " + song.artist + " " + song.shortname + " " + song.origin);
        songs.push_back(std::move(song));
    }
    std::sort(songs.begin(), songs.end(), [](const Song& a, const Song& b) {
        return Lower(a.title) < Lower(b.title);
    });
    songs_ = std::move(songs);
    videos_read_ = {};  // matched to the videos again
}

void VideosTab::RefreshVideos(const std::vector<fs::path>& folders) {
    folders_ = folders;
    videos_read_ = std::chrono::steady_clock::now();
    // keep what's being typed in an offset field
    std::map<fs::path, int> typed;
    for (const Video& v : videos_) typed[v.path] = v.ms;
    videos_.clear();
    std::set<std::string> stems;
    for (const fs::path& folder : folders) {
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(folder, ec)) {
            if (!item.is_regular_file(ec) || !IsVideo(item.path())) continue;
            Video v;
            v.path = item.path();
            v.stem = rex::path_to_utf8(item.path().stem());
            // as FindVideo looks: the first folder's first
            if (!stems.insert(Lower(v.stem)).second) continue;
            const std::string ini = ReadText(video::IniFor(v.path));
            v.offset = video::FindStartTime(ini);
            // the file clock's time as the system's, through both clocks' now
            const auto written = fs::last_write_time(v.path, ec);
            v.written = std::chrono::duration_cast<std::chrono::seconds>(
                            (std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                 written - fs::file_time_type::clock::now()))
                                .time_since_epoch())
                            .count();
            v.segments = video::ParseSegments(video::FindIniValue(ini, "video_segments").value_or(""));
            v.pinned = video::FindIniValue(ini, "video_pinned").value_or("") == "1";
            v.source = video::FindIniValue(ini, "video_source").value_or("");
            v.result = video::LoadResult(video::ResultPath(v.path));
            v.ms = int(std::lround(v.offset.value_or(0.0) * 1000.0));
            if (ImGui::IsAnyItemActive())
                if (auto it = typed.find(v.path); it != typed.end()) v.ms = it->second;
            videos_.push_back(std::move(v));
        }
    }
    std::map<std::string, int> by_stem;
    for (size_t i = 0; i < videos_.size(); i++) by_stem[Lower(videos_[i].stem)] = int(i);
    std::set<int> matched;
    for (Song& s : songs_) {
        const auto it = by_stem.find(Lower(s.shortname));
        s.video = it == by_stem.end() ? -1 : it->second;
        if (s.video >= 0) matched.insert(s.video);
    }
    loose_.clear();
    for (int i = 0; i < int(videos_.size()); i++)
        if (!matched.count(i)) loose_.push_back(i);
}

void VideosTab::StartAlign(const Video& v) {
    job_ = std::make_shared<Job>();
    job_->video = v.path;
    job_message_.clear();
    std::thread([job = job_] {
        std::string error;
        const std::optional<video::Match> m = video::AlignVideo(job->video, {}, true, error);
        const std::optional<video::SyncResult> r = m ? std::optional<video::SyncResult>(m->result) : std::nullopt;
        std::lock_guard lock(job->mutex);
        job->result = r;
        job->error = error;
        job->done = true;
    }).detach();
}

void VideosTab::TakeAlign() {
    if (!job_) return;
    std::lock_guard lock(job_->mutex);
    if (!job_->done || !job_message_.empty()) return;
    const std::string name = rex::path_to_utf8(job_->video.stem());
    if (!job_->result) {
        job_message_ = name + ": " + job_->error;
    } else if (job_->result->confident) {
        job_message_ = job_->result->parts > 1
                           ? std::format("{}: {} parts, the first at {:+.3f} s, saved", name,
                                         job_->result->parts, job_->result->offset)
                           : std::format("{}: {:+.3f} s, saved", name, job_->result->offset);
        ApplyIfPlaying(job_->video);
    } else {
        job_message_ = std::format("{}: not sure (best guess {:+.3f} s), not saved", name,
                                   job_->result->offset);
    }
    videos_read_ = {};
}

void VideosTab::TakeFilePick(const std::vector<fs::path>& folders) {
    if (!pick_) return;
    std::optional<std::string> file;
    std::string song;
    {
        std::lock_guard lock(pick_->mutex);
        if (!pick_->done) return;
        file = pick_->file;
        song = pick_->song;
        if (!pick_->error.empty()) job_message_ = "The file dialog didn't open: " + pick_->error;
    }
    pick_.reset();
    if (!file || folders.empty()) return;
    for (const Song& s : songs_) {
        if (s.shortname != song) continue;
        video::VideoJob job;
        job.kind = video::VideoJob::Kind::kFile;
        job.song = {s.shortname, s.title, s.artist, s.length_ms};
        job.folder = folders.front();
        job.file = rex::to_path(*file);
        // a song with a video asks first, as DrawDetails does
        if (s.video >= 0) confirm_replace_ = std::move(job);
        else video::QueueVideoJobs({std::move(job)});
    }
}

bool VideosTab::Shown(const Song& s) const {
    if (!search_.empty() && s.lower.find(Lower(search_)) == std::string::npos) return false;
    if (filter_ == 0) return true;
    if (filter_ == kUnmatched) return false;
    if (filter_ == 1) return s.video < 0;
    if (filter_ == 2) return s.video >= 0;
    const auto end = video::SongVideoEndOf(s.shortname);
    if (filter_ == 3) return s.video < 0 && end == FetchEnd::kReview;
    return s.video < 0 && end == FetchEnd::kFailed;
}

VideosTab::Song* VideosTab::Selected() {
    for (Song& s : songs_)
        if (s.shortname == selected_) return &s;
    return nullptr;
}

VideosTab::Video* VideosTab::SelectedLoose() {
    if (selected_video_.empty()) return nullptr;
    for (int i : loose_)
        if (videos_[size_t(i)].path == selected_video_) return &videos_[size_t(i)];
    return nullptr;
}

void VideosTab::Select(const std::string& song, const fs::path& video) {
    selected_ = song;
    selected_video_ = video;
    confirm_delete_ = false;
    confirm_replace_.reset();
}

void VideosTab::DrawYtDlp() {
    // music_video_ytdlp_path changed: yt-dlp is looked for again
    const std::string setting = REXCVAR_GET(music_video_ytdlp_path);
    const bool changed = setting != ytdlp_setting_;
    ytdlp_setting_ = setting;
    const video::YtDlpState yt = video::YtDlpNow(changed);
    ImGui::AlignTextToFramePadding();
    if (yt.checking) {
        Muted("Looking for yt-dlp...");
    } else if (yt.updating) {
        Muted("Updating yt-dlp...");
    } else if (!yt.version.empty()) {
        ImGui::TextColored(yt.js_runtime ? kGood : kWarn, "yt-dlp %s", yt.version.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(yt.js_runtime ? "band3 finds and downloads videos with yt-dlp"
                                            : "No JavaScript runtime found: see below");
        ImGui::SameLine();
        if (ImGui::SmallButton("Update")) video::UpdateYtDlp();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs yt-dlp -U: YouTube changes often, and an old yt-dlp stops finding videos");
    } else {
        ImGui::TextColored(kBad, "yt-dlp not found");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", yt.problem.c_str());
    }
}

// what the top bar can't hold: yt-dlp's problems, the batch's progress, what
// it came to
void VideosTab::DrawStatus() {
    const video::YtDlpState yt = video::YtDlpNow(false);
    if (!yt.checking && !yt.updating) {
        if (yt.version.empty()) {
            Wrapped(kWarn, "yt-dlp can't be run (" + yt.problem +
                               "). band3 finds and downloads videos with it, but doesn't come with it: "
                               "set its program file in Settings (yt-dlp program), or put it on PATH.");
            if (ImGui::Button("Get yt-dlp")) OpenUrl("https://github.com/yt-dlp/yt-dlp#installation");
            ImGui::SameLine();
            if (ImGui::Button("Look again")) video::YtDlpNow(true);
        } else if (!yt.js_runtime) {
            Wrapped(kWarn, "yt-dlp needs a JavaScript runtime to fetch from YouTube, and none was found: "
                           "without one, YouTube refuses some videos (HTTP 403). Install Deno, or "
                           "Node.js, and start band3 again.");
            if (ImGui::Button("Get Deno")) OpenUrl("https://docs.deno.com/runtime/getting_started/installation/");
        }
    }
    if (!yt.update_output.empty() && !yt.updating) {
        const size_t last = yt.update_output.rfind('\n');
        Muted(yt.update_output.substr(last == std::string::npos ? 0 : last + 1).c_str());
    }
    const video::VideoJobsProgress p = video::VideoJobsNow();
    if (p.running || p.waiting > 0) {
        std::string what = std::format("{} of {} songs", p.done, p.total);
        if (p.waiting_for_song) what += ": waiting for the song being played to end";
        else if (p.paused && !p.paused_why.empty()) what += ": paused, as " + p.paused_why;
        else if (p.paused) what += ": paused";
        else if (p.running)
            what += std::format(": {}, {}{}", p.song, video::FetchStageText(p.stage),
                                p.share > 0 ? std::format(" {:.0f}%", p.share * 100) : "");
        // yt-dlp's reasons run long
        if (what.size() > 220) what = what.substr(0, 217) + "...";
        Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), what);
    }
    if (p.total > 0 && (p.synced || p.guessed || p.review || p.none || p.failed)) {
        Muted(std::format("{} found, {} to check, {} to review, {} with none found, {} failed", p.synced,
                          p.guessed, p.review, p.none, p.failed)
                  .c_str());
    }
}

void VideosTab::DrawBatch(const std::vector<fs::path>& folders) {
    const video::VideoJobsProgress p = video::VideoJobsNow();
    if (p.running || p.waiting > 0) {
        ImGui::ProgressBar(p.total ? float(p.done) / float(p.total) : 0.0f, ImVec2(Px(200), 0),
                           std::format("{} of {}", p.done, p.total).c_str());
        ImGui::SameLine();
        if (ImGui::Button(p.paused ? "Go on" : "Pause")) video::PauseVideoJobs(!p.paused);
        ImGui::SameLine();
        if (ImGui::Button("Stop")) video::StopVideoJobs();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Find videos for");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(290));
        ImGui::Combo("##scope", &scope_, kScopes, int(std::size(kScopes)));
        ImGui::SameLine();
        if (ImGui::Button("Find them...")) ImGui::OpenPopup("Find videos");
    }

    if (ImGui::BeginPopupModal("Find videos", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::vector<const Song*> chosen;
        for (const Song& s : songs_) {
            if (s.video >= 0 || video::SongVideoQueued(s.shortname)) continue;
            const auto end = video::SongVideoEndOf(s.shortname);
            const bool pick = scope_ == 0   ? end != FetchEnd::kNoVideo && end != FetchEnd::kReview
                              : scope_ == 1 ? end == FetchEnd::kReview
                              : scope_ == 2 ? end == FetchEnd::kFailed
                                            : Shown(s);
            if (pick) chosen.push_back(&s);
        }
        const int height = REXCVAR_GET(music_video_max_height);
        const double gb = double(chosen.size()) * MegabytesAt(height) / 1000.0;
        ImGui::TextUnformatted(std::format("Find music videos for {} songs?", chosen.size()).c_str());
        Muted(std::format("At {}p that's about {:.1f} GB, a song at a time; it goes on in the background, "
                          "and waits while a song is being played.",
                          height, gb)
                  .c_str());
        if (scope_ == 0) Muted("Songs a search found nothing for, or left for you to review, are left out.");
        ImGui::Dummy(ImVec2(0, Px(6)));
        ImGui::BeginDisabled(chosen.empty() || folders.empty());
        if (ImGui::Button("Find them")) {
            std::vector<video::VideoJob> jobs;
            for (const Song* s : chosen) {
                video::VideoJob job;
                job.song = {s->shortname, s->title, s->artist, s->length_ms};
                job.folder = folders.front();
                jobs.push_back(std::move(job));
            }
            video::QueueVideoJobs(std::move(jobs));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// yt-dlp and the batch on the left; the folder, the settings and the help on the right
void VideosTab::DrawTopBar(const std::vector<fs::path>& folders, const Page& page) {
    DrawYtDlp();
    ImGui::SameLine(0, Px(28));
    DrawBatch(folders);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = ButtonWidth("Videos folder") + ButtonWidth("Settings...") + ButtonWidth("?") +
                        style.ItemSpacing.x * 2;
    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail >= right) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - right);
    else ImGui::NewLine();
    ImGui::BeginDisabled(folders.empty());
    if (ImGui::Button("Videos folder")) OpenFolder(folders.front());
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Open the folder music videos are kept in (music_videos_folder)");
    ImGui::SameLine();
    if (ImGui::Button("Settings...")) ImGui::OpenPopup("Video settings");
    ImGui::SameLine();
    ImGui::Button("?");
    if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        ImGui::PushTextWrapPos(Px(560));
        ImGui::TextUnformatted(
            "Each video is named after its song's shortname, with its offset (Clone Hero's "
            "video_start_time, the video's time at the song's start) in an .ini beside it. Auto-sync "
            "finds the offset from the song's sound; [ and ] move it 50 ms while the video plays.\n\n"
            "Videos are found with yt-dlp: a search's likeliest results have their sound matched with "
            "the song's, and a sure one is taken; or choose one, paste a link, or pick a file.\n\n"
            "Pick a song on the left (the arrow keys move through the list) to see its video.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    DrawSettingsPopup(page);
}

// the tab's settings, over the list, with the description the footer would give
void VideosTab::DrawSettingsPopup(const Page& page) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(viewport->WorkSize.x * 0.85f, Px(1100)), viewport->WorkSize.y * 0.85f),
                             ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Video settings", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float footer = page.description_height + ImGui::GetFrameHeight() + style.ItemSpacing.y * 3;
    if (ImGui::BeginChild("##video_settings", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened))
        page.settings();
    ImGui::EndChild();
    ImGui::Separator();
    page.description();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void VideosTab::DrawFilters() {
    const float combo = Px(200);
    InputString("##search", "Search songs", search_,
                std::max(ImGui::GetContentRegionAvail().x - combo - ImGui::GetStyle().ItemSpacing.x, Px(120)));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo);
    const std::string unmatched = std::format("Unmatched videos ({})", loose_.size());
    const char* names[std::size(kFilters) + 1];
    std::copy(std::begin(kFilters), std::end(kFilters), names);
    names[std::size(kFilters)] = unmatched.c_str();
    ImGui::Combo("##filter", &filter_, names, int(std::size(names)));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Unmatched videos are named after no song band3 knows: misnamed, most likely");
}

void VideosTab::DrawList() {
    DrawFilters();
    if (filter_ == kUnmatched) {
        DrawLooseList();
        return;
    }
    std::vector<int> shown;
    for (int i = 0; i < int(songs_.size()); i++)
        if (Shown(songs_[i])) shown.push_back(i);
    if (songs_.empty()) {
        ImGui::PushTextWrapPos(0);
        Muted("No songs yet: band3 lists the game's songs once the game has read them (start the "
              "game once).");
        ImGui::PopTextWrapPos();
        return;
    }
    const float row = ImGui::GetFrameHeight();
    if (!ImGui::BeginTable("songs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Song", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Video", ImGuiTableColumnFlags_WidthStretch, 1.3f);
    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(80));
    ImGui::TableHeadersRow();
    const video::VideoJobsProgress p = video::VideoJobsNow();
    ImGuiListClipper clipper;
    clipper.Begin(int(shown.size()), row);
    while (clipper.Step()) {
        for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; k++) {
            const Song& s = songs_[size_t(shown[size_t(k)])];
            ImGui::PushID(s.shortname.c_str());
            ImGui::TableNextRow(0, row);
            ImGui::TableSetColumnIndex(0);
            std::string label = s.artist.empty() ? s.title : s.title + "  -  " + s.artist;
            if (!s.in_package && !s.origin.empty()) label += "   (" + s.origin + ")";
            // centred in the row, as the other columns' text is; the arrow
            // keys pick the song they move to
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Selectable(label.c_str(), selected_ == s.shortname,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnNav,
                                  ImVec2(0, row)) &&
                selected_ != s.shortname)
                Select(s.shortname, {});
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered() && ImGui::CalcTextSize(label.c_str()).x > ImGui::GetItemRectSize().x)
                ImGui::SetTooltip("%s", label.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            if (p.running && p.song == s.shortname) {
                ImGui::TextColored(kAccent, "%s...", std::string(video::FetchStageText(p.stage)).c_str());
            } else if (video::SongVideoQueued(s.shortname)) {
                Muted("Waiting to be found");
            } else if (s.video >= 0) {
                const Video& v = videos_[size_t(s.video)];
                const std::string parts =
                    v.segments.empty() ? "" : std::format(", {} parts", v.segments.size() + 1);
                if (v.result && v.result->confident && v.offset && std::abs(*v.offset - v.result->offset) < 0.0015)
                    ImGui::TextColored(kGood, "%s", (std::string(v.pinned ? "Synced (chosen" : "Synced (auto") +
                                                     parts + ")").c_str());
                else if (v.result && !v.result->confident && !v.offset)
                    ImGui::TextColored(kWarn, "Not sure");
                else if (v.offset)
                    ImGui::TextUnformatted(v.pinned ? "Set by hand (chosen)" : "Set by hand");
                else
                    Muted("Not synced yet");
            } else if (const auto end = video::SongVideoEndOf(s.shortname)) {
                if (*end == FetchEnd::kReview) ImGui::TextColored(kWarn, "To review");
                else if (*end == FetchEnd::kFailed) ImGui::TextColored(kBad, "Failed");
                else if (*end == FetchEnd::kNoVideo) Muted("None found");
                else Muted("No video");
            } else {
                Muted("No video");
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::AlignTextToFramePadding();
            if (s.video >= 0 && videos_[size_t(s.video)].offset)
                ImGui::TextUnformatted(std::format("{:+} ms", videos_[size_t(s.video)].ms).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

// the videos named after no song, to pick as songs are
void VideosTab::DrawLooseList() {
    if (loose_.empty()) {
        ImGui::PushTextWrapPos(0);
        Muted("Every video in the folder is named after a song band3 knows.");
        ImGui::PopTextWrapPos();
        return;
    }
    const float row = ImGui::GetFrameHeight();
    if (!ImGui::BeginTable("loose", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                           ImVec2(0, ImGui::GetContentRegionAvail().y)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Video", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(80));
    ImGui::TableHeadersRow();
    for (int i : loose_) {
        const Video& v = videos_[size_t(i)];
        const std::string name = rex::path_to_utf8(v.path.filename());
        ImGui::PushID(name.c_str());
        ImGui::TableNextRow(0, row);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        if (ImGui::Selectable(name.c_str(), selected_video_ == v.path,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnNav,
                              ImVec2(0, row)) &&
            selected_video_ != v.path)
            Select({}, v.path);
        ImGui::PopStyleVar();
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        if (v.offset) ImGui::TextUnformatted(std::format("{:+} ms", v.ms).c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void VideosTab::DrawVideoControls(Video& v) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(v.segments.empty() ? "Offset (ms)" : "Part 1 (ms)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(170));
    ImGui::InputInt("##ms", &v.ms, 50, 500);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        video::WriteStartTime(v.path, v.ms / 1000.0);
        ApplyIfPlaying(v.path);
        v.offset = v.ms / 1000.0;
    }
    ImGui::SameLine();
    const bool aligning = job_ && job_->video == v.path && !job_->done;
    ImGui::BeginDisabled(job_ && !job_->done);
    if (ImGui::Button(aligning ? "Aligning..." : "Align")) StartAlign(v);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Match the video's sound with its song's, read from the song's package, or "
                          "the game's archive for its own songs");
    if (v.result && !v.result->confident) {
        ImGui::SameLine();
        if (ImGui::Button("Use the guess")) {
            // one offset throughout
            video::WriteIniValues(v.path, {{"video_start_time", std::to_string(std::lround(v.result->offset * 1000.0))},
                                           {"video_segments", std::nullopt}});
            v.segments.clear();
            ApplyIfPlaying(v.path);
            v.offset = v.result->offset;
            v.ms = int(std::lround(v.result->offset * 1000.0));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", std::format("Auto-sync's best guess, {:+.3f} s, which it wasn't sure of",
                                                v.result->offset)
                                        .c_str());
    }
    // what its last Align came to
    if (job_ && job_->video == v.path && !job_message_.empty()) Wrapped(kMuted, job_message_);
}

// a video cut otherwise than its song: each later part's offset, or its gap
void VideosTab::DrawParts(Video& v) {
    for (size_t k = 0; k < v.segments.size(); k++) {
        video::VideoSegment& seg = v.segments[k];
        ImGui::PushID(int(k));
        ImGui::AlignTextToFramePadding();
        const int at = int(std::lround(seg.song_start));
        ImGui::TextUnformatted(std::format("Part {} from {}:{:02}", k + 2, at / 60, at % 60).c_str());
        ImGui::SameLine(Px(190));
        bool changed = false;
        if (seg.offset) {
            int ms = int(std::lround(*seg.offset * 1000.0));
            ImGui::SetNextItemWidth(Px(170));
            ImGui::InputInt("##part", &ms, 50, 500);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                seg.offset = ms / 1000.0;
                changed = true;
            }
        } else {
            ImGui::SetNextItemWidth(Px(170));
            Muted("No video (black)");
        }
        ImGui::SameLine();
        if (ImGui::Button("Merge into the part before")) {
            v.segments.erase(v.segments.begin() + long(k));
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Play this part at the offset of the part before it: for a change of "
                              "offset that wasn't one");
        ImGui::PopID();
        if (changed) {
            video::WriteIniValues(v.path, {{"video_segments", v.segments.empty()
                                                                  ? std::nullopt
                                                                  : std::optional<std::string>(video::FormatSegments(v.segments))}});
            ApplyIfPlaying(v.path);
            break;
        }
    }
}

// Delete, with a second press to be sure: the video, its .ini and what
// auto-sync kept of both
void VideosTab::DrawDelete(Video& v, const std::string& song) {
    if (!confirm_delete_) {
        if (ImGui::Button("Delete it")) confirm_delete_ = true;
        return;
    }
    Wrapped(kWarn, "Delete " + rex::path_to_utf8(v.path.filename()) + " and its offset?");
    if (ImGui::Button("Delete")) {
        std::error_code ec;
        for (const fs::path& p : {v.path, video::IniFor(v.path), video::SongEnvelopePath(v.path),
                                  video::VideoEnvelopePath(v.path), video::ResultPath(v.path)})
            fs::remove(p, ec);
        if (!song.empty()) video::ForgetSongVideoStatus(song);
        confirm_delete_ = false;
        videos_read_ = {};
    }
    ImGui::SameLine();
    if (ImGui::Button("Keep it")) confirm_delete_ = false;
}

void VideosTab::DrawDetails(const std::vector<fs::path>& folders) {
    if (Video* loose = SelectedLoose()) {
        DrawLooseDetails(*loose);
        return;
    }
    Song* s = Selected();
    if (!s) {
        ImGui::PushTextWrapPos(0);
        Muted("Pick a song on the left to find its video, choose one, or set its offset.");
        ImGui::PopTextWrapPos();
        return;
    }
    const video::SongInfo info{s->shortname, s->title, s->artist, s->length_ms};
    ImGui::PushID("details");
    {
        FontScope font(kHeadingSize);
        Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), s->title);
    }
    if (!s->artist.empty()) ImGui::TextUnformatted(s->artist.c_str());
    Muted(std::format("{}{}{}", s->shortname,
                      s->length_ms ? ", " + Minutes(s->length_ms / 1000.0) : std::string(),
                      s->in_package ? std::string() : ", " + (s->origin.empty() ? "the game's" : s->origin))
              .c_str());
    if (!s->in_package)
        Wrapped(kMuted, "One of the game's own songs: its audio is read from the game's archive where it's "
                        "there (the disc's songs); otherwise its video syncs the first time the song plays.");
    const bool queued = video::SongVideoQueued(s->shortname);
    // a song with a video asks first: the old one stays till the new one's in
    const auto queue = [&](video::VideoJob job) {
        job.song = info;
        job.folder = folders.front();
        if (s->video >= 0) confirm_replace_ = std::move(job);
        else video::QueueVideoJobs({std::move(job)});
    };

    ImGui::Dummy(ImVec2(0, Px(4)));
    ImGui::SeparatorText("Video");
    if (s->video >= 0) {
        Video& v = videos_[size_t(s->video)];
        Wrapped(kMuted, rex::path_to_utf8(v.path.filename()) +
                            (v.pinned ? ", chosen by hand: searches for many songs leave it alone" : ""));
        DrawVideoControls(v);
        DrawParts(v);
        if (!v.source.empty()) {
            if (ImGui::Button("Open its page")) OpenUrl(v.source);
            ImGui::SameLine();
        }
        DrawDelete(v, s->shortname);
    } else {
        Muted("None yet.");
    }

    if (confirm_replace_ && confirm_replace_->song.shortname == s->shortname && s->video >= 0) {
        const std::string what = confirm_replace_->kind == video::VideoJob::Kind::kFind   ? "the best a search finds"
                                 : confirm_replace_->kind == video::VideoJob::Kind::kLink ? "the linked video"
                                                                                          : "the file";
        ImGui::Dummy(ImVec2(0, Px(4)));
        Wrapped(kWarn, "Replace " + rex::path_to_utf8(videos_[size_t(s->video)].path.filename()) + " with " +
                           what + "? The old one stays until the new one's in.");
        if (ImGui::Button("Replace it")) {
            video::QueueVideoJobs({std::move(*confirm_replace_)});
            confirm_replace_.reset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep the old one")) confirm_replace_.reset();
    }

    // finding one: a search, a link, or a file
    ImGui::Dummy(ImVec2(0, Px(4)));
    ImGui::SeparatorText(s->video >= 0 ? "Find another" : "Find one");
    ImGui::BeginDisabled(queued || folders.empty());
    if (ImGui::Button(s->video >= 0 ? "Search for another" : "Search for its video")) {
        video::VideoJob job;
        queue(std::move(job));
    }
    ImGui::SameLine();
    if (ImGui::Button("Choose a file...") && !pick_) {
        pick_ = std::make_shared<FilePick>();
        pick_->song = s->shortname;
        static constexpr SDL_DialogFileFilter kVideoFiles[] = {
            {"Videos", "mp4;m4v;mov;mkv;webm;avi;wmv"}};
        auto* holder = new std::shared_ptr<FilePick>(pick_);
        SDL_ShowOpenFileDialog(OnFilePicked, holder, nullptr, kVideoFiles, 1, nullptr, false);
    }
    if (queued) {
        ImGui::SameLine();
        Muted("(under way)");
    }
    const float get = ButtonWidth("Get it");
    const bool enter = InputString("##link", "or paste a link to a video", link_,
                                   std::max(ImGui::GetContentRegionAvail().x - get - ImGui::GetStyle().ItemSpacing.x,
                                            Px(160)));
    ImGui::SameLine();
    ImGui::BeginDisabled(link_.empty());
    if (ImGui::Button("Get it") || (enter && !link_.empty())) {
        video::VideoJob job;
        job.kind = video::VideoJob::Kind::kLink;
        job.link = link_;
        queue(std::move(job));
        link_.clear();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // what its last search came to, and its results to choose from
    if (const auto status = video::SongVideoStatusOf(s->shortname)) {
        const ImVec4 colour = status->end == FetchEnd::kFailed ? kBad
                              : status->end == FetchEnd::kReview || status->end == FetchEnd::kGuessed
                                  ? kWarn
                                  : kMuted;
        // what a search came to, unless the video's been put there since
        if (s->video < 0 || videos_[size_t(s->video)].written <= status->when + 5)
            Wrapped(colour, std::string(video::FetchEndText(status->end)) +
                                (status->message.empty() ? "" : ": " + status->message));
        if (!status->candidates.empty() &&
            ImGui::BeginTable("results", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, Px(80));
            ImGui::TableSetupColumn("Why", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                    ButtonWidth("Use") + ButtonWidth("Open") + Px(12));
            ImGui::TableHeadersRow();
            for (const video::TriedCandidate& t : status->candidates) {
                const video::YtCandidate& c = t.candidate.video;
                ImGui::PushID(c.url.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(c.title.c_str());
                ImGui::PopTextWrapPos();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", std::format("{}  ({} views)  score {:.2f}", c.channel,
                                                        c.views >= 0 ? std::to_string(c.views) : "?",
                                                        t.candidate.score)
                                                .c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                std::string length = c.duration > 0 ? Minutes(c.duration) : "?";
                if (c.duration > 0 && s->length_ms > 0) {
                    const double d = c.duration - s->length_ms / 1000.0;
                    if (std::abs(d) >= 1) length += std::format(" ({:+.0f})", d);
                }
                ImGui::TextUnformatted(length.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::AlignTextToFramePadding();
                std::string why = !t.problem.empty() ? t.problem : t.candidate.note;
                if (t.match && t.match->confident) {
                    why = std::format("its sound matches at {:+.3f} s", t.match->offset);
                    // and yet it wasn't taken
                    if (!t.problem.empty()) why += ", but " + t.problem;
                }
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(t.match && t.match->confident ? kGood : kMuted, "%s",
                                   why.empty() ? c.channel.c_str() : why.c_str());
                ImGui::PopTextWrapPos();
                ImGui::TableSetColumnIndex(3);
                ImGui::BeginDisabled(queued || folders.empty());
                if (ImGui::Button("Use")) {
                    video::VideoJob job;
                    job.kind = video::VideoJob::Kind::kLink;
                    job.link = c.url;
                    queue(std::move(job));
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Open")) OpenUrl(c.url);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::PopID();
}

void VideosTab::DrawLooseDetails(Video& v) {
    ImGui::PushID("loose");
    {
        FontScope font(kHeadingSize);
        Wrapped(ImGui::GetStyleColorVec4(ImGuiCol_Text), rex::path_to_utf8(v.path.filename()));
    }
    Wrapped(kMuted, "No song band3 knows has the shortname " + v.stem +
                        ". A video is named after its song's shortname, which the log gives as the song "
                        "starts: rename this one to play it with its song.");
    ImGui::Dummy(ImVec2(0, Px(4)));
    ImGui::SeparatorText("Video");
    DrawVideoControls(v);
    DrawParts(v);
    DrawDelete(v, {});
    ImGui::PopID();
}

void VideosTab::Draw(const std::vector<fs::path>& folders, const Page& page) {
    TakeAlign();
    TakeFilePick(folders);
    const auto now = std::chrono::steady_clock::now();
    if (now - songs_read_ > kSongsEvery) RefreshSongs();
    if (folders != folders_ || (now - videos_read_ > kVideosEvery && !ImGui::IsAnyItemActive()))
        RefreshVideos(folders);
    // a job just ended: its song's video is looked for now
    const video::VideoJobsProgress p = video::VideoJobsNow();
    if (p.done != jobs_done_) {
        jobs_done_ = p.done;
        videos_read_ = {};
    }

    DrawTopBar(folders, page);
    DrawStatus();
    ImGui::Dummy(ImVec2(0, Px(2)));

    // the list and the song picked, side by side where there's room, each
    // scrolling on its own, filling what's left of the tab
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float height = std::max(avail.y - style.ItemSpacing.y, Px(260));
    const ImGuiChildFlags flags = ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened;
    const ImGuiWindowFlags details_flags = ImGuiWindowFlags_None;
    if (avail.x >= Px(900)) {
        const float list_width = std::floor(avail.x * 0.46f);
        if (ImGui::BeginChild("##list", ImVec2(list_width, height), flags)) DrawList();
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##details", ImVec2(0, height), flags, details_flags)) DrawDetails(folders);
        ImGui::EndChild();
    } else {
        const float list_height = std::floor(height * 0.45f);
        if (ImGui::BeginChild("##list", ImVec2(0, list_height), flags)) DrawList();
        ImGui::EndChild();
        if (ImGui::BeginChild("##details", ImVec2(0, std::max(height - list_height - style.ItemSpacing.y, Px(120))),
                              flags, details_flags))
            DrawDetails(folders);
        ImGui::EndChild();
    }
}

}
