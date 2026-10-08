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

// a font as /game_asset/font gives it: cells 40 x 20 px on a 200 x 100 texture
const font = {
  cell: [40, 20], texture: [200, 100], base_kerning: 0.05, monospace: false,
  glyphs: { 65: [0, 0, 0.5, 0.6], 66: [0.25, 0.5, 0.25, 0.35], 32: [0.5, 0, 0, 0.3] },
  kerning: [[65, 66, -0.1]],
};

test('text lays out from the font: glyph boxes, their place in the texture, advances and kerning', () => {
  // 40 px tall lines: twice the cell
  const layout = M.layoutText(font, 'AB', 40);
  const [a, b] = layout.items;
  assert.strictEqual(a.missing, false);
  assert.strictEqual(a.w, 40);              // 0.5 cells x 40 px x 2
  assert.strictEqual(a.h, 40);
  assert.strictEqual(a.maskX, 0);
  assert.strictEqual(b.maskX, -100);        // u 0.25 x 200 px x 2
  assert.strictEqual(b.maskY, -100);        // v 0.5 x 100 px x 2
  assert.deepStrictEqual([a.maskW, a.maskH], [400, 200]);
  // A's advance: (0.6 + 0.05 base) cells, less the A-B kerning of 0.1 cells
  assert.ok(Math.abs(a.advance - (0.6 + 0.05 - 0.1) * 80) < 1e-9);
  assert.strictEqual(a.x, 0);
  assert.ok(Math.abs(b.x - a.advance) < 1e-9);
  assert.ok(Math.abs(layout.width - (a.advance + b.advance)) < 1e-9);
});

test('a space advances without a box, and characters the font lacks are marked', () => {
  const [space, z] = M.layoutText(font, ' 中', 20).items;
  assert.strictEqual(space.w, 0);
  assert.ok(space.advance > 0);
  assert.strictEqual(z.missing, true);
  assert.strictEqual(z.ch, '中');
});

test('a glyph fills as the syllable passes over it', () => {
  assert.strictEqual(M.glyphFill(0, 10, 10, 40), 0);
  assert.strictEqual(M.glyphFill(0.375, 10, 10, 40), 0.5);   // 15 of the 40 px sung
  assert.strictEqual(M.glyphFill(1, 10, 10, 40), 1);
  assert.strictEqual(M.glyphFill(0.5, 30, 0, 40), 0);        // a box with no width
  assert.strictEqual(M.glyphFill(0.9, 30, 0, 40), 1);
});

test('words put spaces between words, not inside them', () => {
  assert.deepStrictEqual(M.words(part.lines[0]).map(w => w.text), ['Hel', 'lo']);
  const two = line(0, 1, [syl(0, 1, 'one'), syl(0, 1, 'two')]);
  assert.deepStrictEqual(M.words(two).map(w => w.text), ['one ', 'two']);
});

test('every texture is asked for where band3 serves it, within its allowlists', () => {
  const keys = ['lane_blue', 'lane_brown', 'icons', 'od_lane', 'od_star', 'scoreboard', 'highlight',
                'line_fade', 'tab_top', 'tab_low', 'pause', 'sunburst', 'wall', 'blank_art', 'idle_art',
                'header_normal'];
  assert.deepStrictEqual(Object.keys(M.TEXTURES).sort(), keys.slice().sort());
  for (const key of keys) {
    const url = M.textureUrl(M.TEXTURES[key]);
    assert.match(url, /^\/game_asset\/(milo_texture\?milo=ui%2F[A-Za-z0-9_%.]+\.milo_xbox&name=[a-z0-9_.()-]+\.(png|bmp)|texture\?path=(ui|dx)%2F[^&]+\.(png|bmp)_xbox)$/, key);
  }
  assert.strictEqual(M.textureUrl({ milo: 'ui/a/b.milo_xbox', name: 'c.png' }),
                     '/game_asset/milo_texture?milo=ui%2Fa%2Fb.milo_xbox&name=c.png');
  assert.strictEqual(M.textureUrl({ path: 'ui/image/gen/x.png_xbox' }),
                     '/game_asset/texture?path=ui%2Fimage%2Fgen%2Fx.png_xbox');
});

test("a song's source badge is its origin's texture, if it has one", () => {
  assert.strictEqual(M.originUrl('rb3'), '/game_asset/texture?path=ui%2Fresource%2Fgame_origins_128%2Fgen%2Frb3.png_xbox');
  assert.strictEqual(M.originUrl(''), null);
  assert.strictEqual(M.originUrl(undefined), null);
  assert.strictEqual(M.originUrl('../x'), null);
});

test("instrument icons are rb3_icons' cells, vocals for anything else", () => {
  assert.deepStrictEqual(M.iconCell('guitar'), [0, 0]);
  assert.deepStrictEqual(M.iconCell('drum'), [2, 0]);
  assert.deepStrictEqual(M.iconCell('real_keys'), [3, 1]);
  assert.deepStrictEqual(M.iconCell('vocals'), [0, 2]);
  assert.deepStrictEqual(M.iconCell('harmonies'), [2, 2]);
  assert.deepStrictEqual(M.iconCell('kazoo'), [0, 2]);
});

test("a player's tab says their part over their difficulty, and the singer's says harmonies", () => {
  assert.deepStrictEqual(M.tab({ part: 'guitar', difficulty: 3 }, 'lead'),
                         { top: 'GUITAR', low: 'EXPERT', icon: 'guitar' });
  assert.deepStrictEqual(M.tab({ part: 'real_bass', difficulty: 0 }, 'lead'),
                         { top: 'PRO BASS', low: 'EASY', icon: 'real_bass' });
  assert.deepStrictEqual(M.tab({ part: 'vocals', difficulty: 2 }, 'harmonies'),
                         { top: 'HARMONIES', low: 'HARD', icon: 'harmonies' });
  assert.deepStrictEqual(M.tab({ part: 'vocals', difficulty: 1 }, 'lead'),
                         { top: 'VOCALS', low: 'MEDIUM', icon: 'vocals' });
  assert.strictEqual(M.tab(null, 'lead'), null);
  assert.strictEqual(M.tab({ part: 'drum', difficulty: 9 }, 'none').low, '');
});

test('the score reads with commas, never below zero', () => {
  assert.strictEqual(M.scoreText(0), '0');
  assert.strictEqual(M.scoreText(999), '999');
  assert.strictEqual(M.scoreText(82725), '82,725');
  assert.strictEqual(M.scoreText(1234567), '1,234,567');
  assert.strictEqual(M.scoreText(-5), '0');
  assert.strictEqual(M.scoreText(undefined), '0');
});

test('a tier fills up to five pips', () => {
  assert.strictEqual(M.pips(0), 0);
  assert.strictEqual(M.pips(3), 3);
  assert.strictEqual(M.pips(6), 5);
  assert.strictEqual(M.pips(undefined), 0);
});

test('a texture the game was busy for is asked for again; one that is not there is not', () => {
  assert.strictEqual(M.textureRetryMs(200, 0), null);
  assert.strictEqual(M.textureRetryMs(404, 0), null);
  assert.strictEqual(M.textureRetryMs(503, 0), 1000);
  assert.strictEqual(M.textureRetryMs(0, 2), 4000);
  assert.strictEqual(M.textureRetryMs(503, 20), 10000);
});
