/*
 * CoSync Multiplayer menu.
 *
 * Native bridge (see GUI/CoSyncPrismaUI.cpp):
 *   JS  -> C++ : window.cosync_cmd(
 *                  "close" | "refresh" | "controller_style|0|1" | "anim_mode|0|1" |
 *                  "sound|ok|cancel|focus|prevnext" |
 *                  "host" | "stop" | "search_start" | "search_stop" |
 *                  "join|<sessionId>" | "direct|<ipv4>|<port>" |
 *                  "steam_host" | "steam_invite" | "steam_join|<lobbyHex>" |
 *                  "steam_browse|0|1" |
 *                  "content|accept|cancel|quit"   pre-join content sync (CoSyncContentSync) |
 *                  "team_<op>|<args...>"  team / settlement / workshop-rules
 *                                         requests (CoSyncTeams; the host decides))
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
    steam: { available: false, backend: false, lobby: false, error: '', games: [] },
    roster: [],
    joining: 0,
    transfer: { phase: 'idle', percent: 0, error: '' },
    // Pre-join content check / download (CoSyncContentSync::BuildUiJson).
    content: { phase: 'idle', error: '', detail: '', checkDone: 0, checkTotal: 0, fileCount: 0, fileIndex: 0,
               file: '', fileDone: 0, fileSize: 0, done: 0, total: 0, scripts: 0,
               missing: [], activate: [], blocked: [], warnings: [], native: [] },
    profile: '',
    build: 0,
    maxPlayers: 8,
    teams: null,                    // CoSyncTeams::BuildUiJson
    workshop: { hooks: false, counts: {} },
    settlement: {},                 // CoSyncSettlementSync::BuildUiJson (settlement here)
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
  const networkLabel = () => state.network.available ? state.network.type : 'None';
  const steamSession = () => !!(state.steam && state.steam.backend);

  const FAILED = ['CONNECTION FAILED', 'SESSION FULL', 'VERSION MISMATCH', 'HOST DISCONNECTED', 'FAILED'];

  // Lobby: the live player list (from CoSync's roster, never UI-made).
  // Row = [label, value, valueCls, rowCls].
  // 0.3.4: latency from GNS (host: each client's link; client: its link to
  // the host). Coloured like the vanilla HUD's warning states.
  const pingText = ms => (typeof ms === 'number' && ms >= 0) ? ms + ' ms' : '';
  const pingCls = ms => (typeof ms !== 'number' || ms < 0) ? '' : (ms >= 250 ? 'bad' : (ms >= 120 ? 'warn' : ''));
  const rosterRows = () => {
    const rows = [['Players', state.net.players + ' / ' + state.maxPlayers, '', 'section']];
    state.roster.forEach(p => {
      const status = p.host ? 'Host' : (p.inGame ? 'In Game' : 'Loading');
      const mark = p.talking ? '▶  ' : '●  ';   // ▶ while talking on voice
      const ping = pingText(p.ping);
      rows.push([mark + p.name + (p.you ? '  (You)' : ''), status + (ping ? '  ·  ' + ping : ''),
                 pingCls(p.ping) || (p.inGame || p.host ? '' : 'dim'), 'player']);
    });
    if (state.joining > 0)
      rows.push(['○  Joining…', String(state.joining), 'dim', 'player']);
    return rows;
  };
  const statusCls = s => FAILED.indexOf(s) >= 0 ? 'bad' : (s === 'OFFLINE' ? 'dim' : '');

  // ── Shared host save (world transfer + character) ─────────────────────
  const xferFailed = () => state.transfer.phase === 'failed';
  const inHostWorld = () => status() === 'CONNECTED' && state.transfer.phase === 'ready';
  const worldLabel = () => {
    switch (state.transfer.phase) {
      case 'requested':   return ['Asking host…', 'dim'];
      case 'downloading': return ['Downloading ' + state.transfer.percent + '%', ''];
      case 'loading':     return ['Loading…', ''];
      case 'lobby':       return ['Lobby', 'dim'];
      case 'waitinghost': return ['Host is starting…', 'dim'];
      case 'ready':       return ['Loaded', ''];
      case 'failed':      return ['Failed', 'bad'];
    }
    return null;
  };
  const XFER_ERRORS = {
    host_not_in_game: { head: 'Host isn\u2019t in a game', text: 'The host needs to load a save first.' },
    version_mismatch: { head: 'Version mismatch', text: 'You and the host need the same CoSync version.' },
    save_failed:      { head: 'Host couldn\u2019t save', text: 'Ask the host to try again.' },
    save_unreadable:  { head: 'Host couldn\u2019t save', text: 'Ask the host to try again.' },
    host_busy:        { head: 'Host is busy', text: 'Try again in a moment.' },
    checksum:         { head: 'Download failed', text: 'The world arrived broken. Try again.' },
    write_failed:     { head: 'Couldn\u2019t save the world', text: 'Your save folder is not writable.' },
    load_failed:      { head: 'Couldn\u2019t load the world', text: 'You need the same mods as the host, in the same order.' },
    content_not_verified: { head: 'Mods not checked', text: 'Your mods weren\u2019t checked yet. Try again.' },
    default:          { head: 'Couldn\u2019t join', text: 'Try again.' },
  };

  // ── Pre-join content sync (CoSyncContentSync) ───────────────────────────
  const CT = () => state.content || {};
  const cPhase = () => CT().phase || 'idle';
  const CONTENT_BUSY = ['requesting', 'preparing', 'checking', 'waiting', 'confirm', 'downloading', 'installing'];
  const CONTENT_END = ['restart', 'blocked', 'failed', 'cancelled'];
  const contentShown = () => CONTENT_BUSY.indexOf(cPhase()) >= 0 || CONTENT_END.indexOf(cPhase()) >= 0;
  const fmtBytes = n => {
    n = +n || 0;
    if (n >= 1073741824) return (n / 1073741824).toFixed(2) + ' GB';
    if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
    if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
    return n + ' B';
  };
  const pct = (a, b) => (b > 0 ? Math.min(100, Math.floor(a * 100 / b)) : 0);
  const BLOCK_TEXT = {
    official_missing:       n => n + ' is missing. CoSync can\u2019t send game or DLC files.',
    official_different:     n => n + ' doesn\u2019t match the host\u2019s. Update the game.',
    creationclub_missing:   n => 'You need ' + n + ' from the Creation Club.',
    creationclub_different: n => n + ' doesn\u2019t match the host\u2019s.',
    creations_missing:      n => 'You need ' + n + ' from Creations.',
    creations_different:    n => n + ' doesn\u2019t match the host\u2019s.',
    official_not_loaded:    n => n + ' is installed but off. Turn it on in Load Order.',
    host_unreadable:        n => 'The host can\u2019t read ' + n + '.',
    too_large:              n => 'Too much to download (' + n + ').',
  };
  const CONTENT_ERRORS = {
    version_mismatch:  { head: 'Version mismatch', text: 'You and the host need the same CoSync version.' },
    protocol_version:  { head: 'Version mismatch', text: 'You and the host need the same CoSync version.' },
    host_not_in_game:  { head: 'Host isn\u2019t in a game', text: 'The host needs to load a save first.' },
    bad_manifest:      { head: 'Bad mod list', text: 'The host sent a mod list CoSync can\u2019t use. Nothing changed.' },
    unsafe_path:       { head: 'Unsafe file blocked', text: 'The host tried to send a file outside the game folders. Nothing changed.' },
    refused_official:  { head: 'Blocked', text: 'CoSync never sends game, DLC or Creation Club files.' },
    bad_verdict:       { head: 'Bad reply from host', text: 'Nothing changed.' },
    too_large:         { head: 'Too much to download', text: 'The missing mods are over the size limit.' },
    disk_space:        { head: 'Not enough space', text: 'Free up space on your Fallout 4 drive.' },
    write_failed:      { head: 'Couldn\u2019t save the download', text: 'Data\\CoSyncContent is not writable.' },
    hash_mismatch:     { head: 'Download failed', text: 'A file came through broken. Nothing was installed. Try again.' },
    corrupt_transfer:  { head: 'Download failed', text: 'A file came through broken. Nothing was installed. Try again.' },
    host_stalled:      { head: 'Host stopped sending', text: 'Try again. It picks up where it left off.' },
    no_response:       { head: 'No answer from host', text: 'The host may be on an older CoSync or stopped hosting.' },
    host_read_error:   { head: 'Host file error', text: 'Ask the host to host again.' },
    connection_lost:   { head: 'Connection lost', text: 'Try again. It picks up where it left off.' },
    install_failed:    { head: 'Install failed', text: 'The files were downloaded but not installed.' },
    not_loaded:        { head: 'Mods not loaded', text: 'Using MO2 or Vortex? Enable them there and launch from it.' },
    protocol:          { head: 'Mod check failed', text: 'Try again.' },
    flood:             { head: 'Mod check failed', text: 'Too many requests. Try again later.' },
    too_many_requests: { head: 'Mod check failed', text: 'Reconnect and try again.' },
    default:           { head: 'Mod check failed', text: 'Try again.' },
  };
  const contentRows = () => {
    const c = CT();
    const rows = [];
    switch (cPhase()) {
      case 'requesting': rows.push(['Mods', 'Asking host…', 'dim']); break;
      case 'preparing':  rows.push(['Mods', 'Host is getting ready' + (c.checkTotal ? '  ·  ' + c.checkDone + ' / ' + c.checkTotal + ' files' : '…'), 'dim']); break;
      case 'checking':   rows.push(['Mods', 'Checking  ' + pct(c.checkDone, c.checkTotal) + '%']); break;
      case 'waiting':    rows.push(['Mods', 'Host is checking…', 'dim']); break;
      case 'confirm':
        rows.push(['Missing', c.fileCount + (c.fileCount === 1 ? ' file' : ' files') + '  ·  ' + fmtBytes(c.total), 'warn']);
        (c.missing || []).slice(0, 8).forEach(m => rows.push(['  ' + m.name, fmtBytes(m.size) + (m.here ? '  ·  different' : '  ·  missing'), 'dim']));
        if ((c.missing || []).length > 8) rows.push(['  …', '+' + ((c.missing || []).length - 8) + ' more', 'dim']);
        break;
      case 'downloading':
        rows.push(['Downloading', 'File ' + Math.min(c.fileIndex + 1, c.fileCount) + ' of ' + c.fileCount]);
        rows.push(['Current file', c.file || '…']);
        rows.push(['File', pct(c.fileDone, c.fileSize) + '%  ·  ' + fmtBytes(c.fileDone) + ' / ' + fmtBytes(c.fileSize)]);
        rows.push(['Overall', pct(c.done, c.total) + '%  ·  ' + fmtBytes(c.done) + ' / ' + fmtBytes(c.total)]);
        break;
      case 'installing': rows.push(['Mods', 'Installing…']); break;
      case 'restart':    rows.push(['Mods', 'Restart needed', 'warn']); break;
      case 'blocked':
        (c.blocked || []).slice(0, 8).forEach(b => rows.push([b.name || b.code, String(b.code).replace(/_/g, ' '), 'bad']));
        break;
      case 'failed':     rows.push(['Mods', 'Failed', 'bad']); break;
      case 'cancelled':  rows.push(['Mods', 'Cancelled', 'dim']); break;
    }
    if ((c.activate || []).length && (cPhase() === 'confirm' || cPhase() === 'restart'))
      rows.push(['Will enable', c.activate.slice(0, 4).join(', ') + (c.activate.length > 4 ? ' …' : '')]);
    const extra = (c.warnings || []).filter(w => w.code === 'extra').length;
    if (extra) rows.push(['Your extra mods', extra + ' (host doesn\u2019t use them)', 'dim']);
    if ((c.native || []).length)
      rows.push(['F4SE plugins (not sent)', c.native.slice(0, 3).join(', ') + (c.native.length > 3 ? ' …' : ''), 'dim']);
    return rows;
  };
  const contentNotice = () => {
    const c = CT();
    switch (cPhase()) {
      case 'requesting': return { head: 'Checking mods…', text: 'Getting the host\u2019s mod list.' };
      case 'preparing':  return { head: 'Host is getting ready…', text: 'First time can take a while with big mods.' };
      case 'checking':   return { head: 'Checking your mods…', text: 'Comparing with the host.' };
      case 'waiting':    return { head: 'Checking…', text: 'Waiting on the host.' };
      case 'confirm':    return { head: 'You\u2019re missing mods',
        text: c.fileCount + ' file(s), ' + fmtBytes(c.total) + '. CoSync can download them. Replaced files are backed up. They install on the next game start.' +
              (c.scripts ? ' Includes ' + c.scripts + ' script file(s).' : '') };
      case 'downloading': return { head: 'Downloading…', text: 'Cancel keeps what\u2019s done. It picks up next time.' };
      case 'installing': return { head: 'Installing…', text: 'All files checked out.' };
      case 'restart':    return { head: 'Restart needed',
        text: 'Restart Fallout 4. The mods install and you rejoin automatically (within 30 minutes).' };
      case 'blocked': {
        const b = (c.blocked || [])[0];
        const f = b && BLOCK_TEXT[b.code];
        return { head: 'Can\u2019t sync this',
          text: f ? f(b.name) + ((c.blocked || []).length > 1 ? ' (+' + (c.blocked.length - 1) + ' more)' : '') : 'The host uses something CoSync can\u2019t send.' };
      }
      case 'failed': {
        const e = CONTENT_ERRORS[c.error] || CONTENT_ERRORS.default;
        return { head: e.head, text: e.text + (c.detail ? '  (' + c.detail + ')' : '') };
      }
      case 'cancelled':  return { head: 'Cancelled', text: 'What\u2019s done is kept. It picks up next time.' };
    }
    return null;
  };

  // Last join request, for TRY AGAIN.
  let lastJoin = null;
  const doJoin = cmd => { lastJoin = cmd; native(cmd); };

  // Text fields (Direct Connect, team names, ...), kept between visits.
  const form = { address: '', port: '48000' };
  const direct = form;


  // ── Teams / settlements / workshop rules (state from CoSyncTeams) ──────
  const TM = () => state.teams || { me: {}, teams: [], invites: [], pending: [], online: [], nearest: {}, rules: {}, settlements: [] };
  const me = () => TM().me || {};
  const myTeam = () => (TM().teams || []).find(t => t.id === me().team) || null;
  const teamById = id => (TM().teams || []).find(t => t.id === id) || null;
  const ROLE = ['Member', 'Officer', 'Leader'];
  const roleName = r => ROLE[r] || 'Member';
  const isLeader = () => me().role === 2;
  const isOfficer = () => me().role === 1;
  const clean = v => String(v || '').replace(/[|%]/g, '').trim();
  const teamCmd = cmd => native('team_' + cmd);
  const hex = n => ('00000000' + (n >>> 0).toString(16).toUpperCase()).slice(-8);
  const builtCount = ws => ((state.workshop || {}).counts || {})[String(ws)] || 0;
  const wrap = (v, n) => ((v % n) + n) % n;

  // The last answer to our own request, shown for a few seconds.
  let resultSeen = '', resultUntil = 0;
  const resultNotice = () => {
    const r = TM().lastResult || '';
    if (r && r !== resultSeen) { resultSeen = r; resultUntil = Date.now() + 6000; }
    if (!r || Date.now() > resultUntil) return null;
    const ok = r.charAt(0) === '1';
    return { head: ok ? 'Done' : 'Not allowed', text: r.slice(2) };
  };

  // The settlement the player stands in: who it belongs to and what they may do.
  const MODE_TEXT = ['Free', 'Team', 'Personal'];
  const settlementOwner = s => s.mode === 1 ? (s.teamName || 'a team') : (s.mode === 2 ? (s.mine ? 'You' : s.ownerName) : '—');
  const nearestRows = () => {
    const n = TM().nearest || {};
    if (!n.id) return [['Settlement', 'None nearby', 'dim']];
    const rows = [['Settlement', n.name || hex(n.id)]];
    rows.push(['Type', MODE_TEXT[n.mode] || 'Free', n.mode ? '' : 'dim']);
    if (n.mode) rows.push(['Owner', settlementOwner(n)]);
    rows.push(['You can build', n.build ? 'Yes' : 'No', n.build ? '' : 'bad']);
    return rows;
  };
  // Live numbers of that settlement (the host's when it is shared with you).
  const statRows = () => {
    const s = state.settlement || {};
    if (!s.id || s.food === undefined) return [];
    const n = v => (typeof v === 'number' ? String(v) : '—');
    return [
      ['Resources', s.synced ? 'Shared with host' : 'Local', 'dim', 'section'],
      ['Food · Water', n(s.food) + '  ·  ' + n(s.water)],
      ['Power · Defense', n(s.power) + '  ·  ' + n(s.defense)],
      ['Beds · People', n(s.beds) + '  ·  ' + n(s.population)],
      ['Happiness', n(s.happiness) + '%'],
    ];
  };

  // Selections carried between team screens.
  const sel = { invite: 0, member: '', settlement: 0 };

  // Workshop rules draft (host).
  const wsDraft = { mode: 0, free: false };
  const LIMIT_MODES = ['Default', 'Custom', 'Unlimited'];

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
            desc: 'Players, status, address.' });
        } else if (role() === 'CLIENT') {
          list.push({ key: 'session', label: 'Session', go: 'connect',
            desc: 'The game you\u2019re in.' });
        } else {
          list.push({ key: 'host', label: 'Host Game', go: 'host',
            desc: 'Let friends join your game.' });
          list.push({ key: 'join', label: 'Join Game', go: 'join',
            desc: 'Join a friend\u2019s game.' });
        }
        const myT = myTeam();
        const invN = (TM().invites || []).length;
        list.push({ key: 'team', label: 'Team', go: 'team', disabled: !inGame(),
          meta: myT ? myT.name : (invN ? invN + (invN === 1 ? ' invite' : ' invites') : ''),
          desc: inGame() ? 'Make or join a team.'
                         : 'Load a save first.' });
        list.push({ key: 'settlements', label: 'Settlements', go: 'settlements', disabled: !inGame(),
          desc: inGame() ? 'Ownership and build rules.'
                         : 'Load a save first.' });
        list.push({ key: 'voice', label: 'Voice', go: 'voice',
          desc: 'Mic, speakers, volume.' });
        list.push({ key: 'status', label: 'Network', go: 'status',
          desc: 'Connection info.' });
        list.push({ key: 'settings', label: 'Settings', go: 'settings',
          desc: 'Name and controller.' });
        if (role() === 'HOST') {
          const pvp = !!(state.rules && state.rules.pvp);
          list.push({ key: 'pvp', label: 'Friendly Fire', action: 'pvp_toggle',
            meta: pvp ? 'On' : 'Off',
            desc: pvp ? 'Players can hurt each other.' : 'Players can\u2019t hurt each other.' });
          list.push({ key: 'stop', label: 'Stop Hosting', action: 'stop',
            desc: 'Kicks everyone.' });
        }
        if (role() === 'CLIENT')
          list.push({ key: 'leave', label: 'Leave Game', action: 'stop',
            desc: 'Disconnect.' });
        list.push({ key: 'back', label: 'Back', action: 'back',
          desc: '' });
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
            ['Network', steamSession() ? 'Steam' : networkLabel()],
          ];
          if (!steamSession() && state.network.available && state.net.port)
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
          if (steamSession() && state.steam.error === 'lobby_failed')
            return { head: 'Steam lobby failed',
              text: 'Friends can\u2019t find you. Stop hosting and try again.' };
          if (status() === 'WAITING FOR PLAYERS')
            return steamSession()
              ? { head: 'Waiting for friends…',
                  text: 'They\u2019ll see you under Join Game. Or invite them.' }
              : { head: 'Waiting for players…',
                  text: 'They\u2019ll see you under Join Game.' };
          return null;
        }
        if (!inGame() && role() === 'HOST')
          return { head: 'Lobby open',
            text: 'Start or load a game. Everyone follows you in.' };
        if (status() === 'STARTING')
          return { head: 'Starting…', text: '' };
        if (status() === 'FAILED')
          return { head: 'Couldn\u2019t host',
            text: 'Close other copies of the game and try again.' };
        return null;
      },
      items: () => {
        if (role() === 'HOST') {
          const list = [];
          if (steamSession())
            list.push({ key: 'invite', label: 'Invite Friends', action: 'steam_invite',
              disabled: !state.steam.lobby,
              desc: 'Opens the Steam overlay.' });
          list.push({ key: 'stop', label: 'Stop Hosting', action: 'stop',
            desc: 'Kicks everyone.' });
          list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Your game stays open.' });
          return list;
        }
        return [
          { key: 'steam_host', label: 'Host on Steam', action: 'steam_host',
            disabled: status() === 'STARTING' || !state.steam.available,
            desc: !state.steam.available ? 'Steam isn\u2019t running.'
                : 'Steam friends can join. No port forwarding.' },
          { key: 'host', label: 'Host on LAN', action: 'host',
            disabled: status() === 'STARTING',
            desc: 'Players on your network can join.' },
          { key: 'back', label: 'Back', action: 'back', desc: '' },
        ];
      },
    },

    join: {
      title: () => 'Join Game',
      live: true,
      enter: () => { native('search_start'); native('steam_browse|1'); },
      leave: () => { native('search_stop'); native('steam_browse|0'); },
      notice: () => {
        if (state.search.sessions.length || state.steam.games.length) return null;
        if (!state.network.available)
          return { head: 'No network',
            text: 'You\u2019re not connected.' };
        if (state.search.seconds < 3)
          return { head: 'Searching…', text: '' };
        return { head: 'No games found',
          text: 'Steam friends hosting show up here. On LAN, be on the same network and let Fallout 4 through the firewall.' };
      },
      items: () => {
        const steamList = state.steam.games.map(g => {
          const full = g.players >= state.maxPlayers;
          let desc = g.host + '  ·  ' + g.players + ' / ' + state.maxPlayers + ' players';
          if (!g.compatible) desc = 'Different version (theirs ' + g.build + ', yours ' + state.build + ').';
          else if (full) desc = 'This game is full.';
          return { key: 's:' + g.id, label: g.name, meta: 'Steam  ' + g.players + ' / ' + state.maxPlayers,
                   disabled: full || !g.compatible, steamJoin: g.id, desc: desc };
        });
        const list = steamList.concat(state.search.sessions.map(s => {
          const full = s.players >= s.max;
          let desc = s.host + '  ·  ' + s.players + ' / ' + s.max + ' players';
          if (!s.compatible) desc = 'Different version (theirs ' + s.build + ', yours ' + state.build + ').';
          else if (full) desc = 'This game is full.';
          return { key: 'g:' + s.id, label: s.name, meta: s.players + ' / ' + s.max,
                   disabled: full || !s.compatible, join: s.id, desc: desc };
        }));
        list.push({ key: 'refresh', label: 'Refresh', action: 'refresh', desc: '' });
        list.push({ key: 'direct', label: 'Direct Connect', go: 'direct',
          desc: 'Join by IP address.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    direct: {
      title: () => 'Direct Connect',
      notice: () => ({ head: 'Host address',
        text: 'The host finds it under Network. Port is usually 48000.' }),
      items: () => [
        { key: 'addr', label: 'Address', field: 'address', placeholder: '192.168.1.20',
          desc: 'e.g. 192.168.1.20' },
        { key: 'port', label: 'Port', field: 'port', placeholder: '48000', desc: '' },
        { key: 'go', label: 'Connect', action: 'direct', desc: '' },
        { key: 'back', label: 'Back', action: 'back', desc: '' },
      ],
    },

    connect: {
      title: () => {
        switch (cPhase()) {
          case 'confirm': case 'downloading': case 'installing': return 'Mods';
          case 'restart': return 'Restart Needed';
          case 'blocked': case 'failed': return 'Join Failed';
          case 'cancelled': return 'Cancelled';
        }
        if (contentShown()) return 'Checking Mods';
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
        if (contentShown()) return rows.concat(contentRows());
        const w = worldLabel();
        if (w) rows.push(['World', w[0], w[1]]);
        return inHostWorld() ? rows.concat(rosterRows()) : rows;
      },
      notice: () => {
        if (contentShown()) return contentNotice();
        if (xferFailed()) return XFER_ERRORS[state.transfer.error] || XFER_ERRORS.default;
        if (status() === 'OFFLINE' && state.steam.error === 'version_mismatch')
          return XFER_ERRORS.version_mismatch;
        if (status() === 'OFFLINE' && state.steam.error === 'join_failed')
          return { head: 'Couldn\u2019t join', text: 'The host may have stopped hosting.' };
        if (status() === 'OFFLINE' && state.steam.error === 'steam_unavailable')
          return { head: 'Steam isn\u2019t running', text: 'Start Steam and try again.' };
        switch (status()) {
          case 'SESSION FULL':
            return { head: 'Game is full', text: state.maxPlayers + ' players max.' };
          case 'VERSION MISMATCH':
            return { head: 'Version mismatch',
              text: 'You and the host need the same CoSync version.' };
          case 'HOST DISCONNECTED':
            return { head: 'Host disconnected', text: '' };
          case 'CONNECTION FAILED':
          case 'FAILED':
            return { head: 'Couldn\u2019t connect',
              text: 'The host may have stopped, or a firewall is blocking it.' };
          case 'OFFLINE':
            return { head: 'Not connected', text: 'Pick a game under Join Game.' };
        }
        switch (state.transfer.phase) {
          case 'requested':
            return { head: 'Getting the world…', text: 'The host is saving.' };
          case 'downloading':
            return { head: 'Downloading the world…', text: '' };
          case 'loading':
            return { head: 'Loading…', text: 'If the game asks, choose Yes.' };
          case 'lobby':
            return { head: 'In the lobby', text: 'Waiting for the host to start. You\u2019ll join automatically.' };
          case 'waitinghost':
            return { head: 'Host is starting', text: 'You\u2019ll join in a moment.' };
        }
        if (inHostWorld()) {
          if (state.profile === 'CREATING')
            return { head: 'New character', text: 'Make your character.' };
          if (state.profile === 'RESTORING')
            return { head: 'Loading your character…', text: '' };
          return { head: "You're in", text: 'Your character saves with the host\u2019s game.' };
        }
        return { head: 'Connecting…', text: '' };
      },
      items: () => {
        switch (cPhase()) {
          case 'confirm':
            return [
              { key: 'caccept', label: 'Download', action: 'content_accept',
                desc: fmtBytes(CT().total) + '. Installs on restart.' },
              { key: 'ccancel', label: 'Cancel', action: 'content_cancel', desc: 'Leaves the game.' },
            ];
          case 'requesting': case 'preparing': case 'checking': case 'waiting': case 'downloading':
            return [{ key: 'ccancel', label: 'Cancel', action: 'content_cancel', desc: 'Leaves the game. Keeps what\u2019s done.' }];
          case 'installing':
            return [{ key: 'cwait', label: 'Wait…', disabled: true, desc: '' }];
          case 'restart':
            return [
              { key: 'cquit', label: 'Quit Game', action: 'content_quit', desc: 'Start it again to install and rejoin.' },
              { key: 'back', label: 'Back', action: 'leave_back', desc: 'Installs next time you start.' },
            ];
          case 'blocked': case 'failed': case 'cancelled':
            return [
              { key: 'retry', label: 'Try Again', action: 'retry', disabled: !lastJoin, desc: 'Connect to the same game again.' },
              { key: 'back', label: 'Back', action: 'leave_back', desc: '' },
            ];
        }
        if (xferFailed() || FAILED.indexOf(status()) >= 0 || status() === 'OFFLINE')
          return [
            { key: 'retry', label: 'Try Again', action: 'retry', disabled: !lastJoin, desc: 'Connect to the same game again.' },
            { key: 'back', label: 'Back', action: 'leave_back', desc: '' },
          ];
        if (inHostWorld())
          return [
            { key: 'leave', label: 'Leave Game', action: 'stop_root', desc: 'Your character stays saved.' },
            { key: 'back', label: 'Back', action: 'back', desc: 'You stay connected.' },
          ];
        return [
          { key: 'cancel', label: 'Cancel', action: 'stop_root', desc: '' },
        ];
      },
    },

    status: {
      title: () => 'Network',
      live: true,
      rows: () => {
        const rows = [
          ['Status', status(), statusCls(status())],
          ['Role', role()],
          ['Network', networkLabel()],
        ];
        if (state.network.available) {
          rows.push(['Adapter', state.network.adapter]);
          rows.push(['IP', state.network.address]);
        }
        if (role() === 'HOST')
          rows.push(['Port', String(state.net.port)]);
        if (role() !== 'NONE')
          rows.push(['Players', state.net.players + ' / ' + state.maxPlayers]);
        rows.push(['LAN search', state.discovery]);
        rows.push(['Build', String(state.build)]);
        return rows;
      },
      items: () => [
        { key: 'back', label: 'Back', action: 'back', desc: '' },
      ],
    },

    team: {
      title: () => 'Team',
      live: true,
      rows: () => {
        const t = myTeam();
        if (!t) return [['Team', 'None', 'dim']];
        const online = t.members.filter(m => m.online).length;
        return [
          ['Team', t.name],
          ['Role', roleName(me().role)],
          ['Leader', t.leaderName || '—'],
          ['Members', online + ' / ' + t.members.length + ' online'],
          ['Settlement', t.settlement ? t.settlementName : 'None', t.settlement ? '' : 'dim'],
        ];
      },
      notice: () => {
        const r = resultNotice();
        if (r) return r;
        if (!myTeam() && (TM().invites || []).length)
          return { head: 'You\u2019re invited', text: 'Open it below.' };
        if (!myTeam())
          return { head: 'Teams', text: 'Shared settlement, team chat, team voice. One team at a time.' };
        return null;
      },
      items: () => {
        const t = myTeam();
        const list = [];
        if (!t) {
          list.push({ key: 'create', label: 'Create Team', go: 'team_create', enter: () => { form.teamName = ''; },
            desc: 'You\u2019ll be the leader.' });
          (TM().invites || []).forEach(inv => list.push({ key: 'inv' + inv.team, label: 'Invite: ' + inv.name,
            meta: 'from ' + inv.by, go: 'team_invitation', enter: () => { sel.invite = inv.team; },
            desc: inv.by + ' invited you  ·  ' + inv.members + ' members' }));
        } else {
          list.push({ key: 'members', label: 'Members', meta: String(t.members.length), go: 'team_members',
            desc: 'Roles, promote, kick.' });
          if (isLeader() || isOfficer())
            list.push({ key: 'invite', label: 'Invite Players', go: 'team_invite', desc: '' });
          list.push({ key: 'settle', label: 'Team Settlement', meta: t.settlement ? t.settlementName : 'None', go: 'team_settlement',
            desc: 'Everything there is shared.' });
          if (isLeader())
            list.push({ key: 'rename', label: 'Rename Team', go: 'team_rename', enter: () => { form.teamRename = t.name; },
              desc: '' });
          list.push({ key: 'leave', label: 'Leave Team', run: () => teamCmd('leave'),
            confirm: isLeader() ? 'Press again. An officer takes over.' : 'Press again to leave.',
            desc: '' });
          if (isLeader())
            list.push({ key: 'disband', label: 'Disband Team', run: () => teamCmd('disband'),
              confirm: 'Press again. The team is gone for good.',
              desc: '' });
        }
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    team_create: {
      title: () => 'Create Team',
      live: true,
      notice: () => resultNotice(),
      items: () => [
        { key: 'name', label: 'Name', field: 'teamName', maxlen: 24, placeholder: 'Sole Survivors',
          desc: '3-24 characters.' },
        { key: 'go', label: 'Create', disabled: clean(form.teamName).length < 3,
          run: () => teamCmd('create|' + clean(form.teamName)), after: 'back',
          desc: 'Standing in a free settlement? It becomes the team\u2019s.' },
        { key: 'back', label: 'Back', action: 'back', desc: '' },
      ],
    },

    team_invitation: {
      title: () => 'Invitation',
      live: true,
      rows: () => {
        const inv = (TM().invites || []).find(i => i.team === sel.invite);
        return inv ? [['Team', inv.name], ['From', inv.by], ['Members', String(inv.members)]]
                   : [['Invite', 'Expired', 'dim']];
      },
      items: () => [
        { key: 'accept', label: 'Accept', run: () => teamCmd('accept|' + sel.invite), after: 'back', desc: '' },
        { key: 'decline', label: 'Decline', run: () => teamCmd('decline|' + sel.invite), after: 'back', desc: '' },
        { key: 'back', label: 'Back', action: 'back', desc: 'Invites expire in 2 minutes.' },
      ],
    },

    team_members: {
      title: () => 'Members',
      live: true,
      items: () => {
        const t = myTeam();
        if (!t) return [{ key: 'back', label: 'Back', action: 'back', desc: '' }];
        const list = t.members.slice().sort((a, b) => b.role - a.role).map(m => ({
          key: 'm:' + m.key, label: (m.online ? '● ' : '○ ') + m.name + (m.key === me().key ? ' (You)' : ''),
          meta: roleName(m.role), go: 'team_member', enter: () => { sel.member = m.key; },
          desc: m.online ? 'Online' : 'Offline' }));
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    team_member: {
      title: () => 'Member',
      live: true,
      rows: () => {
        const t = myTeam();
        const m = t && t.members.find(x => x.key === sel.member);
        if (!m) return [['Member', 'Left the team', 'dim']];
        return [['Name', m.name], ['Role', roleName(m.role)], ['Status', m.online ? 'Online' : 'Offline', m.online ? '' : 'dim']];
      },
      notice: () => resultNotice(),
      items: () => {
        const t = myTeam();
        const m = t && t.members.find(x => x.key === sel.member);
        const list = [];
        if (m && m.key !== me().key) {
          if (isLeader() && m.role === 0)
            list.push({ key: 'promote', label: 'Promote to Officer', run: () => teamCmd('promote|' + m.key),
              desc: 'Officers can invite and kick.' });
          if (isLeader() && m.role === 1)
            list.push({ key: 'demote', label: 'Demote to Member', run: () => teamCmd('demote|' + m.key),
              desc: '' });
          if (isLeader())
            list.push({ key: 'lead', label: 'Make Leader', run: () => teamCmd('transfer|' + m.key), after: 'back',
              confirm: 'Press again. You become an officer.', desc: '' });
          if (isLeader() || (isOfficer() && m.role === 0))
            list.push({ key: 'kick', label: 'Kick', run: () => teamCmd('kick|' + m.key), after: 'back',
              confirm: 'Press again to kick ' + m.name + '.', desc: '' });
        }
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    team_invite: {
      title: () => 'Invite Players',
      live: true,
      notice: () => resultNotice(),
      items: () => {
        const pending = TM().pending || [];
        const list = (TM().online || []).filter(o => !o.you && !o.team).map(o => {
          const invited = pending.indexOf(o.key) >= 0;
          return { key: 'i:' + o.key, label: o.name, meta: invited ? 'Invited' : 'Invite', disabled: invited,
            run: () => teamCmd('invite|' + o.key),
            desc: invited ? 'Waiting for an answer.' : 'They answer from the Team menu.' };
        });
        if (!list.length) list.push({ key: 'none', label: 'Nobody to invite', disabled: true,
          desc: 'Everyone online is already in a team.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    team_settlement: {
      title: () => 'Team Settlement',
      live: true,
      rows: () => {
        const t = myTeam();
        const rows = [['Team settlement', t && t.settlement ? t.settlementName : 'None', t && t.settlement ? '' : 'dim']];
        if (t && t.settlement) rows.push(['Objects built', String(builtCount(t.settlement))]);
        return rows.concat(nearestRows()).concat(statRows());
      },
      notice: () => resultNotice() || { head: 'Team settlement',
        text: 'Only your team can build here. Everything is shared.' },
      items: () => {
        const t = myTeam(), n = TM().nearest || {};
        const list = [];
        if (t && (isLeader() || isOfficer())) {
          list.push({ key: 'here', label: 'Use This Settlement', disabled: !n.id || (n.mode === 1 && n.teamId === t.id),
            run: () => teamCmd('settle'),
            desc: !n.id ? 'Go to a settlement first.'
                : (n.mode ? 'Already taken.' : 'Make ' + (n.name || hex(n.id)) + ' your team\u2019s.') });
          list.push({ key: 'clear', label: 'Clear Team Settlement', disabled: !t.settlement, run: () => teamCmd('settle_clear'),
            confirm: 'Press again. Nothing gets removed.', desc: 'Stops sharing it.' });
        }
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    team_rename: {
      title: () => 'Rename Team',
      live: true,
      notice: () => resultNotice(),
      items: () => [
        { key: 'nm', label: 'Name', field: 'teamRename', maxlen: 24, desc: '3-24 characters.' },
        { key: 'go', label: 'Save', disabled: clean(form.teamRename).length < 3,
          run: () => teamCmd('rename|' + clean(form.teamRename)), after: 'back', desc: '' },
        { key: 'back', label: 'Back', action: 'back', desc: '' },
      ],
    },

    settlements: {
      title: () => 'Settlements',
      live: true,
      rows: () => nearestRows().concat(statRows()),
      notice: () => resultNotice() || (!state.workshop.hooks && inGame()
        ? { head: 'Build protection is off', text: 'It doesn\u2019t work on this game version.' }
        : { head: 'Personal settlements', text: 'Only you can build there. Not shared with your team.' }),
      items: () => {
        const n = TM().nearest || {};
        const all = TM().settlements || [];
        const mine = all.find(s => s.mode === 2 && s.mine);
        const list = [];
        list.push({ key: 'personal', label: 'Claim as Personal', disabled: !n.id || !!n.mode || !!mine,
          run: () => teamCmd('personal'),
          desc: !n.id ? 'Go to a settlement first.'
              : (n.mode ? 'Already taken.'
              : (mine ? 'You already have ' + mine.name + '. Release it first.' : 'Only you can build in ' + (n.name || hex(n.id)) + '.')) });
        if (mine)
          list.push({ key: 'release', label: 'Release ' + mine.name, run: () => teamCmd('personal_release'),
            confirm: 'Press again. Anyone can build there again.', desc: '' });
        all.forEach(s => list.push({ key: 's:' + s.id, label: s.name, meta: s.mode === 1 ? 'Team · ' + s.teamName : (s.mine ? 'Personal · You' : 'Personal · ' + s.ownerName),
          go: 'settlement', enter: () => { sel.settlement = s.id; },
          desc: builtCount(s.id) + ' objects built' }));
        if (inGame() && role() !== 'CLIENT')
          list.push({ key: 'rules', label: 'Workshop Rules', go: 'ws_rules',
            enter: () => { const st = TM().rules || {}; wsDraft.mode = st.limitMode || 0; wsDraft.free = !!st.freeBuild; form.wsLimit = String(st.customLimit || 1000); },
            desc: 'Build limits for everyone.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    settlement: {
      title: () => 'Settlement',
      live: true,
      rows: () => {
        const s = (TM().settlements || []).find(x => x.id === sel.settlement);
        if (!s) return [['Settlement', 'Not claimed', 'dim']];
        const lim = (TM().rules || {}).limitMode === 1 ? ' / ' + TM().rules.customLimit : '';
        return [['Name', s.name], ['Type', MODE_TEXT[s.mode] || '—'], ['Owner', settlementOwner(s)],
                ['Workbench', hex(s.id)], ['Objects built', builtCount(s.id) + lim]];
      },
      notice: () => resultNotice(),
      items: () => {
        const list = [];
        if (TM().authority && role() !== 'CLIENT')
          list.push({ key: 'unassign', label: 'Free It (Host)', run: () => teamCmd('admin_release|' + sel.settlement), after: 'back',
            confirm: 'Press again. Nothing gets removed.', desc: '' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: '' });
        return list;
      },
    },

    ws_rules: {
      title: () => 'Workshop Rules',
      live: true,
      rows: () => [
        ['Build protection', state.workshop.hooks ? 'On' : 'Off', state.workshop.hooks ? '' : 'bad'],
        ['Build limit', LIMIT_MODES[(TM().rules || {}).limitMode || 0] +
          (((TM().rules || {}).limitMode === 1) ? ' (' + TM().rules.customLimit + ')' : '')],
      ],
      notice: () => resultNotice() || { head: 'Host rules',
        text: 'Apply to everyone. Claimed settlements stay protected.' },
      items: () => [
        { key: 'mode', label: 'Build Limit', option: LIMIT_MODES[wsDraft.mode], cycle: d => { wsDraft.mode = wrap(wsDraft.mode + d, 3); },
          desc: 'Default uses the normal size bar.' },
        { key: 'lim', label: 'Custom Limit', field: 'wsLimit', maxlen: 6, placeholder: '1000', disabled: wsDraft.mode !== 1,
          desc: 'Max objects per settlement (1-100000).' },
        { key: 'free', label: 'Build Anywhere', option: wsDraft.free ? 'On' : 'Off',
          cycle: () => { wsDraft.free = !wsDraft.free; },
          desc: 'Build outside settlement borders.' },
        { key: 'apply', label: 'Apply', run: () => teamCmd('rules|' + wsDraft.mode + '|' + (parseInt(form.wsLimit, 10) || 1000) + '|' + (wsDraft.free ? 1 : 0)),
          desc: '' },
        { key: 'back', label: 'Back', action: 'back', desc: '' },
      ],
    },


    // 0.3.3 (Stage 8): voice settings. Proximity range is the host's rule and
    // is shown, never edited, here.
    voice: {
      title: () => 'Voice',
      live: true,
      enter: () => native('voice_refresh'),
      rows: () => {
        const v = state.voice || {};
        const inSession = role() !== 'NONE';
        return [
          ['Mic', v.micOk ? 'OK' : (inSession ? 'Not found' : 'Starts in game'), v.micOk ? '' : 'dim'],
          ['Mode', v.ptt ? 'Push to talk' : 'Open mic'],
          ['Range', 'Host decides', 'dim'],
        ];
      },
      items: () => {
        const v = state.voice || {};
        const pick = (list, cur) => {
          const ids = [''].concat((list || []).map(d => d.id));
          const at = Math.max(0, ids.indexOf(cur || ''));
          const d = (list || []).find(x => x.id === cur);
          return { ids, at, name: cur ? (d ? d.name : 'Missing device') : 'Default' };
        };
        const inp = pick(v.inputs, v.input);
        const out = pick(v.outputs, v.output);
        const pct = x => Math.round((x || 0) * 100) + '%';
        const step = (x, d, lo, hi, by) => Math.min(hi, Math.max(lo, Math.round(((x || 0) + d * by) * 100) / 100));
        return [
          { key: 'vin', label: 'Mic', option: inp.name,
            cycle: d => { const id = inp.ids[wrap(inp.at + d, inp.ids.length)]; v.input = id; native('voice|input|' + id); },
            desc: '← / → to change' },
          { key: 'vgain', label: 'Mic Volume', option: pct(v.gain),
            cycle: d => { v.gain = step(v.gain, d, 0, 2, 0.1); native('voice|gain|' + v.gain); },
            desc: '' },
          { key: 'vsens', label: 'Mic Sensitivity', option: Math.round((v.sensitivity || 0) * 10) + ' / 10',
            disabled: !!v.ptt,
            cycle: d => { v.sensitivity = step(v.sensitivity, d, 0, 1, 0.1); native('voice|sensitivity|' + v.sensitivity); },
            desc: v.ptt ? 'Not used with push to talk.' : 'Higher picks up quieter sound.' },
          { key: 'vout', label: 'Speakers', option: out.name,
            cycle: d => { const id = out.ids[wrap(out.at + d, out.ids.length)]; v.output = id; native('voice|output|' + id); },
            desc: '← / → to change' },
          { key: 'vvol', label: 'Voice Volume', option: pct(v.volume),
            cycle: d => { v.volume = step(v.volume, d, 0, 2, 0.1); native('voice|volume|' + v.volume); },
            desc: '' },
          { key: 'vmute', label: 'Mute Mic', option: v.muted ? 'On' : 'Off',
            cycle: () => { v.muted = !v.muted; native('voice|mute|' + (v.muted ? '1' : '0')); },
            desc: '' },
          { key: 'back', label: 'Back', action: 'back', desc: '' },
        ];
      },
    },

    settings: {
      title: () => 'Settings',
      live: true,
      rows: () => [
        ['Player Name', state.playerName || '—'],
      ],
      items: () => [
        { key: 'voice', label: 'Voice', go: 'voice', desc: 'Mic, speakers, volume.' },
        { key: 'pad', label: 'Controller Buttons', option: state.controllerStyle === 1 ? 'PlayStation' : 'Xbox',
          toggle: 'controller_style',
          desc: 'Button icons.' },
        { key: 'anim', label: 'Animation Sync', option: state.animTesting ? 'Testing' : 'Current',
          toggle: 'anim_mode',
          desc: 'Dev option. Leave on Current.' },
        { key: 'back', label: 'Back', action: 'back', desc: '' },
      ],
    },
  };

  // ── Rendering ───────────────────────────────────────────────────────────
  let stack = ['root'];
  let index = 0;
  let open = true;
  let renderedKeys = '';
  let armed = null;          // key of a confirm-item waiting for its second press
  let armedTimer = 0;
  let selectedKey = null;   // key of the highlighted row, survives list rebuilds
  const current = () => screens[stack[stack.length - 1]];
  const currentItems = () => current().items();

  // 0.3.4: screens without their own data (the root menu, settings lists)
  // show the live session overview in the DATA pane instead of leaving it
  // blank.
  const overviewRows = () => {
    const rows = [
      ['Link', status(), statusCls(status())],
      ['Role', role() === 'HOST' ? 'Host' : (role() === 'CLIENT' ? 'Client' : 'None'), role() === 'NONE' ? 'dim' : ''],
      ['Network', steamSession() ? 'Steam' : networkLabel(), state.network.available || steamSession() ? '' : 'dim'],
    ];
    if (role() === 'CLIENT' && state.net.joinTarget) rows.push(['Host', state.net.joinTarget]);
    if (role() === 'HOST' && !steamSession() && state.network.available && state.net.port)
      rows.push(['Address', state.network.address + ' : ' + state.net.port]);
    if (state.build) rows.push(['CoSync', String(Math.floor(state.build / 10000)) + '.' +
                                String(Math.floor(state.build / 100) % 100) + '.' + String(state.build % 100), 'dim']);
    return role() === 'NONE' ? rows : rows.concat(rosterRows());
  };

  // 0.3.4: header status strip -- the live link state at a glance.
  const renderStatus = () => {
    const st = status();
    const led = FAILED.indexOf(st) >= 0 ? 'bad'
      : (st === 'OFFLINE' ? 'off'
      : (st === 'CONNECTED' || st === 'HOSTING' || st === 'WAITING FOR PLAYERS' ? 'on' : 'wait'));
    const segs = ['<span class="seg state ' + statusCls(st) + '"><span class="led ' + led + '"></span>' + escapeHtml(st) + '</span>'];
    if (role() !== 'NONE') {
      segs.push('<span class="seg">' + (role() === 'HOST' ? 'HOST' : 'CLIENT') + '</span>');
      segs.push('<span class="seg">' + state.net.players + ' / ' + state.maxPlayers + '</span>');
      if (role() === 'CLIENT') {
        const h = (state.roster || []).find(p => p.host);
        const ping = h ? pingText(h.ping) : '';
        if (ping) segs.push('<span class="seg">' + ping + '</span>');
      }
    }
    $('status').innerHTML = segs.join('');
  };

  const renderRows = () => {
    const scr = current();
    let rows = scr.rows ? scr.rows() : [];
    const n = scr.notice ? scr.notice() : null;
    if (!rows.length && !n) rows = overviewRows();
    $('rows').innerHTML = rows.map(r =>
      '<div class="row' + (r[3] ? ' ' + r[3] : '') + '"><span class="label">' + escapeHtml(r[0]) + '</span>' +
      '<span class="lead"></span>' +
      '<span class="value ' + (r[2] || '') + '">' + escapeHtml(r[1]) + '</span></div>').join('');
    $('notice').innerHTML = n ? '<span class="head">' + escapeHtml(n.head) + '</span>' + escapeHtml(n.text) : '';
    $('title').textContent = scr.title();
    renderStatus();
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
        (it.maxlen || (it.field === 'port' ? 5 : 15)) + '" placeholder="' + escapeHtml(it.placeholder || '') +
        '" value="' + escapeHtml(form[it.field] || '') + '">';
      return '<div class="item' + (it.disabled ? ' disabled' : '') + '" data-i="' + i + '">' +
             '<span>' + escapeHtml(it.label) + '</span>' + right + '</div>';
    }).join('');

    Array.prototype.forEach.call($('list').children, el => {
      const i = +el.getAttribute('data-i');
      el.addEventListener('mouseenter', () => { if (i !== index) { select(i); sound('focus'); } });
      el.addEventListener('click', () => { select(i); if (!currentItems()[i].field) activate(); });
    });
    Array.prototype.forEach.call(document.querySelectorAll('input.field'), inp => {
      inp.addEventListener('input', () => { form[inp.getAttribute('data-field')] = inp.value; });
    });

    renderedKeys = items.map(it => it.key + (it.disabled ? '!' : '') + (it.meta || '') + (it.option || '') + it.label).join('|');
    highlight();
  };

  const renderCrumb = () => {
    const crumbs = stack.slice(0, -1).map(id => screens[id].title());
    $('crumb').textContent = crumbs.length ? crumbs.join('  ›  ')
      : (inGame() ? 'CoSync  ›  In Game' : 'CoSync  ›  Main Menu');
  };

  const render = keepKey => {
    const scr = current();
    renderCrumb();
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

  const cycleOption = (it, dir) => {
    if (typeof it.cycle === 'function') {
      it.cycle(dir || 1);
      sound('prevnext');
      renderList(selectedKey);
      renderRows();
      return;
    }
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
    if (it.toggle || it.cycle) { cycleOption(it, 1); return; }
    if (it.confirm && armed !== it.key) {
      // Destructive: press again to confirm.
      armed = it.key;
      sound('focus');
      $('desc').textContent = it.confirm;
      clearTimeout(armedTimer);
      armedTimer = setTimeout(() => { armed = null; }, 3000);
      return;
    }
    armed = null;
    if (typeof it.run === 'function') {
      sound('ok');
      it.run();
      if (it.after) { if (it.after === 'back') back(); else go(it.after); }
      else render(selectedKey);
      return;
    }
    if (it.go) { sound('ok'); if (it.enter) it.enter(); go(it.go); return; }
    if (it.join || it.steamJoin) {
      sound('ok');
      doJoin(it.steamJoin ? 'steam_join|' + it.steamJoin : 'join|' + it.join);
      stack = ['root', 'connect'];
      transition(() => render(null));
      return;
    }
    switch (it.action) {
      case 'back':    back(); break;
      case 'host':    sound('ok'); native('host'); break;
      case 'steam_host': sound('ok'); native('steam_host'); break;
      case 'steam_invite': sound('ok'); native('steam_invite'); break;
      case 'stop':    sound('ok'); native('stop'); break;
      case 'pvp_toggle': sound('ok'); native('pvp|' + ((state.rules && state.rules.pvp) ? '0' : '1')); break;
      case 'stop_root': sound('cancel'); native('stop'); goRoot(); break;
      case 'leave_back': sound('cancel'); native('stop'); goRoot(); break;
      case 'refresh': sound('ok'); native('search_start'); break;
      case 'retry':   sound('ok'); if (lastJoin) native(lastJoin); break;
      case 'content_accept': sound('ok'); native('content|accept'); break;
      case 'content_cancel': sound('cancel'); native('content|cancel'); break;
      case 'content_quit':   sound('ok'); native('content|quit'); break;
      case 'direct': {
        const addr = direct.address.trim();
        const port = (direct.port || '48000').trim();
        if (!/^\d{1,3}(\.\d{1,3}){3}$/.test(addr) || !/^\d{1,5}$/.test(port)) {
          sound('cancel');
          $('desc').textContent = 'Use an address like 192.168.1.20 and port 48000.';
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
        if (it && (it.toggle || it.cycle) && !it.disabled) cycleOption(it, action === 'left' ? -1 : 1);
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
    renderCrumb();

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
