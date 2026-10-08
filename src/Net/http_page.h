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
#karaoke-link { color: var(--muted); text-decoration: none; font-size: 14px; white-space: nowrap; }
#karaoke-link:hover { color: var(--accent); }
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
button:disabled { cursor: default; }
/* a link that looks like a plain button: a RhythmVerse page */
a.button {
  display: inline-block; text-decoration: none; text-align: center; white-space: nowrap;
  border-radius: 8px; padding: 8px 14px; background: var(--tag); color: var(--text); font-weight: 500;
}
.bar + .bar { flex-wrap: wrap; }
#lib-tools, #rv-tools { display: contents; }
#lib-tools[hidden], #rv-tools[hidden] { display: none; }
/* the song library or RhythmVerse */
.tabs { display: inline-flex; background: var(--tag); border-radius: 8px; padding: 2px; }
.tabs[hidden] { display: none; }
.tabs button {
  background: transparent; color: var(--text); font-weight: 500; padding: 5px 12px; border-radius: 6px;
}
.tabs button[aria-pressed=true] { background: var(--panel); font-weight: 600; box-shadow: 0 1px 2px rgba(0, 0, 0, .15); }
.note {
  background: var(--panel); border: 1px solid var(--line); border-left: 3px solid var(--accent);
  border-radius: 8px; padding: 10px 12px; margin: 8px 0; font-size: 14px;
}
.note[hidden] { display: none; }
.tag.have { color: var(--accent); }
button.dl { min-width: 104px; }
/* newer versions of songs band3 downloaded, above RhythmVerse's songs */
#rv-updates { margin-bottom: 12px; }
#rv-updates[hidden] { display: none; }
.updates-bar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; padding: 8px 0; font-size: 14px; color: var(--muted); }
.updates-bar span { flex: 1; min-width: 12em; }
.updates-bar button { padding: 7px 12px; }
/* a song in more than one package: each copy, and whether the game has it */
.dupe {
  display: grid; grid-template-columns: minmax(0, 1fr) auto auto; column-gap: 8px;
  padding: 6px 0; border-top: 1px solid var(--line);
}
.dupe .title, .dupe .sub { grid-column: 1; }
.dupe .tag { display: inline; grid-column: 2; grid-row: 1 / span 2; align-self: center; }
.dupe button { grid-column: 3; grid-row: 1 / span 2; align-self: center; padding: 5px 10px; font-size: 14px; }
.sheet .bulk { margin-top: 12px; }
.sheet h3 .key { text-transform: none; letter-spacing: 0; }

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
#message, #rv-message { color: var(--muted); font-size: 14px; padding: 8px 0; }
#message:empty, #rv-message:empty { display: none; }
ul { list-style: none; margin: 0; padding: 0; }
li {
  display: grid; grid-template-columns: 48px minmax(0, 1fr) auto auto;
  column-gap: 12px; align-items: center; padding: 10px 12px;
  background: var(--panel); border: 1px solid var(--line); border-radius: 10px;
  margin-bottom: 6px; cursor: pointer;
}
li > .art { grid-row: span 2; }
li > .meters { grid-column: 2 / 4; margin-top: 4px; }
li > button, li > a.button { grid-column: 4; grid-row: 1 / span 2; }
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

#more, #rv-more { display: block; margin: 12px auto; }
#more[hidden], #rv-more[hidden] { display: none; }
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
  li > button, li > a.button { grid-column: 3; }
  button.dl { min-width: 0; }
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
    <a id="karaoke-link" href="/karaoke" target="_blank" rel="noopener">Karaoke</a>
    <input id="search" type="search" placeholder="Search songs, artists, albums" autocomplete="off">
  </div>
  <div class="bar">
    <div class="tabs" id="tabs" hidden>
      <button data-mode="library" aria-pressed="true">Library</button>
      <button data-mode="rv" aria-pressed="false">RhythmVerse</button>
    </div>
    <span id="lib-tools">
      <select id="sort" aria-label="Sort by">
        <option value="artist">Artist</option>
        <option value="title">Title</option>
        <option value="album">Album</option>
        <option value="origin">Source</option>
      </select>
      <button id="filter" class="plain" hidden>Filters</button>
      <button id="random" class="plain">Random</button>
      <button id="dupes" class="plain" hidden>Duplicates</button>
    </span>
    <span id="rv-tools" hidden>
      <select id="rv-sort" aria-label="Sort RhythmVerse by">
        <option value="">Best match</option>
        <option value="newest">Newest</option>
        <option value="updated">Recently updated</option>
        <option value="downloads">Most downloaded</option>
        <option value="title">Title</option>
        <option value="artist">Artist</option>
        <option value="length">Shortest</option>
      </select>
      <button id="rv-filter" class="plain">Filters</button>
    </span>
    <span id="count"></span>
  </div>
  <div id="banner" hidden></div>
</header>
<main>
  <section id="lib">
    <div id="message">Loading songs&hellip;</div>
    <ul id="list"></ul>
    <button id="more" class="plain" hidden>Show more</button>
  </section>
  <section id="rv" hidden>
    <div id="rv-updates" hidden>
      <div class="updates-bar">
        <span id="rv-updates-text"></span>
        <button class="plain" id="rv-check">Check for updates</button>
        <button id="rv-update-all" hidden>Update all</button>
      </div>
      <ul id="rv-update-list"></ul>
    </div>
    <div class="note" id="rv-note" hidden></div>
    <div id="rv-message"></div>
    <ul id="rv-list"></ul>
    <button id="rv-more" class="plain" hidden>Show more</button>
  </section>
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
<dialog id="rv-filters"><div class="sheet">
  <h2>RhythmVerse filters</h2>
  <h3>Show</h3>
  <div class="chips" id="rvf-show"></div>
  <h3>Has</h3>
  <div class="chips" id="rvf-has"></div>
  <h3>Difficulty at most</h3>
  <div class="caps" id="rvf-cap"></div>
  <h3>Genre</h3>
  <div class="chips" id="rvf-genres"></div>
  <h3>Decade</h3>
  <div class="chips" id="rvf-decades"></div>
  <div class="actions">
    <button class="plain" id="rvf-clear">Clear</button>
    <button id="rvf-done">Done</button>
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
// the list that's up: the song library, or RhythmVerse's songs ("rv")
let mode = "library";

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
  showCount();
  $("message").textContent = songs.length && !matches.length ? "No songs match." : "";
}

function showCount() {
  if (mode === "rv") {
    $("count").textContent = rv.text === null ? "" : rv.total.toLocaleString() + " on RhythmVerse";
  } else {
    $("count").textContent = matches.length === songs.length
      ? songs.length + " songs" : matches.length + " of " + songs.length;
  }
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
    body.append(el("h3", "", "Difficulty"), partsGrid(d.tiers));
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

// every part's difficulty, pro parts too, for a song's sheet
function partsGrid(tiers) {
  const parts = el("div", "parts");
  for (const [part, label] of PARTS.concat(PRO)) {
    const has = part in tiers;
    parts.append(el("span", "", label), has ? dots(tiers[part]) : el("span", "", "–"),
                 el("span", "tier", has ? TIERS[tiers[part]] : "None"));
  }
  return parts;
}

// ---- duplicates: songs in more than one package, and clashes with the
// game's own (band3's /library/duplicates), read from the packages in the
// background the first time
let dupes = null, dupesTimer = null, dupesWaits = 0;
const DUPE_KINDS = {
  same_file: ["Same file", "Copies of one package: band3 loads only the first, and the rest only take up space."],
  song_id: ["Same song ID", "The game takes the first of these it loads and leaves the rest out."],
  shortname: ["Same shortname", "The game has all of these, but what finds a song by its shortname (Select, here) finds only one."],
  similar: ["Same artist and title", "Other charts of the same song, maybe: the game has them all."],
};

async function loadDupes() {
  clearTimeout(dupesTimer);
  try {
    const r = await fetch("/library/duplicates");
    if (!r.ok) return;
    dupes = await r.json();
  } catch (e) {
    return;
  }
  const n = dupes.groups.filter(g => g.kind !== "similar").length;
  $("dupes").hidden = false;
  $("dupes").textContent = dupes.reading ? "Duplicates…" : n ? "Duplicates (" + n + ")" : "Duplicates";
  if ($("sheet").open && $("sheet").dataset.view === "dupes") showDupes();
  // the packages being read, or the game not done loading its songs (just
  // after it starts), for a minute or so at most
  if (dupes.reading) dupesTimer = setTimeout(loadDupes, 1000);
  else if (!dupes.game && ++dupesWaits <= 20) dupesTimer = setTimeout(loadDupes, 3000);
}

function showDupes() {
  const body = $("sheet-body");
  body.replaceChildren(el("h2", "", "Duplicates"));
  const notes = [];
  if (dupes.reading) notes.push(`Reading the song packages: ${dupes.read.toLocaleString()} of ${dupes.total.toLocaleString()}…`);
  else if (!dupes.groups.length) notes.push("No song is in more than one package.");
  if (dupes.unreadable) notes.push((dupes.unreadable === 1 ? "1 package's" : dupes.unreadable + " packages'") + " songs couldn't be read.");
  if (!dupes.reading && !dupes.game) notes.push("The game hasn't said what songs it has yet (it's busy, or still loading them), so its own songs aren't compared yet.");
  for (const note of notes) body.append(el("div", "facts", note));
  // file -> true when it's set aside at the next launch, false when it's done
  const aside = new Map((dupes.set_aside || []).map(s => [s.file, s.next_launch]));
  // as band3 takes them: songs on their own the game leaves out, and copies of a
  // package the size of the one it loads
  const leftOut = dupes.groups.flatMap(g => g.copies.filter(c => !c.in_use && !aside.has(c.file) &&
    (g.kind === "same_file" ? !c.differs : g.kind === "song_id" && c.songs_in_file === 1))).length;
  if (leftOut) {
    const bulk = el("button", "plain bulk", "Set aside the left-out copies (" + leftOut + ")");
    bulk.title = "Each is left out already: when band3 next starts, it renames them to end in .setaside and passes them over";
    bulk.onclick = () => setAside({left_out: true});
    body.append(bulk);
  }
  for (const g of dupes.groups) {
    const [label, why] = DUPE_KINDS[g.kind];
    // the key as it is: shortnames are told apart by case
    const heading = el("h3", "", g.kind === "similar" ? label : label + ": ");
    if (g.kind !== "similar") heading.append(el("span", "key", g.key));
    let note = why;
    if (g.kind === "same_file") {
      const spare = g.copies.filter(c => !c.in_use).reduce((sum, c) => sum + c.size, 0);
      note += " The copies take " + megabytes(spare) + ".";
    }
    body.append(heading, el("div", "facts", note));
    for (const c of g.copies) {
      const row = el("div", "dupe");
      const pack = c.songs_in_file > 1;
      // a copy of a package is told apart by its folder, a song by its file
      const where = c.file
        ? (g.kind === "same_file" ? c.file : c.file.split(/[\\/]/).pop()) +
          (pack && g.kind !== "same_file" ? " (a pack of " + c.songs_in_file + ")" : "") + " · " + megabytes(c.size)
        : "The game's own songs";
      const sub = el("div", "sub", g.kind === "same_file" ? where : c.shortname + " · " + where);
      if (c.file) sub.title = c.file;
      const title = g.kind === "same_file" && pack ? c.songs_in_file + " songs: " + c.title + " and more" : c.title + " – " + c.artist;
      row.append(el("div", "title", title), sub);
      if (aside.has(c.file)) row.append(el("span", "tag", "Set aside at next launch"));
      else if (g.kind === "same_file" && c.differs) row.append(el("span", "tag", "Another size"));
      else if (g.kind === "song_id" || g.kind === "same_file") row.append(el("span", c.in_use ? "tag have" : "tag", c.in_use ? "In use" : "Left out"));
      // a song on its own, never a pack's (its other songs would go with it),
      // but for a copy band3 leaves out: the one it loads has them all
      const leftOutCopy = g.kind === "same_file" && !c.in_use;
      if (g.kind !== "similar" && c.file && (c.songs_in_file === 1 || leftOutCopy)) {
        if (aside.has(c.file)) {
          row.append(putBackButton(c.file, true));
        } else {
          const b = el("button", "plain", "Set aside");
          b.title = (g.kind === "song_id" || g.kind === "same_file") && c.in_use
            ? "When band3 next starts, it renames this one to end in .setaside and passes it over: the next copy takes its place"
            : c.differs ? "Its size isn't the one band3 loads: look before setting it aside. When band3 next starts, it renames this one to end in .setaside and passes it over"
            : "When band3 next starts, it renames this one to end in .setaside and passes it over";
          b.onclick = () => setAside({file: c.file});
          row.append(b);
        }
      }
      body.append(row);
    }
  }
  // what's set aside, to put back
  if (dupes.set_aside && dupes.set_aside.length) {
    body.append(el("h3", "", "Set aside"),
                el("div", "facts", "Renamed to end in .setaside, which band3 passes over; Put back renames them back, for the next launch."));
    for (const s of dupes.set_aside) {
      const row = el("div", "dupe");
      const sub = el("div", "sub", s.file);
      sub.title = s.file;
      row.append(el("div", "title", s.file.split(/[\\/]/).pop()), sub,
                 el("span", "tag", s.next_launch ? "At next launch" : "Set aside"), putBackButton(s.file, s.next_launch));
      body.append(row);
    }
  }
  const actions = el("div", "actions");
  const close = el("button", "plain", "Close");
  close.onclick = () => $("sheet").close();
  actions.append(close);
  body.append(actions);
}

// band3's answer as a toast, and the list again
async function dupeAction(path, request) {
  try {
    const r = await fetch(path, {
      method: "POST", headers: {"Content-Type": "application/json"}, body: JSON.stringify(request),
    });
    toast(await r.text());
  } catch (e) {
    toast("band3 didn't answer");
  }
  loadDupes();
}
function setAside(request) { dupeAction("/library/set_aside", request); }

// Cancel for one set aside at the next launch, Put back for one set aside already
function putBackButton(file, next_launch) {
  const b = el("button", "plain", next_launch ? "Cancel" : "Put back");
  b.title = next_launch ? "Leave it as it is" : "Rename it back, for band3's next launch";
  b.onclick = () => dupeAction("/library/put_back", {file});
  return b;
}

$("dupes").onclick = () => {
  dupesWaits = 0;
  $("sheet").dataset.view = "dupes";
  showDupes();
  if (!$("sheet").open) $("sheet").showModal();
  loadDupes();
};
$("sheet").addEventListener("close", () => { $("sheet").dataset.view = ""; });

function pickRandom() {
  if (!matches.length) { toast("No songs to pick from"); return; }
  openSong(matches[Math.floor(Math.random() * matches.length)], true);
}
$("random").onclick = pickRandom;

for (const dialog of [$("filters"), $("sheet"), $("rv-filters")]) {
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

// ---- RhythmVerse (band3's /rv/...): search its custom songs, and download
// the ones it hosts into the songs folder, for the game's next launch
const rv = {
  text: null,        // what the list is a search for; null before the first
  page: 0, total: 0,
  rows: new Map(),   // file_id -> {s, li}: the row's song and its element
  downloads: new Map(),  // file_id -> this session's download, from /rv/downloads
  folder: "",
  updates: null,     // /rv/updates' last answer
  updateRows: new Map(),  // as rows, for the updates' list
};
const searchText = {library: "", rv: ""};

// ---- RhythmVerse's sort and filters, kept on this device; RhythmVerse
// applies them, but for Downloadable only, which band3 does
const RV_GENRES = [["rock", "Rock"], ["poprock", "Pop-Rock"], ["alternative", "Alternative"],
  ["glam", "Glam"], ["popdanceelectronic", "Pop/Dance/Electronic"], ["country", "Country"],
  ["classicrock", "Classic Rock"], ["reggaeska", "Reggae/Ska"], ["numetal", "Nu-Metal"],
  ["emo", "Emo"], ["metal", "Metal"], ["indierock", "Indie Rock"], ["punk", "Punk"],
  ["new_wave", "New Wave"], ["fusion", "Fusion"], ["grunge", "Grunge"], ["prog", "Prog"],
  ["southernrock", "Southern Rock"], ["novelty", "Novelty"], ["rbsoulfunk", "R&B/Soul/Funk"],
  ["hiphoprap", "Hip-Hop/Rap"], ["blues", "Blues"], ["jazz", "Jazz"], ["jrock", "J-Rock"],
  ["classical", "Classical"], ["world", "World"], ["latin", "Latin"],
  ["inspirational", "Inspirational"], ["other", "Other"]];
const RV_DECADES = [1960, 1970, 1980, 1990, 2000, 2010, 2020];
// the parts a difficulty can be capped on: the game's names, as /song_details'
const RV_CAP_PARTS = [["guitar", "Guitar"], ["bass", "Bass"], ["drum", "Drums"], ["vocals", "Vocals"],
  ["keys", "Keys"], ["real_guitar", "Pro Guitar"], ["real_bass", "Pro Bass"], ["real_keys", "Pro Keys"]];
const RV_DEFAULTS = {sort: "", downloadable: true, has: [], genres: [], decades: [],
                     cap_part: "guitar", cap_tier: null};
const rvFilters = structuredClone(RV_DEFAULTS);
try { Object.assign(rvFilters, JSON.parse(localStorage.getItem("band3.rvfilters")) || {}); } catch (e) {}

function rvFilterCount() {
  return (rvFilters.downloadable ? 1 : 0) + rvFilters.has.length + (rvFilters.cap_tier === null ? 0 : 1) +
         (rvFilters.genres.length ? 1 : 0) + (rvFilters.decades.length ? 1 : 0);
}

// /rv/search's query for a page
function rvQuery(page) {
  const q = new URLSearchParams({text: rv.text, page});
  const has = rvFilters.has.filter(h => h !== "harmonies");
  if (rvFilters.sort) q.set("sort", rvFilters.sort);
  if (rvFilters.downloadable) q.set("downloadable", "1");
  if (has.length) q.set("has", has.join(","));
  if (rvFilters.has.includes("harmonies")) q.set("harmonies", "1");
  if (rvFilters.genres.length) q.set("genre", rvFilters.genres.join(","));
  if (rvFilters.decades.length) q.set("decade", rvFilters.decades.join(","));
  if (rvFilters.cap_tier !== null) q.set("cap", rvFilters.cap_part + ":" + rvFilters.cap_tier);
  return q.toString();
}

// a filter changed: kept, and searched again once the clicking stops
let rvFilterTimer;
function rvFiltersChanged() {
  try { localStorage.setItem("band3.rvfilters", JSON.stringify(rvFilters)); } catch (e) {}
  const n = rvFilterCount();
  $("rv-filter").textContent = n ? "Filters (" + n + ")" : "Filters";
  $("rv-sort").value = rvFilters.sort;
  clearTimeout(rvFilterTimer);
  if (rv.text !== null) rvFilterTimer = setTimeout(() => rvSearch(false), 400);
}

function rvChip(label, on, toggle) {
  const b = el("button", "chip", label);
  b.setAttribute("aria-pressed", on);
  b.onclick = () => {
    toggle();
    b.setAttribute("aria-pressed", b.getAttribute("aria-pressed") !== "true");
    rvFiltersChanged();
  };
  return b;
}

function buildRvFilters() {
  $("rvf-show").replaceChildren(rvChip("Downloadable only", rvFilters.downloadable,
    () => { rvFilters.downloadable = !rvFilters.downloadable; }));
  $("rvf-has").replaceChildren(...HAS.map(([key, label]) =>
    rvChip(label, rvFilters.has.includes(key), () => toggleIn(rvFilters.has, key))));
  const tier = el("select"), part = el("select");
  tier.setAttribute("aria-label", "Difficulty at most");
  part.setAttribute("aria-label", "On the part");
  tier.append(new Option("Any", ""));
  TIERS.forEach((name, i) => tier.append(new Option(name, i)));
  RV_CAP_PARTS.forEach(([key, label]) => part.append(new Option(label, key)));
  tier.value = rvFilters.cap_tier === null ? "" : rvFilters.cap_tier;
  part.value = rvFilters.cap_part;
  tier.onchange = () => { rvFilters.cap_tier = tier.value === "" ? null : Number(tier.value); rvFiltersChanged(); };
  part.onchange = () => { rvFilters.cap_part = part.value; if (rvFilters.cap_tier !== null) rvFiltersChanged(); };
  $("rvf-cap").replaceChildren(tier, part);
  $("rvf-genres").replaceChildren(...RV_GENRES.map(([key, label]) =>
    rvChip(label, rvFilters.genres.includes(key), () => toggleIn(rvFilters.genres, key))));
  $("rvf-decades").replaceChildren(...RV_DECADES.map(y =>
    rvChip(y + "s", rvFilters.decades.includes(y), () => toggleIn(rvFilters.decades, y))));
}

$("rv-filter").onclick = () => { buildRvFilters(); $("rv-filters").showModal(); };
$("rvf-done").onclick = () => $("rv-filters").close();
$("rvf-clear").onclick = () => {
  Object.assign(rvFilters, structuredClone(RV_DEFAULTS), {sort: rvFilters.sort});
  buildRvFilters();
  rvFiltersChanged();
};
$("rv-sort").onchange = () => { rvFilters.sort = $("rv-sort").value; rvFiltersChanged(); };

function norm(text) { return (text || "").toLowerCase().replace(/[^a-z0-9]+/g, ""); }
// a song by this artist and title is in the library already
let libraryKeys = null;
function inLibrary(s) {
  if (!libraryKeys) libraryKeys = new Set(songs.map(x => norm(x.artist) + "|" + norm(x.title)));
  return libraryKeys.has(norm(s.artist) + "|" + norm(s.title));
}

function megabytes(bytes) { return (bytes / 1048576).toFixed(1) + " MB"; }

function rvArt(s, className) {
  const tile = el("div", className || "art");
  if (s.art.startsWith("https://rhythmverse.co/")) {
    const img = el("img");
    img.alt = "";
    img.loading = "lazy";
    img.decoding = "async";
    img.referrerPolicy = "no-referrer";
    img.onerror = () => img.remove();
    img.src = s.art;
    tile.append(img);
  }
  return tile;
}

// downloaded (here or before), downloading, failed, or null
function rvState(s) {
  const d = rv.downloads.get(s.file_id);
  if (d && (d.state === "queued" || d.state === "downloading")) return d;
  if (s.downloaded || (d && d.state === "done")) return {state: "done"};
  return d || null;
}

function rvPageLink(s, text) {
  const a = el("a", "button", text);
  a.href = s.page.startsWith("https://rhythmverse.co/") ? s.page : "https://rhythmverse.co/";
  a.target = "_blank";
  a.rel = "noopener";
  a.onclick = e => e.stopPropagation();
  return a;
}

// the game has this song (by its song ID), so Select can find it
function inGame(s) {
  const d = rv.downloads.get(s.file_id);
  return !!s.song_id && !!(s.in_library || (d && d.in_library));
}

// Select, as the Library tab's: the game's shortname for the song comes from
// its song ID (RB3E's /song_<id>)
function rvSelectButton(s) {
  const b = el("button", "pick", "Select");
  b.title = "The game has this song: select it in the Music Library";
  b.onclick = async e => {
    e.stopPropagation();
    try {
      const r = await fetch("/song_" + s.song_id);
      const song = r.ok && parseSongs("[song]\n" + await r.text())[0];
      if (!song || !song.shortname) { toast("The game doesn't have it yet"); return; }
      jump({shortname: song.shortname, title: song.title || s.title});
    } catch (err) {
      toast("The game didn't answer");
    }
  };
  return b;
}

// a song's button: Select when the game has it, else Download, how far along
// it is, or Open for a song
// RhythmVerse doesn't host, to download from its page
// a newer version of one band3 downloaded is on RhythmVerse, and not being
// downloaded already
function updateAvailable(s) {
  const d = rv.downloads.get(s.file_id);
  return s.update === "available" && !(d && d.update && d.state !== "failed");
}

// the newer version is downloaded, for band3's next launch
function updatePending(s) {
  const d = rv.downloads.get(s.file_id);
  return s.update === "pending" || !!(d && d.update && d.state === "done");
}

function rvUpdateButton(s) {
  const b = el("button", "dl", "Update");
  b.title = "Download RhythmVerse's newer version; band3 puts it in place when it next starts";
  b.onclick = e => { e.stopPropagation(); download(s, true); };
  return b;
}

function rvAction(s) {
  if (updateAvailable(s)) return rvUpdateButton(s);
  const d = rvState(s);
  // an update downloading: its progress, below
  if (inGame(s) && !(d && d.update && (d.state === "queued" || d.state === "downloading"))) {
    return rvSelectButton(s);
  }
  if (!s.download && !(d && d.state === "done")) {
    const a = rvPageLink(s, "Open");
    a.title = s.host === "rhythmverse.co" ? "Zipped: download it from its RhythmVerse page"
      : s.host ? "On " + s.host + ": download it from its RhythmVerse page"
      : "Download it from its RhythmVerse page";
    return a;
  }
  const b = el("button", "dl", "Download");
  b.onclick = e => { e.stopPropagation(); download(s); };
  if (!d) return b;
  if (d.state === "failed") {
    b.textContent = "Retry";
    b.title = d.error;
    return b;
  }
  b.className = "dl plain";
  b.disabled = true;
  b.textContent = d.state === "done" ? "Downloaded" : d.state === "queued" ? "Waiting"
    : d.total ? Math.min(99, Math.floor(100 * d.received / d.total)) + "%" : megabytes(d.received);
  return b;
}

// what the game has of a song, as [tag, its tooltip]: by song ID when band3
// says (in_library), else by artist and title; [null] for nothing
function libraryTag(s) {
  const d = rv.downloads.get(s.file_id);
  const downloaded = rvState(s) && rvState(s).state === "done";
  if (updateAvailable(s)) return ["Update available", "RhythmVerse has a newer version than the one band3 downloaded"];
  if (updatePending(s)) return ["Updated at next launch", "The newer version is downloaded; band3 puts it in place of the old one (kept as .replaced) when it next starts"];
  if (s.in_library || (d && d.in_library)) return ["In library", "The game has this song (by its song ID)"];
  if (downloaded) return ["Not in game yet", "The game adds it as it did songs from the store: in the Music Library, or after a song that's playing"];
  if (s.in_library === null && inLibrary(s)) return ["In library", "A song by this artist and title is in the game"];
  if (inLibrary(s)) return ["Similar in library", "Another chart of this artist and title is in the game"];
  return [null, null];
}

function rvRow(s, rows = rv.rows) {
  const li = el("li");
  li.append(rvArt(s));
  const info = el("div", "info");
  info.append(el("div", "title", s.title),
              el("div", "sub", [s.artist, s.album].filter(Boolean).join(" · ")));
  const [text, title] = libraryTag(s);
  const tag = el("span", text ? "tag have" : "tag", text || s.author);
  if (title) tag.title = title;
  const action = rvAction(s);
  li.append(info, tag, action, meters(s));
  li.onclick = () => openRvSong(s);
  rows.set(s.file_id, {s, li});
  return li;
}

// a row again, in RhythmVerse's songs and the updates, for a download's progress
function refreshRow(fileId) {
  for (const rows of [rv.rows, rv.updateRows]) {
    const row = rows.get(fileId);
    if (row) row.li.replaceWith(rvRow(row.s, rows));
  }
  showUpdatesBar();
}

// ---- updates: newer versions on RhythmVerse of songs band3 downloaded, found
// by band3's check at launch, a search, or Check for updates (/rv/updates)
let updatesTimer = null;

function ago(seconds) {
  const s = Math.max(0, Date.now() / 1000 - seconds);
  if (s < 90) return "just now";
  if (s < 90 * 60) return Math.round(s / 60) + " minutes ago";
  if (s < 36 * 3600) return Math.round(s / 3600) + " hours ago";
  return Math.round(s / 86400) + " days ago";
}

// what the check says, its buttons, and the count on the tab
function showUpdatesBar() {
  const u = rv.updates;
  const available = u ? u.updates.filter(updateAvailable).length : 0;
  $("tabs").children[1].textContent = available ? "RhythmVerse (" + available + ")" : "RhythmVerse";
  if (!u) return;
  const songs = u.downloads === 1 ? "the song" : "the " + u.downloads + " songs";
  $("rv-updates-text").textContent = u.checking ? "Checking RhythmVerse for updates…"
    : u.error ? "Couldn't check for updates: " + u.error
    : available ? (available === 1 ? "1 update" : available + " updates") + " for songs band3 downloaded"
    : u.updates.length ? "Updates downloaded, for band3's next launch"
    : u.checked ? "No updates for " + songs + " band3 downloaded (checked " + ago(u.checked) + ")"
    : "Not checked for updates yet";
  $("rv-check").disabled = u.checking;
  $("rv-check").textContent = u.checking ? "Checking…" : "Check for updates";
  $("rv-update-all").hidden = available < 2;
}

function showUpdates() {
  const u = rv.updates;
  // nothing band3 downloaded, nothing to check
  $("rv-updates").hidden = !u.downloads;
  rv.updateRows.clear();
  $("rv-update-list").replaceChildren(...u.updates.map(s => rvRow(s, rv.updateRows)));
  showUpdatesBar();
}

// asked again every couple of seconds while a check runs
async function loadUpdates() {
  clearTimeout(updatesTimer);
  try {
    const r = await fetch("/rv/updates");
    if (!r.ok) return;
    rv.updates = await r.json();
  } catch (e) {
    return;
  }
  showUpdates();
  if (rv.updates.checking) updatesTimer = setTimeout(loadUpdates, 2000);
}

$("rv-check").onclick = async () => {
  try {
    const r = await fetch("/rv/check", {
      method: "POST", headers: {"Content-Type": "application/json"}, body: "{}",
    });
    if (!r.ok) { toast(await r.text()); return; }
  } catch (e) {
    toast("band3 didn't answer");
    return;
  }
  rv.updates.checking = true;
  rv.updates.error = "";
  showUpdatesBar();
  clearTimeout(updatesTimer);
  updatesTimer = setTimeout(loadUpdates, 1000);
};

$("rv-update-all").onclick = async () => {
  for (const s of rv.updates.updates.filter(updateAvailable)) await download(s, true);
};

function openRvSong(s) {
  const body = $("sheet-body");
  body.replaceChildren(rvArt(s, "cover"), el("h2", "", s.title),
                       el("div", "sub", [s.artist, s.album].filter(Boolean).join(" · ")));
  const facts = [s.year || null, s.genre || null, s.length_ms ? minutes(s.length_ms) : null,
                 s.vocal_parts > 1 ? s.vocal_parts + "-part harmonies" : null];
  body.append(el("div", "facts", facts.filter(Boolean).join(" · ")));
  const by = [s.author ? "By " + s.author : null, s.size ? megabytes(s.size) : null,
              s.downloads.toLocaleString() + " downloads"];
  body.append(el("div", "facts", by.filter(Boolean).join(" · ")));
  const [have, why] = libraryTag(s);
  if (have) body.append(el("div", "facts", have + ": " + why.charAt(0).toLowerCase() + why.slice(1) + "."));
  body.append(el("h3", "", "Difficulty"), partsGrid(s.tiers));
  const actions = el("div", "actions");
  const close = el("button", "plain", "Close");
  close.onclick = () => $("sheet").close();
  actions.append(close, rvPageLink(s, "RhythmVerse page"));
  if (inGame(s) || s.download || rvState(s)) {
    const action = rvAction(s);
    if (action.tagName === "BUTTON") action.addEventListener("click", () => $("sheet").close());
    // Select as well as Update, for a song the game has
    if (updateAvailable(s) && inGame(s)) actions.append(rvSelectButton(s));
    actions.append(action);
  }
  body.append(actions);
  if (!$("sheet").open) $("sheet").showModal();
}

// a new search (or the newest songs, without text), or its next page
let rvSeq = 0;
async function rvSearch(more) {
  if (!more) {
    rv.text = $("search").value.trim();
    rv.page = 0;
    rv.total = 0;
    rv.rows.clear();
    rv.emptyPages = 0;
    $("rv-list").replaceChildren();
    $("rv-message").textContent = rv.text ? "Searching RhythmVerse…" : "Loading RhythmVerse's newest songs…";
  }
  const seq = ++rvSeq;
  $("rv-more").hidden = true;
  try {
    const r = await fetch("/rv/search?" + rvQuery(rv.page + 1));
    if (seq !== rvSeq) return;
    if (!r.ok) throw new Error(await r.text());
    const res = await r.json();
    if (seq !== rvSeq) return;
    rv.page = res.page;
    rv.total = res.total;
    $("rv-list").append(...res.songs.map(s => rvRow(s)));
    const last = rv.page * res.page_size >= rv.total;
    $("rv-more").hidden = last;
    // with Downloadable only, a page can have none: on to the next, a few at most
    rv.emptyPages = res.songs.length ? 0 : (rv.emptyPages || 0) + 1;
    if (!res.songs.length && !last && rv.emptyPages < 5) {
      rvSearch(true);
      return;
    }
    $("rv-message").textContent = rv.rows.size ? "" : rvFilters.downloadable
      ? "Nothing RhythmVerse hosts matches. Turn off Downloadable only for songs on other sites."
      : "Nothing on RhythmVerse matches.";
    showCount();
  } catch (e) {
    if (seq !== rvSeq) return;
    $("rv-message").textContent = "Couldn't search RhythmVerse: " + e.message + " ";
    const retry = el("button", "plain", "Retry");
    retry.onclick = () => rvSearch(more);
    $("rv-message").append(retry);
  }
}

async function download(s, update) {
  try {
    const r = await fetch("/rv/download", {
      method: "POST", headers: {"Content-Type": "application/json"},
      body: JSON.stringify({file_id: s.file_id, update: !!update}),
    });
    const text = await r.text();
    if (!r.ok) { toast(text); return; }
    if (text === "Already in the song folders") {
      s.downloaded = true;
      refreshRow(s.file_id);
      toast(`“${s.title}” is in the song folders already`);
      return;
    }
    rv.downloads.set(s.file_id, {file_id: s.file_id, title: s.title, state: "queued", received: 0,
                                 total: s.size, song_id: s.song_id, update: !!update});
    waitingSince = null;
    refreshRow(s.file_id);
    pollDownloads();
  } catch (e) {
    toast("band3 didn't answer");
  }
}

// the downloads, every second while one's going, and every few while
// downloaded songs wait for the game, for up to WAIT_FOR_GAME_MS
let downloadsTimer = null;
let waitingSince = null;
const WAIT_FOR_GAME_MS = 10 * 60 * 1000;
async function pollDownloads() {
  clearTimeout(downloadsTimer);
  try {
    const r = await fetch("/rv/downloads");
    if (!r.ok) return;
    const res = await r.json();
    rv.folder = res.folder;
    let busy = false;
    for (const d of res.downloads) {
      const was = rv.downloads.get(d.file_id);
      rv.downloads.set(d.file_id, d);
      if (was && was.state !== d.state) {
        if (d.state === "done") toast(`Downloaded “${d.title}”`);
        if (d.state === "failed") toast(`Couldn't download “${d.title}”: ${d.error}`);
      } else if (was && d.in_library && was.in_library === false) {
        toast(`“${d.title}” is in the game`);
      }
      refreshRow(d.file_id);
      busy = busy || d.state === "queued" || d.state === "downloading";
    }
    const updates = res.downloads.filter(d => d.state === "done" && d.update).length;
    const done = res.downloads.filter(d => d.state === "done" && !d.update);
    // in_library is null while the game's busy: still waiting, as far as anyone knows
    // only songs with a song ID can be seen joining the game
    const waiting = done.filter(d => d.song_id && !d.in_library).length;
    const songs = n => n === 1 ? "1 song" : n + " songs";
    const notes = [];
    if (done.length) {
      notes.push(songs(done.length) + " downloaded to " + rv.folder + ". " + (waiting
        ? (done.length === 1 ? "It's not" : waiting === 1 ? "1 isn't" : waiting + " aren't") +
          " in the game yet: " +
          "the game adds songs as it did ones from the store, when you're in the Music Library (or once the song that's playing is over)."
        : (done.length === 1 ? "It's" : "They're") + " in the game."));
    }
    if (updates) {
      notes.push((updates === 1 ? "1 update" : updates + " updates") + " downloaded: the game has the " +
        "old version open, so band3 puts the new one in place when it next starts, keeping the old " +
        "one beside it as .replaced.");
    }
    $("rv-note").hidden = !notes.length;
    $("rv-note").textContent = notes.join(" ");
    // downloading, every second; waiting for the game, every few
    if (!waiting) waitingSince = null;
    else if (waitingSince === null) waitingSince = Date.now();
    if (busy) downloadsTimer = setTimeout(pollDownloads, 1000);
    // not for ever: a song can wait on the game for a while, e.g. a long
    // song playing, and the page's next download or reload asks again
    else if (waiting && Date.now() - waitingSince < WAIT_FOR_GAME_MS) {
      downloadsTimer = setTimeout(pollDownloads, 3000);
    }
  } catch (e) {}
}

function setMode(next) {
  // a search typed, or a filter changed, on the other tab isn't this one's
  clearTimeout(searchTimer);
  clearTimeout(rvFilterTimer);
  searchText[mode] = $("search").value;
  mode = next;
  try { localStorage.setItem("band3.mode", mode); } catch (e) {}
  for (const b of $("tabs").children) b.setAttribute("aria-pressed", b.dataset.mode === mode);
  $("lib").hidden = mode !== "library";
  $("rv").hidden = mode !== "rv";
  $("lib-tools").hidden = mode !== "library";
  $("rv-tools").hidden = mode !== "rv";
  $("search").value = searchText[mode];
  $("search").placeholder = mode === "rv" ? "Search RhythmVerse's custom songs"
                                          : "Search songs, artists, albums";
  if (mode === "rv" && rv.text === null) rvSearch(false);
  if (mode === "rv") loadUpdates();
  showCount();
}
for (const b of $("tabs").children) b.onclick = () => { if (b.dataset.mode !== mode) setMode(b.dataset.mode); };

// the tab, when band3 has RhythmVerse on (/rv/downloads answers)
async function initRhythmVerse() {
  try {
    const r = await fetch("/rv/downloads");
    if (!r.ok) return;
  } catch (e) {
    return;
  }
  $("tabs").hidden = false;
  rvFiltersChanged();  // the saved filters' count on the button
  pollDownloads();
  let saved = null;
  try { saved = localStorage.getItem("band3.mode"); } catch (e) {}
  if (saved === "rv") setMode("rv");
  else loadUpdates();  // for the count of them on the tab
}

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
    libraryKeys = null;
    filter();
    if (hasArt) {
      loadDetails(true);
      pollStatus();
      initRhythmVerse();
      loadDupes();
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
  // RhythmVerse is asked once typing stops for a moment, not at every key
  if (mode === "rv") searchTimer = setTimeout(() => rvSearch(false), 700);
  else searchTimer = setTimeout(filter, 150);
});
$("search").addEventListener("keydown", e => {
  if (e.key !== "Enter" || mode !== "rv") return;
  clearTimeout(searchTimer);
  rvSearch(false);
});
$("rv-more").addEventListener("click", () => rvSearch(true));
$("sort").addEventListener("change", filter);
$("more").addEventListener("click", showMore);
load();
</script>
</body>
</html>
)html";

}
