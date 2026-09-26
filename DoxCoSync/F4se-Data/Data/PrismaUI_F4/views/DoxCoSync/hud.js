/*
 * CoSync teammate HUD. Passive: never takes input.
 *   C++ -> JS : window.cosyncHud(json)
 *     { hud:[r,g,b], players:[{ id, name, level, hp, maxHp, rads, ap, maxAp, downed, hurt }] }
 * Pushed by CoSyncPrismaUI::TickHud only when the data changed (<= 10 Hz);
 * senders stream their vitals at 10 Hz with a 1 s exact resync.
 *
 * Cards are kept per player and updated IN PLACE, so the bar widths animate
 * (CSS transition matching the 10 Hz stream) instead of jumping -- AP regen
 * and rads glide, and always settle on the sender's exact value.
 */
(() => {
  'use strict';

  const team = document.getElementById('team');
  const cards = new Map();   // player id -> { root, name, right, hp, rads, apWrap, ap }

  const clamp01 = v => (v < 0 ? 0 : (v > 1 ? 1 : v));
  const pct = v => (clamp01(v) * 100).toFixed(2) + '%';

  // Same radiation weighting as the retired ImGui bar: rads eat into max
  // health visually, scaled down and capped so a heavily irradiated teammate's
  // bar still reads as health, and never overlapping the current health.
  const RAD_SCALE = 0.5;
  const RAD_MAX = 0.35;

  const applyHud = rgb => {
    const [r, g, b] = rgb;
    const s = document.documentElement.style;
    const rgba = a => 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
    s.setProperty('--hud', 'rgb(' + r + ',' + g + ',' + b + ')');
    s.setProperty('--hud-55', rgba(0.55));
    s.setProperty('--hud-25', rgba(0.25));
    s.setProperty('--hud-glow', rgba(0.45));
  };

  const el = (tag, cls, parent) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (parent) parent.appendChild(e);
    return e;
  };

  const createCard = () => {
    const root = el('div', 'mate');
    const top = el('div', 'top', root);
    const name = el('span', 'name', top);
    const right = el('span', '', top);
    const bar = el('div', 'bar', root);
    const hp = el('div', 'hp', bar);
    const rads = el('div', 'rads', bar);
    const apWrap = el('div', 'ap', root);
    const ap = el('div', 'fill', apWrap);
    return { root, name, right, hp, rads, apWrap, ap };
  };

  const setText = (node, text) => { if (node.textContent !== text) node.textContent = text; };
  const setWidth = (node, w) => { if (node.style.width !== w) node.style.width = w; };

  const updateCard = (c, p) => {
    const maxHp = p.maxHp > 1 ? p.maxHp : 1;
    const hp = clamp01(p.hp / maxHp);
    let rads = Math.min((p.rads / maxHp) * RAD_SCALE, RAD_MAX);
    if (rads < 0) rads = 0;
    if (rads > 1 - hp) rads = 1 - hp;

    c.root.classList.toggle('hurt', !!p.hurt);
    c.root.classList.toggle('down', !!p.downed);

    setText(c.name, p.name);
    if (p.downed) {
      c.right.className = 'downed';
      setText(c.right, 'DOWNED');
    } else {
      c.right.className = 'lvl';
      setText(c.right, p.level > 0 ? 'LVL ' + p.level : '');
    }

    setWidth(c.hp, pct(hp));
    setWidth(c.rads, pct(rads));

    if (p.ap >= 0) {
      c.apWrap.style.display = '';
      setWidth(c.ap, pct(p.ap / (p.maxAp > 1 ? p.maxAp : 1)));
    } else {
      c.apWrap.style.display = 'none';
    }
  };

  window.cosyncHud = json => {
    let data;
    try { data = JSON.parse(json); }
    catch (e) { console.error('[CoSyncHUD] bad JSON'); return; }
    if (data.hud) applyHud(data.hud);

    const players = data.players || [];
    const seen = new Set();
    players.forEach(p => {
      seen.add(p.id);
      let c = cards.get(p.id);
      if (!c) {
        c = createCard();
        cards.set(p.id, c);
        team.appendChild(c.root);
      }
      updateCard(c, p);
    });

    // Players who left.
    cards.forEach((c, id) => {
      if (!seen.has(id)) { c.root.remove(); cards.delete(id); }
    });
  };

  console.info('[CoSyncHUD] ready');
})();
