#include "videos_tab.h"

#include <SDL3/SDL_misc.h>
#include <imgui.h>
#include <rex/filesystem.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>

#include "launcher_style.h"
#include "src/Video/auto_sync.h"
#include "src/Video/music_video.h"
#include "src/Video/sync_cache.h"
#include "src/Video/video_files.h"

namespace band3::launcher {

struct VideosTab::Job {
    std::mutex mutex;
    std::filesystem::path video;
    bool done = false;
    std::optional<video::SyncResult> result;
    std::string error;
};

namespace {

// how often the folder is looked at again while the tab shows
constexpr auto kRefreshEvery = std::chrono::seconds(2);

bool IsVideo(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::any_of(std::begin(video::kVideoExtensions), std::end(video::kVideoExtensions),
                       [&](std::string_view v) { return v == ext; });
}

std::optional<double> ReadOffset(const std::filesystem::path& video) {
    std::ifstream in(video::IniFor(video), std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream text;
    text << in.rdbuf();
    return video::FindStartTime(text.str());
}

// a new offset for `video`, saved by the caller: the playing video moves too,
// if it's this one (in game, behind the settings)
void ApplyIfPlaying(const std::filesystem::path& video, double seconds) {
    uint64_t song = 0;
    const std::optional<video::VideoFile> playing = video::CurrentMusicVideo(song);
    std::error_code ec;
    if (playing && std::filesystem::equivalent(playing->path, video, ec))
        video::SetMusicVideoStartTime(song, seconds);
}

void Muted(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

}

VideosTab::VideosTab() = default;
VideosTab::~VideosTab() = default;

void VideosTab::Refresh(const std::vector<std::filesystem::path>& folders) {
    folders_ = folders;
    refreshed_ = std::chrono::steady_clock::now();
    entries_.clear();
    for (const auto& folder : folders) {
        std::error_code ec;
        for (const auto& item : std::filesystem::directory_iterator(folder, ec)) {
            if (!item.is_regular_file(ec) || !IsVideo(item.path())) continue;
            Entry e;
            e.path = item.path();
            e.name = rex::path_to_utf8(item.path().stem());
            e.offset = ReadOffset(e.path);
            e.result = video::LoadResult(video::ResultPath(e.path));
            e.captured = std::filesystem::exists(video::SongEnvelopePath(e.path), ec);
            e.ms = int(std::lround(e.offset.value_or(0.0) * 1000.0));
            entries_.push_back(std::move(e));
        }
    }
    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
}

void VideosTab::StartAlign(const Entry& e) {
    job_ = std::make_shared<Job>();
    job_->video = e.path;
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

void VideosTab::DrawEntry(Entry& e) {
    ImGui::PushID(rex::path_to_utf8(e.path).c_str());
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(e.name.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", rex::path_to_utf8(e.path.filename()).c_str());

    // the offset, ms: written to the .ini when the field's left
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputInt("##ms", &e.ms, 50, 500);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        video::WriteStartTime(e.path, e.ms / 1000.0);
        ApplyIfPlaying(e.path, e.ms / 1000.0);
        e.offset = e.ms / 1000.0;
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::AlignTextToFramePadding();
    const bool aligning = job_ && job_->video == e.path && !job_->done;
    if (aligning) {
        Muted("Aligning...");
    } else if (e.result && e.result->confident && e.offset &&
               std::abs(*e.offset - e.result->offset) < 0.0015) {
        ImGui::TextColored(kGood, "Synced automatically");
    } else if (e.result && e.result->confident) {
        const std::string text =
            std::format("Set by hand (auto-sync found {:+.3f} s)", e.result->offset);
        ImGui::TextUnformatted(text.c_str());
    } else if (e.result) {
        const std::string text = std::format("Not sure: best guess {:+.3f} s", e.result->offset);
        ImGui::TextColored(kWarn, "%s", text.c_str());
    } else if (e.offset) {
        ImGui::TextUnformatted("Set by hand");
    } else if (!e.captured) {
        Muted("Play its song once to sync it");
    } else {
        Muted("Not aligned yet");
    }

    ImGui::TableSetColumnIndex(3);
    ImGui::BeginDisabled(!e.captured || (job_ && !job_->done));
    if (ImGui::Button("Align")) StartAlign(e);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(e.captured ? "Match the video's sound with its song's again"
                                     : "Play its song once first: its sound is taken then");
    if (e.result && !e.result->confident && !aligning) {
        ImGui::SameLine();
        if (ImGui::Button("Use it")) {
            video::WriteStartTime(e.path, e.result->offset);
            ApplyIfPlaying(e.path, e.result->offset);
            e.offset = e.result->offset;
            e.ms = int(std::lround(e.result->offset * 1000.0));
        }
    }
    ImGui::PopID();
}

void VideosTab::Draw(const std::vector<std::filesystem::path>& folders) {
    // a finished Align: its result shows, and the folder's read again
    if (job_) {
        std::lock_guard lock(job_->mutex);
        if (job_->done && job_message_.empty()) {
            const std::string name = rex::path_to_utf8(job_->video.stem());
            if (!job_->result)
                job_message_ = name + ": " + job_->error;
            else if (job_->result->confident) {
                job_message_ = std::format("{}: {:+.3f} s, saved", name, job_->result->offset);
                ApplyIfPlaying(job_->video, job_->result->offset);
            }
            else
                job_message_ = std::format("{}: not sure (best guess {:+.3f} s), not saved", name,
                                           job_->result->offset);
            refreshed_ = {};
        }
    }
    if (folders != folders_ ||
        (std::chrono::steady_clock::now() - refreshed_ > kRefreshEvery && !ImGui::IsAnyItemActive()))
        Refresh(folders);

    SectionHeading("Your videos");
    ImGui::PushTextWrapPos(0);
    Muted("Each video is named after its song's shortname (nooneknows.webm), which the log gives "
          "as the song starts. The offset is the video's time at the song's start, in "
          "milliseconds (Clone Hero's video_start_time); auto-sync finds it the first time the "
          "song plays, and [ and ] move it 50 ms while the video plays.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, Px(4)));

    if (entries_.empty()) {
        std::string where;
        for (const auto& f : folders) where += (where.empty() ? "" : ", ") + rex::path_to_utf8(f);
        const std::string text = "No videos in " + (where.empty() ? std::string("the videos folder") : where);
        Muted(text.c_str());
    } else if (ImGui::BeginTable("videos", 4, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Video", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Offset (ms)", ImGuiTableColumnFlags_WidthFixed, Px(170));
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                ButtonWidth("Align") + ButtonWidth("Use it") + Px(12));
        ImGui::TableHeadersRow();
        for (Entry& e : entries_) DrawEntry(e);
        ImGui::EndTable();
    }
    if (!job_message_.empty()) {
        ImGui::Dummy(ImVec2(0, Px(4)));
        Muted(job_message_.c_str());
    }
    ImGui::Dummy(ImVec2(0, Px(6)));
    if (!folders.empty() && ImGui::Button("Open the videos folder")) {
        std::error_code ec;
        std::filesystem::create_directories(folders.front(), ec);
        const std::string url = "file:///" + rex::path_to_utf8(folders.front().generic_wstring());
        SDL_OpenURL(url.c_str());
    }
}

}
