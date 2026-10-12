// Checks getting a song's music video into the videos folder
// (src/Video/video_fetch.cpp), with tests/fake_ytdlp.cpp for yt-dlp: its
// search gives tests/data/ytdlp_search.jsonl (Master of Puppets' results),
// the official video's sound is sync_clicks_late.m4a (the tones 2.5 s late)
// and its picture the red-blue clip. The song's sound stands in as
// sync_clicks.webm's first 12 s.

#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "src/Video/sync_cache.h"
#include "src/Video/video_decoder.h"
#include "src/Video/video_fetch.h"
#include "src/Video/video_files.h"

#ifdef BAND3_HAVE_FFMPEG

using namespace band3::video;
namespace fs = std::filesystem;

namespace {

const fs::path kFake = fs::path(BAND3_FAKE_YTDLP);

std::string Clip(const char* name) { return std::string(BAND3_ROOT_DIR) + "/tests/data/" + name; }

std::string Read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void SetStill(bool still) {
#ifdef _WIN32
    _putenv_s("FAKE_YTDLP_STILL", still ? "1" : "");
#else
    if (still) setenv("FAKE_YTDLP_STILL", "1", 1);
    else unsetenv("FAKE_YTDLP_STILL");
#endif
}

const SongInfo kSong{"masterofpuppets", "Master of Puppets", "Metallica", 519095};

struct Fixture {
    fs::path folder = fs::temp_directory_path() / "band3_video_fetch_test";
    YtDlp yt{kFake, {}};
    FetchSetup setup;
    bool song_sound = true;
    std::vector<FetchStage> stages;

    Fixture() {
        fs::remove_all(folder);
        std::string error;
        StartVideoThread(error);
        setup.ytdlp = &yt;
        setup.folder = folder;
        setup.min_overlap = 8.0;
        setup.song_envelope = [this](const SongInfo&, std::vector<float>& env, std::string& why) {
            if (!song_sound) {
                why = "its song's audio is encrypted";
                return false;
            }
            std::string error;
            env = SoundtrackEnvelope(Clip("sync_clicks.webm"), 12.0, error);
            return !env.empty();
        };
        setup.progress = [this](FetchStage s, double) {
            if (stages.empty() || stages.back() != s) stages.push_back(s);
        };
    }
    ~Fixture() {
        EndVideoThread();
        SetStill(false);
        fs::remove_all(folder);
    }
    fs::path Ini() const { return folder / "masterofpuppets.ini"; }
};

const TriedCandidate* Find(const FetchResult& r, std::string_view id) {
    for (const TriedCandidate& t : r.candidates)
        if (t.candidate.video.id == id) return &t;
    return nullptr;
}

}

TEST_CASE("a search takes the official video once its sound matches the song's") {
    Fixture f;
    const FetchResult r = FindSongVideo(kSong, f.setup);
    REQUIRE_MESSAGE(r.end == FetchEnd::kDone, r.message);
    CHECK(r.video == f.folder / "masterofpuppets.mp4");
    CHECK(fs::exists(r.video));
    REQUIRE(r.offset);
    CHECK(std::abs(*r.offset - 2.5) < 0.03);
    const std::string ini = Read(f.Ini());
    CHECK(FindIniValue(ini, "video_source") == "https://www.youtube.com/watch?v=offic0001");
    CHECK_FALSE(FindIniValue(ini, "video_pinned"));
    REQUIRE(FindStartTime(ini));
    CHECK(std::abs(*FindStartTime(ini) - 2.5) < 0.03);
    // its sound's envelope is kept for aligning again, the sound itself not
    CHECK(LoadEnvelope(VideoEnvelopePath(r.video), FileStamp(r.video)));
    CHECK(LoadResult(ResultPath(r.video)));
    CHECK_FALSE(fs::exists(f.folder / ".downloads"));
    // the lyric video wasn't fetched: it says what it is
    const TriedCandidate* lyric = Find(r, "lyric0001");
    REQUIRE(lyric);
    CHECK_FALSE(lyric->match);
    CHECK(lyric->candidate.penalized);
    CHECK(Find(r, "offic0001")->match);
    CHECK(std::find(f.stages.begin(), f.stages.end(), FetchStage::kChecking) != f.stages.end());
}

TEST_CASE("a still picture isn't taken, and with nothing else sure it's for review") {
    Fixture f;
    SetStill(true);
    const FetchResult r = FindSongVideo(kSong, f.setup);
    CHECK(r.end == FetchEnd::kReview);
    CHECK_FALSE(fs::exists(f.folder / "masterofpuppets.mp4"));
    const TriedCandidate* official = Find(r, "offic0001");
    REQUIRE(official);
    CHECK(official->problem.find("still") != std::string::npos);
}

TEST_CASE("without the song's sound, a likely video is taken to sync as it plays") {
    Fixture f;
    f.song_sound = false;
    const FetchResult r = FindSongVideo(kSong, f.setup);
    REQUIRE_MESSAGE(r.end == FetchEnd::kUnsynced, r.message);
    CHECK(r.message.find("encrypted") != std::string::npos);
    CHECK_FALSE(FindStartTime(Read(f.Ini())));
    // its soundtrack's envelope is kept for when the song's sound is
    CHECK(LoadEnvelope(VideoEnvelopePath(r.video), FileStamp(r.video)));
}

TEST_CASE("with taking without asking off, a search only proposes") {
    Fixture f;
    f.setup.auto_accept = false;
    const FetchResult r = FindSongVideo(kSong, f.setup);
    CHECK(r.end == FetchEnd::kReview);
    CHECK_FALSE(fs::exists(f.folder / "masterofpuppets.mp4"));
    const TriedCandidate* official = Find(r, "offic0001");
    REQUIRE(official);
    REQUIRE(official->match);
    CHECK(official->match->confident);
}

TEST_CASE("a song nothing looks like gets none") {
    Fixture f;
    const FetchResult r = FindSongVideo({"zzz", "Zzyzx Road Hymn", "Nobody Known", 200000}, f.setup);
    CHECK(r.end == FetchEnd::kNoVideo);
}

TEST_CASE("a link chosen by hand is taken, pinned, in place of the song's old video") {
    Fixture f;
    fs::create_directories(f.folder);
    std::ofstream(f.folder / "masterofpuppets.webm") << "old";
    const std::string url = "https://vimeo.com/clicks_late";
    FetchResult r = FetchSongVideo(kSong, url, f.setup);
    REQUIRE_MESSAGE(r.end == FetchEnd::kDone, r.message);
    CHECK(fs::exists(f.folder / "masterofpuppets.mp4"));
    CHECK_FALSE(fs::exists(f.folder / "masterofpuppets.webm"));
    const std::string ini = Read(f.Ini());
    CHECK(FindIniValue(ini, "video_pinned") == "1");
    CHECK(FindIniValue(ini, "video_source") == url);
    // a link that fails leaves the video as it was
    r = FetchSongVideo(kSong, "https://x/fail", f.setup);
    CHECK(r.end == FetchEnd::kFailed);
    CHECK(r.message == "Video unavailable");
    CHECK(fs::exists(f.folder / "masterofpuppets.mp4"));
    CHECK(FindIniValue(Read(f.Ini()), "video_source") == url);
}

TEST_CASE("a file of the player's is copied in, pinned; without sound it's set by hand") {
    Fixture f;
    const FetchResult r = UseVideoFile(kSong, Clip("music_video_red_blue.mp4"), f.setup);
    REQUIRE_MESSAGE(r.end == FetchEnd::kGuessed, r.message);
    CHECK(fs::exists(f.folder / "masterofpuppets.mp4"));
    CHECK(fs::exists(Clip("music_video_red_blue.mp4")));
    const std::string ini = Read(f.Ini());
    CHECK(FindIniValue(ini, "video_pinned") == "1");
    CHECK_FALSE(FindIniValue(ini, "video_source"));
}

#endif
