/*
 * CoSync overhead nameplates. Passive: never takes input.
 *   C++ -> JS : window.cosyncPlatesHud(rgb)  HUD colour [r,g,b]
 *               window.cosyncPlates(json)
 *     [{ id, name, level, faction, rank, talking, x, y, d }]
 *       x, y: 0..1 of the screen (top-left origin), the point above the head
 *       d:    distance from you in game units
 * Pushed by CoSyncPrismaUI::TickNameplates only when something changed (the
 * plugin already culls by location, distance, camera and open menus).
 * Plates are kept per player and moved in place.
 */
(() => {
  'use strict';

  const root = document.getElementById('plates');
  const plates = new Map();   // id -> { el, name, lvl, fac, key }

  const TALK_SVG = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">' +
    '<path d="M2 6h3l4-3v10l-4-3H2z" fill="currentColor"/><path d="M11 5.5a3.5 3.5 0 0 1 0 5M13 3.5a6.5 6.5 0 0 1 0 9"/></svg>';

  const el = (tag, cls, parent) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (parent) parent.appendChild(e);
    return e;
  };

  const create = () => {
    const e = el('div', 'plate', root);
    const nameRow = el('div', 'name', e);
    const talk = el('span', 'talk', nameRow);
    talk.innerHTML = TALK_SVG;
    const name = el('span', '', nameRow);
    const lvl = el('span', 'lvl', nameRow);
    const fac = el('div', 'fac', e);
    return { el: e, name, lvl, fac, key: '' };
  };

  // Near: full size; shrinks to 70% and fades over the far end of the range.
  const scaleFor = d => Math.max(0.7, Math.min(1, 1.15 - d / 5000));
  const alphaFor = d => Math.max(0.35, Math.min(1, 1.6 - d / 3000));

  window.cosyncPlates = json => {
    let list;
    try { list = JSON.parse(json); } catch (e) { console.error('[CoSyncPlates] bad JSON'); return; }
    const w = window.innerWidth, h = window.innerHeight;
    const seen = new Set();
    list.forEach(p => {
      seen.add(p.id);
      let c = plates.get(p.id);
      if (!c) { c = create(); plates.set(p.id, c); }
      const key = p.name + '|' + p.level + '|' + (p.faction || '') + '|' + (p.rank || '');
      if (key !== c.key) {
        c.key = key;
        c.name.textContent = p.name;
        c.lvl.textContent = p.level > 0 ? 'LVL ' + p.level : '';
        c.fac.textContent = p.faction ? (p.rank ? p.rank + '  ·  ' : '') + p.faction : '';
      }
      c.el.classList.toggle('talking', !!p.talking);
      const s = scaleFor(p.d);
      c.el.style.opacity = alphaFor(p.d).toFixed(2);
      c.el.style.transform = 'translate(' + (p.x * w).toFixed(1) + 'px,' + (p.y * h).toFixed(1) + 'px) ' +
                             'translate(-50%,-100%) scale(' + s.toFixed(3) + ')';
    });
    plates.forEach((c, id) => {
      if (!seen.has(id)) { c.el.remove(); plates.delete(id); }
    });
    // Exactly one element per tracked player: drop any plate node the map
    // does not own (never expected; guards against a ghost copy).
    const owned = new Set();
    plates.forEach(c => owned.add(c.el));
    Array.from(root.children).forEach(n => { if (!owned.has(n)) n.remove(); });
  };

  window.cosyncPlatesHud = rgbJson => {
    let rgb;
    try { rgb = JSON.parse(rgbJson); } catch (e) { return; }
    const [r, g, b] = rgb;
    const st = document.documentElement.style;
    st.setProperty('--hud', 'rgb(' + r + ',' + g + ',' + b + ')');
    st.setProperty('--hud-55', 'rgba(' + r + ',' + g + ',' + b + ',0.55)');
    st.setProperty('--hud-glow', 'rgba(' + r + ',' + g + ',' + b + ',0.45)');
  };

  console.info('[CoSyncPlates] ready');
})();
