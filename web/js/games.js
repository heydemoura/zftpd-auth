/* zftpd web — Games view. */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var api = ZF.api;
  var $ = ZF.byId;

  var dom = {
    sub: $('games-sub'),
    filter: $('games-filter'),
    repairAll: $('games-repair-all'),
    refresh: $('games-refresh'),
    install: $('games-install'),
    list: $('games-list')
  };

  var games = [];
  var loaded = false;
  var error = null;
  var seq = 0;
  var installTimer = null;

  function load() {
    var my = ++seq;
    if (!loaded) showMessage('Loading titles\u2026');
    return api.games().then(function (res) {
      if (my !== seq) return;
      games = (res.entries || []).slice().sort(function (a, b) { return ZF.naturalCompare(a.name || a.id, b.name || b.id); });
      loaded = true;
      error = null;
      render();
    }, function (err) {
      if (my !== seq) return;
      error = err;
      loaded = true;
      render();
    });
  }

  function showMessage(text, isError) {
    dom.list.className = 'card';
    ZF.clear(dom.list).appendChild(el('div', { class: 'empty-inline' + (isError ? ' text-danger' : ''), text: text }));
  }

  function stateIcon(name, isError) {
    return el('span', { class: 'list-state-icon' + (isError ? ' is-error' : '') }, ZF.icon(name, 'ic-xl'));
  }

  function render() {
    dom.repairAll.disabled = !!error || !games.length;
    if (error) {
      dom.sub.textContent = '';
      dom.list.className = 'card';
      ZF.clear(dom.list).appendChild(el('div', { class: 'list-state' }, [
        stateIcon('alert', true),
        el('div', { class: 'list-state-title', text: 'Could not load installed titles' }),
        el('p', { class: 'list-state-desc', text: error.status === 404 ? 'This build of zftpd does not include game management.' : error.message }),
        el('div', { class: 'list-state-actions' }, ZF.button({ label: 'Try again', icon: 'refresh', onclick: load }))
      ]));
      return;
    }
    dom.sub.textContent = games.length ? ZF.plural(games.length, 'title') : '';

    var q = dom.filter.value.trim().toLowerCase();
    var list = [];
    for (var i = 0; i < games.length; i++) {
      var g = games[i];
      if (!q || (g.name || '').toLowerCase().indexOf(q) >= 0 || (g.id || '').toLowerCase().indexOf(q) >= 0) list.push(g);
    }
    ZF.clear(dom.list);
    if (!games.length) {
      dom.list.className = 'card';
      dom.list.appendChild(el('div', { class: 'list-state' }, [
        stateIcon('gamepad'),
        el('div', { class: 'list-state-title', text: 'No titles found' }),
        el('p', { class: 'list-state-desc', text: 'Installed games and apps appear here. If a title is missing from the home screen, try Repair all.' })
      ]));
      return;
    }
    if (!list.length) {
      showMessage('No titles match \u201c' + dom.filter.value.trim() + '\u201d.');
      return;
    }
    dom.list.className = 'games-grid';
    for (var j = 0; j < list.length; j++) dom.list.appendChild(tile(list[j]));
  }

  function initials(name) {
    var words = String(name || '').replace(/[^A-Za-z0-9 ]+/g, ' ').split(/\s+/).filter(Boolean);
    if (!words.length) return '?';
    if (words.length === 1) return words[0].slice(0, 2).toUpperCase();
    return (words[0].charAt(0) + words[1].charAt(0)).toUpperCase();
  }

  function coverTone(id) {
    var h = 0;
    for (var i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) | 0;
    return 'cover-' + (Math.abs(h) % 6);
  }

  function fallbackCover(cover, g) {
    ZF.clear(cover);
    cover.className = 'game-cover ' + coverTone(g.id || g.name || '');
    cover.appendChild(el('span', { class: 'game-initials', 'aria-hidden': 'true', text: initials(g.name || g.id) }));
  }

  function tile(g) {
    var title = g.name || g.id;
    var cover = el('div', { class: 'game-cover' });
    if (g.has_icon) {
      var img = el('img', { src: api.gameIconUrl(g), alt: '', loading: 'lazy' });
      img.addEventListener('error', function () { fallbackCover(cover, g); });
      cover.appendChild(img);
    } else {
      fallbackCover(cover, g);
    }
    var launch = ZF.button({ icon: 'play', label: 'Launch', size: 'sm', cls: 'game-launch' });
    launch.addEventListener('click', function () { doLaunch(g, launch); });
    var more = ZF.button({ icon: 'more', aria: 'More actions for ' + title, title: 'More actions', size: 'sm' });
    more.addEventListener('click', function () {
      ZF.menu([
        { label: 'Repair home screen entry', icon: 'wrench', onclick: function () { repair(g); } },
        g.path ? { label: 'Show in Files', icon: 'folder', onclick: function () { ZF.go(ZF.files.hashFor(g.path)); } } : null,
        { label: 'Copy title ID', icon: 'copy', onclick: function () {
          ZF.copyText(g.id).then(function () { ZF.toast('Title ID copied', { type: 'success', timeout: 2000 }); });
        } },
        '-',
        { label: 'Uninstall\u2026', icon: 'trash', danger: true, onclick: function () { uninstall(g); } }
      ], more);
    });
    return el('div', { class: 'game-tile' }, [
      el('div', { class: 'game-cover-wrap' }, [
        cover,
        g.source ? el('span', { class: 'game-src', text: g.source, title: g.path || g.source }) : null
      ]),
      el('div', { class: 'game-info' }, [
        el('div', { class: 'game-name', text: title, title: title }),
        el('div', { class: 'game-id', text: g.id })
      ]),
      el('div', { class: 'game-actions' }, [launch, more])
    ]);
  }

  function doLaunch(g, btn) {
    btn.disabled = true;
    api.gameLaunch(g.id).then(function () {
      ZF.toast('Launching ' + (g.name || g.id) + ' on the console', { type: 'success' });
    }, function (err) {
      if (err.code === -30) {
        ZF.dialog({
          title: 'Title not registered',
          content: el('div', null, [
            el('p', { text: (g.name || g.id) + ' is not registered on this console\u2019s home screen, so it cannot be launched.' }),
            el('p', { class: 'muted', text: 'Repairing rebuilds the home screen entry from the installed files. It is safe to run.' })
          ]),
          actions: [{ id: 'cancel', label: 'Cancel' }, { id: 'repair', label: 'Repair and launch', kind: 'primary', submit: true }],
          onAction: function (id) {
            if (id !== 'repair') return true;
            return api.gameRepair(g.id).then(function () { return api.gameLaunch(g.id); }).then(function () {
              ZF.toast('Launching ' + (g.name || g.id), { type: 'success' });
            });
          }
        });
      } else {
        ZF.toastError(err, 'Could not launch ' + (g.name || g.id));
      }
    }).then(function () { btn.disabled = false; });
  }

  function repairMessage(res) {
    var rows = res && res.sqlite_repair && typeof res.sqlite_repair.rows === 'number' ? res.sqlite_repair.rows : null;
    if (rows === null) return 'Home screen entries checked';
    return rows ? 'Repaired ' + ZF.plural(rows, 'home screen entry', 'home screen entries') : 'Home screen entries are already up to date';
  }

  function repair(g) {
    api.gameRepair(g.id).then(function (res) {
      ZF.toast(repairMessage(res), { type: 'success' });
    }, function (err) { ZF.toastError(err, 'Repair failed'); });
  }

  dom.repairAll.addEventListener('click', function () {
    dom.repairAll.disabled = true;
    api.gameRepair().then(function (res) {
      ZF.toast(repairMessage(res), { type: 'success' });
      return load();
    }, function (err) { ZF.toastError(err, 'Repair failed'); }).then(function () { dom.repairAll.disabled = !games.length; });
  });

  function uninstall(g) {
    ZF.dialog({
      title: 'Uninstall \u201c' + (g.name || g.id) + '\u201d?',
      content: el('div', null, [
        el('p', { text: 'The title is removed from this console. This cannot be undone.' }),
        el('p', { class: 'muted mono', text: g.id + (g.path ? ' \u00b7 ' + g.path : '') })
      ]),
      actions: [{ id: 'cancel', label: 'Cancel' }, { id: 'ok', label: 'Uninstall', kind: 'danger', submit: true }],
      onOpen: function (d) { d.buttons.cancel.focus(); },
      onAction: function (id) {
        if (id !== 'ok') return true;
        return api.gameUninstall(g.id).then(function () {
          ZF.toast((g.name || g.id) + ' uninstalled', { type: 'success' });
          load();
        });
      }
    });
  }

  /* Package installation progress (only when the build supports it). */
  function checkInstall() {
    clearTimeout(installTimer);
    api.installStatus().then(function (st) {
      if (!st || !st.active) { dom.install.hidden = true; return; }
      var pct = Math.max(0, Math.min(100, st.progress || 0));
      ZF.clear(dom.install);
      dom.install.appendChild(el('div', { class: 'xfer-top' }, [
        el('div', { class: 'xfer-name', text: 'Installing ' + (st.title_id || 'package') }),
        el('span', { class: 'status status-active', text: Math.round(pct) + '%' })
      ]));
      dom.install.appendChild(el('div', { class: 'progress' }, el('span', { style: 'width:' + pct + '%' })));
      dom.install.hidden = false;
      installTimer = setTimeout(checkInstall, 1500);
    }, function () { dom.install.hidden = true; });
  }

  dom.filter.addEventListener('input', ZF.debounce(render, 80));
  dom.filter.addEventListener('keydown', function (e) {
    if ((e.key === 'Escape' || e.key === 'Esc') && dom.filter.value) { e.preventDefault(); dom.filter.value = ''; render(); }
  });
  dom.refresh.addEventListener('click', load);

  ZF.views.games = {
    title: 'Games',
    enter: function () { load(); checkInstall(); },
    leave: function () { clearTimeout(installTimer); }
  };
})(ZF);
