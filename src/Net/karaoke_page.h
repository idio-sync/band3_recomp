#pragma once
#include <string_view>

// The web server's /karaoke page (http_server.h): a song's lyrics, in time,
// for a screen facing the room. It follows /live/events and reads
// /lyrics?shortname= and /album_art?shortname=, and loads nothing from
// outside the server. kKaraokeModel is its logic, served apart at
// /karaoke/model.js so tools/test_karaoke_model.js can run it under node;
// tools/web_preview.py serves both without the game. Only band3's build
// includes this file: MSVC (the unit tests) can't take literals this long.

namespace band3::http {

inline constexpr std::string_view kKaraokeModel = R"js('use strict';
var KaraokeModel = (function () {
  // under this far off, the clock eases towards the game's; further, it jumps
  var kJumpMs = 250;
  var kEase = 0.1;
  // a gap between lines longer than this counts down to the next
  var kGapMs = 3000;

  // The song's time: the game's clock as of its last update, run on since
  function Clock() { this.base = null; this.at = 0; this.paused = false; }
  Clock.prototype.now = function (nowMs) {
    if (this.base === null) return null;
    return this.paused ? this.base : this.base + (nowMs - this.at);
  };
  Clock.prototype.update = function (songMs, nowMs, paused) {
    var predicted = this.now(nowMs);
    this.paused = paused;
    this.at = nowMs;
    if (predicted === null || paused || Math.abs(predicted - songMs) > kJumpMs) {
      this.base = songMs;
    } else {
      this.base = predicted + (songMs - predicted) * kEase;
    }
  };
  Clock.prototype.reset = function () { this.base = null; this.paused = false; };

  // harmonies show harm1 to harm3; otherwise the lead, or harm1 where a song has no lead
  function partsToShow(lyrics, vocals) {
    if (!lyrics) return [];
    var byName = {};
    lyrics.parts.forEach(function (p) { byName[p.part] = p; });
    if (vocals === 'harmonies') {
      var harmonies = ['harm1', 'harm2', 'harm3'].map(function (n) { return byName[n]; })
        .filter(Boolean);
      if (harmonies.length) return harmonies;
    }
    var lead = byName.lead || byName.harm1;
    return lead ? [lead] : [];
  }

  // the line being sung, or else the next to come; and the one after it
  function linesAt(part, ms) {
    var lines = part.lines;
    for (var i = 0; i < lines.length; i++) {
      if (ms < lines[i].end_ms) return { current: lines[i], next: lines[i + 1] || null, index: i };
    }
    return { current: null, next: null, index: lines.length };
  }

  function fill(syllable, ms) {
    if (ms <= syllable.start_ms) return 0;
    if (ms >= syllable.end_ms) return 1;
    return (ms - syllable.start_ms) / (syllable.end_ms - syllable.start_ms);
  }

  // in a gap over kGapMs before the next line, how much of it is left (1 to 0)
  function countdown(part, ms) {
    var at = linesAt(part, ms);
    if (!at.current || ms >= at.current.start_ms) return null;
    var from = at.index > 0 ? part.lines[at.index - 1].end_ms : 0;
    var gap = at.current.start_ms - from;
    if (gap <= kGapMs) return null;
    return Math.max(0, Math.min(1, (at.current.start_ms - ms) / gap));
  }

  // which screen to show; done: every part's last line is over
  function screen(state, clockKnown, partsCount, done) {
    if (!state || !state.song) return 'idle';
    if (!state.in_game) return 'just_played';
    if (!clockKnown) return 'up_next';
    return partsCount && !done ? 'lyrics' : 'now_playing';
  }

  // a line's syllables, each with the space after it unless it joins the next
  function words(line) {
    var last = line.syllables.length - 1;
    return line.syllables.map(function (s, i) {
      return { text: s.text + (s.join || i === last ? '' : ' '), syllable: s };
    });
  }

  return { Clock: Clock, partsToShow: partsToShow, linesAt: linesAt, fill: fill,
           countdown: countdown, screen: screen, words: words, kGapMs: kGapMs };
})();
if (typeof module !== 'undefined') module.exports = KaraokeModel;
)js";

inline constexpr std::string_view kKaraokePage = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>band3 karaoke</title>
<style>
/* Oswald Bold (Latin), by Vernon Adams and others, SIL Open Font License 1.1 */
@font-face {
  font-family: "Karaoke"; font-weight: 700; font-display: block;
  src: url(data:font/woff2;base64,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) format("woff2");
}
:root {
  --bg: #05060a; --band: rgba(12, 14, 26, 0.82); --edge: rgba(130, 150, 255, 0.22);
  --unsung: #f2f2f6; --next: rgba(242, 242, 246, 0.42); --muted: #8c8ca6;
  --lead: #57c1ff; --harm1: #57c1ff; --harm2: #ff9d3b; --harm3: #c8813f;
  --scale: 1;
}
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; }
body {
  background: radial-gradient(ellipse at 50% 30%, #1a1d3a 0%, var(--bg) 70%);
  color: var(--unsung); overflow: hidden;
  font-family: "Karaoke", "Arial Narrow", "Roboto Condensed", system-ui, sans-serif;
  font-weight: 700;
}
#stage { position: fixed; inset: 0; display: flex; align-items: center; justify-content: center; }
#lyrics {
  width: 100%; padding: 4vh 4vw; background: var(--band);
  border-top: 2px solid var(--edge); border-bottom: 2px solid var(--edge);
  box-shadow: 0 0 60px rgba(0, 0, 0, 0.6);
}
.part { --sung: var(--lead); padding: 1vh 0; }
.part[data-part="harm1"] { --sung: var(--harm1); }
.part[data-part="harm2"] { --sung: var(--harm2); }
.part[data-part="harm3"] { --sung: var(--harm3); }
.line {
  text-align: center; line-height: 1.15; white-space: pre-wrap; overflow-wrap: anywhere;
  text-wrap: balance; filter: drop-shadow(0 3px 6px rgba(0, 0, 0, 0.7));
}
.line.current { font-size: calc(var(--scale) * min(7.5vw, 13vh)); min-height: 1.15em; }
.line.next { font-size: calc(var(--scale) * min(4.5vw, 7vh)); color: var(--next); min-height: 1.15em; }
#parts[data-count="2"] .line.current { font-size: calc(var(--scale) * min(6vw, 9vh)); }
#parts[data-count="3"] .line.current { font-size: calc(var(--scale) * min(5vw, 7vh)); }
#parts[data-count="2"] .line.next, #parts[data-count="3"] .line.next { display: none; }
.syl {
  background: linear-gradient(90deg, var(--sung) 0 calc(var(--fill, 0) * 100%),
                              var(--unsung) calc(var(--fill, 0) * 100%) 100%);
  -webkit-background-clip: text; background-clip: text; color: transparent;
}
/* room for italics' overhang, which background-clip would cut off */
.syl.spoken { font-style: italic; padding-right: 0.08em; margin-right: -0.08em; }
#countdown { height: 0.8vh; margin: 2vh auto 0; width: 40%; background: rgba(255, 255, 255, 0.12); }
#countdown div { height: 100%; background: var(--lead); transform-origin: left; }
#card { text-align: center; padding: 4vw; }
#card img { width: min(40vh, 60vw); aspect-ratio: 1; object-fit: cover; border-radius: 6px;
            box-shadow: 0 10px 40px rgba(0, 0, 0, 0.6); }
#card-label { color: var(--lead); letter-spacing: 0.2em; text-transform: uppercase;
              font-size: min(3vw, 4vh); margin-top: 3vh; }
#card-title { font-size: min(7vw, 10vh); line-height: 1.1; }
#card-artist { color: var(--muted); font-size: min(4vw, 6vh); }
#paused, #conn {
  position: fixed; left: 50%; transform: translateX(-50%); padding: 0.4em 1em; border-radius: 999px;
  background: rgba(0, 0, 0, 0.7); color: var(--unsung); font-size: min(3vw, 4vh);
}
#paused { top: 6vh; }
#conn { bottom: 4vh; font-family: system-ui, sans-serif; font-weight: 400; font-size: 16px; }
#gear {
  position: fixed; right: 12px; bottom: 12px; width: 44px; height: 44px; border-radius: 50%;
  border: 0; background: rgba(255, 255, 255, 0.08); color: var(--muted); font-size: 22px; cursor: pointer;
}
#panel {
  position: fixed; right: 12px; bottom: 64px; width: min(340px, calc(100vw - 24px)); padding: 16px;
  border-radius: 10px; background: #15172a; color: var(--unsung);
  font: 15px/1.4 system-ui, sans-serif;
}
#panel .row { display: flex; gap: 8px; align-items: center; margin-bottom: 10px; flex-wrap: wrap; }
#panel .row > span:first-child { flex: 1 0 100%; color: var(--muted); }
#panel button { padding: 8px 12px; border-radius: 6px; border: 1px solid #33365a;
                background: #1f2240; color: inherit; font: inherit; cursor: pointer; }
#panel .note { color: var(--muted); font-size: 13px; margin: 6px 0 0; }
[hidden] { display: none !important; }
</style>
</head>
<body>
<div id="stage">
  <div id="lyrics" hidden>
    <div id="parts"></div>
    <div id="countdown" hidden><div></div></div>
  </div>
  <div id="card">
    <img id="art" alt="" hidden>
    <div id="card-label"></div>
    <div id="card-title">band3 karaoke</div>
    <div id="card-artist">Waiting for a song</div>
  </div>
</div>
<div id="paused" hidden>Paused</div>
<div id="conn" hidden></div>
<button id="gear" aria-label="Settings" aria-expanded="false">&#9881;</button>
<div id="panel" hidden>
  <div class="row"><span>Timing: if the words come early, press Later</span>
    <button data-act="earlier">Earlier</button><b id="offset">0 ms</b>
    <button data-act="later">Later</button><button data-act="reset">Reset</button></div>
  <div class="row"><span>Text size</span>
    <button data-act="smaller">A&minus;</button><button data-act="bigger">A+</button></div>
  <div class="row"><button data-act="full">Full screen</button></div>
  <p class="note">Turn off sleep on this device: a page served over plain http can't keep
  the screen awake.</p>
</div>
<script src="/karaoke/model.js"></script>
<script>
(function () {
  'use strict';
  var M = KaraokeModel;
  function $(id) { return document.getElementById(id); }

  // per device, in this browser only; the page works without it
  var settings = { offset: 0, scale: 1 };
  try { Object.assign(settings, JSON.parse(localStorage.getItem('band3.karaoke') || '{}')); } catch (e) {}
  function save() { try { localStorage.setItem('band3.karaoke', JSON.stringify(settings)); } catch (e) {} }
  function apply() {
    document.documentElement.style.setProperty('--scale', settings.scale);
    $('offset').textContent = (settings.offset > 0 ? '+' : '') + settings.offset + ' ms';
  }

  var state = null, lyrics = null, lyricsFor = null, clock = new M.Clock(), clockKnown = false;

  function loadLyrics(shortname) {
    lyricsFor = shortname;
    lyrics = null;
    fetch('/lyrics?shortname=' + encodeURIComponent(shortname)).then(function (r) {
      if (r.status === 503) {
        // the game is busy (a loading screen): try again shortly
        setTimeout(function () { if (lyricsFor === shortname) loadLyrics(shortname); }, 2000);
        return null;
      }
      return r.ok ? r.json() : null;
    }).then(function (j) { if (j && lyricsFor === shortname) lyrics = j; }).catch(function () {});
  }

  function onState(s) {
    if (s.song && s.song.shortname !== lyricsFor) loadLyrics(s.song.shortname);
    if (!s.in_game || !state || !state.in_game) { clock.reset(); clockKnown = false; }
    state = s;
  }

  function onClock(c) {
    clock.update(c.song_ms, performance.now(), !!(state && state.paused));
    clockKnown = true;
  }

  function connect() {
    var es = new EventSource('/live/events');
    es.addEventListener('state', function (e) { $('conn').hidden = true; onState(JSON.parse(e.data)); });
    es.addEventListener('clock', function (e) { onClock(JSON.parse(e.data)); });
    es.onerror = function () {
      if (es.readyState === EventSource.CLOSED) {
        // not a stream: too many live pages open, or a band3 from before them
        es.close();
        $('conn').textContent = "Can't follow the game: close another live page, or update band3";
        $('conn').hidden = false;
        setTimeout(connect, 5000);
      } else {
        $('conn').textContent = 'Reconnecting…';
        $('conn').hidden = false;
      }
    };
  }

  // the lyrics on screen: each part's element, and the line each shows
  var partEls = [], shownKey = '';
  function setParts(parts) {
    var key = parts.map(function (p) { return p.part; }).join(',');
    if (key === shownKey) return;
    shownKey = key;
    var box = $('parts');
    box.textContent = '';
    box.dataset.count = String(parts.length);
    partEls = parts.map(function (p) {
      var el = document.createElement('div');
      el.className = 'part';
      el.dataset.part = p.part;
      var cur = document.createElement('div'); cur.className = 'line current';
      var nxt = document.createElement('div'); nxt.className = 'line next';
      el.appendChild(cur); el.appendChild(nxt);
      box.appendChild(el);
      return { cur: cur, next: nxt, line: undefined, spans: [] };
    });
  }

  function showLine(slot, at) {
    if (slot.line === at.current) return;
    slot.line = at.current;
    slot.cur.textContent = '';
    slot.spans = at.current ? M.words(at.current).map(function (w) {
      var s = document.createElement('span');
      s.className = 'syl' + (w.syllable.spoken ? ' spoken' : '');
      s.textContent = w.text;
      slot.cur.appendChild(s);
      return { el: s, syllable: w.syllable, fill: -1 };
    }) : [];
    slot.next.textContent = at.next ? M.words(at.next).map(function (w) { return w.text; }).join('') : '';
  }

  function drawLyrics(parts, ms) {
    setParts(parts);
    parts.forEach(function (p, i) {
      var slot = partEls[i];
      showLine(slot, M.linesAt(p, ms));
      slot.spans.forEach(function (s) {
        var f = Math.round(M.fill(s.syllable, ms) * 100) / 100;
        if (f !== s.fill) { s.fill = f; s.el.style.setProperty('--fill', f); }
      });
    });
    var left = M.countdown(parts[0], ms);
    $('countdown').hidden = left === null;
    if (left !== null) $('countdown').firstChild.style.transform = 'scaleX(' + left + ')';
  }

  var cardKey = '';
  function drawCard(mode) {
    var song = state && state.song;
    var key = mode + '|' + (song ? song.shortname : '');
    if (key === cardKey) return;
    cardKey = key;
    var labels = { up_next: 'Up next', now_playing: 'Now playing', just_played: 'Just played', idle: '' };
    $('card-label').textContent = labels[mode];
    $('card-title').textContent = song ? song.title : 'band3 karaoke';
    $('card-artist').textContent = song ? song.artist : 'Waiting for a song';
    var art = $('art');
    art.hidden = true;
    if (song) {
      art.onload = function () { art.hidden = false; };
      art.onerror = function () { art.hidden = true; };
      art.src = '/album_art?shortname=' + encodeURIComponent(song.shortname);
    }
  }

  function frame() {
    requestAnimationFrame(frame);
    var now = clock.now(performance.now());
    var ms = now === null ? null : now - settings.offset;
    var parts = M.partsToShow(lyrics, state && state.vocals);
    var done = ms !== null && parts.length > 0 &&
      parts.every(function (p) { return M.linesAt(p, ms).current === null; });
    var mode = M.screen(state, clockKnown && ms !== null, parts.length, done);
    $('lyrics').hidden = mode !== 'lyrics';
    $('card').hidden = mode === 'lyrics';
    $('paused').hidden = !(state && state.in_game && state.paused);
    if (mode === 'lyrics') drawLyrics(parts, ms); else drawCard(mode);
  }

  $('gear').onclick = function () {
    var open = $('panel').hidden;
    $('panel').hidden = !open;
    $('gear').setAttribute('aria-expanded', String(open));
  };
  $('panel').onclick = function (e) {
    var act = e.target.dataset && e.target.dataset.act;
    if (act === 'earlier') settings.offset -= 50;
    else if (act === 'later') settings.offset += 50;
    else if (act === 'reset') settings.offset = 0;
    else if (act === 'smaller') settings.scale = Math.max(0.6, Math.round((settings.scale - 0.1) * 10) / 10);
    else if (act === 'bigger') settings.scale = Math.min(1.6, Math.round((settings.scale + 0.1) * 10) / 10);
    else if (act === 'full') {
      if (document.fullscreenElement) document.exitFullscreen();
      else if (document.documentElement.requestFullscreen) document.documentElement.requestFullscreen();
      return;
    } else return;
    save();
    apply();
  };

  apply();
  connect();
  requestAnimationFrame(frame);
})();
</script>
</body>
</html>
)html";

}
