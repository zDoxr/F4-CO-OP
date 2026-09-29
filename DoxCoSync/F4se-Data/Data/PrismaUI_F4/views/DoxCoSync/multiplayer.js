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
 *                  "fac_<op>|<args...>"   faction / territory / workshop-settings
 *                                         requests (CoSyncFactions; the host decides))
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
    profile: '',
    build: 0,
    maxPlayers: 8,
    factions: null,                 // CoSyncFactions::BuildUiJson
    workshop: { hooks: false, counts: {} },
    customBanners: [],              // files in banners/custom
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

  // Text fields (Direct Connect, faction names, ...), kept between visits.
  const form = { address: '', port: '48000' };
  const direct = form;


  // ── Factions / territory / workshop (state from CoSyncFactions) ────────
  // Permission bits: CoSyncFactions::Perm.
  const PERM = { build: 1, move: 2, scrap: 4, repair: 8, invite: 256, kick: 512, ban: 1024,
                 promote: 2048, ranks: 4096, territory: 8192, edit: 16384 };
  const PERM_LIST = [
    ['build', 'Build'], ['move', 'Move Objects'], ['scrap', 'Scrap / Store'], ['repair', 'Repair'],
    ['invite', 'Invite Players'], ['kick', 'Kick Members'], ['ban', 'Ban Players'],
    ['promote', 'Promote / Demote'], ['ranks', 'Manage Ranks'], ['territory', 'Manage Territory'],
    ['edit', 'Faction Name & Banner'],
  ];
  const FX = () => state.factions || { me: {}, factions: [], invites: [], online: [], nearest: {}, settings: {} };
  const me = () => FX().me || {};
  const myFaction = () => (FX().factions || []).find(f => f.id === me().faction) || null;
  const factionById = id => (FX().factions || []).find(f => f.id === id) || null;
  const can = bit => (me().perms & bit) !== 0;
  const rankOf = (f, id) => (f && f.ranks.find(r => r.id === id)) || null;
  const rankName = (f, id) => { const r = rankOf(f, id); return r ? r.name : '—'; };
  const rankPrio = (f, id, key) => (f && key === f.leader) ? 1000 : ((rankOf(f, id) || {}).priority || 0);
  const clean = v => String(v || '').replace(/[|%]/g, '').trim();
  const fac = cmd => native('fac_' + cmd);
  const hex = n => ('00000000' + (n >>> 0).toString(16).toUpperCase()).slice(-8);
  const builtCount = ws => ((state.workshop || {}).counts || {})[String(ws)] || 0;

  // The last answer to our own request, shown for a few seconds.
  let resultSeen = '', resultUntil = 0;
  const resultNotice = () => {
    const r = FX().lastResult || '';
    if (r && r !== resultSeen) { resultSeen = r; resultUntil = Date.now() + 6000; }
    if (!r || Date.now() > resultUntil) return null;
    const ok = r.charAt(0) === '1';
    return { head: ok ? 'Done' : 'Refused', text: r.slice(2) };
  };

  const nearestRows = () => {
    const n = FX().nearest || {};
    if (!n.id) return [['Settlement Here', 'None nearby', 'dim']];
    const rows = [['Settlement Here', n.owner ? (n.name || hex(n.id)) : 'Unclaimed (' + hex(n.id) + ')']];
    if (n.owner) rows.push(['Controlled By', n.ownerName]);
    const may = [n.build && 'Build', n.move && 'Move', n.scrap && 'Scrap', n.repair && 'Repair'].filter(Boolean);
    rows.push(['You May Here', may.length ? may.join(' · ') : 'Nothing', may.length ? '' : 'bad']);
    return rows;
  };

  // Banner editor draft (create / change banner).
  const bannerDraft = { emblem: 0, color: 0, custom: -1 };
  const loadBannerDraft = id => {
    const b = window.CoSyncBanners.parse(id);
    bannerDraft.emblem = Math.max(0, window.CoSyncBanners.EMBLEM_NAMES.indexOf(b.emblem));
    bannerDraft.color = Math.max(0, window.CoSyncBanners.COLORS.indexOf(b.color));
    bannerDraft.custom = (state.customBanners || []).indexOf(b.custom);
  };
  const draftBannerId = () => {
    const B = window.CoSyncBanners, list = state.customBanners || [];
    return B.make({ emblem: B.EMBLEM_NAMES[bannerDraft.emblem], color: B.COLORS[bannerDraft.color],
                    custom: bannerDraft.custom >= 0 && bannerDraft.custom < list.length ? list[bannerDraft.custom] : '' });
  };
  const wrap = (v, n) => ((v % n) + n) % n;
  const bannerItems = () => {
    const B = window.CoSyncBanners, list = state.customBanners || [];
    const col = B.COLORS[bannerDraft.color];
    return [
      { key: 'bn_emblem', label: 'Emblem', option: B.emblemLabel(B.EMBLEM_NAMES[bannerDraft.emblem]),
        cycle: d => { bannerDraft.emblem = wrap(bannerDraft.emblem + d, B.EMBLEM_NAMES.length); },
        desc: 'The symbol on the banner. Press ← / → to change.' },
      { key: 'bn_color', label: 'Colour', option: B.COLOR_NAMES[col] || col,
        cycle: d => { bannerDraft.color = wrap(bannerDraft.color + d, B.COLORS.length); },
        desc: 'The banner cloth colour.' },
      { key: 'bn_custom', label: 'Custom Image',
        option: bannerDraft.custom >= 0 && bannerDraft.custom < list.length ? list[bannerDraft.custom] : 'None',
        cycle: d => { bannerDraft.custom = list.length ? wrap(bannerDraft.custom + 1 + d, list.length + 1) - 1 : -1; },
        desc: (() => {
          const f = bannerDraft.custom >= 0 && bannerDraft.custom < list.length ? list[bannerDraft.custom] : '';
          if (f && B.isBroken(f)) return f + ' could not be displayed (the image data is damaged). Pick another or re-save it.';
          const bad = (state.bannerRejects || []).length;
          const note = bad ? '  ' + bad + (bad === 1 ? ' file' : ' files') + ' in the folder cannot be used; see the list above.' : '';
          return (list.length
            ? 'An image from Data\\PrismaUI_F4\\views\\DoxCoSync\\banners\\custom. Other players see it if they have the same file; otherwise they see the emblem.'
            : 'Put PNG/JPG/WEBP/SVG files in Data\\PrismaUI_F4\\views\\DoxCoSync\\banners\\custom to use your own banner art.') + note;
        })() },
    ];
  };
  // Custom images the plugin could not use, with the reason (rows).
  const bannerRejectRows = () => {
    const bad = state.bannerRejects || [];
    if (!bad.length) return [];
    return [['Unusable Images', String(bad.length), 'bad', 'section']].concat(
      bad.slice(0, 8).map(r => [r.file, r.why, 'bad', 'player']));
  };

  // Selections carried between faction screens.
  const sel = { invite: 0, member: '', rank: 0, terr: 0, rankDraft: { perms: 0, target: 0 } };

  // Workshop settings draft (host).
  const wsDraft = { mode: 0, free: false, unclaimed: 0 };
  const LIMIT_MODES = ['Fallout 4 Default', 'Custom Limit', 'Unlimited'];

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
            desc: 'Open your game to your Steam friends or to players on your network.' });
          list.push({ key: 'join', label: 'Join Game', go: 'join',
            desc: 'Join a Steam friend’s game or a game hosted on your network.' });
        }
        list.push({ key: 'factions', label: 'Factions', go: 'factions', disabled: !inGame(),
          desc: inGame() ? 'Your faction, its ranks, members and the settlements it controls.'
                         : 'Load a save first. Factions and territory live in the game world.' });
        if (inGame() && role() !== 'CLIENT')
          list.push({ key: 'wsset', label: 'Workshop Settings', go: 'ws_settings',
            enter: () => { const st = FX().settings || {}; wsDraft.mode = st.limitMode || 0; wsDraft.free = !!st.freeBuild;
                           wsDraft.unclaimed = st.unclaimed || 0; form.wsLimit = String(st.customLimit || 1000); },
            desc: 'Build limits and building rules for everyone in your game.' });
        list.push({ key: 'voice', label: 'Voice & Audio', go: 'voice',
          desc: 'Microphone, speakers and voice volume.' });
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
            ['Network', steamSession() ? 'Steam (friends)' : networkLabel()],
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
              text: 'Your game is open, but Steam could not create the lobby friends use to find it. Stop hosting and try again.' };
          if (status() === 'WAITING FOR PLAYERS')
            return steamSession()
              ? { head: 'Waiting for friends…',
                  text: 'Your Steam friends see your game under JOIN GAME, or invite them with INVITE FRIENDS.' }
              : { head: 'Waiting for players…',
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
        if (role() === 'HOST') {
          const list = [];
          if (steamSession())
            list.push({ key: 'invite', label: 'Invite Friends', action: 'steam_invite',
              disabled: !state.steam.lobby,
              desc: 'Open the Steam overlay and invite friends to your game.' });
          list.push({ key: 'stop', label: 'Stop Hosting', action: 'stop',
            desc: 'Close your game to other players. Everyone connected will be disconnected.' });
          list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu. Your game stays open.' });
          return list;
        }
        return [
          { key: 'steam_host', label: 'Host with Steam Friends', action: 'steam_host',
            disabled: !inGame() || status() === 'STARTING' || !state.steam.available,
            desc: !state.steam.available ? 'Steam is not available.'
                : (inGame() ? 'Open your game to your Steam friends over the internet. No port forwarding needed.'
                            : 'Load a save first. Hosting starts from inside your game.') },
          { key: 'host', label: 'Host on Local Network', action: 'host',
            disabled: !inGame() || status() === 'STARTING',
            desc: inGame() ? 'Open your game to players on your network (same router or Wi-Fi).'
                           : 'Load a save first. Hosting starts from inside your game.' },
          { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' },
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
            text: 'This PC is not connected to a network. Connect to your router by cable or Wi-Fi.' };
        if (state.search.seconds < 3)
          return { head: 'Searching for games…', text: 'Looking for CoSync games on your network.' };
        return { head: 'No games found',
          text: 'Steam friends who host with Steam show up here. For a local game, make sure the host has started hosting and both PCs are on the same network (same router or Wi-Fi); Windows Firewall must allow Fallout 4.' };
      },
      items: () => {
        const steamList = state.steam.games.map(g => {
          const full = g.players >= state.maxPlayers;
          let desc = 'Steam friend ' + g.host + '.  ' + g.players + ' / ' + state.maxPlayers + ' players.';
          if (!g.compatible) desc = 'Different CoSync version (host build ' + g.build + ', yours ' + state.build + '). Both players need the same version.';
          else if (full) desc = 'This game is full.';
          return { key: 's:' + g.id, label: g.name, meta: 'Steam  ' + g.players + ' / ' + state.maxPlayers,
                   disabled: full || !g.compatible, steamJoin: g.id, desc: desc };
        });
        const list = steamList.concat(state.search.sessions.map(s => {
          const full = s.players >= s.max;
          let desc = 'Hosted by ' + s.host + '.  ' + s.players + ' / ' + s.max + ' players.';
          if (!s.compatible) desc = 'Different CoSync version (host build ' + s.build + ', yours ' + state.build + '). Both players need the same version.';
          else if (full) desc = 'This game is full.';
          return { key: 'g:' + s.id, label: s.name, meta: s.players + ' / ' + s.max,
                   disabled: full || !s.compatible, join: s.id, desc: desc };
        }));
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
        if (status() === 'OFFLINE' && state.steam.error === 'version_mismatch')
          return XFER_ERRORS.version_mismatch;
        if (status() === 'OFFLINE' && state.steam.error === 'join_failed')
          return { head: 'Could not join', text: 'The Steam lobby could not be joined. The host may have stopped hosting.' };
        if (status() === 'OFFLINE' && state.steam.error === 'steam_unavailable')
          return { head: 'Steam unavailable', text: 'Steam must be running to join a friend’s game.' };
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

    factions: {
      title: () => 'Factions',
      live: true,
      banner: () => { const f = myFaction(); return f ? { id: f.banner, title: f.name, sub: rankName(f, me().rank) + (me().leader ? ' · Leader' : '') } : null; },
      rows: () => {
        const f = myFaction();
        const rows = [];
        if (!f) rows.push(['Faction', 'None', 'dim']);
        else {
          rows.push(['Leader', f.leaderName || '—']);
          rows.push(['Members', String(f.members.length)]);
          rows.push(['Territory', f.territories.length + (f.territories.length === 1 ? ' settlement' : ' settlements')]);
        }
        return rows.concat(nearestRows());
      },
      notice: () => {
        const r = resultNotice();
        if (r) return r;
        if (!myFaction() && (FX().invites || []).length)
          return { head: 'You have been invited', text: 'Open an invitation below to accept or decline it.' };
        if (!state.workshop.hooks && inGame())
          return { head: 'Workshop protection unavailable', text: 'The workshop hooks could not be installed on this game version, so building is not protected. See the CoSync log.' };
        return null;
      },
      items: () => {
        const f = myFaction();
        const list = [];
        if (!f) {
          list.push({ key: 'create', label: 'Create Faction', go: 'fac_create',
            enter: () => { form.facName = ''; loadBannerDraft(''); },
            desc: 'Found a faction. You become its leader.' });
          (FX().invites || []).forEach(inv => list.push({ key: 'inv' + inv.faction, label: 'Invitation: ' + inv.name,
            meta: 'from ' + inv.by, go: 'fac_invitation', enter: () => { sel.invite = inv.faction; },
            desc: inv.by + ' invited you to join ' + inv.name + '.' }));
        } else {
          list.push({ key: 'members', label: 'Members', meta: String(f.members.length), go: 'fac_members',
            desc: 'Members, their ranks, and kick / ban / promote.' });
          if (can(PERM.invite))
            list.push({ key: 'invite', label: 'Invite Players', go: 'fac_invite', desc: 'Invite players in this game to join.' });
          list.push({ key: 'ranks', label: 'Ranks', meta: String(f.ranks.length), go: 'fac_ranks',
            desc: 'Rank names, order and what each rank may do.' });
          list.push({ key: 'terr', label: 'Territory', meta: String(f.territories.length), go: 'fac_territory',
            enter: () => { form.terrName = ''; },
            desc: 'Claim and release settlements. Only your members with the right rank can build in them.' });
          if (can(PERM.edit))
            list.push({ key: 'banner', label: 'Banner', go: 'fac_banner', enter: () => loadBannerDraft(f.banner),
              desc: 'Choose the faction banner shown on members in the HUD.' });
          if (can(PERM.edit) || me().leader)
            list.push({ key: 'fsettings', label: 'Faction Settings', go: 'fac_settings', enter: () => { form.facRename = f.name; },
              desc: 'Rename or disband the faction.' });
          list.push({ key: 'leave', label: 'Leave Faction', run: () => fac('leave'),
            confirm: me().leader ? 'Press again to leave. Leadership passes to your highest-ranked member.' : 'Press again to leave the faction.',
            desc: 'Leave ' + f.name + '.' });
        }
        list.push({ key: 'all', label: 'All Factions', meta: String((FX().factions || []).length), go: 'fac_list',
          desc: 'Every faction in this world and the settlements they hold.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' });
        return list;
      },
    },

    fac_create: {
      title: () => 'Create Faction',
      live: true,
      banner: () => ({ id: draftBannerId(), title: clean(form.facName) || 'New Faction', sub: 'Banner preview' }),
      rows: () => bannerRejectRows(),
      notice: () => resultNotice(),
      items: () => [
        { key: 'name', label: 'Name', field: 'facName', maxlen: 24, placeholder: 'Minutemen Reborn',
          desc: '3-24 characters. Must be unique in this world.' },
      ].concat(bannerItems()).concat([
        { key: 'go', label: 'Found Faction', disabled: clean(form.facName).length < 3,
          run: () => fac('create|' + clean(form.facName) + '|' + draftBannerId()), after: 'back',
          desc: 'Create the faction with this name and banner.' },
        { key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions.' },
      ]),
    },

    fac_invitation: {
      title: () => 'Invitation',
      live: true,
      banner: () => { const f = factionById(sel.invite); return f ? { id: f.banner, title: f.name, sub: f.members.length + ' members' } : null; },
      rows: () => {
        const inv = (FX().invites || []).find(i => i.faction === sel.invite);
        return inv ? [['Faction', inv.name], ['Invited By', inv.by]] : [['Invitation', 'No longer valid', 'dim']];
      },
      items: () => [
        { key: 'accept', label: 'Accept', run: () => fac('accept|' + sel.invite), after: 'back', desc: 'Join this faction at its lowest rank.' },
        { key: 'decline', label: 'Decline', run: () => fac('decline|' + sel.invite), after: 'back', desc: 'Turn the invitation down.' },
        { key: 'back', label: 'Back', action: 'back', desc: 'Decide later.' },
      ],
    },

    fac_list: {
      title: () => 'All Factions',
      live: true,
      rows: () => {
        const fs = FX().factions || [];
        if (!fs.length) return [['Factions', 'None yet', 'dim']];
        const rows = [];
        fs.forEach(f => {
          rows.push([f.name, f.members.length + ' members · ' + f.territories.length + ' settlements', '', 'section']);
          f.territories.forEach(t => rows.push(['  ' + t.name, hex(t.id), 'dim', 'player']));
        });
        return rows;
      },
      items: () => [{ key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions.' }],
    },

    fac_members: {
      title: () => 'Members',
      live: true,
      items: () => {
        const f = myFaction();
        if (!f) return [{ key: 'back', label: 'Back', action: 'back', desc: '' }];
        const list = f.members.slice().sort((a, b) => rankPrio(f, b.rank, b.key) - rankPrio(f, a.rank, a.key)).map(m => ({
          key: 'm:' + m.key, label: (m.online ? '● ' : '○ ') + m.name + (m.key === me().key ? ' (You)' : ''),
          meta: m.key === f.leader ? 'Leader' : rankName(f, m.rank),
          go: 'fac_member', enter: () => { sel.member = m.key; sel.rankDraft.target = m.rank; },
          desc: (m.online ? 'Online.' : 'Offline.') + ' Rank: ' + rankName(f, m.rank) + '.' }));
        if (can(PERM.ban))
          list.push({ key: 'bans', label: 'Banned Players', meta: String(f.bans.length), go: 'fac_bans', desc: 'Players who cannot join until unbanned.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions.' });
        return list;
      },
    },

    fac_member: {
      title: () => 'Member',
      live: true,
      rows: () => {
        const f = myFaction();
        const m = f && f.members.find(x => x.key === sel.member);
        if (!m) return [['Member', 'No longer in the faction', 'dim']];
        return [['Name', m.name], ['Rank', m.key === f.leader ? 'Leader' : rankName(f, m.rank)], ['Status', m.online ? 'Online' : 'Offline', m.online ? '' : 'dim']];
      },
      notice: () => resultNotice(),
      items: () => {
        const f = myFaction();
        const m = f && f.members.find(x => x.key === sel.member);
        const list = [];
        if (m && m.key !== me().key) {
          const myP = rankPrio(f, me().rank, me().key);
          const outranks = rankPrio(f, m.rank, m.key) < myP;
          const choices = f.ranks.filter(r => r.id !== 1 && r.priority < myP).sort((a, b) => b.priority - a.priority);
          if (can(PERM.promote) && outranks && choices.length) {
            let i = Math.max(0, choices.findIndex(r => r.id === sel.rankDraft.target));
            const cur = choices[i] || choices[0];
            list.push({ key: 'rk', label: 'New Rank', option: cur.name,
              cycle: d => { i = wrap(i + d, choices.length); sel.rankDraft.target = choices[i].id; },
              desc: 'Choose a rank below your own, then Apply Rank.' });
            list.push({ key: 'rkgo', label: 'Apply Rank', disabled: cur.id === m.rank,
              run: () => fac('setrank|' + m.key + '|' + cur.id), desc: 'Promote or demote ' + m.name + ' to ' + cur.name + '.' });
          }
          if (can(PERM.kick) && outranks)
            list.push({ key: 'kick', label: 'Kick', run: () => fac('kick|' + m.key), after: 'back',
              confirm: 'Press again to remove ' + m.name + ' from the faction.', desc: 'Remove this member.' });
          if (can(PERM.ban) && outranks)
            list.push({ key: 'ban', label: 'Ban', run: () => fac('ban|' + m.key), after: 'back',
              confirm: 'Press again to ban ' + m.name + '. They cannot rejoin until unbanned.', desc: 'Remove this member and stop them rejoining.' });
          if (me().leader)
            list.push({ key: 'lead', label: 'Make Leader', run: () => fac('transfer|' + m.key), after: 'back',
              confirm: 'Press again to hand leadership to ' + m.name + '. You become their second-in-command.', desc: 'Transfer leadership.' });
        }
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Members.' });
        return list;
      },
    },

    fac_bans: {
      title: () => 'Banned Players',
      live: true,
      items: () => {
        const f = myFaction();
        const list = (f ? f.bans : []).map(b => ({ key: 'b:' + b.key, label: b.name, meta: 'Unban',
          run: () => fac('unban|' + b.key), confirm: 'Press again to unban ' + b.name + '.', desc: 'Allow this player to be invited again.' }));
        if (!list.length) list.push({ key: 'none', label: 'Nobody is banned', disabled: true, desc: '' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Members.' });
        return list;
      },
    },

    fac_invite: {
      title: () => 'Invite Players',
      live: true,
      notice: () => resultNotice(),
      items: () => {
        const list = (FX().online || []).filter(o => !o.you && !o.faction).map(o => ({
          key: 'i:' + o.key, label: o.name, meta: 'Invite', run: () => fac('invite|' + o.key),
          desc: 'Send ' + o.name + ' an invitation. They answer from their Factions menu.' }));
        if (!list.length) list.push({ key: 'none', label: 'No players without a faction', disabled: true,
          desc: 'Only players who are online in this game and not in a faction can be invited.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions.' });
        return list;
      },
    },

    fac_ranks: {
      title: () => 'Ranks',
      live: true,
      items: () => {
        const f = myFaction();
        const list = (f ? f.ranks.slice().sort((a, b) => b.priority - a.priority) : []).map(r => ({
          key: 'r:' + r.id, label: r.name, meta: r.id === 1 ? 'Leader' : 'Priority ' + r.priority,
          go: 'fac_rank', enter: () => { sel.rank = r.id; form.rankName = r.name; form.rankPrio = String(r.priority); sel.rankDraft.perms = r.perms; },
          desc: PERM_LIST.filter(p => r.id === 1 || (r.perms & PERM[p[0]])).map(p => p[1]).join(', ') || 'No permissions.' }));
        if (can(PERM.ranks))
          list.push({ key: 'new', label: 'New Rank', go: 'fac_rank',
            enter: () => { sel.rank = 0; form.rankName = ''; form.rankPrio = '20'; sel.rankDraft.perms = 0; },
            desc: 'Add a rank (at most 10).' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions.' });
        return list;
      },
    },

    fac_rank: {
      title: () => sel.rank ? 'Edit Rank' : 'New Rank',
      live: true,
      notice: () => resultNotice() || (sel.rank === 1 ? { head: 'Leader rank', text: 'The leader always has every permission. Only its name can change.' } : null),
      items: () => {
        const f = myFaction();
        const editable = can(PERM.ranks) && (sel.rank === 0 || (sel.rank !== 1 && rankPrio(f, sel.rank) < rankPrio(f, me().rank, me().key)));
        const list = [
          { key: 'nm', label: 'Name', field: 'rankName', maxlen: 20, placeholder: 'Knight', desc: 'Shown on members in the HUD.' },
        ];
        if (sel.rank !== 1) {
          list.push({ key: 'pr', label: 'Priority', field: 'rankPrio', maxlen: 3, placeholder: '40',
            desc: 'Higher ranks outrank lower ones. Must be below your own rank (' + rankPrio(f, me().rank, me().key) + ').' });
          PERM_LIST.forEach(p => {
            const bit = PERM[p[0]];
            const on = (sel.rankDraft.perms & bit) !== 0;
            list.push({ key: 'p:' + p[0], label: p[1], option: on ? 'Yes' : 'No', disabled: !editable || !can(bit),
              cycle: () => { sel.rankDraft.perms ^= bit; },
              desc: can(bit) ? 'Whether this rank may ' + p[1].toLowerCase() + '.' : 'You cannot grant a permission you do not have.' });
          });
        }
        const nm = clean(form.rankName), prio = parseInt(form.rankPrio, 10) || 0;
        if (sel.rank === 0)
          list.push({ key: 'save', label: 'Create Rank', disabled: !editable || nm.length < 2,
            run: () => fac('rank_add|' + nm + '|' + prio + '|' + sel.rankDraft.perms), after: 'back', desc: 'Add this rank.' });
        else
          list.push({ key: 'save', label: 'Save Rank', disabled: !(editable || (sel.rank === 1 && me().leader)) || nm.length < 2,
            run: () => fac('rank_edit|' + sel.rank + '|' + nm + '|' + prio + '|' + sel.rankDraft.perms), after: 'back', desc: 'Save the changes.' });
        if (sel.rank > 1 && editable)
          list.push({ key: 'del', label: 'Delete Rank', run: () => fac('rank_del|' + sel.rank), after: 'back',
            confirm: 'Press again to delete. Its members move to the lowest rank.', desc: 'Remove this rank.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Ranks without saving.' });
        return list;
      },
    },

    fac_territory: {
      title: () => 'Territory',
      live: true,
      rows: () => nearestRows(),
      notice: () => resultNotice() || { head: 'How territory works',
        text: 'In a settlement your faction holds, only your members whose rank allows it can build, move, scrap or repair. Everyone else is refused, by the host.' },
      items: () => {
        const f = myFaction(), n = FX().nearest || {};
        const list = [];
        if (can(PERM.territory)) {
          list.push({ key: 'tn', label: 'Name', field: 'terrName', maxlen: 32, placeholder: 'Sanctuary Hills',
            desc: 'A name for the settlement you are standing in, used when you claim it.' });
          list.push({ key: 'claim', label: 'Claim This Settlement', disabled: !n.id || !!n.owner,
            run: () => fac('claim|' + clean(form.terrName)),
            desc: !n.id ? 'Stand in a settlement (near its workbench) to claim it.'
                : (n.owner ? 'Already held by ' + n.ownerName + '.' : 'Claim ' + hex(n.id) + ' for ' + (f ? f.name : 'your faction') + '.') });
        }
        (f ? f.territories : []).forEach(t => list.push({ key: 't:' + t.id, label: t.name, meta: builtCount(t.id) + ' built',
          go: 'fac_terr', enter: () => { sel.terr = t.id; form.terrRename = t.name; },
          desc: 'Workbench ' + hex(t.id) + '. ' + builtCount(t.id) + ' objects built through CoSync.' }));
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions.' });
        return list;
      },
    },

    fac_terr: {
      title: () => 'Settlement',
      live: true,
      rows: () => {
        const f = myFaction();
        const t = f && f.territories.find(x => x.id === sel.terr);
        if (!t) return [['Settlement', 'No longer held', 'dim']];
        const lim = (FX().settings || {}).limitMode === 1 ? ' / ' + FX().settings.customLimit : '';
        return [['Name', t.name], ['Workbench', hex(t.id)], ['Objects Built', builtCount(t.id) + lim]];
      },
      notice: () => resultNotice(),
      items: () => {
        const list = [];
        if (can(PERM.territory)) {
          list.push({ key: 'rn', label: 'Name', field: 'terrRename', maxlen: 32, desc: 'Rename this settlement.' });
          list.push({ key: 'rngo', label: 'Rename', disabled: !clean(form.terrRename),
            run: () => fac('rename_terr|' + sel.terr + '|' + clean(form.terrRename)), desc: 'Save the new name.' });
          list.push({ key: 'rel', label: 'Release Settlement', run: () => fac('release|' + sel.terr), after: 'back',
            confirm: 'Press again to release it. Anyone may claim it afterwards.', desc: 'Give up control of this settlement.' });
        }
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Territory.' });
        return list;
      },
    },

    fac_banner: {
      title: () => 'Banner',
      live: true,
      banner: () => { const f = myFaction(); return { id: draftBannerId(), title: f ? f.name : '', sub: 'Preview' }; },
      rows: () => bannerRejectRows(),
      notice: () => resultNotice(),
      items: () => bannerItems().concat([
        { key: 'apply', label: 'Apply Banner', run: () => fac('banner|' + draftBannerId()), after: 'back',
          desc: 'Use this banner for the faction.' },
        { key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions without changing it.' },
      ]),
    },

    fac_settings: {
      title: () => 'Faction Settings',
      live: true,
      notice: () => resultNotice(),
      items: () => {
        const list = [];
        if (can(PERM.edit)) {
          list.push({ key: 'nm', label: 'Name', field: 'facRename', maxlen: 24, desc: '3-24 characters, unique in this world.' });
          list.push({ key: 'nmgo', label: 'Rename Faction', disabled: clean(form.facRename).length < 3,
            run: () => fac('rename|' + clean(form.facRename)), desc: 'Save the new name.' });
        }
        if (me().leader)
          list.push({ key: 'disband', label: 'Disband Faction', run: () => fac('disband'), after: 'back',
            confirm: 'Press again to disband. Members are released and its settlements become unclaimed.', desc: 'End the faction.' });
        list.push({ key: 'back', label: 'Back', action: 'back', desc: 'Return to Factions.' });
        return list;
      },
    },

    ws_settings: {
      title: () => 'Workshop Settings',
      live: true,
      rows: () => [
        ['Workshop Protection', state.workshop.hooks ? 'Active' : 'Unavailable', state.workshop.hooks ? '' : 'bad'],
        ['Current Build Limit', LIMIT_MODES[(FX().settings || {}).limitMode || 0] +
          (((FX().settings || {}).limitMode === 1) ? ' (' + FX().settings.customLimit + ')' : '')],
      ],
      notice: () => resultNotice() || { head: 'Host rules',
        text: 'These apply to everyone in your game. Removing world build restrictions lets people build anywhere, but faction territory still protects claimed settlements.' },
      items: () => [
        { key: 'mode', label: 'Build Limit', option: LIMIT_MODES[wsDraft.mode], cycle: d => { wsDraft.mode = wrap(wsDraft.mode + d, 3); },
          desc: 'Fallout 4 Default keeps the normal size bar. Custom counts objects built per settlement. Unlimited removes the limit.' },
        { key: 'lim', label: 'Custom Limit', field: 'wsLimit', maxlen: 6, placeholder: '1000', disabled: wsDraft.mode !== 1,
          desc: 'Objects that may be built in one settlement (1-100000).' },
        { key: 'free', label: 'World Build Limits', option: wsDraft.free ? 'Removed' : 'Normal',
          cycle: () => { wsDraft.free = !wsDraft.free; },
          desc: 'Removed: build outside settlement borders (objects belong to the nearest settlement). Claimed territory is still protected.' },
        { key: 'unc', label: 'Unclaimed Settlements', option: wsDraft.unclaimed ? 'Claim Required' : 'Anyone Can Build',
          cycle: () => { wsDraft.unclaimed = wsDraft.unclaimed ? 0 : 1; },
          desc: 'Whether players may build in settlements no faction holds.' },
        { key: 'apply', label: 'Apply', run: () => fac('settings|' + wsDraft.mode + '|' + (parseInt(form.wsLimit, 10) || 1000) + '|' +
            (wsDraft.free ? 1 : 0) + '|' + wsDraft.unclaimed), desc: 'Save these rules for everyone.' },
        { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' },
      ],
    },


    // 0.3.3 (Stage 8): voice settings. Proximity range is the host's rule and
    // is shown, never edited, here.
    voice: {
      title: () => 'Voice & Audio',
      live: true,
      enter: () => native('voice_refresh'),
      rows: () => {
        const v = state.voice || {};
        const inSession = role() !== 'NONE';
        return [
          ['Microphone', v.micOk ? 'Working' : (inSession ? 'Not available' : 'Starts with a game'), v.micOk ? '' : 'dim'],
          ['Talk Mode', v.ptt ? 'Push to talk' : 'Voice activation'],
          ['Voice Range', 'Set by the host', 'dim'],
        ];
      },
      items: () => {
        const v = state.voice || {};
        const pick = (list, cur) => {
          const ids = [''].concat((list || []).map(d => d.id));
          const at = Math.max(0, ids.indexOf(cur || ''));
          const d = (list || []).find(x => x.id === cur);
          return { ids, at, name: cur ? (d ? d.name : 'Unavailable device') : 'Windows Default' };
        };
        const inp = pick(v.inputs, v.input);
        const out = pick(v.outputs, v.output);
        const pct = x => Math.round((x || 0) * 100) + '%';
        const step = (x, d, lo, hi, by) => Math.min(hi, Math.max(lo, Math.round(((x || 0) + d * by) * 100) / 100));
        return [
          { key: 'vin', label: 'Microphone', option: inp.name,
            cycle: d => { const id = inp.ids[wrap(inp.at + d, inp.ids.length)]; v.input = id; native('voice|input|' + id); },
            desc: 'Which microphone to use. Press ← / → to change.' },
          { key: 'vgain', label: 'Microphone Volume', option: pct(v.gain),
            cycle: d => { v.gain = step(v.gain, d, 0, 2, 0.1); native('voice|gain|' + v.gain); },
            desc: 'How loud you are to other players.' },
          { key: 'vsens', label: 'Input Sensitivity', option: Math.round((v.sensitivity || 0) * 10) + ' / 10',
            disabled: !!v.ptt,
            cycle: d => { v.sensitivity = step(v.sensitivity, d, 0, 1, 0.1); native('voice|sensitivity|' + v.sensitivity); },
            desc: v.ptt ? 'Push to talk is on ([Voice] iPushToTalkKey), so sensitivity is not used.'
                        : 'How quiet a sound starts your microphone. Higher picks up softer speech (and more background noise).' },
          { key: 'vout', label: 'Output Device', option: out.name,
            cycle: d => { const id = out.ids[wrap(out.at + d, out.ids.length)]; v.output = id; native('voice|output|' + id); },
            desc: 'Where other players\u2019 voices play. Press ← / → to change.' },
          { key: 'vvol', label: 'Voice Volume', option: pct(v.volume),
            cycle: d => { v.volume = step(v.volume, d, 0, 2, 0.1); native('voice|volume|' + v.volume); },
            desc: 'How loud other players are.' },
          { key: 'vmute', label: 'Mute Microphone', option: v.muted ? 'On' : 'Off',
            cycle: () => { v.muted = !v.muted; native('voice|mute|' + (v.muted ? '1' : '0')); },
            desc: 'Stop sending your voice.' },
          { key: 'back', label: 'Back', action: 'back', desc: 'Return to the Multiplayer menu.' },
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
        { key: 'voice', label: 'Voice & Audio', go: 'voice', desc: 'Microphone, speakers and voice volume.' },
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
      ['Role', role() === 'HOST' ? 'Host' : (role() === 'CLIENT' ? 'Client' : 'Not in a session'), role() === 'NONE' ? 'dim' : ''],
      ['Network', steamSession() ? 'Steam (friends)' : networkLabel(), state.network.available || steamSession() ? '' : 'dim'],
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
    const bn = scr.banner ? scr.banner() : null;
    if (!rows.length && !n && !bn) rows = overviewRows();
    $('rows').innerHTML = rows.map(r =>
      '<div class="row' + (r[3] ? ' ' + r[3] : '') + '"><span class="label">' + escapeHtml(r[0]) + '</span>' +
      '<span class="lead"></span>' +
      '<span class="value ' + (r[2] || '') + '">' + escapeHtml(r[1]) + '</span></div>').join('');
    $('notice').innerHTML = n ? '<span class="head">' + escapeHtml(n.head) + '</span>' + escapeHtml(n.text) : '';
    $('title').textContent = scr.title();
    renderStatus();
    // Faction screens show their banner above the rows.
    $('bannerArea').innerHTML = bn ? window.CoSyncBanners.html(bn.id, 'banner-lg') +
      '<span class="caption">' + escapeHtml(bn.title || '') + '<span class="sub">' + escapeHtml(bn.sub || '') + '</span></span>' : '';
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
