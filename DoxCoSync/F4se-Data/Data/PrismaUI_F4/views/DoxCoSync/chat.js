/*
 * CoSync chat box.
 *   C++ -> JS : window.cosyncChat(json)      new messages {seq, msgs:[{seq,scope,name,team,text,me}]}
 *               window.cosyncChatOpen(scope) take input ("G" everyone / "T" team)
 *               window.cosyncChatHud(rgb)     HUD colour [r,g,b]
 *               window.cosyncChatTalk(json)   names heard on voice right now ["You","Ann"]
 *   JS -> C++ : window.cosync_chat("send|G|text" | "send|F|text" | "close")
 * Messages are pushed by the plugin only when new ones arrive; nothing here
 * polls. Text is inserted with textContent (never as HTML).
 */
(() => {
  'use strict';

  const chat = document.getElementById('chat');
  const log = document.getElementById('log');
  const input = document.getElementById('input');
  const scopeEl = document.getElementById('scope');
  const talkers = document.getElementById('talkers');

  const MAX_LINES = 60;
  const FADE_MS = 12000;
  let scope = 'G';
  let open = false;

  const native = cmd => { if (typeof window.cosync_chat === 'function') window.cosync_chat(cmd); };

  const setScope = s => {
    scope = s === 'T' ? 'T' : 'G';
    scopeEl.textContent = scope === 'T' ? 'TEAM' : 'ALL';
    scopeEl.classList.toggle('t', scope === 'T');
  };

  const addLine = m => {
    const line = document.createElement('div');
    line.className = 'line' + (m.scope === 'T' ? ' t' : '') + (m.scope === 'S' ? ' s' : '') + (m.me ? ' me' : '');
    if (m.scope === 'S') {
      line.textContent = m.text;
    } else {
      if (m.scope === 'T') {
        const tag = document.createElement('span');
        tag.className = 'tag';
        tag.textContent = '[' + (m.team || 'TEAM') + ']';
        line.appendChild(tag);
      }
      const who = document.createElement('span');
      who.className = 'who';
      who.textContent = m.name + ': ';
      line.appendChild(who);
      line.appendChild(document.createTextNode(m.text));
    }
    log.appendChild(line);
    while (log.children.length > MAX_LINES) log.removeChild(log.firstChild);
    setTimeout(() => line.classList.add('faded'), FADE_MS);
  };

  const close = () => {
    open = false;
    chat.classList.remove('open');
    input.value = '';
    input.blur();
    native('close');
  };

  window.cosyncChat = json => {
    let data;
    try { data = JSON.parse(json); } catch (e) { console.error('[CoSyncChat] bad JSON'); return; }
    (data.msgs || []).forEach(addLine);
  };

  window.cosyncChatTalk = json => {
    let names;
    try { names = JSON.parse(json); } catch (e) { return; }
    talkers.textContent = names.length ? '▶ ' + names.join(', ') : '';
    talkers.classList.toggle('on', names.length > 0);
  };

  window.cosyncChatOpen = s => {
    setScope(s || scope);
    open = true;
    chat.classList.add('open');
    input.value = '';
    setTimeout(() => input.focus(), 0);
  };

  window.cosyncChatHud = rgbJson => {
    let rgb;
    try { rgb = JSON.parse(rgbJson); } catch (e) { return; }
    const [r, g, b] = rgb;
    const st = document.documentElement.style;
    const rgba = a => 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
    st.setProperty('--hud', 'rgb(' + r + ',' + g + ',' + b + ')');
    st.setProperty('--hud-80', rgba(0.8));
    st.setProperty('--hud-55', rgba(0.55));
    st.setProperty('--hud-25', rgba(0.25));
    st.setProperty('--hud-glow', rgba(0.45));
  };

  // Ultralight reports Escape as keyCode 27 (key "Unidentified").
  document.addEventListener('keydown', e => {
    if (!open) return;
    if (e.keyCode === 27) { e.preventDefault(); close(); return; }
    if (e.keyCode === 9) { e.preventDefault(); setScope(scope === 'G' ? 'T' : 'G'); return; }
    if (e.keyCode === 13) {
      e.preventDefault();
      const text = input.value.replace(/[|]/g, '/').trim();
      if (text) native('send|' + scope + '|' + text);
      close();
    }
  });

  setScope('G');
  console.info('[CoSyncChat] ready');
})();
