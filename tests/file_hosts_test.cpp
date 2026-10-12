// Checks reading the links and pages of the sites RhythmVerse's songs are on
// (src/Net/file_hosts.cpp), with pages cut down from real ones.

#include <doctest/doctest.h>
#include <string>
#include "src/Net/file_hosts.h"

using namespace band3::file_hosts;

TEST_CASE("the sites band3 downloads from, by their links") {
    CHECK(HostOf("https://rhythmverse.co/download_file/u/abc/x") == Host::kRhythmVerse);
    CHECK(HostOf("http://www.mediafire.com/file/kxs79tbidgj86fb/SoaDADD") == Host::kMediaFire);
    CHECK(HostOf("https://www.mediafire.com/file/abt7o4p7d09d9ee/Evanescence_-_Afterlife/file") == Host::kMediaFire);
    CHECK(HostOf("http://www.mediafire.com/download/oy66aokmhhotlmw/BlackNo1_rb3con") == Host::kMediaFire);
    CHECK(HostOf("http://www.mediafire.com/?b5k3c9aq1ab") == Host::kMediaFire);
    CHECK(HostOf("https://drive.google.com/file/d/1pJ1Jw2C8ohFtZiRsW40bUaShxtwtKDZD/view?usp=sharing") == Host::kDrive);
    CHECK(HostOf("https://drive.google.com/open?id=1oVKCbP6sNYg_WrUVfi-dicdp0Lbk37yp") == Host::kDrive);
    CHECK(HostOf("https://drive.google.com/uc?export=download&id=1LDrLIKc-vbUS4aiTslYo-PaT3kPzH264") == Host::kDrive);
    CHECK(HostOf("https://www.dropbox.com/s/tn6qthbjsqnl4g3/TDN_DB_rb3con?dl=0") == Host::kDropbox);
    CHECK(HostOf("https://www.dropbox.com/scl/fi/abc/song_rb3con?rlkey=x&dl=0") == Host::kDropbox);
    CHECK(HostOf("https://www.dingorb.com/?id=684d86be35cba8585569891a") == Host::kDingo);
    CHECK(HostOf("https://bit.ly/default2rbcustom") == Host::kShortLink);
    CHECK(HostOf("http://bit.ly/onlyaladrbcustom") == Host::kShortLink);
}

TEST_CASE("folders, archives, encrypted and unknown sites aren't downloaded from") {
    CHECK(HostOf("https://drive.google.com/drive/folders/10Q7YFNiYl0gRNWxSJShjADttnsgZIDPR?usp=drive_link") ==
          Host::kNone);
    CHECK(HostOf("https://www.mediafire.com/folder/abc/Packs") == Host::kNone);
    CHECK(HostOf("https://www.mediafire.com/file/abc/x.rar") == Host::kNone);
    CHECK(HostOf("https://www.mediafire.com/file/abc/pack.ZIP/file") == Host::kNone);
    CHECK(HostOf("https://www.dropbox.com/s/abc/pack.7z?dl=0") == Host::kNone);
    CHECK(HostOf("https://mega.nz/#!eCxBVTbR!dHWJIJAL") == Host::kNone);
    CHECK(HostOf("http://marketplace.xbox.com/en-US/games/offers/0CCFF15A") == Host::kNone);
    CHECK(HostOf("https://evil.example/mediafire.com/file/abc/x") == Host::kNone);
    CHECK(HostOf("https://www.dingorb.com/about") == Host::kNone);
    CHECK(HostOf("https://bit.ly/") == Host::kNone);
    CHECK(HostOf("ftp://www.mediafire.com/file/abc/x") == Host::kNone);
    CHECK(HostOf("") == Host::kNone);
    CHECK_FALSE(CanDownload("https://drive.google.com/file/d/../view"));  // not an ID
}

TEST_CASE("a Drive link's file ID, and the link to the file itself") {
    CHECK(DriveFileId("https://drive.google.com/file/d/1FJcm93bqTdzeAV3G0tBqe_wdcunMilfn/view?usp=share_link") ==
          "1FJcm93bqTdzeAV3G0tBqe_wdcunMilfn");
    CHECK(DriveFileId("https://drive.google.com/open?id=1oVKCbP6sNYg_WrUVfi-dicdp0Lbk37yp") ==
          "1oVKCbP6sNYg_WrUVfi-dicdp0Lbk37yp");
    CHECK(DriveFileId("https://drive.google.com/drive/folders/10Q7YFNi") == "");
    CHECK(DriveDownloadUrl("abc") == "https://drive.usercontent.google.com/download?id=abc&export=download&confirm=t");
}

TEST_CASE("Drive's warning for a file too big to scan gives the file through its form") {
    constexpr std::string_view kPage = R"(<!DOCTYPE html><html><head><title>Google Drive - Virus scan warning</title>
<body><div class="uc-main"><p class="uc-warning-caption">Google Drive can't scan this file for viruses.</p>
<form id="download-form" action="https://drive.usercontent.google.com/download" method="get">
<input type="submit" id="uc-download-link" class="goog-inline-block jfk-button jfk-button-action" value="Download anyway"/>
<input type="hidden" name="id" value="1HQ90eQywNpG_rw9v">
<input type="hidden" name="export" value="download">
<input type="hidden" name="confirm" value="t">
<input type="hidden" name="uuid" value="1f6e5b02-7d3c-4b3a-9c8e-2a1b3c4d5e6f"></form></div></body></html>)";
    CHECK(DriveFormUrl(kPage) ==
          "https://drive.usercontent.google.com/download?id=1HQ90eQywNpG_rw9v&export=download&confirm=t"
          "&uuid=1f6e5b02-7d3c-4b3a-9c8e-2a1b3c4d5e6f");
    // a sign-in page, as for a file that isn't shared
    CHECK_FALSE(DriveFormUrl(R"(<html><form action="https://accounts.google.com/signin"><input name="Email" value=""></form>)"));
    CHECK_FALSE(DriveFormUrl("<html><body>Sorry, the file you have requested does not exist.</body></html>"));
}

TEST_CASE("a Dropbox link is made to download") {
    CHECK(DropboxDirect("https://www.dropbox.com/s/tn6qthbjsqnl4g3/TDN_DB_rb3con?dl=0") ==
          "https://www.dropbox.com/s/tn6qthbjsqnl4g3/TDN_DB_rb3con?dl=1");
    CHECK(DropboxDirect("https://www.dropbox.com/scl/fi/abc/x?rlkey=k&dl=0") ==
          "https://www.dropbox.com/scl/fi/abc/x?rlkey=k&dl=1");
    CHECK(DropboxDirect("https://www.dropbox.com/s/abc/x") == "https://www.dropbox.com/s/abc/x?dl=1");
    CHECK(DropboxDirect("https://www.dropbox.com/s/abc/x?dl=1") == "https://www.dropbox.com/s/abc/x?dl=1");
}

TEST_CASE("a MediaFire page's download link, plain or scrambled") {
    constexpr std::string_view kPage = R"(<div class="download_link" id="download_link">
          <a class="input popsok" aria-label="Download file"
           href="https://download853.mediafire.com/cij70tuhh3yg/kxs79tbidgj86fb/SoaDADD"
           id="downloadButton" rel="nofollow">Download (4.04MB)</a>
        </div><a href="https://www.mediafire.com/upgrade/">Upgrade</a>)";
    CHECK(MediaFireLink(kPage) == "https://download853.mediafire.com/cij70tuhh3yg/kxs79tbidgj86fb/SoaDADD");

    // "https://download1234.mediafire.com/k/abc/x" as base64
    constexpr std::string_view kScrambled = R"html(<a class="input popsok" id="downloadButton" href="javascript:void(0)"
        data-scrambled-url="aHR0cHM6Ly9kb3dubG9hZDEyMzQubWVkaWFmaXJlLmNvbS9rL2FiYy94">Download</a>)html";
    CHECK(MediaFireLink(kScrambled) == "https://download1234.mediafire.com/k/abc/x");

    // a removed file's page, and a look-alike that isn't MediaFire's
    CHECK_FALSE(MediaFireLink("<html><body>Invalid or Deleted File.</body></html>"));
    CHECK_FALSE(MediaFireLink(R"(<a href="https://download9.mediafire.com.evil.example/x">x</a>)"));
}

TEST_CASE("dingorb's song list gives a song's file on Drive") {
    constexpr std::string_view kSongs = R"json([
      {"_id":"67f6369551a4ac4832c9a342","title":"Turn It Up","drive_urls":[]},
      {"_id":"684d86be35cba8585569891a","title":"drivers license","artist":"Olivia Rodrigo",
       "drive_urls":["https://drive.google.com/uc?export=download&id=1LDrLIKc-vbUS4aiTslYo-PaT3kPzH264"],
       "dateUpdated":"2025-06-14"}])json";
    CHECK(DingoDriveUrl(kSongs, "684d86be35cba8585569891a") ==
          "https://drive.google.com/uc?export=download&id=1LDrLIKc-vbUS4aiTslYo-PaT3kPzH264");
    CHECK_FALSE(DingoDriveUrl(kSongs, "67f6369551a4ac4832c9a342"));  // no file
    CHECK_FALSE(DingoDriveUrl(kSongs, "000000000000000000000000"));
    CHECK_FALSE(DingoDriveUrl("not json", "684d86be35cba8585569891a"));
}

TEST_CASE("a package's first bytes") {
    CHECK(LooksLikePackage("CON \x01\x02"));
    CHECK(LooksLikePackage("LIVE"));
    CHECK_FALSE(LooksLikePackage("<!DOCTYPE html>"));
    CHECK_FALSE(LooksLikePackage(""));
}
