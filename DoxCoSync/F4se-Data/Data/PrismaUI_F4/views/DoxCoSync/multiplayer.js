/*
 * CoSync Multiplayer menu.
 *
 * Native bridge (see GUI/CoSyncPrismaUI.cpp):
 *   JS  -> C++ : window.cosync_cmd(
 *                  "close" | "refresh" | "controller_style|0|1" | "anim_mode|0|1" |
 *                  "sound|ok|cancel|focus|prevnext" |
 *                  "host" | "stop" | "search_start" | "search_stop" |
 *                  "join|<sessionId>" | "direct|<ipv4>|<port>")
 *   C++ -> JS  : window.cosyncState(json)   real state, pushed on demand
 *                window.cosyncOpen()        menu (re)opened: back to root
 *
 * Every status shown here comes from cosyncState (GNS / discovery / CoSync).
 * Nothing is simulated.
 */
(() => {
  'use strict';

  // ── State pushed by the plugin ──────────────────────────────────────────
  const state = {
    context: 'mainmenu',
    playerName: '',
    hud: [18, 255, 21],
    gamepad: false,
    controllerStyle: 0,
    animTesting: false,
    net: { status: 'OFFLINE', role: 'NONE', players: 0, port: 0, joinTarget: '' },
    network: { available: false, type: '', adapter: '', address: '' },
    discovery: 'OFF',
    search: { seconds: 0, sessions: [] },
    roster: [],
    joining: 0,
    transfer: { phase: 'idle', percent: 0, error: '' },
    profile: '',
    build: 0,
    maxPlayers: 8,
  };

  const $ = id => document.getElementById(id);
  const native = cmd => {
    const fn = window.cosync_cmd;
    if (typeof fn === 'function') fn(cmd);
    else console.error('[CoSyncMP] native bridge window.cosync_cmd missing (cmd=' + cmd + ')');
  };
  const sound = name => native('sound|' + name);

  const escapeHtml = s => String(s).replace(/[&<>"']/g, c => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const role = () => state.net.role;
  const status = () => state.net.status;
  const inGame = () => state.context === 'ingame';
  const gameName = () => (state.playerName ? state.playerName + "'s Game" : 'Your Game');
  const networkLabel = () => state.network.available ? state.network.type : 'No Network';

  const FAILED = ['CONNECTION FAILED', 'SESSION FULL', 'VERSION MISMATCH', 'HOST DISCONNECTED', 'FAILED'];

  // Lobby: the live player list (from CoSync's roster, never UI-made).
  // Row = [label, value, valueCls, rowCls].
  const rosterRows = () => {
    const rows = [['Players', state.net.players + ' / ' + state.maxPlayers, '', 'section']];
    state.roster.forEach(p => {
      const status = p.host ? 'Host' : (p.inGame ? 'In Game' : 'Loading');
      rows.push(['●  ' + p.name + (p.you ? '  (You)' : ''), status, p.inGame || p.host ? '' : 'dim', 'player']);
    });
    if (state.joining > 0)
      rows.push(['○  Joining…', state.joining + (state.joining === 1 ? ' player' : ' players'), 'dim', 'player']);
    return rows;
  };
  const statusCls = s => FAILED.indexOf(s) >= 0 ? 'bad' : (s === 'OFFLINE' ? 'dim' : '');

  // ── Shared host save (world transfer + character) ─────────────────────
  const xferFailed = () => state.transfer.phase === 'failed';
  const inHostWorld = () => status() === 'CONNECTED' && state.transfer.phase === 'ready';
  const worldLabel = () => {
    switch (state.transfer.phase) {
      case 'requested':   return ['Requesting…', 'dim'];
      case 'downloading': return ['Downloading ' + state.transfer.percent + '%', ''];
      case 'loading':     return ['Loading…', ''];
      case 'ready':       return ['Loaded', ''];
      case 'failed':      return ['Failed', 'bad'];
    }
    return null;
  };
  const XFER_ERRORS = {
    host_not_in_game: { head: 'The host is not in a game', text: 'The host has to load a save and host from inside their game.' },
    version_mismatch: { head: 'Different CoSync version', text: 'You and the host are running different versions of CoSync. Both players need the same version.' },
    save_failed:      { head: 'The host could not save', text: 'The host’s game could not create the world save. Ask the host to try again.' },
    save_unreadable:  { head: 'The host could not save', text: 'The host’s world save could not be read. Ask the host to try again.' },
    host_busy:        { head: 'The host is busy', text: 'Try again in a moment.' },
    checksum:         { head: 'Download damaged', text: 'The world arrived damaged. Try again.' },
    write_failed:     { head: 'Could not store the world', text: 'Your save folder could not be written to.' },
    load_failed:      { head: 'Could not load the world', text: 'The host’s world failed to load. Both players need the same mods in the same order.' },
    default:          { head: 'Could not join', text: 'Joining the host’s world failed. Try again.' },
  };

  // Last join request, for TRY AGAIN.
  let lastJoin = null;
  const doJoin = cmd => { lastJoin = cmd; native(cmd); };

  // Direct Connect form (kept between visits).
  const direct = { address: '', port: '48000' };

  // ── Screens ─────────────────────────────────────────────────────────────
  // title, rows(), notice(), items(); live: re-render on every state push;
  // enter()/leave(): side effects when the screen is shown / left.
  const screens = {
    root: {
      title: () => 'Multiplayer',
      live: true,
      items: () => {
        const list = [];
        if (role() === 'HOST') {
          list.push({ key: 'yours', label: 'Your Game', go: 'host',
            desc: 'Players, status and your address.' });
        } else if (role() === 'CLIENT') {
          list.push({ key: 'session', label: 'Session', go: 'connect',
            desc: 'The game you joined.' });
        } else {
          list.push({ key: 'host', label: 'Host Game', go: 'host',
            desc: 'Open your game so other players on your network can join.' });
          list.push({ key: 'join', label: 'Join Game', go: 'join',
            desc: 'Find games hosted on your network and join one.' });
        }
        list.push({ key: 'status', label: 'Network Status', go: 'status',
          desc: 'Connection details, for troubleshooting.' });
        list.push({ key: 'settings', label: 'Settings', go: 'settings',
          desc: 'Your multiplayer name and controller options.' });
        if (role() === 'HOST')
          list.push({ key: 'stop', label: 'Stop Hosting', action: 'stop',
            desc: 'Close your game to other players. Everyone connected will be disconnected.' });
        if (role() === 'CLIENT')
          list.push({ key: 'leave', label: 'Leave Game', action: 'stop',
            desc: 'Disconnect from the host.' });
        list.push({ key: 'back', label: 'Back', action: 'back',
          desc: inGame() ? 'Return to the game.' : 'Return to the main menu.' });
        return list;
      },
    },

    host: {
      title: () => role() === 'HOST' ? 'Your Game' : 'Host Game',
      live: true,
      rows: () => {
        if (role() === 'HOST') {
          const rows = [
            ['Game Name', gameName()],
            ['Status', status(), statusCls(status())],
            ['Network', networkLabel()],
          ];
          if (state.network.available && state.net.port)
            rows.push(['Address', state.network.address + ' : ' + state.net.port]);
          return rows.concat(rosterRows());
        }
        return [
          ['Game Name', gameName()],
          ['Max Players', String(state.maxPlayers)],
          ['Network', networkLabel()],
          ['Status', status(), statusCls(status())],
        ];
      },
      notice: () => {
        if (role() === 'HOST') {
          if (status() === 'WAITING FOR PLAYERS')
            return { head: 'Waiting for players…',
              text: 'Players on your network will see your game under JOIN GAME.' };
          return null;
        }
        if (!inGame())
          return { head: 'Load a save first',
            text: 'Hosting starts from inside your game. Load or start a save, then open Multiplayer with F10.' };
        if (status() === 'STARTING')
          return { head: 'Starting…', text: 'Opening your game to the network.' };
        if (status() === 'FAILED')
          return { head: 'Could not start hosting',
            text: 'The network could not be opened. Close other copies of the game and try again. Details are in the CoSync log.' };
        return null;
      },
      items: () => {
        if (role() === 'HOST')
          return [
            { key: 'stop', label: 'Stop Hosting', action: 'stop',
              desc: 'Close your game to other players. Everyone connected will be disconnected.' },
            { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu. Your game stays open.' },
          ];
        return [
          { key: 'host', label: 'Host Game', action: 'host',
            disabled: !inGame() || status() === 'STARTING',
            desc: inGame() ? 'Open your game. CoSync picks the network automatically.'
                           : 'Load a save first. Hosting starts from inside your game.' },
          { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' },
        ];
      },
    },

    join: {
      title: () => 'Join Game',
      live: true,
      enter: () => native('search_start'),
      leave: () => native('search_stop'),
      notice: () => {
        if (state.search.sessions.length) return null;
        if (!state.network.available)
          return { head: 'No network',
            text: 'This PC is not connected to a network. Connect to your router by cable or Wi-Fi.' };
        if (state.search.seconds < 3)
          return { head: 'Searching for games…', text: 'Looking for CoSync games on your network.' };
        return { head: 'No games found',
          text: 'Make sure the host has started hosting and both PCs are on the same network (same router or Wi-Fi). Windows Firewall must allow Fallout 4.' };
      },
      items: () => {
        const list = state.search.sessions.map(s => {
          const full = s.players >= s.max;
          let desc = 'Hosted by ' + s.host + '.  ' + s.players + ' / ' + s.max + ' players.';
          if (!s.compatible) desc = 'Different CoSync version (host build ' + s.build + ', yours ' + state.build + '). Both players need the same version.';
          else if (full) desc = 'This game is full.';
          return { key: 'g:' + s.id, label: s.name, meta: s.players + ' / ' + s.max,
                   disabled: full || !s.compatible, join: s.id, desc: desc };
        });
        list.push({ key: 'refresh', label: 'Refresh', action: 'refresh', desc: 'Search your network again.' });
        list.push({ key: 'direct', label: 'Direct Connect', go: 'direct',
          desc: 'Connect to a host by address. For unusual networks and troubleshooting.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' });
        return list;
      },
    },

    direct: {
      title: () => 'Direct Connect',
      notice: () => ({ head: 'Host address',
        text: "The host can find their address under NETWORK STATUS. The port is 48000 unless the host's screen shows another." }),
      items: () => [
        { key: 'addr', label: 'Address', field: 'address', placeholder: '192.168.1.20',
          desc: 'The host\'s IPv4 address, e.g. 192.168.1.20.' },
        { key: 'port', label: 'Port', field: 'port', placeholder: '48000', desc: 'Usually 48000.' },
        { key: 'go', label: 'Connect', action: 'direct', desc: 'Connect to this address.' },
        { key: 'back', label: 'Back', action: 'back', desc: 'Return to Join Game.' },
      ],
    },

    connect: {
      title: () => {
        if (xferFailed()) return 'Join Failed';
        if (FAILED.indexOf(status()) >= 0) return status();
        return inHostWorld() ? 'Connected' : 'Joining';
      },
      live: true,
      rows: () => {
        const rows = [
          ['Game', state.net.joinTarget || '—'],
          ['Status', status(), statusCls(status())],
        ];
        const w = worldLabel();
        if (w) rows.push(['World', w[0], w[1]]);
        return inHostWorld() ? rows.concat(rosterRows()) : rows;
      },
      notice: () => {
        if (xferFailed()) return XFER_ERRORS[state.transfer.error] || XFER_ERRORS.default;
        switch (status()) {
          case 'SESSION FULL':
            return { head: 'Session full', text: 'This game already has ' + state.maxPlayers + ' players.' };
          case 'VERSION MISMATCH':
            return { head: 'Different CoSync version',
              text: 'You and the host are running different versions of CoSync. Both players need the same version.' };
          case 'HOST DISCONNECTED':
            return { head: 'Host disconnected', text: 'The host left or the connection was lost.' };
          case 'CONNECTION FAILED':
          case 'FAILED':
            return { head: 'Unable to connect to the host',
              text: 'The host may have stopped hosting, or the connection may be blocked (Windows Firewall, or a network that keeps devices apart).' };
          case 'OFFLINE':
            return { head: 'Not connected', text: 'Choose a game under JOIN GAME.' };
        }
        switch (state.transfer.phase) {
          case 'requested':
            return { head: 'Requesting the world…', text: 'The host is saving its world for you.' };
          case 'downloading':
            return { head: 'Downloading the world…', text: 'Receiving the host’s save. You will play in the same world.' };
          case 'loading':
            return { head: 'Loading the world…', text: 'Entering the host’s world. If the game asks you to confirm anything, choose Yes.' };
        }
        if (inHostWorld()) {
          if (state.profile === 'CREATING')
            return { head: 'New character', text: 'This is your first time in this world. Create your character in the editor.' };
          if (state.profile === 'RESTORING')
            return { head: 'Restoring your character…', text: 'Bringing back your level, perks, inventory and appearance.' };
          return { head: "You're in", text: "You are playing in the host's world. Your character is saved with the host's game." };
        }
        return { head: 'Connecting…', text: 'Contacting the host.' };
      },
      items: () => {
        if (xferFailed() || FAILED.indexOf(status()) >= 0 || status() === 'OFFLINE')
          return [
            { key: 'retry', label: 'Try Again', action: 'retry', disabled: !lastJoin, desc: 'Connect to the same game again.' },
            { key: 'back', label: 'Back', action: 'leave_back', desc: 'Return to the Multiplayer menu.' },
          ];
        if (inHostWorld())
          return [
            { key: 'leave', label: 'Leave Game', action: 'stop_root', desc: 'Disconnect from the host. Your character stays saved in their world.' },
            { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu. You stay connected.' },
          ];
        return [
          { key: 'cancel', label: 'Cancel', action: 'stop_root', desc: 'Stop joining.' },
        ];
      },
    },

    status: {
      title: () => 'Network Status',
      live: true,
      rows: () => {
        const rows = [
          ['Status', status(), statusCls(status())],
          ['Role', role()],
          ['Network', networkLabel()],
        ];
        if (state.network.available) {
          rows.push(['Adapter', state.network.adapter]);
          rows.push(['Local Address', state.network.address]);
        }
        if (role() === 'HOST')
          rows.push(['Game Port', String(state.net.port)]);
        if (role() !== 'NONE')
          rows.push(['Players', state.net.players + ' / ' + state.maxPlayers]);
        rows.push(['Discovery', state.discovery]);
        rows.push(['CoSync Build', String(state.build)]);
        return rows;
      },
      items: () => [
        { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' },
      ],
    },

    settings: {
      title: () => 'Settings',
      live: true,
      rows: () => [
        ['Player Name', state.playerName || '—'],
      ],
      items: () => [
        { key: 'pad', label: 'Controller Buttons', option: state.controllerStyle === 1 ? 'PlayStation' : 'Xbox',
          toggle: 'controller_style',
          desc: 'Which button art to show for your controller. Press ← / → or SELECT to change.' },
        { key: 'anim', label: 'Animation Sync', option: state.animTesting ? 'Testing' : 'Current',
          toggle: 'anim_mode',
          desc: 'Developer option. Current is the production animation sync; Testing enables the experimental path.' },
        { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' },
      ],
    },
  };

  // ── Rendering ───────────────────────────────────────────────────────────
  let stack = ['root'];
  let index = 0;
  let open = true;
  let renderedKeys = '';
  let selectedKey = null;   // key of the highlighted row, survives list rebuilds
  const current = () => screens[stack[stack.length - 1]];
  const currentItems = () => current().items();

  const renderRows = () => {
    const scr = current();
    const rows = scr.rows ? scr.rows() : [];
    $('rows').innerHTML = rows.map(r =>
      '<div class="row' + (r[3] ? ' ' + r[3] : '') + '"><span class="label">' + escapeHtml(r[0]) + '</span>' +
      '<span class="value ' + (r[2] || '') + '">' + escapeHtml(r[1]) + '</span></div>').join('');
    const n = scr.notice ? scr.notice() : null;
    $('notice').innerHTML = n ? '<span class="head">' + escapeHtml(n.head) + '</span>' + escapeHtml(n.text) : '';
    $('title').textContent = scr.title();
  };

  const renderList = keepKey => {
    const items = currentItems();
    let newIndex = 0;
    if (keepKey) {
      const k = items.findIndex(it => it.key === keepKey);
      if (k >= 0) newIndex = k;
      else newIndex = Math.min(index, items.length - 1);
    }
    index = newIndex;

    $('list').innerHTML = items.map((it, i) => {
      let right = '';
      if (it.option) right = '<span class="opt">' + escapeHtml(it.option) + '</span>';
      else if (it.meta) right = '<span class="meta">' + escapeHtml(it.meta) + '</span>';
      else if (it.field) right = '<input class="field" data-field="' + it.field + '" maxlength="' +
        (it.field === 'port' ? 5 : 15) + '" placeholder="' + escapeHtml(it.placeholder || '') +
        '" value="' + escapeHtml(direct[it.field]) + '">';
      return '<div class="item' + (it.disabled ? ' disabled' : '') + '" data-i="' + i + '">' +
             '<span>' + escapeHtml(it.label) + '</span>' + right + '</div>';
    }).join('');

    Array.prototype.forEach.call($('list').children, el => {
      const i = +el.getAttribute('data-i');
      el.addEventListener('mouseenter', () => { if (i !== index) { select(i); sound('focus'); } });
      el.addEventListener('click', () => { select(i); if (!currentItems()[i].field) activate(); });
    });
    Array.prototype.forEach.call(document.querySelectorAll('input.field'), inp => {
      inp.addEventListener('input', () => { direct[inp.getAttribute('data-field')] = inp.value; });
    });

    renderedKeys = items.map(it => it.key + (it.disabled ? '!' : '') + (it.meta || '') + (it.option || '') + it.label).join('|');
    highlight();
  };

  const render = keepKey => {
    const scr = current();
    const crumbs = stack.slice(0, -1).map(id => screens[id].title());
    $('crumb').textContent = crumbs.length ? crumbs.join('  ›  ')
      : (inGame() ? 'CoSync' : 'CoSync  ›  Main Menu');
    renderRows();
    renderList(keepKey);
    renderHints();
    void scr;
  };

  const highlight = () => {
    const items = currentItems();
    selectedKey = items[index] ? items[index].key : null;
    Array.prototype.forEach.call($('list').children, (el, i) => el.classList.toggle('selected', i === index));
    $('desc').textContent = items[index] ? (items[index].desc || '') : '';

    // Text fields take keyboard focus only while their row is selected.
    const it = items[index];
    const active = document.activeElement;
    if (it && it.field) {
      const inp = document.querySelector('input.field[data-field="' + it.field + '"]');
      if (inp && active !== inp) inp.focus();
    } else if (active && active.tagName === 'INPUT') {
      active.blur();
    }
  };

  const select = i => {
    const count = currentItems().length;
    index = (i + count) % count;
    highlight();
  };
  const move = delta => { select(index + delta); sound('focus'); };

  const transition = fn => {
    const el = $('screen');
    el.classList.add('leaving');
    setTimeout(() => { fn(); el.classList.remove('leaving'); }, 110);
  };

  const go = id => {
    const leaving = current();
    stack.push(id);
    if (leaving.leave && id !== 'direct') leaving.leave();
    if (screens[id].enter) screens[id].enter();
    transition(() => render(null));
  };

  const goRoot = () => {
    const leaving = current();
    if (leaving.leave) leaving.leave();
    stack = ['root'];
    transition(() => render(null));
  };

  const back = () => {
    sound('cancel');
    if (stack.length <= 1) { closeMenu(); return; }
    const leaving = current();
    stack.pop();
    if (leaving.leave) leaving.leave();
    const now = current();
    if (now.enter && leaving !== screens.direct) now.enter();
    transition(() => render(null));
  };

  const closeMenu = () => {
    const leaving = current();
    if (leaving.leave) leaving.leave();
    open = false;
    $('panel').classList.remove('shown');
    $('hints').classList.remove('shown');
    native('close');
  };

  const cycleOption = it => {
    if (it.toggle === 'controller_style') {
      native('controller_style|' + (state.controllerStyle === 1 ? '0' : '1'));
      sound('prevnext');
    } else if (it.toggle === 'anim_mode') {
      native('anim_mode|' + (state.animTesting ? '0' : '1'));
      sound('prevnext');
    }
  };

  const activate = () => {
    const it = currentItems()[index];
    if (!it) return;
    if (it.disabled) { sound('cancel'); return; }
    if (it.field) { move(+1); return; }          // Enter in a field: next row
    if (it.toggle) { cycleOption(it); return; }
    if (it.go) { sound('ok'); go(it.go); return; }
    if (it.join) {
      sound('ok');
      doJoin('join|' + it.join);
      stack = ['root', 'connect'];
      transition(() => render(null));
      return;
    }
    switch (it.action) {
      case 'back':    back(); break;
      case 'host':    sound('ok'); native('host'); break;
      case 'stop':    sound('ok'); native('stop'); break;
      case 'stop_root': sound('cancel'); native('stop'); goRoot(); break;
      case 'leave_back': sound('cancel'); native('stop'); goRoot(); break;
      case 'refresh': sound('ok'); native('search_start'); break;
      case 'retry':   sound('ok'); if (lastJoin) native(lastJoin); break;
      case 'direct': {
        const addr = direct.address.trim();
        const port = (direct.port || '48000').trim();
        if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(addr) || !/^\d{1,5}$/.test(port)) {
          sound('cancel');
          $('desc').textContent = 'Enter an address like 192.168.1.20 and a port like 48000.';
          return;
        }
        sound('ok');
        doJoin('direct|' + addr + '|' + port);
        stack = ['root', 'connect'];
        transition(() => render(null));
        break;
      }
    }
  };

  // ── Input ───────────────────────────────────────────────────────────────
  // Ultralight reports Escape as key "Unidentified" + keyCode 27 (verified
  // in game), so keys are matched by keyCode.
  const KEY = { 27: 'back', 8: 'back', 9: 'down', 13: 'accept', 32: 'accept',
                69: 'accept', 38: 'up', 40: 'down', 87: 'up', 83: 'down',
                37: 'left', 39: 'right', 65: 'left', 68: 'right' };
  // While typing in a field only these keys navigate; the rest are text.
  const FIELD_KEYS = { 27: 'back', 9: 'down', 13: 'accept', 38: 'up', 40: 'down' };

  const onAction = action => {
    if (!open) return;
    switch (action) {
      case 'up':     move(-1); break;
      case 'down':   move(+1); break;
      case 'accept': activate(); break;
      case 'back':   back(); break;
      case 'left':
      case 'right': {
        const it = currentItems()[index];
        if (it && it.toggle && !it.disabled) cycleOption(it);
        break;
      }
    }
  };

  document.addEventListener('keydown', e => {
    const typing = document.activeElement && document.activeElement.tagName === 'INPUT';
    if (e.repeat && (e.keyCode === 13 || e.keyCode === 27)) return;
    const action = (typing ? FIELD_KEYS : KEY)[e.keyCode];
    if (!action) return;
    e.preventDefault();
    onAction(action);
  });

  window.addEventListener('prisma-controller-action', e => {
    const d = e.detail || {};
    if (d.state && d.state !== 'pressed') return;
    onAction(d.action);
  });

  window.addEventListener('prisma-input-device-change', e => {
    const d = e.detail || {};
    if (typeof d.gamepad === 'boolean') state.gamepad = d.gamepad;
    if (typeof d.style === 'number') state.controllerStyle = d.style;
    renderHints();
  });

  // ── Hint bar ────────────────────────────────────────────────────────────
  const renderHints = () => {
    const ps = state.controllerStyle === 1;
    const pad = state.gamepad && window.PrismaCG;
    const key = (kb, gp) => pad
      ? '<span class="glyph">' + window.PrismaCG.chip(gp, ps) + '</span>'
      : '<span class="key">' + kb + '</span>';
    $('hints').innerHTML =
      '<span class="hint">' + key('ENTER', 'A') + 'Select</span>' +
      '<span class="hint">' + key('ESC', 'B') + (stack.length > 1 ? 'Back' : 'Close') + '</span>';
  };

  // ── Native -> JS ────────────────────────────────────────────────────────
  const applyHud = () => {
    const [r, g, b] = state.hud;
    const root = document.documentElement.style;
    const rgba = a => 'rgba(' + r + ',' + g + ',' + b + ',' + a + ')';
    root.setProperty('--hud', 'rgb(' + r + ',' + g + ',' + b + ')');
    root.setProperty('--hud-80', rgba(0.80));
    root.setProperty('--hud-55', rgba(0.55));
    root.setProperty('--hud-35', rgba(0.35));
    root.setProperty('--hud-12', rgba(0.12));
    root.setProperty('--hud-glow', rgba(0.45));
  };

  window.cosyncState = json => {
    let data;
    try { data = JSON.parse(json); }
    catch (e) { console.error('[CoSyncMP] bad state JSON'); return; }
    Object.assign(state, data);
    applyHud();

    // Screens that no longer apply to the current role go back to root.
    const top = stack[stack.length - 1];
    if ((top === 'join' || top === 'direct') && role() === 'HOST') { goRoot(); return; }

    renderRows();
    const items = currentItems();
    const keys = items.map(it => it.key + (it.disabled ? '!' : '') + (it.meta || '') + (it.option || '') + it.label).join('|');
    if (keys !== renderedKeys) {
      renderList(selectedKey);
    } else {
      $('desc').textContent = items[index] ? (items[index].desc || '') : '';
    }
    renderHints();
  };

  window.cosyncOpen = () => {
    open = true;
    stack = ['root'];
    render(null);
    $('panel').classList.add('shown');
    $('hints').classList.add('shown');
    native('refresh');
  };

  // Live screens poll real state once a second while the menu is open.
  setInterval(() => {
    if (open && current().live) native('refresh');
  }, 1000);

  window.cosyncOpen();
  console.info('[CoSyncMP] menu script ready');
})();
