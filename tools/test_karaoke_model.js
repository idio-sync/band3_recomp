// Checks the karaoke page's logic: src/Net/karaoke_page.h's kKaraokeModel,
// read out of the header as band3 serves it at /karaoke/model.js.
//   node --test tools/test_karaoke_model.js
'use strict';
const test = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');

const header = fs.readFileSync(path.join(__dirname, '..', 'src', 'Net', 'karaoke_page.h'), 'utf8');
const source = header.split('kKaraokeModel = R"js(')[1].split(')js"')[0];
// run in this context, so its arrays are this context's (deepStrictEqual compares prototypes)
const mod = {};
new Function('module', source)(mod);
const M = mod.exports;

const line = (start, end, syllables) => ({ start_ms: start, end_ms: end, syllables });
const syl = (start, end, text, join = false) => ({ start_ms: start, end_ms: end, text, join, spoken: false });
const part = { part: 'lead', lines: [
  line(1000, 2000, [syl(1000, 1400, 'Hel', true), syl(1400, 2000, 'lo')]),
  line(2500, 3000, [syl(2500, 3000, 'there')]),
  line(8000, 9000, [syl(8000, 9000, 'later')]),
] };

test('the clock runs on from its last update, and holds while paused', () => {
  const c = new M.Clock();
  assert.strictEqual(c.now(0), null);
  c.update(5000, 100, false);
  assert.strictEqual(c.now(600), 5500);
  c.update(5600, 700, true);
  assert.strictEqual(c.now(5000), 5600);
});

test('the clock eases towards a small difference and jumps at a big one', () => {
  const c = new M.Clock();
  c.update(5000, 0, false);
  c.update(5100, 0, false);  // predicted 5000, told 5100: a tenth of the way
  assert.strictEqual(c.now(0), 5010);
  c.update(0, 0, false);     // a restart: straight there
  assert.strictEqual(c.now(0), 0);
  c.update(9000, 1000, false);  // a jump forward too
  assert.strictEqual(c.now(1000), 9000);
});

test('reset forgets the clock', () => {
  const c = new M.Clock();
  c.update(5000, 0, false);
  c.reset();
  assert.strictEqual(c.now(0), null);
});

test('harmonies show the harmony parts, otherwise the lead, else harm1', () => {
  const lyrics = { parts: [{ part: 'lead' }, { part: 'harm1' }, { part: 'harm2' }] };
  assert.deepStrictEqual(M.partsToShow(lyrics, 'harmonies').map(p => p.part), ['harm1', 'harm2']);
  assert.deepStrictEqual(M.partsToShow(lyrics, 'lead').map(p => p.part), ['lead']);
  assert.deepStrictEqual(M.partsToShow(lyrics, 'none').map(p => p.part), ['lead']);
  const harmonyOnly = { parts: [{ part: 'harm1' }, { part: 'harm2' }] };
  assert.deepStrictEqual(M.partsToShow(harmonyOnly, 'lead').map(p => p.part), ['harm1']);
  assert.deepStrictEqual(M.partsToShow({ parts: [{ part: 'lead' }] }, 'harmonies').map(p => p.part), ['lead']);
  assert.deepStrictEqual(M.partsToShow(null, 'lead'), []);
});

test('the line shown is the one sung, or the next to come, with the one after', () => {
  assert.strictEqual(M.linesAt(part, 0).current, part.lines[0]);
  assert.strictEqual(M.linesAt(part, 1500).current, part.lines[0]);
  assert.strictEqual(M.linesAt(part, 1500).next, part.lines[1]);
  assert.strictEqual(M.linesAt(part, 2200).current, part.lines[1]);
  assert.strictEqual(M.linesAt(part, 8500).next, null);
  assert.strictEqual(M.linesAt(part, 9500).current, null);
});

test('a syllable fills over its own time', () => {
  const s = syl(1000, 1400, 'Hel');
  assert.strictEqual(M.fill(s, 900), 0);
  assert.strictEqual(M.fill(s, 1200), 0.5);
  assert.strictEqual(M.fill(s, 1400), 1);
  assert.strictEqual(M.fill(syl(1000, 1000, 'x'), 1000), 0);
  assert.strictEqual(M.fill(syl(1000, 1000, 'x'), 1001), 1);
});

test('a long gap counts down to the next line, a short one does not', () => {
  assert.strictEqual(M.countdown(part, 2200), null);   // 500 ms gap
  assert.strictEqual(M.countdown(part, 1500), null);   // being sung
  assert.strictEqual(M.countdown(part, 5500), 0.5);    // 3000..8000, halfway
  assert.strictEqual(M.countdown(part, 500), null);    // 1000 ms from the start
  const late = { lines: [line(5000, 6000, [syl(5000, 6000, 'x')])] };
  assert.strictEqual(M.countdown(late, 0), 1);         // from the song's start
});

test('the screen follows the game: idle, up next, lyrics, now playing, just played', () => {
  const song = { shortname: 's' };
  assert.strictEqual(M.screen(null, false, 0, false), 'idle');
  assert.strictEqual(M.screen({ in_game: false, song: null }, false, 0, false), 'idle');
  assert.strictEqual(M.screen({ in_game: true, song }, false, 1, false), 'up_next');
  assert.strictEqual(M.screen({ in_game: true, song }, true, 1, false), 'lyrics');
  assert.strictEqual(M.screen({ in_game: true, song }, true, 0, false), 'now_playing');
  assert.strictEqual(M.screen({ in_game: true, song }, true, 1, true), 'now_playing');
  assert.strictEqual(M.screen({ in_game: false, song }, true, 1, false), 'just_played');
});

test('a lyrics fetch that failed is tried again, sooner at first, but not a song without any', () => {
  assert.strictEqual(M.lyricsRetryMs(200, 0), null);
  assert.strictEqual(M.lyricsRetryMs(404, 0), null);
  assert.strictEqual(M.lyricsRetryMs(503, 0), 1000);  // the game busy
  assert.strictEqual(M.lyricsRetryMs(0, 0), 1000);    // no reply at all, or no JSON
  assert.strictEqual(M.lyricsRetryMs(500, 1), 2000);
  assert.strictEqual(M.lyricsRetryMs(0, 2), 4000);
  assert.strictEqual(M.lyricsRetryMs(0, 9), 10000);   // at most 10 s apart
});

test('words put spaces between words, not inside them', () => {
  assert.deepStrictEqual(M.words(part.lines[0]).map(w => w.text), ['Hel', 'lo']);
  const two = line(0, 1, [syl(0, 1, 'one'), syl(0, 1, 'two')]);
  assert.deepStrictEqual(M.words(two).map(w => w.text), ['one ', 'two']);
});
