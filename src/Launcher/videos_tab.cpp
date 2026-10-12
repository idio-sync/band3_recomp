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

// a new offset for `video`, saved by the caller: the playing video moves too,
// if it's this one (in game, behind the settings)
void ApplyIfPlaying(const fs::path& video, double seconds) {
    uint64_t song = 0;
    const std::optional<video::VideoFile> playing = video::CurrentMusicVideo(song);
    std::error_code ec;
    if (playing && fs::equivalent(playing->path, video, ec)) video::SetMusicVideoStartTime(song, seconds);
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
        std::optional<video::SyncResult> r = video::AlignVideo(job->video, {}, true, error);
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
        job_message_ = std::format("{}: {:+.3f} s, saved", name, job_->result->offset);
        ApplyIfPlaying(job_->video, job_->result->offset);
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
        video::QueueVideoJobs({std::move(job)});
    }
}

bool VideosTab::Shown(const Song& s) const {
    if (!search_.empty() && s.lower.find(Lower(search_)) == std::string::npos) return false;
    if (filter_ == 0) return true;
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
        ImGui::TextColored(kGood, "yt-dlp %s", yt.version.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Update yt-dlp")) video::UpdateYtDlp();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs yt-dlp -U: YouTube changes often, and an old yt-dlp stops finding videos");
        if (!yt.js_runtime) {
            Wrapped(kWarn, "yt-dlp needs a JavaScript runtime to fetch from YouTube, and none was found: "
                           "without one, YouTube refuses some videos (HTTP 403). Install Deno, or "
                           "Node.js, and start band3 again.");
            if (ImGui::Button("Get Deno")) OpenUrl("https://docs.deno.com/runtime/getting_started/installation/");
        }
    } else {
        Wrapped(kWarn, "yt-dlp can't be run (" + yt.problem +
                           "). band3 finds and downloads videos with it, but doesn't come with it: "
                           "set its program file below (yt-dlp program), or put it on PATH.");
        if (ImGui::Button("Get yt-dlp")) OpenUrl("https://github.com/yt-dlp/yt-dlp#installation");
        ImGui::SameLine();
        if (ImGui::Button("Look again")) video::YtDlpNow(true);
    }
    if (!yt.update_output.empty() && !yt.updating) {
        const size_t last = yt.update_output.rfind('\n');
        Muted(yt.update_output.substr(last == std::string::npos ? 0 : last + 1).c_str());
    }
}

void VideosTab::DrawBatch(const std::vector<fs::path>& folders) {
    const video::VideoJobsProgress p = video::VideoJobsNow();
    const bool busy = p.running || p.waiting > 0;
    if (busy) {
        std::string what = std::format("{} of {} songs", p.done, p.total);
        if (p.waiting_for_song) what += ": waiting for the song being played to end";
        else if (p.paused) what += ": paused";
        else if (p.running)
            what += std::format(": {}, {}{}", p.song, video::FetchStageText(p.stage),
                                p.share > 0 ? std::format(" {:.0f}%", p.share * 100) : "");
        ImGui::ProgressBar(p.total ? float(p.done) / float(p.total) : 0.0f, ImVec2(-1, 0), "");
        ImGui::TextUnformatted(what.c_str());
        if (ImGui::Button(p.paused ? "Go on" : "Pause")) video::PauseVideoJobs(!p.paused);
        ImGui::SameLine();
        if (ImGui::Button("Stop")) video::StopVideoJobs();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Find videos for");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Px(330));
        ImGui::Combo("##scope", &scope_, kScopes, int(std::size(kScopes)));
        ImGui::SameLine();
        if (ImGui::Button("Find them...")) ImGui::OpenPopup("Find videos");
    }
    if (p.total > 0 && (p.synced || p.guessed || p.review || p.none || p.failed)) {
        Muted(std::format("{} found, {} to check, {} to review, {} with none found, {} failed", p.synced,
                          p.guessed, p.review, p.none, p.failed)
                  .c_str());
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

void VideosTab::DrawFilters() {
    InputString("##search", "Search songs", search_, Px(300));
    for (int i = 0; i < int(std::size(kFilters)); i++) {
        ImGui::SameLine();
        if (ImGui::RadioButton(kFilters[i], filter_ == i)) filter_ = i;
    }
}

void VideosTab::DrawSongs() {
    std::vector<int> shown;
    for (int i = 0; i < int(songs_.size()); i++)
        if (Shown(songs_[i])) shown.push_back(i);
    if (songs_.empty()) {
        Muted("No songs yet: band3 lists the songs in its packages once the game has read them (start "
              "the game once).");
        return;
    }
    const float row = ImGui::GetFrameHeight();
    const float height = std::min(float(std::max<size_t>(shown.size(), 3)) * row, row * 14) + row + Px(4);
    if (!ImGui::BeginTable("songs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                           ImVec2(0, height)))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Song", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Video", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, Px(90));
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(int(shown.size()), row);
    while (clipper.Step()) {
        for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; k++) {
            const Song& s = songs_[size_t(shown[size_t(k)])];
            ImGui::PushID(s.shortname.c_str());
            ImGui::TableNextRow(0, row);
            ImGui::TableSetColumnIndex(0);
            const std::string label = s.artist.empty() ? s.title : s.title + "  -  " + s.artist;
            // centred in the row, as the other columns' text is
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
            if (ImGui::Selectable(label.c_str(), selected_ == s.shortname,
                                  ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, row))) {
                selected_ = s.shortname;
                confirm_delete_ = false;
            }
            ImGui::PopStyleVar();
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            const video::VideoJobsProgress p = video::VideoJobsNow();
            if (p.running && p.song == s.shortname) {
                ImGui::TextColored(kAccent, "%s...", std::string(video::FetchStageText(p.stage)).c_str());
            } else if (video::SongVideoQueued(s.shortname)) {
                Muted("Waiting to be found");
            } else if (s.video >= 0) {
                const Video& v = videos_[size_t(s.video)];
                if (v.result && v.result->confident && v.offset && std::abs(*v.offset - v.result->offset) < 0.0015)
                    ImGui::TextColored(kGood, v.pinned ? "Synced (chosen)" : "Synced");
                else if (v.result && !v.result->confident && !v.offset)
                    ImGui::TextColored(kWarn, "Not sure: see below");
                else if (v.offset)
                    ImGui::TextUnformatted(v.pinned ? "Set by hand (chosen)" : "Set by hand");
                else
                    Muted("Not synced yet");
            } else if (const auto end = video::SongVideoEndOf(s.shortname)) {
                if (*end == FetchEnd::kReview) ImGui::TextColored(kWarn, "To review");
                else if (*end == FetchEnd::kFailed) ImGui::TextColored(kBad, "Failed");
                else if (*end == FetchEnd::kNoVideo) Muted("No music video found");
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

void VideosTab::DrawVideoControls(Video& v) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Offset (ms)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Px(170));
    ImGui::InputInt("##ms", &v.ms, 50, 500);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        video::WriteStartTime(v.path, v.ms / 1000.0);
        ApplyIfPlaying(v.path, v.ms / 1000.0);
        v.offset = v.ms / 1000.0;
    }
    ImGui::SameLine();
    const bool aligning = job_ && job_->video == v.path && !job_->done;
    ImGui::BeginDisabled(job_ && !job_->done);
    if (ImGui::Button(aligning ? "Aligning..." : "Align")) StartAlign(v);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Match the video's sound with its song's, read from the song's package (an "
                          "encrypted one needs the song played once)");
    if (v.result && !v.result->confident) {
        ImGui::SameLine();
        if (ImGui::Button("Use the guess")) {
            video::WriteStartTime(v.path, v.result->offset);
            ApplyIfPlaying(v.path, v.result->offset);
            v.offset = v.result->offset;
            v.ms = int(std::lround(v.result->offset * 1000.0));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", std::format("Auto-sync's best guess, {:+.3f} s, which it wasn't sure of",
                                                v.result->offset)
                                        .c_str());
    }
}

void VideosTab::DrawDetails(const std::vector<fs::path>& folders) {
    Song* s = Selected();
    if (!s) {
        Muted("Pick a song above to find its video, choose one, or set its offset.");
        return;
    }
    const video::SongInfo info{s->shortname, s->title, s->artist, s->length_ms};
    ImGui::PushID("details");
    ImGui::TextUnformatted(std::format("{} - {}", s->title, s->artist).c_str());
    ImGui::SameLine();
    Muted(std::format("({}{})", s->shortname,
                      s->length_ms ? ", " + Minutes(s->length_ms / 1000.0) : std::string())
              .c_str());
    const bool queued = video::SongVideoQueued(s->shortname);
    const auto queue = [&](video::VideoJob job) {
        job.song = info;
        job.folder = folders.front();
        video::QueueVideoJobs({std::move(job)});
    };

    if (s->video >= 0) {
        Video& v = videos_[size_t(s->video)];
        Muted(std::format("{}{}", rex::path_to_utf8(v.path.filename()),
                          v.pinned ? ", chosen by hand: searches for many songs leave it alone" : "")
                  .c_str());
        DrawVideoControls(v);
        if (!v.source.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Open its page")) OpenUrl(v.source);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete it")) confirm_delete_ = true;
        if (confirm_delete_) {
            ImGui::TextColored(kWarn, "Delete %s and its offset?", rex::path_to_utf8(v.path.filename()).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                std::error_code ec;
                for (const fs::path& p : {v.path, video::IniFor(v.path), video::SongEnvelopePath(v.path),
                                          video::VideoEnvelopePath(v.path), video::ResultPath(v.path)})
                    fs::remove(p, ec);
                video::ForgetSongVideoStatus(s->shortname);
                confirm_delete_ = false;
                videos_read_ = {};
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep it")) confirm_delete_ = false;
        }
    }

    // finding one: a search, a link, or a file
    ImGui::Dummy(ImVec2(0, Px(4)));
    ImGui::BeginDisabled(queued || folders.empty());
    if (ImGui::Button(s->video >= 0 ? "Search for another" : "Search for its video")) {
        video::VideoJob job;
        queue(std::move(job));
    }
    ImGui::SameLine();
    const bool enter = InputString("##link", "or paste a link to a video", link_, Px(380));
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
    ImGui::SameLine();
    if (ImGui::Button("Choose a file...") && !pick_) {
        pick_ = std::make_shared<FilePick>();
        pick_->song = s->shortname;
        static constexpr SDL_DialogFileFilter kVideoFiles[] = {
            {"Videos", "mp4;m4v;mov;mkv;webm;avi;wmv"}};
        auto* holder = new std::shared_ptr<FilePick>(pick_);
        SDL_ShowOpenFileDialog(OnFilePicked, holder, nullptr, kVideoFiles, 1, nullptr, false);
    }
    ImGui::EndDisabled();
    if (queued) {
        ImGui::SameLine();
        Muted("(under way)");
    }

    // what its last search came to, and its results to choose from
    if (const auto status = video::SongVideoStatusOf(s->shortname)) {
        const ImVec4 colour = status->end == FetchEnd::kFailed ? kBad
                              : status->end == FetchEnd::kReview || status->end == FetchEnd::kGuessed
                                  ? kWarn
                                  : kMuted;
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
                ImGui::TextUnformatted(c.title.c_str());
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

void VideosTab::DrawLoose() {
    if (loose_.empty()) return;
    ImGui::Dummy(ImVec2(0, Px(8)));
    SectionHeading("Videos named after no song in band3's packages");
    ImGui::PushTextWrapPos(0);
    Muted("A video is named after its song's shortname, which the log gives as the song starts; these "
          "may be the disc's songs, or misnamed.");
    ImGui::PopTextWrapPos();
    for (int i : loose_) {
        Video& v = videos_[size_t(i)];
        ImGui::PushID(rex::path_to_utf8(v.path).c_str());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(v.stem.c_str());
        ImGui::SameLine(Px(260));
        DrawVideoControls(v);
        ImGui::PopID();
    }
}

void VideosTab::Draw(const std::vector<fs::path>& folders) {
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

    SectionHeading("Songs and their videos");
    ImGui::PushTextWrapPos(0);
    Muted("Each video is named after its song's shortname, with its offset (Clone Hero's "
          "video_start_time, the video's time at the song's start) in an .ini beside it. Auto-sync "
          "finds the offset from the song's sound; [ and ] move it 50 ms while the video plays. "
          "Videos are found with yt-dlp: a search's likeliest results have their sound matched with "
          "the song's, and a sure one is taken; or choose one, paste a link, or pick a file.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, Px(4)));
    DrawYtDlp();
    ImGui::Dummy(ImVec2(0, Px(4)));
    DrawBatch(folders);
    ImGui::Dummy(ImVec2(0, Px(6)));
    DrawFilters();
    DrawSongs();
    ImGui::Dummy(ImVec2(0, Px(6)));
    DrawDetails(folders);
    if (!job_message_.empty()) {
        ImGui::Dummy(ImVec2(0, Px(4)));
        Muted(job_message_.c_str());
    }
    DrawLoose();
    ImGui::Dummy(ImVec2(0, Px(6)));
    if (!folders.empty() && ImGui::Button("Open the videos folder")) OpenFolder(folders.front());
}

}
