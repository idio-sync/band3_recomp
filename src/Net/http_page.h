#pragma once
#include <string_view>

// The web server's page (http_server.h): the song library, searchable, with a
// button that selects a song in the game's Music Library. It uses RB3E's
// endpoints only, so RB3E's own page (rb3e_index.html in the game data root)
// can replace it, and this one works against RB3E too. Nothing loads from
// outside the server: the logo and favicon are band3.ico's 32x32 image, as a
// PNG data URI (redo them if the icon changes).

namespace band3::http {

inline constexpr std::string_view kIndexPage = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>band3 songs</title>
<link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAABLElEQVR42sVX7Q6CMAzsNeW/PpvhJcVngwcgsf4RFNxYt3W4hIQAY9ePu7aQ+0ihNfdX8lzdMAWfS2KfOp2P2Aum9gsx608B0D2mQ0+2BgAClgBoCAi3jvt8u2TlgDpa/7FYN4moOSyA8SBrAuILGPYA1MLbnVZUe4wLrbYIVYx+m1BwSezfh2sp9488AK8MTHho9QIX/lgtDJj7azJU3ODwIJA4ANAZKwpCyK+yFVESMoxEum6GQ+JZ2fGmIeDddCBHNdmXfD5K2KT1iumD0FP/YfiacxKqUke8tUqsdT8navnPlVI2YzeNEADUioqFhvt7iXyAxI8WEKgVInFKpmKGcKGguA0nXKFqLsMJ14xVzSejbphqPZFszTizyCAj3qa+ELHxvFRscpXyBalCeV1YdpzwAAAAAElFTkSuQmCC">
<style>
:root {
  --bg: #f5f5f3; --panel: #ffffff; --text: #1d1d1f; --muted: #6b6b70;
  --line: #e3e3e0; --accent: #c2410c; --accent-text: #ffffff; --tag: #efeeea;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #141416; --panel: #1d1d20; --text: #ececee; --muted: #9a9aa2;
    --line: #2c2c31; --accent: #fb923c; --accent-text: #1a1a1a; --tag: #2a2a2f;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 16px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
header {
  position: sticky; top: 0; z-index: 1; background: var(--bg);
  border-bottom: 1px solid var(--line); padding: 16px;
}
.bar { max-width: 760px; margin: 0 auto; display: flex; gap: 8px; align-items: center; }
h1 { font-size: 18px; margin: 0 8px 0 0; white-space: nowrap; }
/* a dark tile in both themes: the icon's "3" is white */
.logo {
  flex: none; width: 40px; height: 40px; padding: 4px; border-radius: 8px;
  background: #1d1d20; image-rendering: pixelated;
}
input, select, button { font: inherit; color: inherit; }
input[type=search] {
  flex: 1; min-width: 0; padding: 9px 12px; border-radius: 8px;
  border: 1px solid var(--line); background: var(--panel);
}
select {
  padding: 9px 8px; border-radius: 8px; border: 1px solid var(--line);
  background: var(--panel);
}
main { max-width: 760px; margin: 0 auto; padding: 8px 16px 48px; }
#status { color: var(--muted); font-size: 14px; padding: 8px 0; }
ul { list-style: none; margin: 0; padding: 0; }
li {
  display: flex; align-items: center; gap: 12px; padding: 10px 12px;
  background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
  margin-bottom: 6px;
}
.info { flex: 1; min-width: 0; }
.title, .sub { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.title { font-weight: 600; }
.sub { color: var(--muted); font-size: 14px; }
.tag {
  font-size: 12px; color: var(--muted); background: var(--tag);
  padding: 2px 8px; border-radius: 999px; white-space: nowrap;
}
button {
  border: 0; border-radius: 8px; padding: 8px 14px; cursor: pointer;
  background: var(--accent); color: var(--accent-text); font-weight: 600;
}
button.plain { background: var(--tag); color: var(--text); font-weight: 500; }
#more { display: block; margin: 12px auto; }
#toast {
  position: fixed; left: 50%; bottom: 20px; transform: translateX(-50%);
  background: var(--text); color: var(--bg); padding: 10px 16px; border-radius: 8px;
  font-size: 14px; opacity: 0; transition: opacity .2s; pointer-events: none;
  max-width: calc(100% - 32px);
}
#toast.show { opacity: 1; }
@media (max-width: 520px) {
  h1 { display: none; }
  .tag { display: none; }
}
</style>
</head>
<body>
<header>
  <div class="bar">
    <img class="logo" alt="" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAABLElEQVR42sVX7Q6CMAzsNeW/PpvhJcVngwcgsf4RFNxYt3W4hIQAY9ePu7aQ+0ihNfdX8lzdMAWfS2KfOp2P2Aum9gsx608B0D2mQ0+2BgAClgBoCAi3jvt8u2TlgDpa/7FYN4moOSyA8SBrAuILGPYA1MLbnVZUe4wLrbYIVYx+m1BwSezfh2sp9488AK8MTHho9QIX/lgtDJj7azJU3ODwIJA4ANAZKwpCyK+yFVESMoxEum6GQ+JZ2fGmIeDddCBHNdmXfD5K2KT1iumD0FP/YfiacxKqUke8tUqsdT8navnPlVI2YzeNEADUioqFhvt7iXyAxI8WEKgVInFKpmKGcKGguA0nXKFqLsMJ14xVzSejbphqPZFszTizyCAj3qa+ELHxvFRscpXyBalCeV1YdpzwAAAAAElFTkSuQmCC">
    <h1>band3</h1>
    <input id="search" type="search" placeholder="Search songs, artists, albums" autocomplete="off">
    <select id="sort" aria-label="Sort by">
      <option value="artist">Artist</option>
      <option value="title">Title</option>
      <option value="album">Album</option>
      <option value="origin">Source</option>
    </select>
  </div>
</header>
<main>
  <div id="status">Loading songs&hellip;</div>
  <ul id="list"></ul>
  <button id="more" class="plain" hidden>Show more</button>
</main>
<div id="toast" role="status"></div>
<script>
const PAGE = 200;
let songs = [], matches = [], shown = 0;
const $ = id => document.getElementById(id);

// RB3E's /list_songs: a [shortname] section of key=value lines per song
function parseSongs(text) {
  const out = [];
  let song = null;
  for (const line of text.split(/\r?\n/)) {
    if (/^\[.*\]$/.test(line)) { song = {}; out.push(song); continue; }
    const eq = line.indexOf("=");
    if (song && eq > 0) song[line.slice(0, eq)] = line.slice(eq + 1);
  }
  return out;
}

function key(s, field) {
  return (s[field] || "").toLowerCase().replace(/^the /, "");
}

function sortSongs() {
  const field = $("sort").value;
  const order = [field, "artist", "title"];
  matches.sort((a, b) => {
    for (const f of order) {
      const c = key(a, f).localeCompare(key(b, f));
      if (c) return c;
    }
    return 0;
  });
}

function filter() {
  const words = $("search").value.toLowerCase().split(/\s+/).filter(Boolean);
  matches = songs.filter(s => {
    const hay = [s.title, s.artist, s.album, s.origin].join(" ").toLowerCase();
    return words.every(w => hay.includes(w));
  });
  sortSongs();
  $("list").replaceChildren();
  shown = 0;
  showMore();
}

function row(s) {
  const li = document.createElement("li");
  const info = document.createElement("div");
  info.className = "info";
  const title = document.createElement("div");
  title.className = "title";
  title.textContent = s.title || s.shortname;
  const sub = document.createElement("div");
  sub.className = "sub";
  sub.textContent = [s.artist, s.album].filter(Boolean).join(" · ");
  info.append(title, sub);
  li.append(info);
  if (s.origin) {
    const tag = document.createElement("span");
    tag.className = "tag";
    tag.textContent = s.origin;
    li.append(tag);
  }
  const pick = document.createElement("button");
  pick.textContent = "Select";
  pick.onclick = () => jump(s);
  li.append(pick);
  return li;
}

function showMore() {
  const next = matches.slice(shown, shown + PAGE);
  $("list").append(...next.map(row));
  shown += next.length;
  $("more").hidden = shown >= matches.length;
  $("status").textContent = matches.length === songs.length
    ? `${songs.length} songs`
    : `${matches.length} of ${songs.length} songs`;
}

let toastTimer;
function toast(text) {
  const t = $("toast");
  t.textContent = text;
  t.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove("show"), 2500);
}

async function jump(s) {
  try {
    const r = await fetch("/jump?shortname=" + encodeURIComponent(s.shortname));
    toast(r.ok ? `Selected “${s.title}”` : await r.text());
  } catch (e) {
    toast("The game didn't answer");
  }
}

async function load() {
  $("status").textContent = "Loading songs…";
  try {
    const r = await fetch("/list_songs");
    if (!r.ok) throw new Error(await r.text());
    songs = parseSongs(await r.text());
    filter();
  } catch (e) {
    $("status").textContent = "Couldn't load the songs: " + e.message + " ";
    const retry = document.createElement("button");
    retry.className = "plain";
    retry.textContent = "Retry";
    retry.onclick = load;
    $("status").append(retry);
  }
}

let searchTimer;
$("search").addEventListener("input", () => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(filter, 150);
});
$("sort").addEventListener("change", filter);
$("more").addEventListener("click", showMore);
load();
</script>
</body>
</html>
)html";

}
