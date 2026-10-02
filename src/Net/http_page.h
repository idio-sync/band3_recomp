#pragma once
#include <string_view>

// The web server's page (http_server.h): the song library, searchable and
// filterable, with a button that selects a song in the game's Music Library.
// It uses RB3E's endpoints, so RB3E's own page (rb3e_index.html in the game
// data root) can replace it, and this one works against RB3E too. band3's own
// (/album_art, /song_details, /status) add album art, difficulties and filters,
// a song's details, and a banner saying what the game is doing; without them
// the page goes on as RB3E's endpoints allow. Nothing loads from outside the
// server: the logo and favicon are band3.ico's 32x32 image, as a PNG data URI
// (redo them if the icon changes). tools/web_preview.py serves this page
// without the game, to work on it.

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
  --meter: #d6d5d0; --lit: #3f3f46; --devil: #dc2626;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #141416; --panel: #1d1d20; --text: #ececee; --muted: #9a9aa2;
    --line: #2c2c31; --accent: #fb923c; --accent-text: #1a1a1a; --tag: #2a2a2f;
    --meter: #3a3a40; --lit: #d4d4d8; --devil: #f87171;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--text);
  font: 16px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
header {
  position: sticky; top: 0; z-index: 1; background: var(--bg);
  border-bottom: 1px solid var(--line); padding: 12px 16px;
}
.bar { max-width: 760px; margin: 0 auto; display: flex; gap: 8px; align-items: center; }
.bar + .bar { margin-top: 8px; }
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
  padding: 7px 8px; border-radius: 8px; border: 1px solid var(--line);
  background: var(--panel);
}
#count { margin-left: auto; color: var(--muted); font-size: 14px; white-space: nowrap; }
button {
  border: 0; border-radius: 8px; padding: 8px 14px; cursor: pointer;
  background: var(--accent); color: var(--accent-text); font-weight: 600;
}
button.plain { background: var(--tag); color: var(--text); font-weight: 500; }
.bar button.plain { padding: 7px 12px; }

/* what the game is doing, from band3's /status */
#banner {
  max-width: 760px; margin: 8px auto 0; display: flex; gap: 10px; align-items: center;
  font-size: 14px; color: var(--muted);
}
#banner[hidden] { display: none; }
#banner .art { width: 40px; height: 40px; }
#banner .now { flex: 1; min-width: 0; }
#banner .title { color: var(--text); }
.progress { height: 4px; background: var(--meter); border-radius: 2px; margin-top: 4px; }
.progress div { height: 100%; background: var(--accent); border-radius: 2px; }
#banner .score { color: var(--text); font-weight: 600; font-variant-numeric: tabular-nums; }

main { max-width: 760px; margin: 0 auto; padding: 8px 16px 48px; }
#message { color: var(--muted); font-size: 14px; padding: 8px 0; }
#message:empty { display: none; }
ul { list-style: none; margin: 0; padding: 0; }
li {
  display: grid; grid-template-columns: 48px minmax(0, 1fr) auto auto;
  column-gap: 12px; align-items: center; padding: 10px 12px;
  background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
  margin-bottom: 6px; cursor: pointer;
}
li > .art { grid-row: span 2; }
li > .meters { grid-column: 2 / 4; margin-top: 4px; }
li > button { grid-column: 4; grid-row: 1 / span 2; }
/* against RB3E: no album art, so no column for it */
body.plain li { grid-template-columns: minmax(0, 1fr) auto auto; }
body.plain li > button { grid-column: 3; }
/* album art, from band3's /album_art; a plain tile for a song without any */
.art {
  flex: none; width: 48px; height: 48px; border-radius: 6px; overflow: hidden;
  background: var(--tag);
}
.art img { display: block; width: 100%; height: 100%; object-fit: cover; }
.info { min-width: 0; }
.title, .sub { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.title { font-weight: 600; }
.sub { color: var(--muted); font-size: 14px; }
.tag {
  font-size: 12px; color: var(--muted); background: var(--tag);
  padding: 2px 8px; border-radius: 999px; white-space: nowrap;
}
/* Select needs the Music Library open, when /status says it isn't */
body.closed button.pick { opacity: .35; pointer-events: none; }

/* a part's difficulty, as the Music Library's rings: Warmup is none lit,
   Impossible (6) all five in red */
.meters { display: flex; flex-wrap: wrap; gap: 4px 10px; font-size: 11px; color: var(--muted); }
.meter { display: inline-flex; align-items: center; gap: 3px; white-space: nowrap; }
.dots { display: inline-flex; gap: 1px; }
.dots i { width: 4px; height: 8px; border-radius: 1px; background: var(--meter); }
.dots i.on { background: var(--lit); }
.dots.devil i { background: var(--devil); }
.meter.none { opacity: .45; }

/* the filters, and a song's details: a sheet from the bottom on a phone */
dialog {
  border: 0; padding: 0; background: var(--panel); color: var(--text);
  width: 100%; max-width: 560px; max-height: 90vh; margin: auto auto 0;
  border-radius: 16px 16px 0 0;
}
dialog::backdrop { background: rgba(0, 0, 0, .5); }
@media (min-width: 600px) {
  dialog { margin: auto; border-radius: 16px; }
}
.sheet { padding: 16px; overflow-y: auto; max-height: 90vh; }
.sheet h2 { font-size: 18px; margin: 0; }
.sheet h3 { font-size: 13px; margin: 16px 0 6px; color: var(--muted); text-transform: uppercase; letter-spacing: .04em; }
/* the buttons stay in view while the rest scrolls */
.sheet .actions {
  display: flex; gap: 8px; justify-content: flex-end; position: sticky; bottom: -16px;
  background: var(--panel); margin: 8px 0 -16px; padding: 12px 0 16px;
}
.chips { display: flex; flex-wrap: wrap; gap: 6px; }
.chip {
  border: 1px solid var(--line); background: var(--panel); color: var(--text);
  font-weight: 500; padding: 5px 10px; border-radius: 999px; font-size: 14px;
}
.chip[aria-pressed=true] { background: var(--accent); border-color: var(--accent); color: var(--accent-text); }
.caps { display: grid; grid-template-columns: auto 1fr; gap: 6px 12px; align-items: center; font-size: 14px; }
.cover { width: 100%; max-width: 256px; aspect-ratio: 1; margin: 0 auto 12px; border-radius: 10px; overflow: hidden; background: var(--tag); }
.cover img { display: block; width: 100%; height: 100%; object-fit: cover; }
.facts { color: var(--muted); font-size: 14px; margin-top: 4px; }
.parts { display: grid; grid-template-columns: auto auto 1fr; gap: 6px 12px; align-items: center; font-size: 14px; }
.parts .tier { color: var(--muted); }

#more { display: block; margin: 12px auto; }
#toast {
  position: fixed; left: 50%; bottom: 20px; transform: translateX(-50%); z-index: 2;
  background: var(--text); color: var(--bg); padding: 10px 16px; border-radius: 8px;
  font-size: 14px; opacity: 0; transition: opacity .2s; pointer-events: none;
  max-width: calc(100% - 32px);
}
#toast.show { opacity: 1; }
@media (max-width: 520px) {
  h1 { display: none; }
  .tag { display: none; }
  li { grid-template-columns: 48px minmax(0, 1fr) auto; }
  li > .meters { grid-column: 2 / 3; }
  li > button { grid-column: 3; }
  body.plain li { grid-template-columns: minmax(0, 1fr) auto; }
  body.plain li > button { grid-column: 2; }
}
</style>
</head>
<body>
<header>
  <div class="bar">
    <img class="logo" alt="" src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAABLElEQVR42sVX7Q6CMAzsNeW/PpvhJcVngwcgsf4RFNxYt3W4hIQAY9ePu7aQ+0ihNfdX8lzdMAWfS2KfOp2P2Aum9gsx608B0D2mQ0+2BgAClgBoCAi3jvt8u2TlgDpa/7FYN4moOSyA8SBrAuILGPYA1MLbnVZUe4wLrbYIVYx+m1BwSezfh2sp9488AK8MTHho9QIX/lgtDJj7azJU3ODwIJA4ANAZKwpCyK+yFVESMoxEum6GQ+JZ2fGmIeDddCBHNdmXfD5K2KT1iumD0FP/YfiacxKqUke8tUqsdT8navnPlVI2YzeNEADUioqFhvt7iXyAxI8WEKgVInFKpmKGcKGguA0nXKFqLsMJ14xVzSejbphqPZFszTizyCAj3qa+ELHxvFRscpXyBalCeV1YdpzwAAAAAElFTkSuQmCC">
    <h1>band3</h1>
    <input id="search" type="search" placeholder="Search songs, artists, albums" autocomplete="off">
  </div>
  <div class="bar">
    <select id="sort" aria-label="Sort by">
      <option value="artist">Artist</option>
      <option value="title">Title</option>
      <option value="album">Album</option>
      <option value="origin">Source</option>
    </select>
    <button id="filter" class="plain" hidden>Filters</button>
    <button id="random" class="plain">Random</button>
    <span id="count"></span>
  </div>
  <div id="banner" hidden></div>
</header>
<main>
  <div id="message">Loading songs&hellip;</div>
  <ul id="list"></ul>
  <button id="more" class="plain" hidden>Show more</button>
</main>
<dialog id="filters"><div class="sheet">
  <h2>Filters</h2>
  <h3>Has</h3>
  <div class="chips" id="f-parts"></div>
  <h3>Difficulty at most</h3>
  <div class="caps" id="f-caps"></div>
  <h3>Genre</h3>
  <div class="chips" id="f-genres"></div>
  <h3>Decade</h3>
  <div class="chips" id="f-decades"></div>
  <div class="actions">
    <button class="plain" id="f-clear">Clear</button>
    <button id="f-done">Done</button>
  </div>
</div></dialog>
<dialog id="sheet"><div class="sheet" id="sheet-body"></div></dialog>
<div id="toast" role="status"></div>
<script>
const PAGE = 200;
const $ = id => document.getElementById(id);
let songs = [], matches = [], shown = 0;
// band3's /song_details: null until (and unless) the server has it
let details = null;
let hasArt = false;

// the parts the rows show, and the pro parts the details add
const PARTS = [["band", "Band"], ["guitar", "Guitar"], ["bass", "Bass"],
               ["drum", "Drums"], ["vocals", "Vocals"], ["keys", "Keys"]];
const PRO = [["real_guitar", "Pro Guitar"], ["real_bass", "Pro Bass"], ["real_keys", "Pro Keys"]];
const SHORT = {band: "Band", guitar: "Gtr", bass: "Bass", drum: "Drm", vocals: "Vox", keys: "Keys"};
const TIERS = ["Warmup", "Apprentice", "Solid", "Moderate", "Challenging", "Nightmare", "Impossible"];
// what "Has" can ask for: a part, or harmonies (two or more vocal parts)
const HAS = [["keys", "Keys"], ["real_guitar", "Pro Guitar"], ["real_bass", "Pro Bass"],
             ["real_keys", "Pro Keys"], ["harmonies", "Harmonies"]];

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

function el(tag, className, text) {
  const e = document.createElement(tag);
  if (className) e.className = className;
  if (text !== undefined) e.textContent = text;
  return e;
}

function minutes(ms) {
  const s = Math.round(ms / 1000);
  return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
}

// ---- filters, kept on this device
const filters = {has: [], caps: {}, genres: [], decades: []};
try { Object.assign(filters, JSON.parse(localStorage.getItem("band3.filters")) || {}); } catch (e) {}
function saveFilters() {
  try { localStorage.setItem("band3.filters", JSON.stringify(filters)); } catch (e) {}
}
function filterCount() {
  return filters.has.length + Object.keys(filters.caps).length +
         (filters.genres.length ? 1 : 0) + (filters.decades.length ? 1 : 0);
}
function decade(year) { return year ? Math.floor(year / 10) * 10 : 0; }

function passes(s) {
  if (!details) return true;
  const d = details[s.shortname];
  if (!d) return filterCount() === 0;
  for (const h of filters.has) {
    if (h === "harmonies" ? d.vocal_parts < 2 : !(h in d.tiers)) return false;
  }
  // a cap wants the part as well
  for (const [part, cap] of Object.entries(filters.caps)) {
    if (!(part in d.tiers) || d.tiers[part] > cap) return false;
  }
  if (filters.genres.length && !filters.genres.includes(d.genre)) return false;
  if (filters.decades.length && !filters.decades.includes(decade(d.year))) return false;
  return true;
}

function chip(label, on, toggle) {
  const b = el("button", "chip", label);
  b.setAttribute("aria-pressed", on);
  b.onclick = () => { toggle(); b.setAttribute("aria-pressed", b.getAttribute("aria-pressed") !== "true"); changed(); };
  return b;
}

function toggleIn(list, value) {
  const i = list.indexOf(value);
  if (i < 0) list.push(value); else list.splice(i, 1);
}

function buildFilters() {
  $("f-parts").replaceChildren(...HAS.map(([key, label]) =>
    chip(label, filters.has.includes(key), () => toggleIn(filters.has, key))));

  $("f-caps").replaceChildren(...PARTS.flatMap(([part, label]) => {
    const select = el("select");
    select.setAttribute("aria-label", label + " at most");
    select.append(new Option("Any", ""));
    TIERS.forEach((name, i) => select.append(new Option(name, i)));
    select.value = part in filters.caps ? filters.caps[part] : "";
    select.onchange = () => {
      if (select.value === "") delete filters.caps[part];
      else filters.caps[part] = Number(select.value);
      changed();
    };
    return [el("span", "", label), select];
  }));

  const all = Object.values(details || {});
  const genres = [...new Set(all.map(d => d.genre).filter(Boolean))].sort();
  $("f-genres").replaceChildren(...genres.map(g =>
    chip(g, filters.genres.includes(g), () => toggleIn(filters.genres, g))));
  const decades = [...new Set(all.map(d => decade(d.year)).filter(Boolean))].sort();
  $("f-decades").replaceChildren(...decades.map(y =>
    chip(y + "s", filters.decades.includes(y), () => toggleIn(filters.decades, y))));
}

function changed() {
  saveFilters();
  const n = filterCount();
  $("filter").textContent = n ? "Filters (" + n + ")" : "Filters";
  filter();
}

$("filter").onclick = () => { buildFilters(); $("filters").showModal(); };
$("f-done").onclick = () => $("filters").close();
$("f-clear").onclick = () => {
  Object.assign(filters, {has: [], caps: {}, genres: [], decades: []});
  buildFilters();
  changed();
};

// ---- the list
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
    const d = details && details[s.shortname];
    const hay = [s.title, s.artist, s.album, s.origin, d && d.genre].join(" ").toLowerCase();
    return words.every(w => hay.includes(w)) && passes(s);
  });
  sortSongs();
  $("list").replaceChildren();
  shown = 0;
  showMore();
}

function art(s, className) {
  const tile = el("div", className || "art");
  const img = el("img");
  img.alt = "";
  img.loading = "lazy";
  img.decoding = "async";
  img.onerror = () => img.remove();
  img.src = "/album_art?shortname=" + encodeURIComponent(s.shortname);
  tile.append(img);
  return tile;
}

function dots(tier) {
  const d = el("span", tier === 6 ? "dots devil" : "dots");
  for (let i = 1; i <= 5; i++) d.append(el("i", tier >= i ? "on" : ""));
  return d;
}

function meters(d) {
  const box = el("div", "meters");
  for (const [part] of PARTS) {
    const has = part in d.tiers;
    const m = el("span", has ? "meter" : "meter none", SHORT[part]);
    m.title = has ? SHORT[part] + ": " + TIERS[d.tiers[part]] : "No " + part;
    m.append(has ? dots(d.tiers[part]) : el("span", "", "–"));
    box.append(m);
  }
  return box;
}

function pickButton(s) {
  const b = el("button", "pick", "Select");
  b.onclick = e => { e.stopPropagation(); jump(s); };
  return b;
}

function row(s) {
  const li = el("li");
  if (hasArt) li.append(art(s));
  const info = el("div", "info");
  info.append(el("div", "title", s.title || s.shortname),
              el("div", "sub", [s.artist, s.album].filter(Boolean).join(" · ")));
  li.append(info, s.origin ? el("span", "tag", s.origin) : el("span"), pickButton(s));
  const d = details && details[s.shortname];
  if (d) li.append(meters(d));
  li.onclick = () => openSong(s, false);
  return li;
}

function showMore() {
  const next = matches.slice(shown, shown + PAGE);
  $("list").append(...next.map(row));
  shown += next.length;
  $("more").hidden = shown >= matches.length;
  $("count").textContent = matches.length === songs.length
    ? songs.length + " songs" : matches.length + " of " + songs.length;
  $("message").textContent = songs.length && !matches.length ? "No songs match." : "";
}

// ---- a song's details; from Random, with another pick
function openSong(s, random) {
  const body = $("sheet-body");
  body.replaceChildren();
  if (hasArt) body.append(art(s, "cover"));
  body.append(el("h2", "", s.title || s.shortname),
              el("div", "sub", [s.artist, s.album].filter(Boolean).join(" · ")));
  const d = details && details[s.shortname];
  if (d) {
    const facts = [d.year || null, d.genre || null, d.length_ms ? minutes(d.length_ms) : null,
                   d.vocal_parts > 1 ? d.vocal_parts + "-part harmonies" : null];
    body.append(el("div", "facts", facts.filter(Boolean).join(" · ")));
    body.append(el("h3", "", "Difficulty"));
    const parts = el("div", "parts");
    for (const [part, label] of PARTS.concat(PRO)) {
      const has = part in d.tiers;
      parts.append(el("span", "", label), has ? dots(d.tiers[part]) : el("span", "", "–"),
                   el("span", "tier", has ? TIERS[d.tiers[part]] : "None"));
    }
    body.append(parts);
  }
  const actions = el("div", "actions");
  const close = el("button", "plain", "Close");
  close.onclick = () => $("sheet").close();
  actions.append(close);
  if (random) {
    const another = el("button", "plain", "Another");
    another.onclick = pickRandom;
    actions.append(another);
  }
  const pick = pickButton(s);
  pick.addEventListener("click", () => $("sheet").close());
  actions.append(pick);
  body.append(actions);
  if (!$("sheet").open) $("sheet").showModal();
}

function pickRandom() {
  if (!matches.length) { toast("No songs to pick from"); return; }
  openSong(matches[Math.floor(Math.random() * matches.length)], true);
}
$("random").onclick = pickRandom;

for (const dialog of [$("filters"), $("sheet")]) {
  // a tap outside the sheet closes it
  dialog.addEventListener("click", e => { if (e.target === dialog) dialog.close(); });
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

// ---- what the game is doing, every 2 s while the page is looked at
// the song the banner was built for, so a poll during it only moves the
// progress, time and score, and the cover stays put
let bannerSong = null, bannerParts = null;
function showStatus(st) {
  const banner = $("banner");
  // Select works unless /status says the Music Library is closed
  document.body.classList.toggle("closed", !!st && !st.in_library);
  const p = st && st.playing;
  if (!p || p.shortname !== bannerSong) {
    banner.replaceChildren();
    bannerSong = null;
    if (!st) { banner.hidden = true; return; }
    if (p) {
      bannerSong = p.shortname;
      const now = el("div", "now"), bar = el("div", "progress");
      bannerParts = {fill: el("div"), time: el("span"), score: el("span", "score")};
      bar.append(bannerParts.fill);
      now.append(el("div", "title", p.title + " – " + p.artist), bar);
      banner.append(art(p, "art"), now, bannerParts.time, bannerParts.score);
    } else {
      banner.append(el("span", "", st.in_library
        ? "Music Library open: Select a song to highlight it"
        : "Open the Music Library in the game to select songs"));
    }
    banner.hidden = false;
  }
  if (p) {
    const known = p.position_ms !== null && p.length_ms;
    bannerParts.fill.style.width = (known ? Math.min(100, 100 * p.position_ms / p.length_ms) : 0) + "%";
    bannerParts.time.textContent = known
      ? minutes(p.position_ms) + " / " + minutes(p.length_ms) : "Starting…";
    bannerParts.score.textContent = p.score.toLocaleString();
  }
}

let statusTimer = null;
async function pollStatus() {
  clearTimeout(statusTimer);
  if (document.hidden) return;
  try {
    const r = await fetch("/status");
    // RB3E has no /status: no banner, and no more asking
    if (r.status === 404) { showStatus(null); return; }
    showStatus(r.ok ? await r.json() : null);
  } catch (e) {
    showStatus(null);
  }
  statusTimer = setTimeout(pollStatus, 2000);
}
document.addEventListener("visibilitychange", () => { if (!document.hidden) pollStatus(); });

// ---- loading
async function loadDetails(retry) {
  try {
    const r = await fetch("/song_details");
    if (r.ok) {
      details = await r.json();
      $("filter").hidden = false;
      changed();
      return;
    }
    // busy: once more, a little later
    if (r.status === 503 && retry) setTimeout(() => loadDetails(false), 5000);
  } catch (e) {}
}

async function load() {
  $("message").textContent = "Loading songs…";
  try {
    const r = await fetch("/list_songs");
    if (!r.ok) throw new Error(await r.text());
    // band3's own endpoints only when the server says it's band3
    hasArt = r.headers.get("Server") === "band3";
    document.body.classList.toggle("plain", !hasArt);
    songs = parseSongs(await r.text());
    filter();
    if (hasArt) {
      loadDetails(true);
      pollStatus();
    }
  } catch (e) {
    $("message").textContent = "Couldn't load the songs: " + e.message + " ";
    const retry = el("button", "plain", "Retry");
    retry.onclick = load;
    $("message").append(retry);
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
