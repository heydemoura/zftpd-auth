/* zftpd web — application shell: routing, navigation, theme, connection UI. */
(function (ZF) {
  'use strict';

  var $ = ZF.byId;
  var P = ZF.path;
  var VIEWS = ['files', 'shares', 'transfers', 'games', 'system', 'settings'];
  var TITLES = { files: 'Files', shares: 'Shares', transfers: 'Transfers', games: 'Games', system: 'System', settings: 'Settings' };
  /* Views a restricted ("user") account never sees. */
  var ADMIN_VIEWS = { shares: true, transfers: true, games: true, system: true };

  var app = $('app');
  var current = null;
  var lastHash = null;
  var navIndex = 0;
  var navMax = 0;
  var hasPushState = !!(window.history && history.pushState);

  try { navMax = parseInt(sessionStorage.getItem('zf.navmax') || '0', 10) || 0; } catch (e) { navMax = 0; }

  /* ── Routing ────────────────────────────────────────────────────────── */

  function parse(hash) {
    var h = String(hash || '').replace(/^#\/?/, '');
    var slash = h.indexOf('/');
    var name = slash < 0 ? h : h.slice(0, slash);
    var rest = slash < 0 ? null : h.slice(slash + 1);
    if (VIEWS.indexOf(name) < 0) return null;
    var params = {};
    if (name === 'files') {
      var segs = (rest || '').split('/');
      var out = [];
      for (var i = 0; i < segs.length; i++) {
        if (!segs[i]) continue;
        try { out.push(decodeURIComponent(segs[i])); } catch (e) { out.push(segs[i]); }
      }
      params.path = out.length ? P.norm('/' + out.join('/')) : ZF.auth.homePath();
    }
    return { name: name, params: params };
  }

  function setNavState() {
    var back = $('f-back'), fwd = $('f-forward');
    back.disabled = navIndex <= 0;
    fwd.disabled = navIndex >= navMax;
  }

  function viewAllowed(name) {
    return !ADMIN_VIEWS[name] || ZF.auth.isAdmin();
  }

  function route(meta) {
    var hash = location.hash || '';
    var r = parse(hash);
    if (!r || !viewAllowed(r.name)) { replace(ZF.files.hashFor(ZF.auth.homePath())); return; }
    if (hash === lastHash && !(meta && meta.force)) return;
    lastHash = hash;

    ZF.closeMenu();
    closeNav();
    if (current !== r.name) {
      if (current && ZF.views[current] && ZF.views[current].leave) ZF.views[current].leave();
      for (var i = 0; i < VIEWS.length; i++) $('view-' + VIEWS[i]).hidden = VIEWS[i] !== r.name;
      var items = ZF.$$('.nav-item');
      for (var j = 0; j < items.length; j++) {
        var on = items[j].getAttribute('data-view') === r.name;
        items[j].className = 'nav-item' + (on ? ' is-active' : '');
        if (on) items[j].setAttribute('aria-current', 'page'); else items[j].removeAttribute('aria-current');
      }
      current = r.name;
      $('mobile-title').textContent = TITLES[r.name];
      ZF.emit('view', r.name);
    }
    if (ZF.views[r.name]) ZF.views[r.name].enter(r.params, meta || {});
    updateTitle();
    setNavState();
  }

  function updateTitle() {
    var t = TITLES[current] || '';
    if (current === 'files' && ZF.files.current()) {
      var p = ZF.files.current();
      t = p === '/' ? 'Files' : P.base(p);
    }
    document.title = t + ' \u2014 zftpd';
  }
  ZF.on('files-path', updateTitle);

  function replace(hash) {
    if (hasPushState) {
      history.replaceState({ zi: navIndex }, '', hash);
      route();
    } else {
      location.replace(hash);
    }
  }

  /** Navigates to `hash`, adding a history entry. */
  ZF.go = function (hash, opts) {
    opts = opts || {};
    if (hash === location.hash) { route({ force: true, userNav: true }); return; }
    if (!hasPushState) { location.hash = hash; return; }
    if (opts.replace) {
      history.replaceState({ zi: navIndex }, '', hash);
    } else {
      navIndex++;
      navMax = navIndex;
      try { sessionStorage.setItem('zf.navmax', String(navMax)); } catch (e) { /* ignore */ }
      history.pushState({ zi: navIndex }, '', hash);
    }
    route({ userNav: true });
  };

  window.addEventListener('popstate', function (e) {
    navIndex = e.state && typeof e.state.zi === 'number' ? e.state.zi : 0;
    route({ userNav: true });
  });
  window.addEventListener('hashchange', function () { route({ userNav: true }); });

  ZF.on('open-path', function (path) { ZF.go(ZF.files.hashFor(path)); });

  /* ── Sidebar and drawer ─────────────────────────────────────────────── */

  function openNav() {
    app.className = 'app nav-open';
    $('nav-toggle').setAttribute('aria-expanded', 'true');
    var active = ZF.$('.nav-item.is-active') || ZF.$('.nav-item');
    if (active) active.focus();
  }
  function closeNav() {
    if (app.className.indexOf('nav-open') < 0) return;
    app.className = 'app';
    $('nav-toggle').setAttribute('aria-expanded', 'false');
  }
  ZF.on('nav-close', closeNav);

  $('nav-toggle').addEventListener('click', function () {
    if (app.className.indexOf('nav-open') >= 0) closeNav(); else openNav();
  });
  $('scrim').addEventListener('click', closeNav);
  document.addEventListener('keydown', function (e) {
    if ((e.key === 'Escape' || e.key === 'Esc') && app.className.indexOf('nav-open') >= 0 && !ZF.dialogOpen() && !ZF.menuOpen()) {
      closeNav();
      $('nav-toggle').focus();
    }
  });

  ZF.$('#nav').addEventListener('click', function (e) {
    var a = e.target;
    while (a && a.tagName !== 'A') a = a.parentNode;
    if (!a || e.ctrlKey || e.metaKey || e.shiftKey || e.button > 0) return;
    e.preventDefault();
    var view = a.getAttribute('data-view');
    if (view === 'files') {
      ZF.go(ZF.files.hashFor(ZF.files.current() || ZF.auth.homePath()));
    } else {
      ZF.go('#/' + view);
    }
  });

  /* Desktop sidebar collapses to an icon rail; the state is a preference. */
  var collapseBtn = $('side-collapse');

  function applySidebar() {
    var on = !!ZF.settings.sidebarCollapsed;
    var root = document.documentElement;
    var cls = root.className.replace(/\s*sidebar-collapsed/g, '');
    root.className = on ? (cls + ' sidebar-collapsed').replace(/^\s+/, '') : cls;
    var label = on ? 'Expand sidebar' : 'Collapse sidebar';
    collapseBtn.setAttribute('aria-label', label);
    collapseBtn.setAttribute('aria-expanded', on ? 'false' : 'true');
    collapseBtn.title = label + ' (Ctrl+B)';
    var items = ZF.$$('.nav-item');
    for (var i = 0; i < items.length; i++) {
      var text = ZF.$('.nav-label', items[i]).textContent;
      if (on) items[i].title = text; else items[i].removeAttribute('title');
    }
    var support = ZF.$('.side-support');
    if (on) support.title = 'Support zftpd'; else support.removeAttribute('title');
    $('conn').title = on ? $('conn-text').textContent : '';
  }

  function toggleSidebar() { ZF.setSetting('sidebarCollapsed', !ZF.settings.sidebarCollapsed); }

  collapseBtn.addEventListener('click', toggleSidebar);
  document.addEventListener('keydown', function (e) {
    if ((e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey && (e.key === 'b' || e.key === 'B') &&
        !ZF.isTyping(e.target) && !ZF.dialogOpen()) {
      e.preventDefault();
      if (window.matchMedia && window.matchMedia('(max-width: 860px)').matches) {
        if (app.className.indexOf('nav-open') >= 0) closeNav(); else openNav();
      } else {
        toggleSidebar();
      }
    }
  });

  /* ── Donation banner ────────────────────────────────────────────────── */

  var promo = $('promo');
  try { if (sessionStorage.getItem('zf.promo') === 'hidden') promo.hidden = true; } catch (e) { /* ignore */ }
  $('promo-close').addEventListener('click', function () {
    promo.hidden = true;
    try { sessionStorage.setItem('zf.promo', 'hidden'); } catch (e) { /* ignore */ }
  });

  /* ── Theme and density ──────────────────────────────────────────────── */

  var darkQuery = window.matchMedia ? window.matchMedia('(prefers-color-scheme: dark)') : null;

  function applyAppearance() {
    var t = ZF.settings.theme || 'system';
    if (t === 'system') t = darkQuery && darkQuery.matches ? 'dark' : 'light';
    var root = document.documentElement;
    root.setAttribute('data-theme', t);
    var cls = root.className.replace(/\s*density-compact/g, '');
    root.className = ZF.settings.density === 'compact' ? (cls + ' density-compact').replace(/^\s+/, '') : cls;
  }
  if (darkQuery) {
    var onScheme = function () { if ((ZF.settings.theme || 'system') === 'system') applyAppearance(); };
    if (darkQuery.addEventListener) darkQuery.addEventListener('change', onScheme);
    else if (darkQuery.addListener) darkQuery.addListener(onScheme);
  }
  ZF.on('settings', function (key) {
    if (key === 'theme' || key === 'density' || key === '*') applyAppearance();
    if (key === 'sidebarCollapsed' || key === '*') applySidebar();
  });

  /* ── Connection status ──────────────────────────────────────────────── */

  var conn = $('conn');
  var connText = $('conn-text');
  var banner = $('conn-banner');
  var bannerText = $('conn-banner-text');
  var countdown = null;

  function renderConnection(info) {
    var phase = info.phase;
    conn.className = 'conn is-' + phase;
    connText.textContent = phase === 'online' ? 'Connected' : phase === 'offline' ? 'Disconnected' : 'Connecting\u2026';
    if (ZF.settings.sidebarCollapsed) conn.title = connText.textContent;
    clearInterval(countdown);
    if (phase === 'online') { banner.hidden = true; return; }
    if (phase === 'connecting' && banner.hidden) return;
    banner.hidden = false;
    var tick = function () {
      var s = Math.max(0, Math.ceil((ZF.api.retryAt() - Date.now()) / 1000));
      bannerText.textContent = ZF.api.phase() === 'connecting' || s <= 0
        ? 'Connection to zftpd lost. Reconnecting\u2026'
        : 'Connection to zftpd lost. Retrying in ' + s + ' s. Changes are paused until the connection is back.';
    };
    tick();
    if (phase === 'offline') countdown = setInterval(tick, 500);
  }

  ZF.on('connection', renderConnection);
  $('conn-retry').addEventListener('click', function () { ZF.api.retryNow(); });

  ZF.on('reconnected', function (info) {
    if (info && info.restarted) {
      ZF.toast('zftpd was restarted. Operations that were running on the server have stopped.', { type: 'warning', timeout: 8000 });
    } else {
      ZF.toast('Connection restored', { type: 'success', timeout: 2500 });
    }
    renderInfo();
    if (current && current !== 'files' && ZF.views[current]) ZF.views[current].enter(parse(location.hash).params || {}, {});
  });

  function renderInfo() {
    var info = ZF.api.info || {};
    $('side-version').textContent = info.version ? 'v' + info.version : '';
    $('brand-host').textContent = location.hostname || 'localhost';
  }
  ZF.on('status', renderInfo);

  /* ── Login gate ─────────────────────────────────────────────────────── */

  /* Hide what the signed-in account cannot use, and leave a view that just
   * became off-limits (role change, sign-out) for the Files view. */
  function applyRole() {
    var items = ZF.$$('.nav-item');
    for (var i = 0; i < items.length; i++) {
      var name = items[i].getAttribute('data-view');
      items[i].hidden = !viewAllowed(name);
    }
    if (current && !viewAllowed(current)) {
      replace(ZF.files.hashFor(ZF.auth.homePath()));
    } else if (current === 'files' && ZF.files.current() !== null &&
               !ZF.auth.pathAllowed(ZF.files.current())) {
      replace(ZF.files.hashFor(ZF.auth.homePath()));
    } else if (ZF.auth.loggedIn() && current && ZF.views[current] && ZF.views[current].enter) {
      /* Signed in: the views skipped their first load while the gate was up. */
      route({ force: true });
    }
    if (ZF.auth.loggedIn()) ZF.transfers.refresh();
  }
  ZF.on('auth', applyRole);

  /* ── Transfers badge ────────────────────────────────────────────────── */

  var badge = $('nav-badge');
  function renderBadge() {
    var n = ZF.jobs.activeCount() + (ZF.transfers ? ZF.transfers.activeCount() : 0);
    badge.hidden = !n;
    badge.textContent = n ? String(n) : '';
    badge.setAttribute('aria-label', n ? n + ' active' : '');
  }
  ZF.on('jobs', renderBadge);
  ZF.on('badge', renderBadge);

  /* ── Start ──────────────────────────────────────────────────────────── */

  applyAppearance();
  applySidebar();
  renderInfo();
  if (hasPushState) {
    var st = history.state;
    navIndex = st && typeof st.zi === 'number' ? st.zi : 0;
    if (!st) history.replaceState({ zi: 0 }, '', location.hash || ZF.files.hashFor(ZF.settings.startPath || '/'));
  }
  ZF.api.start();
  route();
})(ZF);
