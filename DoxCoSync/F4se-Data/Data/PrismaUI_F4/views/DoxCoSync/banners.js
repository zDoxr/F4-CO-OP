/*
 * CoSync faction banners -- shared by the Multiplayer menu and the HUD.
 *
 * A banner id is text (it travels with the faction state, never an image):
 *   "emblem:<name>:<#rrggbb>"                    built-in emblem on a coloured pennant
 *   "custom:<file>;emblem:<name>:<#rrggbb>"      player image from
 *                                                banners/custom/<file>, with the emblem
 *                                                as the fallback for anyone who lacks the file
 * Custom images are local files: they are not sent over the network. Players
 * who want to see a faction's custom banner copy the same file into their own
 * banners/custom folder; everyone else sees the fallback emblem.
 */
window.CoSyncBanners = (() => {
  'use strict';

  // Pennant 64 x 80, emblem centred on (32, 34). Original geometric designs
  // (the four faction-style emblems are drawn here too: no game art is used).
  const gearPath = (cx, cy, r1, r2, teeth) => {
    let d = '';
    for (let i = 0; i < teeth * 4; i++) {
      const a = (i / (teeth * 4)) * Math.PI * 2;
      const r = (i % 4 === 1 || i % 4 === 2) ? r2 : r1;
      d += (i ? 'L' : 'M') + (cx + r * Math.cos(a)).toFixed(1) + ' ' + (cy + r * Math.sin(a)).toFixed(1);
    }
    return d + 'Z';
  };
  const starPath = (cx, cy, outer, inner) => {
    let d = '';
    for (let i = 0; i < 10; i++) {
      const a = -Math.PI / 2 + i * Math.PI / 5;
      const r = i % 2 ? inner : outer;
      d += (i ? 'L' : 'M') + (cx + r * Math.cos(a)).toFixed(1) + ' ' + (cy + r * Math.sin(a)).toFixed(1);
    }
    return d + 'Z';
  };

  const gear = '<path d="' + gearPath(32, 34, 15, 19, 8) + '"/><circle cx="32" cy="34" r="6" class="cut"/>';
  const star = '<path d="' + starPath(32, 35, 18, 7.5) + '"/>';

  // Minutemen style: a star in a ring over two crossed muskets.
  const minutemen =
    '<path d="M13 55 L47 15" class="ln"/><path d="M51 55 L17 15" class="ln"/>' +
    '<path d="M11.5 53.5 L15 57 L18 54 Z"/><path d="M52.5 53.5 L49 57 L46 54 Z"/>' +
    '<circle cx="32" cy="35" r="14" class="cut"/><circle cx="32" cy="35" r="14" class="ring"/>' +
    '<path d="' + starPath(32, 35.5, 10, 4.2) + '"/>';

  // Brotherhood style: a sword pointing down through a gear, with wings.
  const brotherhood =
    '<path d="M24 33 C18 27 11 26 5 21 C7 30 13 36 24 38 Z"/>' +
    '<path d="M24 40 C17 39 11 37 7 33 C10 40 16 43 24 44 Z"/>' +
    '<path d="M40 33 C46 27 53 26 59 21 C57 30 51 36 40 38 Z"/>' +
    '<path d="M40 40 C47 39 53 37 57 33 C54 40 48 43 40 44 Z"/>' +
    '<path d="' + gearPath(32, 36, 10.5, 13.5, 8) + '"/><circle cx="32" cy="36" r="7.5" class="cut"/>' +
    '<path d="M30.3 13 H33.7 V52 L32 59 L30.3 52 Z"/><rect x="24" y="18" width="16" height="3.2" rx="1"/>' +
    '<circle cx="32" cy="12" r="2.3"/>';

  // Railroad style: a signal lantern.
  const railroad =
    '<circle cx="32" cy="14.5" r="4" class="ring"/>' +
    '<path d="M23 19 H41 L38 24 H26 Z"/>' +
    '<rect x="24.5" y="24" width="15" height="21" rx="2"/>' +
    '<path d="M32 28 C35.5 32 36.5 35 35.5 38 C34.8 40.4 33.4 41.5 32 41.5 C30.4 41.5 29 40.4 28.5 38 C28 35.5 29.8 33.5 30.6 31.5 C31.4 33.3 32.2 34 33 34.5 C33.3 32.5 33 30 32 28 Z" class="cut"/>' +
    '<rect x="21.5" y="45" width="21" height="4" rx="1"/>' +
    '<path d="M14 55 H50" class="ln"/><path d="M19 52 V58 M27 52 V58 M37 52 V58 M45 52 V58" class="thin"/>';

  // Institute style: a figure with open arms inside a ring.
  const institute =
    '<circle cx="32" cy="35" r="19" class="ring"/>' +
    '<circle cx="32" cy="22.5" r="3.6"/>' +
    '<path d="M16 31 L48 31" class="ln"/><path d="M32 27 L32 43" class="ln"/>' +
    '<path d="M32 42 L23 53" class="ln"/><path d="M32 42 L41 53" class="ln"/>';

  const EMBLEMS = {
    star,
    gear,
    minutemen,
    brotherhood,
    railroad,
    institute,
    atom: '<ellipse cx="32" cy="34" rx="19" ry="7" class="ring"/>' +
          '<ellipse cx="32" cy="34" rx="19" ry="7" class="ring" transform="rotate(60 32 34)"/>' +
          '<ellipse cx="32" cy="34" rx="19" ry="7" class="ring" transform="rotate(120 32 34)"/>' +
          '<circle cx="32" cy="34" r="4"/>',
    bolt: '<path d="M36 14 L20 38 L30 38 L26 56 L44 29 L33 29 Z"/>',
    skull: '<circle cx="32" cy="31" r="14"/><rect x="24" y="40" width="16" height="10" rx="2"/>' +
           '<circle cx="26.5" cy="31" r="4" class="cut"/><circle cx="37.5" cy="31" r="4" class="cut"/>' +
           '<path d="M32 36 L29.5 40 L34.5 40 Z" class="cut"/>',
    cross: '<path d="M27 16 H37 V29 H50 V39 H37 V54 H27 V39 H14 V29 H27 Z"/>',
    chevron: '<path d="M14 24 L32 36 L50 24 L50 31 L32 43 L14 31 Z"/><path d="M14 36 L32 48 L50 36 L50 43 L32 55 L14 43 Z"/>',
    sword: '<path d="M30 12 L34 12 L34 42 L30 42 Z"/><path d="M22 42 H42 V46 H22 Z"/><path d="M30 46 H34 V54 H30 Z"/><circle cx="32" cy="56" r="2.5"/>',
    crown: '<path d="M14 46 L16 22 L24 34 L32 18 L40 34 L48 22 L50 46 Z"/><rect x="14" y="48" width="36" height="5"/>',
    flame: '<path d="M32 14 C40 24 46 30 44 42 C43 50 37 55 32 55 C26 55 20 50 20 42 C20 35 25 31 27 25 C29 31 31 33 33 34 C34 28 33 21 32 14 Z"/>',
  };

  const EMBLEM_NAMES = Object.keys(EMBLEMS);
  const COLORS = ['#c8a24a', '#2e6fbf', '#b3312a', '#3a8a3a', '#6e4aa6', '#d9d4c0', '#d9771c', '#39403a'];
  const COLOR_NAMES = { '#c8a24a': 'Brass', '#2e6fbf': 'Vault Blue', '#b3312a': 'Blood Red', '#3a8a3a': 'Army Green',
                        '#6e4aa6': 'Violet', '#d9d4c0': 'Bone', '#d9771c': 'Rust', '#39403a': 'Gunmetal' };

  const isColor = c => /^#[0-9a-fA-F]{6}$/.test(c || '');
  const isFile = f => /^[A-Za-z0-9_.-]{1,48}$/.test(f || '') && /\.(png|jpg|jpeg|webp|svg)$/i.test(f);

  const parse = id => {
    const b = { custom: '', emblem: 'star', color: COLORS[0] };
    String(id || '').split(';').forEach(part => {
      const p = part.split(':');
      if (p[0] === 'custom' && isFile(p[1])) b.custom = p[1];
      else if (p[0] === 'emblem') {
        if (EMBLEMS[p[1]]) b.emblem = p[1];
        if (isColor(p[2])) b.color = p[2];
      }
    });
    return b;
  };

  const make = b => (b.custom ? 'custom:' + b.custom + ';' : '') + 'emblem:' + b.emblem + ':' + b.color;

  // White emblem on dark cloth, dark emblem on light cloth.
  const ink = hex => {
    const n = parseInt(hex.slice(1), 16);
    const lum = 0.2126 * ((n >> 16) & 255) + 0.7152 * ((n >> 8) & 255) + 0.0722 * (n & 255);
    return lum > 150 ? '#141810' : '#f2efe4';
  };

  // Emblem parts are tagged with classes (cut / ring / ln / thin) and turned
  // into inline attributes here. A <style> block inside an inline SVG is
  // global to the page, so with several banners on screen the last one's
  // colours used to repaint every other banner's cut-outs and rings.
  const paint = (markup, cloth, fg) => markup
    .replace(/class="cut"/g, 'fill="' + cloth + '"')
    .replace(/class="ring"/g, 'fill="none" stroke="' + fg + '" stroke-width="3"')
    .replace(/class="ln"/g, 'fill="none" stroke="' + fg + '" stroke-width="3.4" stroke-linecap="round"')
    .replace(/class="thin"/g, 'fill="none" stroke="' + fg + '" stroke-width="2"');

  const svg = (b, cls) => {
    const fg = ink(b.color);
    return '<svg class="' + cls + '" viewBox="0 0 64 80" xmlns="http://www.w3.org/2000/svg">' +
      '<path d="M4 2 H60 V62 L32 78 L4 62 Z" fill="' + b.color + '" stroke="rgba(0,0,0,0.55)" stroke-width="2"/>' +
      '<path d="M8 6 H56 V60 L32 74 L8 60 Z" fill="none" stroke="' + fg + '" stroke-opacity="0.35" stroke-width="1.5"/>' +
      '<g fill="' + fg + '">' + paint(EMBLEMS[b.emblem], b.color, fg) + '</g></svg>';
  };

  // HTML for a banner. cls sizes it (see .banner-* rules in the page CSS).
  const html = (id, cls) => {
    const b = parse(id);
    let out = '<span class="banner ' + (cls || '') + '">' + svg(b, 'banner-svg');
    if (b.custom)
      out += '<img class="banner-img" src="banners/custom/' + encodeURIComponent(b.custom) +
             '" data-file="' + b.custom + '" onerror="this.style.display=\'none\';' +
             'window.CoSyncBanners.markBroken(this.dataset.file)">';
    return out + '</span>';
  };

  // Images that passed the plugin's checks but the page still could not draw.
  const broken = new Set();
  const markBroken = f => { if (f && !broken.has(f)) { broken.add(f); console.warn('[CoSyncBanners] cannot display ' + f); } };
  const isBroken = f => broken.has(f);

  const EMBLEM_LABELS = { minutemen: 'Minutemen', brotherhood: 'Brotherhood', railroad: 'Railroad', institute: 'Institute' };
  const emblemLabel = n => EMBLEM_LABELS[n] || (n.charAt(0).toUpperCase() + n.slice(1));

  return { parse, make, html, EMBLEM_NAMES, COLORS, COLOR_NAMES, isFile, markBroken, isBroken, emblemLabel };
})();
