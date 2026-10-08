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

}
