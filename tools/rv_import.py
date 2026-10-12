"""rv_import: takes custom songs downloaded from RhythmVerse by hand into
band3's RhythmVerse download folder, with the records band3 keeps of its own
downloads, so the web page's RhythmVerse tab tells of their updates.

band3 checks for updates only to what it downloaded itself: the songs in
<first of content_folders>/rhythmverse, each with a record in that folder's
rhythmverse.json (src/Net/song_downloads.h) saying which upload it is and which
version (RhythmVerse's hashes of the upload's files, or for an upload without
them when RhythmVerse last changed it). This finds each package's upload on
RhythmVerse and writes that record:

  - an index of RhythmVerse's uploads, read a page at a time from its API
    (as band3's own search does, two seconds apart) as far as its list goes
    (some 25,000), then a search by title for each package it didn't have,
    then by artist for those still missing; kept in --index, so a second run
    asks only what's new
  - each package in --from: its name and size, and the MD5s of its songs.dta
    and .mid, which are RhythmVerse's configuration_file_hash and
    gameplay_file_hash (its gamefile_hash isn't the .mogg's, nor the file's)
  - a package with the upload's name and size, or its two hashes, is that
    upload as it is now; one with only its name is an older version, and its
    record says so, so the update shows at once

Only uploads band3 could download again move (RhythmVerse hosts them itself,
unzipped, or they're on a site src/Net/file_hosts.h downloads from); the rest
stay where they are, as do packages no upload matches.
Either way they still play from any content folder.

  python tools/rv_import.py --from <folder of packages> --to <content folder>/rhythmverse
  python tools/rv_import.py ... --apply     moves the packages and writes the records

Without --apply it only says what it would do. Writes a CSV of every package
and its outcome to --report.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import struct
import sys
import time
import urllib.parse
import urllib.request
import zlib
from datetime import datetime, timezone

SITE = "https://rhythmverse.co"
LIST_API = SITE + "/api/rb3xbox/songfiles/list"
SEARCH_API = SITE + "/api/rb3xbox/songfiles/search/live"
PAGE_SIZE = 100
# between requests, as band3's update check (song_downloads.cpp's kCheckPause)
PAUSE_S = 2.0

# RhythmVerse's difficulty fields, by the game's names for the parts
# (rhythmverse.cpp's kParts)
PARTS = [
    ("diff_band", "band"), ("diff_guitar", "guitar"), ("diff_bass", "bass"),
    ("diff_drums", "drum"), ("diff_vocals", "vocals"), ("diff_keys", "keys"),
    ("diff_proguitar", "real_guitar"), ("diff_probass", "real_bass"),
    ("diff_prokeys", "real_keys"),
]


class Stfs:
    """A CON/LIVE package's files, read only (block maps as Velocity's)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0x1000)
        if h[:4] not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError("not an STFS package")
        header_size = struct.unpack(">I", h[0x340:0x344])[0]
        vd = h[0x379:0x379 + 0x24]
        self.data_start = (header_size + 0xFFF) & 0xF000
        # a package with block separation bit 0 clear keeps two tables per level
        self.shift = 0 if (vd[2] & 1) else 1
        ft_blocks = struct.unpack("<H", vd[3:5])[0]
        ft_start = vd[5] | vd[6] << 8 | vd[7] << 16
        self.files = self._table(self._read(ft_start, ft_blocks, ft_blocks * 0x1000, False))

    def close(self):
        self.f.close()

    def _block_offset(self, block):
        s = self.shift
        n = (((block + 0xAA) // 0xAA) << s) + block
        if block >= 0xAA:
            n += ((block + 0x70E4) // 0x70E4) << s
            if block >= 0x70E4:
                n += 1 << s
        return self.data_start + (n << 12)

    def _next_block(self, block):
        s = self.shift
        num = 0
        if block >= 0xAA:
            num = (block // 0xAA) * (0xAB if s == 0 else 0xAC)
            num += ((block // 0x70E4) + 1) << s
            if block >= 0x70E4:
                num += 1 << s
        self.f.seek(self.data_start + (num << 12) + (block % 0xAA) * 0x18)
        e = self.f.read(0x18)
        if len(e) < 0x18:
            raise ValueError("block chain leaves the file")
        return e[0x15] << 16 | e[0x16] << 8 | e[0x17]

    def _read(self, start, count, size, contiguous):
        out = bytearray()
        block = start
        for _ in range(count):
            self.f.seek(self._block_offset(block))
            out += self.f.read(0x1000)
            block = block + 1 if contiguous else self._next_block(block)
        return bytes(out[:size])

    @staticmethod
    def _table(raw):
        entries = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            n = e[0x28] & 0x3F
            if n == 0:
                break
            entries.append({
                "name": e[:n].decode("latin-1"),
                "dir": bool(e[0x28] & 0x80),
                "contig": bool(e[0x28] & 0x40),
                "blocks": e[0x29] | e[0x2A] << 8 | e[0x2B] << 16,
                "start": e[0x2F] | e[0x30] << 8 | e[0x31] << 16,
                "parent": struct.unpack(">h", e[0x32:0x34])[0],
                "size": struct.unpack(">I", e[0x34:0x38])[0],
            })
        for e in entries:
            parts, p, depth = [e["name"]], e["parent"], 0
            while p != -1 and 0 <= p < len(entries) and depth < 64:
                parts.append(entries[p]["name"])
                p, depth = entries[p]["parent"], depth + 1
            e["path"] = "/".join(reversed(parts)).lower()
        return entries

    def read(self, entry):
        return self._read(entry["start"], entry["blocks"], entry["size"], entry["contig"])


# ---- RhythmVerse's index ----

def post(url, fields, what):
    request = urllib.request.Request(url, data=urllib.parse.urlencode(fields).encode(),
                                     headers={"User-Agent": "band3 rv_import"})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request, timeout=60) as reply:
                body = json.load(reply)
            if body.get("status") != "success":
                raise ValueError(f"RhythmVerse answered {body.get('status')!r}")
            return body["data"]
        except Exception as error:
            if attempt == 3:
                raise
            print(f"  {what}: {error}; again in {10 * (attempt + 1)} s", flush=True)
            time.sleep(10 * (attempt + 1))


def fetch_page(page):
    return post(LIST_API, {
        "records": PAGE_SIZE, "page": page, "data_type": "full",
        # the oldest first, so uploads made while this reads land at the end
        "sort[0][sort_by]": "release_date", "sort[0][sort_order]": "ASC",
    }, f"page {page}")


def add_songs(index, songs):
    added = 0
    for entry in songs or []:
        file_id = (entry.get("file") or {}).get("file_id")
        if file_id and file_id not in index["songs"]:
            index["songs"][file_id] = {"file": entry["file"], "data": entry.get("data") or {}}
            added += 1
    return added


def save_index(index, path):
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(index, f)
    os.replace(path + ".tmp", path)


# RhythmVerse's list can't be read to the end: past some 25,000 uploads (a run
# of thousands with the same release day) every page is the same, whatever the
# sort. Reading stops at a few pages with nothing new, and the packages it
# missed are searched for by title (search_for).
STALE_PAGES = 3


def load_index(path):
    """The uploads the list gives, by file ID: {"pages": n read, "total":,
    "songs": {id: entry}, "searched": [titles searched for]}."""
    index = {"pages": 0, "total": 0, "songs": {}, "searched": []}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            index = json.load(f)
    index.setdefault("searched", [])
    if index.get("complete"):
        return index
    page, stale = index["pages"] + 1, 0
    while True:
        data = fetch_page(page)
        songs = data.get("songs") or []
        index["total"] = int(data["records"]["total_filtered"])
        stale = 0 if add_songs(index, songs) else stale + 1
        index["pages"] = page
        pages = -(-index["total"] // PAGE_SIZE)
        done = not songs or page >= pages or stale >= STALE_PAGES
        if page % 10 == 0 or done:
            print(f"  RhythmVerse: page {page} of {pages}, {len(index['songs'])} uploads", flush=True)
            save_index(index, path)
        if done:
            break
        page += 1
        time.sleep(PAUSE_S)
    index["complete"] = True
    save_index(index, path)
    return index


def search_for(index, title):
    """Adds the uploads a search for the title finds; once per title, ever."""
    key = title.lower()
    if key in index["searched"]:
        return 0
    data = post(SEARCH_API, {"records": 25, "page": 1, "data_type": "full", "text": title}, f"search {title!r}")
    index["searched"].append(key)
    time.sleep(PAUSE_S)
    return add_songs(index, data.get("songs") if isinstance(data.get("songs"), list) else [])


def search_artist(index, artist):
    """Adds the uploads a search for the artist finds (two pages of 100 at
    most); once per artist, ever."""
    searched = index.setdefault("searched_artists", [])
    if artist.lower() in searched:
        return 0
    added = 0
    for page in (1, 2):
        data = post(SEARCH_API, {"records": 100, "page": page, "data_type": "full", "text": artist},
                    f"search {artist!r}")
        time.sleep(PAUSE_S)
        songs = data.get("songs") if isinstance(data.get("songs"), list) else []
        added += add_songs(index, songs)
        if len(songs) < 100:
            break
    searched.append(artist.lower())
    return added


def text(value):
    if value is None or value is False:
        return ""
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def whole(value):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return 0


def field(file, data, file_key, data_key):
    own = file.get(file_key)
    return data.get(data_key) if own is None or text(own) == "" else own


def song_id_of(custom_id):
    """rhythmverse.cpp's SongIdOf: a text ID as band3 corrects it."""
    custom_id = text(custom_id)
    if custom_id in ("", "0"):
        return 0
    if re.fullmatch(r"[+-]?\d+", custom_id):
        n = int(custom_id)
        return n if 0 < n <= 0x7FFFFFFF else 0
    return zlib.crc32(custom_id.encode("utf-8")) % 9999999 + 2130000000


def rv_time(value):
    try:
        t = datetime.strptime(text(value), "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return 0
    return int(t.replace(tzinfo=timezone.utc).timestamp())


def absolute(url):
    return SITE + url if url.startswith("/") else url


def can_download(url):
    """src/Net/file_hosts.cpp's HostOf, as a yes or no: a file on MediaFire,
    Google Drive or Dropbox, a dingorb song, a bit.ly or tinyurl link; never a
    folder, mega.nz or an archive."""
    parts = urllib.parse.urlsplit(url)
    if parts.scheme.lower() not in ("http", "https"):
        return False
    host = parts.netloc.lower().removeprefix("www.")
    path = parts.path
    name = path.lower().removesuffix("/file")
    if name.endswith((".zip", ".rar", ".7z")):
        return False
    query = urllib.parse.parse_qs(parts.query)
    ident = re.compile(r"[A-Za-z0-9_-]{1,128}")
    if host == "mediafire.com":
        return path.startswith(("/file/", "/download/", "/file_premium/")) or (
            path in ("", "/") and parts.query and "=" not in parts.query)
    if host in ("drive.google.com", "docs.google.com", "drive.usercontent.google.com"):
        if "/folders/" in path:
            return False
        m = re.search(r"/file/d/([^/]+)", path)
        file_id = m.group(1) if m else (query.get("id") or [""])[0]
        return bool(ident.fullmatch(file_id))
    if host in ("dropbox.com", "dl.dropboxusercontent.com", "dl.dropbox.com"):
        return path.startswith(("/s/", "/scl/fi/"))
    if host == "dingorb.com":
        return bool(ident.fullmatch((query.get("id") or [""])[0]))
    if host in ("bit.ly", "tinyurl.com"):
        return len(path) > 1
    return False


def version_of(s):
    """rhythmverse.cpp's VersionOf: the hashes, or when RhythmVerse last
    changed the upload, for the many on other sites without hashes."""
    if s["hash"]:
        return s["hash"]
    return f"updated:{s['updated']}" if s["updated"] else ""


def song(entry):
    """An upload as rhythmverse.json's "latest" keeps it (rhythmverse.cpp's
    ParseSong, then StoredSong)."""
    file, data = entry["file"], entry["data"]
    tiers = {}
    for key, part in PARTS:
        value = whole(field(file, data, key, key))
        if value >= 1:
            tiers[part] = min(value - 1, 6)
    hashes = [text(file.get(k)) for k in ("gamefile_hash", "gameplay_file_hash", "configuration_file_hash")]
    url = text(file.get("external_url")) or text(file.get("download_url"))
    host = urllib.parse.urlparse(url).netloc if "://" in url else ""
    hosted = bool(url) and host in ("", "rhythmverse.co")
    zipped = file.get("zippata") not in (None, False, 0, "0", "", "false")
    if not hosted and can_download(url):
        # on a site band3 downloads from too (src/Net/file_hosts.h)
        hosted, zipped, own = True, False, False
    else:
        own = True
    author = text((file.get("author") or {}).get("name")) if isinstance(file.get("author"), dict) else ""
    art = text(field(file, data, "album_art", "album_art"))
    return {
        "title": text(field(file, data, "file_title", "title")),
        "artist": text(field(file, data, "file_artist", "artist")),
        "album": text(field(file, data, "file_album", "album")),
        "genre": text(field(file, data, "file_genre", "genre")),
        "author": author or text(file.get("user")),
        "year": whole(field(file, data, "file_year", "year")),
        "length_s": whole(field(file, data, "song_length", "song_length")),
        "vocal_parts": whole(field(file, data, "vocal_parts_authored", "vocal_parts")),
        "size": whole(file.get("size")),
        "tiers": tiers,
        "art": absolute(art) if art else "",
        "page": absolute(text(file.get("file_url"))),
        "file_name": text(file.get("file_name")),
        "song_id": song_id_of(file.get("custom_id")),
        "hash": "/".join(hashes) if any(hashes) else "",
        "download_url": (absolute(url) if own else url) if hosted and not zipped else "",
        "host": ("" if own else host) if hosted and not zipped else ("rhythmverse.co" if hosted else host),
        "updated": rv_time(file.get("update_date")),
    }


# ---- the packages here ----

def package_info(path):
    info = {"dta_md5": "", "mid_md5": "", "songs": 0, "title": "", "artist": "", "error": ""}
    try:
        package = Stfs(path)
    except Exception as error:
        info["error"] = str(error)
        return info
    try:
        files = {e["path"]: e for e in package.files if not e["dir"]}
        mids = [e for p, e in files.items() if p.endswith(".mid")]
        info["songs"] = len(mids)
        if "songs/songs.dta" in files:
            dta = package.read(files["songs/songs.dta"])
            info["dta_md5"] = hashlib.md5(dta).hexdigest()
            title = re.search(rb"\(\s*['\"]?name['\"]?\s+\"([^\"]+)\"", dta)
            if title:
                info["title"] = title.group(1).decode("utf-8", "replace").strip()
            artist = re.search(rb"\(\s*['\"]?artist['\"]?\s+\"([^\"]+)\"", dta)
            if artist:
                info["artist"] = artist.group(1).decode("utf-8", "replace").strip()
        if len(mids) == 1:
            info["mid_md5"] = hashlib.md5(package.read(mids[0])).hexdigest()
    except Exception as error:
        info["error"] = str(error)
    finally:
        package.close()
    return info


def match(name, size, info, by_name, by_hashes):
    """(file ID, "current" | "older") or (None, why)."""
    lower = name.lower()
    named = by_name.get(lower, [])
    hashes = (info["mid_md5"], info["dta_md5"])
    for file_id, s, raw in named:
        rv = (text(raw.get("gameplay_file_hash")), text(raw.get("configuration_file_hash")))
        if s["size"] == size:
            # the same name and size, and the same files where RhythmVerse says
            if not all(rv) or not all(hashes) or rv == hashes:
                return file_id, "current"
        elif not s["size"] and not any(rv) and len(named) == 1:
            # RhythmVerse knows neither its size nor its files (most uploads on
            # other sites): nothing says it's older
            return file_id, "current"
    if all(hashes) and hashes in by_hashes:
        candidates = by_hashes[hashes]
        # the one by this name, else one band3 can download, else the newest
        candidates = sorted(candidates, key=lambda c: (c[1]["file_name"].lower() != lower,
                                                       not c[1]["download_url"], -c[1]["updated"]))
        return candidates[0][0], "current"
    if len(named) == 1:
        return named[0][0], "older"
    if len(named) > 1:
        return None, f"{len(named)} uploads share its name"
    return None, "no upload matches"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--from", dest="source", required=True, help="the folder of packages")
    parser.add_argument("--to", dest="dest", required=True,
                        help="band3's download folder: <first of content_folders>/rhythmverse")
    parser.add_argument("--index", default="out/rv_index.json", help="RhythmVerse's uploads, kept")
    parser.add_argument("--report", default="out/rv_import_report.csv")
    parser.add_argument("--apply", action="store_true", help="move packages and write records")
    args = parser.parse_args()

    print("Reading RhythmVerse's uploads...", flush=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.index)), exist_ok=True)
    index = load_index(args.index)

    def lookups():
        songs = {file_id: (song(entry), entry["file"]) for file_id, entry in index["songs"].items()
                 if re.fullmatch(r"[A-Za-z0-9.]{1,64}", file_id)}
        by_name, by_hashes = {}, {}
        for file_id, (s, raw) in songs.items():
            if s["file_name"]:
                by_name.setdefault(s["file_name"].lower(), []).append((file_id, s, raw))
            h = (text(raw.get("gameplay_file_hash")), text(raw.get("configuration_file_hash")))
            if all(h):
                by_hashes.setdefault(h, []).append((file_id, s, raw))
        return songs, by_name, by_hashes

    songs, by_name, by_hashes = lookups()
    print(f"{len(songs)} uploads from RhythmVerse's list", flush=True)

    records_path = os.path.join(args.dest, "rhythmverse.json")
    existing = {}
    if os.path.exists(records_path):
        with open(records_path, encoding="utf-8") as f:
            existing = json.load(f)

    names = sorted(n for n in os.listdir(args.source) if os.path.isfile(os.path.join(args.source, n)))
    print(f"Reading {len(names)} files in {args.source}...", flush=True)
    packages = []
    for i, name in enumerate(names, 1):
        path = os.path.join(args.source, name)
        packages.append((name, os.path.getsize(path), package_info(path)))
        if i % 100 == 0:
            print(f"  {i} of {len(names)}", flush=True)

    # the packages the list didn't have, searched for by title, then those
    # still missing by artist (a title search gives 25 songs, too few for a
    # common title)
    for field, kind in (("title", "titles"), ("artist", "artists")):
        missed = {info[field] for n, size, info in packages
                  if not info["error"] and info[field] and not match(n, size, info, by_name, by_hashes)[0]}
        to_search = sorted(t for t in missed if t.lower() not in index["searched" if field == "title" else "searched_artists"])
        if not to_search:
            continue
        print(f"Searching RhythmVerse for {len(to_search)} {kind} the list missed "
              f"(about {len(to_search) * 3 // 60 + 1} min)...", flush=True)
        found = 0
        for i, value in enumerate(to_search, 1):
            try:
                found += search_for(index, value) if field == "title" else search_artist(index, value)
            except Exception as error:
                print(f"  search {value!r} failed: {error}", flush=True)
            if i % 50 == 0 or i == len(to_search):
                print(f"  {i} of {len(to_search)}, {found} uploads found", flush=True)
                save_index(index, args.index)
        songs, by_name, by_hashes = lookups()
    newest = max((s["updated"] for s, _ in songs.values()), default=0)

    rows, chosen = [], {}
    for name, size, info in packages:
        row = {"file": name, "size": size, "file_id": "", "version": "", "outcome": "", "artist": "",
               "title": "", "page": ""}
        if info["error"]:
            row["outcome"] = "stays: not a package band3 can read (" + info["error"] + ")"
        else:
            file_id, how = match(name, size, info, by_name, by_hashes)
            if not file_id:
                row["outcome"] = "stays: " + how
            else:
                s = songs[file_id][0]
                row.update(file_id=file_id, version=how, artist=s["artist"], title=s["title"], page=s["page"])
                if not s["download_url"]:
                    row["outcome"] = "stays: band3 can't download it (" + (s["host"] or "not hosted") + ")"
                elif file_id in existing:
                    row["outcome"] = "stays: band3 already has a record of it"
                elif file_id in chosen:
                    other = chosen[file_id]
                    # the current version over an older copy of the same upload
                    if how == "current" and other["version"] == "older":
                        other["outcome"] = "stays: another copy of " + name
                        chosen[file_id] = row
                        row["outcome"] = "moves"
                    else:
                        row["outcome"] = "stays: another copy of " + other["file"]
                elif os.path.exists(os.path.join(args.dest, name)):
                    row["outcome"] = "stays: a file by its name is in " + args.dest
                else:
                    chosen[file_id] = row
                    row["outcome"] = "moves"
                if row["outcome"] == "moves":
                    row["_hashes"] = (info["mid_md5"], info["dta_md5"])
        rows.append(row)

    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    with open(args.report, "w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=[k for k in rows[0] if not k.startswith("_")] if rows else ["file"],
                                extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

    tally = {}
    for row in rows:
        key = row["outcome"].split(" (")[0]
        key = "stays: another copy" if key.startswith("stays: another copy") else key
        tally[key] = tally.get(key, 0) + 1
    moving = [r for r in rows if r["outcome"] == "moves"]
    older = sum(1 for r in moving if r["version"] == "older")
    print()
    for key, count in sorted(tally.items(), key=lambda kv: -kv[1]):
        print(f"  {count:5}  {key}")
    print(f"  of those moving, {older} are older than RhythmVerse's version: their updates show at once")
    print(f"report: {args.report}")
    if not args.apply:
        print("Nothing moved: --apply moves them and writes", records_path)
        return 0

    os.makedirs(args.dest, exist_ok=True)
    now = int(time.time())
    records = dict(existing)
    moved = 0
    for row in moving:
        file_id = row["file_id"]
        target = os.path.join(args.dest, row["file"])
        if os.path.exists(target):
            print(f"  {row['file']}: a file by its name is in {args.dest} already; left where it is")
            continue
        s = songs[file_id][0]
        if row["version"] == "current" and version_of(s):
            version = version_of(s)
        else:
            # not one of RhythmVerse's, so its version shows as an update
            mid, dta = row["_hashes"]
            version = f"local/{mid}/{dta}"
        os.rename(os.path.join(args.source, row["file"]), target)
        records[file_id] = {"file": row["file"], "hash": version, "pending": "", "downloaded": now,
                            "latest": s}
        moved += 1
    # the update check reads back from here, not through years of uploads
    records["_check"] = {"through": max(newest, existing.get("_check", {}).get("through", 0)), "at": now}
    with open(records_path + ".tmp", "w", encoding="utf-8") as f:
        json.dump(records, f, indent=2, ensure_ascii=False)
    os.replace(records_path + ".tmp", records_path)
    print(f"Moved {moved} packages to {args.dest} and wrote {records_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
