/*
 * CoSync teammate HUD. Passive: never takes input.
 *   C++ -> JS : window.cosyncHud(json)
 *     { hud:[r,g,b], players:[{ id, name, level, hp, maxHp, rads, ap, maxAp, downed, hurt,
 *                               team, role, talking }],
 *       voice:{ on, mic, mode:'P'|'T', muted, tx, ptt } }   (team fields: CoSyncTeams)
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
  const voice = document.getElementById('voice');

  // Microphone / speaker marks (inline SVG, HUD colour via currentColor).
  const MIC_SVG = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">' +
    '<rect x="5.5" y="1.5" width="5" height="8" rx="2.5" fill="currentColor"/>' +
    '<path d="M3 7.5a5 5 0 0 0 10 0M8 12.5v2.5M5.5 15h5"/></svg>';
  const TALK_SVG = '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6">' +
    '<path d="M2 6h3l4-3v10l-4-3H2z" fill="currentColor"/><path d="M11 5.5a3.5 3.5 0 0 1 0 5M13 3.5a6.5 6.5 0 0 1 0 9"/></svg>';
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
    const who = el('span', 'who', top);
    const talk = el('span', 'talk', who);
    talk.innerHTML = TALK_SVG;
    const name = el('span', 'name', who);
    const right = el('span', '', top);
    const bar = el('div', 'bar', root);
    const hp = el('div', 'hp', bar);
    const rads = el('div', 'rads', bar);
    const apWrap = el('div', 'ap', root);
    const ap = el('div', 'fill', apWrap);
    const teamLine = el('div', 'teamline', root);
    return { root, name, right, hp, rads, apWrap, ap, teamLine, teamText: null };
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
    c.root.classList.toggle('talking', !!p.talking);

    setText(c.name, p.name);

    // Team line: role · team. Rebuilt only when it changes.
    const teamText = p.team ? (p.role || '') + '|' + p.team : '';
    if (teamText !== c.teamText) {
      c.teamText = teamText;
      c.teamLine.innerHTML = '';
      c.teamLine.style.display = p.team ? '' : 'none';
      if (p.team) {
        const r = el('span', 'role', c.teamLine);
        r.textContent = p.role || '';
        c.teamLine.appendChild(document.createTextNode((p.role ? '  ·  ' : '') + p.team));
      }
    }
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

  let voiceKey = null;
  const updateVoice = v => {
    const on = !!(v && v.on);
    const key = on ? [v.mic, v.mode, v.muted, v.tx, v.ptt].join('|') : '';
    if (key === voiceKey) return;
    voiceKey = key;
    voice.className = 'voice' + (on ? ' on' : '') + (on && v.tx ? ' tx' : '') +
      (on && v.mode === 'T' ? ' teamvoice' : '') + (on && v.muted ? ' muted' : '') + (on && !v.mic ? ' nomic' : '');
    if (!on) { voice.innerHTML = ''; return; }
    let text;
    if (!v.mic) text = 'NO MICROPHONE';
    else if (v.muted) text = 'MIC MUTED';
    else text = (v.mode === 'T' ? 'TEAM' : 'PROXIMITY') + ' VOICE';
    voice.innerHTML = MIC_SVG;
    const mode = el('span', 'mode', voice);
    mode.textContent = text;
    if (v.mic && !v.muted) {
      const k = el('span', 'key', voice);
      k.textContent = v.ptt ? 'PUSH TO TALK' : 'OPEN MIC';
    }
  };

  window.cosyncHud = json => {
    let data;
    try { data = JSON.parse(json); }
    catch (e) { console.error('[CoSyncHUD] bad JSON'); return; }
    if (data.hud) applyHud(data.hud);
    updateVoice(data.voice);

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
