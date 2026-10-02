/* zftpd web — login gate: session state, sign-in screen, account chip.
 *
 * The daemon reports the gate in /api/status (auth.enabled, authenticated,
 * user, role).  When a login is required the sign-in screen covers the app
 * until a session cookie is set; every later 401 brings it back.
 */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var $ = ZF.byId;
  var api = ZF.api;

  var state = {
    known: false,         /* status seen at least once */
    enabled: false,
    authenticated: false,
    user: '',
    role: 'admin',
    folders: []           /* allowed folders for the user role */
  };

  var auth = {};
  auth.state = state;
  auth.enabled = function () { return state.enabled; };
  auth.loggedIn = function () { return !state.enabled || state.authenticated; };
  auth.isAdmin = function () { return !state.enabled || (state.authenticated && state.role === 'admin'); };
  auth.isUser = function () { return state.enabled && state.authenticated && state.role === 'user'; };
  auth.canShare = function () { return auth.isAdmin(); };
  auth.user = function () { return state.user; };
  auth.role = function () { return state.enabled ? state.role : 'admin'; };
  auth.folders = function () { return state.folders.slice(); };

  /** Where a freshly signed-in account should land. */
  auth.homePath = function () {
    var start = ZF.settings.startPath || '/';
    if (auth.pathAllowed(start)) return start;
    if (auth.isUser()) return state.folders.length ? state.folders[0] : ZF.rootPath();
    return ZF.rootPath();
  };

  /** Mirrors the daemon rules: everything lives inside the served root, and a
   *  restricted account only sees its allowed folders. */
  auth.pathAllowed = function (path) {
    if (!ZF.path.within(path, ZF.rootPath())) return false;
    if (!auth.isUser()) return true;
    for (var i = 0; i < state.folders.length; i++) {
      if (ZF.path.within(path, state.folders[i])) return true;
    }
    return false;
  };

  /* ── Sign-in screen ─────────────────────────────────────────────────── */

  var dom = {
    screen: $('login'),
    form: $('login-form'),
    user: $('login-user'),
    pass: $('login-pass'),
    error: $('login-error'),
    submit: $('login-submit'),
    sub: $('login-sub')
  };
  var busy = false;

  function showLogin() {
    if (!dom.screen.hidden) return;
    dom.error.textContent = '';
    dom.pass.value = '';
    dom.screen.hidden = false;
    document.documentElement.className += ' has-login';
    ZF.closeMenu();
    setTimeout(function () { (dom.user.value ? dom.pass : dom.user).focus(); }, 0);
  }

  function hideLogin() {
    if (dom.screen.hidden) return;
    dom.screen.hidden = true;
    document.documentElement.className = document.documentElement.className.replace(/\s*has-login/g, '');
  }

  dom.form.addEventListener('submit', function (e) {
    e.preventDefault();
    if (busy) return;
    var login = dom.user.value.replace(/^\s+|\s+$/g, '');
    var pass = dom.pass.value;
    if (!login || !pass) {
      dom.error.textContent = 'Enter your login and password.';
      (login ? dom.pass : dom.user).focus();
      return;
    }
    busy = true;
    dom.submit.disabled = true;
    dom.error.textContent = '';
    api.login(login, pass).then(function (res) {
      busy = false;
      dom.submit.disabled = false;
      dom.pass.value = '';
      applyStatus({ enabled: res.enabled !== false, authenticated: true, user: res.user || login, role: res.role || 'user' });
      ZF.toast('Signed in as ' + state.user, { type: 'success', timeout: 2500 });
    }, function (err) {
      busy = false;
      dom.submit.disabled = false;
      dom.error.textContent = err && err.status === 401 ? 'Invalid login or password.' :
        (err && err.message) || 'Could not sign in.';
      dom.pass.focus();
      dom.pass.select();
    });
  });

  /* ── Account chip (sidebar) ─────────────────────────────────────────── */

  var chip = {
    box: $('side-user'),
    avatar: $('side-user-avatar'),
    name: $('side-user-name'),
    role: $('side-user-role'),
    logout: $('side-logout')
  };

  function renderChip() {
    var show = state.enabled && state.authenticated;
    chip.box.hidden = !show;
    if (!show) return;
    chip.avatar.textContent = (state.user || '?').charAt(0).toUpperCase();
    chip.name.textContent = state.user;
    chip.name.title = state.user;
    chip.role.textContent = state.role === 'admin' ? 'Administrator' : 'User';
  }

  chip.logout.addEventListener('click', function () {
    api.logout().then(function () {
      ZF.toast('Signed out', { timeout: 2000 });
      applyStatus({ enabled: true, authenticated: false, user: '', role: 'none' });
    }, function (err) {
      /* The cookie may already be gone: treat it as signed out either way. */
      if (err && err.status === 401) applyStatus({ enabled: true, authenticated: false, user: '', role: 'none' });
      else ZF.toastError(err);
    });
  });

  /* ── State changes ──────────────────────────────────────────────────── */

  /* Fetches the allow-list; `announce` emits 'auth' once it is known so a
   * restricted account never starts in a folder it cannot open. */
  function loadFolders(announce) {
    if (!auth.loggedIn()) return;
    api.me().then(function (me) {
      var folders = (me && me.folders) || [];
      var changed = folders.join('\n') !== state.folders.join('\n');
      state.folders = folders;
      if (changed || announce) ZF.emit('auth', state);
    }, function () {
      if (announce) ZF.emit('auth', state);
    });
  }

  function applyStatus(a) {
    if (!a) return;
    var before = [state.enabled, state.authenticated, state.user, state.role].join('|');
    state.enabled = a.enabled === true;
    state.authenticated = state.enabled && a.authenticated === true;
    state.user = state.authenticated ? String(a.user || '') : '';
    state.role = state.enabled ? (state.authenticated ? String(a.role || 'user') : 'none') : 'admin';
    var first = !state.known;
    state.known = true;
    var after = [state.enabled, state.authenticated, state.user, state.role].join('|');

    if (state.enabled && !state.authenticated) showLogin(); else hideLogin();
    renderChip();
    if (first || before !== after) {
      if (!state.authenticated) state.folders = [];
      if (state.authenticated && state.role === 'user') {
        loadFolders(true);
      } else {
        ZF.emit('auth', state);
        loadFolders(false);
      }
    }
  }

  ZF.on('auth-status', applyStatus);
  ZF.on('auth-required', function () {
    if (!state.enabled || !state.authenticated) {
      if (state.enabled) showLogin();
      return;
    }
    ZF.toast('Your session has ended. Sign in again.', { type: 'warning', timeout: 5000 });
    applyStatus({ enabled: true, authenticated: false, user: '', role: 'none' });
  });
  /* The allow-list can change while a user is signed in: refresh it when
   * the Files view is entered so navigation keeps matching the daemon. */
  ZF.on('view', function (name) { if (name === 'files' && auth.isUser()) loadFolders(); });

  ZF.auth = auth;
})(ZF);
