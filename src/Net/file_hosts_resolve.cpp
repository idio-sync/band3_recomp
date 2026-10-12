#include "file_hosts.h"

#include "http_request.h"
#include "web_client.h"

// file_hosts.h's Resolve, apart from the rest, which needs no network

namespace band3::file_hosts {

std::string Resolve(const std::string& url, std::string& direct, int depth) {
    // a page, an API's list, or a file's first bytes to tell which it is
    constexpr size_t kPageBytes = 4 << 20;
    constexpr size_t kPeekBytes = 64 << 10;
    if (depth > 3) return "its link goes round in circles";
    switch (HostOf(url)) {
        case Host::kRhythmVerse:
            direct = url;
            return {};
        case Host::kDropbox: {
            // Dropbox sends the file from a dropboxusercontent.com link it
            // redirects to, which answers WinHTTP's following of the redirect
            // with 400 but gives the file when asked anew (a link that lasts
            // hours)
            const std::string link = DropboxDirect(url);
            const web::Reply reply = web::Get(link, kPeekBytes);
            if (!reply.error.empty()) return "couldn't reach Dropbox: " + reply.error;
            if (reply.status == 200 && LooksLikePackage(reply.body)) {
                direct = link;
                return {};
            }
            const size_t scheme = reply.url.find("://");
            const std::string host = scheme == std::string::npos
                                         ? std::string()
                                         : reply.url.substr(scheme + 3, reply.url.find('/', scheme + 3) - scheme - 3);
            if (host.ends_with(".dropboxusercontent.com")) {
                direct = reply.url;
                return {};
            }
            return "Dropbox didn't give the file (it may be gone) (answered " + std::to_string(reply.status) + ")";
        }
        case Host::kDrive: {
            const std::string file = DriveDownloadUrl(DriveFileId(url));
            const web::Reply reply = web::Get(file, kPeekBytes);
            if (!reply.error.empty()) return "couldn't reach Google Drive: " + reply.error;
            if (reply.status != 200) return "Google Drive answered " + std::to_string(reply.status);
            if (LooksLikePackage(reply.body)) {
                direct = file;
                return {};
            }
            if (const auto form = DriveFormUrl(reply.body)) {
                direct = *form;
                return {};
            }
            return "Google Drive didn't give the file (it may not be shared, or gone)";
        }
        case Host::kMediaFire: {
            const web::Reply reply = web::Get(url, kPageBytes);
            if (!reply.error.empty()) return "couldn't reach MediaFire: " + reply.error;
            if (reply.status != 200) return "MediaFire answered " + std::to_string(reply.status);
            const auto link = MediaFireLink(reply.body);
            if (!link) return "MediaFire's page had no download link (the file may be gone)";
            direct = *link;
            return {};
        }
        case Host::kDingo: {
            const web::Reply reply = web::Get(kDingoSongs, kPageBytes);
            if (!reply.error.empty()) return "couldn't reach dingorb: " + reply.error;
            if (reply.status != 200) return "dingorb answered " + std::to_string(reply.status);
            const auto id = http::QueryParam(url, "id");
            const auto drive = DingoDriveUrl(reply.body, id.value_or(""));
            if (!drive) return "dingorb doesn't list the song";
            return Resolve(*drive, direct, depth + 1);
        }
        case Host::kShortLink: {
            const web::Reply reply = web::Get(url, kPeekBytes);
            if (!reply.error.empty()) return "couldn't follow its link: " + reply.error;
            if (reply.url == url || HostOf(reply.url) == Host::kShortLink) {
                return "its short link didn't lead anywhere band3 knows";
            }
            if (!CanDownload(reply.url)) {
                return "its link leads to a site band3 can't download from";
            }
            return Resolve(reply.url, direct, depth + 1);
        }
        case Host::kNone:
            break;
    }
    return "band3 can't download from its site";
}

}
