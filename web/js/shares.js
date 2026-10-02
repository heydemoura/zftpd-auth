/* zftpd web — Shares view and the share dialog used from Files.
 *
 * A share is a public link (/s/<id>) to a file (direct download) or a folder
 * (browsable listing with per-file downloads and a ZIP of the folder).
 * Only administrators create and manage shares.
 */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var $ = ZF.byId;
  var api = ZF.api;

  var dom = {
    view: $('view-shares'),
    list: $('shares-list'),
    meta: $('shares-meta'),
    create: $('shares-new'),
    purge: $('shares-purge'),
    refresh: $('shares-refresh')
  };

  var shares = [];
  var serverNow = 0;
  var loaded = false;
  var loadError = null;
  var seq = 0;

  var TTL_OPTIONS = [
    { value: '0', label: 'Never expires' },
    { value: '3600', label: '1 hour' },
    { value: '86400', label: '1 day' },
    { value: '604800', label: '7 days' },
    { value: '2592000', label: '30 days' },
    { value: 'custom', label: 'Custom date and time…' }
  ];

  /* ── Formatting ─────────────────────────────────────────────────────── */

  function pad(n) { return (n < 10 ? '0' : '') + n; }

  function formatDate(unix) {
    if (!unix) return '';
    var d = new Date(unix * 1000);
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + ' ' +
      pad(d.getHours()) + ':' + pad(d.getMinutes());
  }

  function expiryText(s) {
    if (!s.expires) return 'Never';
    if (s.expired) return 'Expired ' + formatDate(s.expires);
    var left = s.expires - (serverNow || Math.round(Date.now() / 1000));
    return formatDate(s.expires) + ' (in ' + ZF.duration(left) + ')';
  }

  /* Reads a datetime-local value (or a typed "YYYY-MM-DD HH:MM") as unix seconds. */
  function parseLocal(text) {
    var m = /^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})/.exec(String(text || '').replace(/^\s+|\s+$/g, ''));
    if (!m) return 0;
    var d = new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], 0, 0);
    return isNaN(d.getTime()) ? 0 : Math.round(d.getTime() / 1000);
  }

  function defaultCustom() {
    var d = new Date(Date.now() + 7 * 86400000);
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) + 'T' + pad(d.getHours()) + ':' + pad(d.getMinutes());
  }

  /* ── Link dialog ────────────────────────────────────────────────────── */

  function showLink(share) {
    var url = api.shareUrl(share);
    var input = el('input', { class: 'input mono share-link-input', type: 'text', value: url, readonly: true, 'aria-label': 'Share link' });
    var copyBtn = ZF.button({ icon: 'copy', label: 'Copy link', kind: 'primary', onclick: function () {
      ZF.copyText(url).then(function () { ZF.toast('Link copied', { type: 'success', timeout: 2000 }); },
        function () { input.focus(); input.select(); ZF.toast('Select the link and copy it manually', { type: 'warning' }); });
    } });
    var content = el('div', null, [
      el('p', null, [
        el('span', { text: (share.type === 'directory' ? 'Folder ' : 'File ') }),
        el('strong', { text: share.name }),
        el('span', { text: share.type === 'directory'
          ? ' can be browsed and downloaded by anyone who has this link.'
          : ' downloads directly for anyone who has this link.' })
      ]),
      el('div', { class: 'share-link' }, [input, copyBtn]),
      el('div', { class: 'field-hint', text: share.expires ? 'Expires ' + formatDate(share.expires) + '.' : 'This link never expires. Remove it from Shares when it is no longer needed.' })
    ]);
    ZF.dialog({
      title: 'Share link ready',
      size: 'md',
      content: content,
      actions: [
        { id: 'open', label: 'Open link' },
        { id: 'close', label: 'Done', kind: 'primary', submit: true }
      ],
      onOpen: function () { input.focus(); input.select(); },
      onAction: function (id) {
        if (id === 'open') { window.open(url, '_blank', 'noopener'); return false; }
        return true;
      }
    });
  }

  /* ── Create dialog ──────────────────────────────────────────────────── */

  /** ZF.shares.create(path, isDir) → Promise<share | null> */
  function create(path, isDir) {
    if (!ZF.auth.canShare()) {
      ZF.toast('Only administrators can share files', { type: 'error' });
      return Promise.resolve(null);
    }
    var select = el('select', { class: 'input', id: 'share-ttl', 'aria-label': 'Expiry' });
    for (var i = 0; i < TTL_OPTIONS.length; i++) {
      select.appendChild(el('option', { value: TTL_OPTIONS[i].value, text: TTL_OPTIONS[i].label }));
    }
    var custom = el('input', { class: 'input', id: 'share-custom', type: 'datetime-local', value: defaultCustom(), 'aria-label': 'Expiry date and time' });
    var customField = el('div', { class: 'field', hidden: true }, [
      el('label', { class: 'field-label', for: 'share-custom', text: 'Expires on' }),
      custom,
      el('div', { class: 'field-hint', text: 'Local time. Format: YYYY-MM-DD HH:MM.' })
    ]);
    var error = el('div', { class: 'field-error', role: 'alert' });
    select.addEventListener('change', function () { customField.hidden = select.value !== 'custom'; });

    var content = el('div', null, [
      el('p', null, [
        el('span', { text: (isDir ? 'Share folder ' : 'Share file ') }),
        el('strong', { text: ZF.path.base(path) })
      ]),
      el('div', { class: 'field-hint share-path', text: path }),
      el('div', { class: 'field' }, [
        el('label', { class: 'field-label', for: 'share-ttl', text: 'Link validity' }),
        select
      ]),
      customField,
      error,
      el('div', { class: 'note' }, [
        ZF.icon('info'),
        el('span', { text: isDir
          ? 'Anyone with the link can browse this folder, download its files and fetch it as a ZIP. No login is needed.'
          : 'Anyone with the link can download this file. No login is needed.' })
      ])
    ]);

    var created = null;
    return ZF.dialog({
      title: 'Create share link',
      size: 'md',
      content: content,
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'ok', label: 'Create link', kind: 'primary', submit: true }
      ],
      onAction: function (id) {
        if (id !== 'ok') return true;
        var opts = {};
        if (select.value === 'custom') {
          var when = parseLocal(custom.value);
          if (!when || when <= Math.round(Date.now() / 1000) + 60) {
            error.textContent = 'Choose a date and time in the future.';
            custom.focus();
            return false;
          }
          opts.expires = when;
        } else if (select.value !== '0') {
          opts.ttl = parseInt(select.value, 10);
        }
        error.textContent = '';
        return api.shareCreate(path, opts).then(function (res) {
          created = res && res.share;
          return true;
        }, function (err) {
          error.textContent = (err && err.message) || 'Could not create the share.';
          return false;
        });
      }
    }).promise.then(function (id) {
      if (id !== 'ok' || !created) return null;
      ZF.toast('Share link created', { type: 'success', timeout: 2500 });
      if (!dom.view.hidden) load();
      showLink(created);
      return created;
    });
  }

  /* ── List ───────────────────────────────────────────────────────────── */

  function remove(s) {
    ZF.confirm({
      title: 'Remove share link?',
      message: 'The link to ' + s.name + ' stops working immediately. The file or folder itself is not touched.',
      confirmLabel: 'Remove link',
      danger: true
    }).then(function (ok) {
      if (!ok) return;
      api.shareDelete(s.id).then(function () {
        ZF.toast('Share link removed', { type: 'success', timeout: 2000 });
        load();
      }, ZF.toastError);
    });
  }

  function copyLink(s, button) {
    var url = api.shareUrl(s);
    ZF.copyText(url).then(function () { ZF.toast('Link copied', { type: 'success', timeout: 2000 }); },
      function () { ZF.prompt({ title: 'Share link', label: 'Link', value: url, confirmLabel: 'Done' }); });
    if (button) button.blur();
  }

  function row(s) {
    var type = ZF.fileType(s.name, s.type === 'directory');
    var copyBtn = ZF.button({ icon: 'copy', aria: 'Copy link', title: 'Copy link', kind: 'ghost', size: 'sm' });
    copyBtn.addEventListener('click', function () { copyLink(s, copyBtn); });
    var actions = el('div', { class: 'row-actions' }, [
      copyBtn,
      ZF.button({ icon: 'external', aria: 'Open link', title: 'Open link', kind: 'ghost', size: 'sm',
        onclick: function () { window.open(api.shareUrl(s), '_blank', 'noopener'); } }),
      ZF.button({ icon: 'folder', aria: 'Show in Files', title: 'Show in Files', kind: 'ghost', size: 'sm',
        onclick: function () { ZF.files.reveal(s.path); } }),
      ZF.button({ icon: 'trash', aria: 'Remove link', title: 'Remove link', kind: 'ghost', size: 'sm', cls: 'btn-danger-text',
        onclick: function () { remove(s); } })
    ]);
    return el('tr', { class: s.expired ? 'is-expired' : null }, [
      el('td', { class: 'col-name' }, el('div', { class: 'share-name' }, [
        ZF.icon(type.icon, 'ic-' + type.kind),
        el('div', { class: 'share-text' }, [
          el('div', { class: 'share-title', text: s.name, title: s.name }),
          el('div', { class: 'share-path', text: s.path, title: s.path })
        ])
      ])),
      el('td', { text: s.type === 'directory' ? 'Folder' : 'File' }),
      el('td', { class: s.expired ? 'text-warning' : null, text: expiryText(s) }),
      el('td', { text: formatDate(s.created) }),
      el('td', { class: 'col-actions' }, actions)
    ]);
  }

  function render() {
    ZF.clear(dom.list);
    var expired = 0;
    for (var i = 0; i < shares.length; i++) if (shares[i].expired) expired++;
    dom.purge.hidden = !expired;
    dom.meta.textContent = loaded ? (shares.length ? ZF.plural(shares.length, 'link') + (expired ? ' · ' + expired + ' expired' : '') : '') : '';

    if (loadError) {
      dom.list.appendChild(el('div', { class: 'list-state' }, [
        el('div', { class: 'list-state-icon is-error' }, ZF.icon('alert', 'ic-xl')),
        el('div', { class: 'list-state-title', text: 'Could not load shares' }),
        el('div', { class: 'list-state-desc', text: loadError }),
        el('div', { class: 'list-state-actions' }, ZF.button({ label: 'Try again', icon: 'refresh', onclick: load }))
      ]));
      return;
    }
    if (!loaded) {
      dom.list.appendChild(el('div', { class: 'empty-inline', text: 'Loading…' }));
      return;
    }
    if (!shares.length) {
      dom.list.appendChild(el('div', { class: 'list-state' }, [
        el('div', { class: 'list-state-icon' }, ZF.icon('link', 'ic-xl')),
        el('div', { class: 'list-state-title', text: 'No share links yet' }),
        el('div', { class: 'list-state-desc', text: 'Select a file or folder in Files and choose “Share…”, or share a folder from here.' }),
        el('div', { class: 'list-state-actions' }, ZF.button({ label: 'Share a folder', icon: 'link', kind: 'primary', onclick: shareFolder }))
      ]));
      return;
    }
    var tbody = el('tbody');
    var sorted = shares.slice().sort(function (a, b) { return (b.created || 0) - (a.created || 0); });
    for (var j = 0; j < sorted.length; j++) tbody.appendChild(row(sorted[j]));
    dom.list.appendChild(el('div', { class: 'table-scroll' }, el('table', { class: 'table shares-table' }, [
      el('colgroup', null, [
        el('col'), el('col', { style: 'width:80px' }), el('col', { style: 'width:220px' }),
        el('col', { style: 'width:140px' }), el('col', { style: 'width:150px' })
      ]),
      el('thead', null, el('tr', null, [
        el('th', { text: 'Name' }), el('th', { text: 'Type' }), el('th', { text: 'Expires' }),
        el('th', { text: 'Created' }), el('th', null, el('span', { class: 'sr-only', text: 'Actions' }))
      ])),
      tbody
    ])));
  }

  function load() {
    if (!ZF.auth.isAdmin()) { shares = []; loaded = true; loadError = null; render(); return; }
    var my = ++seq;
    api.shares().then(function (res) {
      if (my !== seq) return;
      shares = (res && res.shares) || [];
      serverNow = (res && res.now) || 0;
      loaded = true;
      loadError = null;
      render();
    }, function (err) {
      if (my !== seq) return;
      loaded = true;
      loadError = err.message;
      render();
    });
  }

  function shareFolder() {
    ZF.pickFolder({ title: 'Share a folder', start: ZF.files.current() || '/', confirmLabel: 'Choose this folder' })
      .then(function (p) { if (p) create(p, true); });
  }

  function purge() {
    api.sharePurge().then(function (res) {
      ZF.toast(ZF.plural((res && res.removed) || 0, 'expired link') + ' removed', { type: 'success', timeout: 2500 });
      load();
    }, ZF.toastError);
  }

  dom.create.addEventListener('click', shareFolder);
  dom.purge.addEventListener('click', purge);
  dom.refresh.addEventListener('click', load);
  ZF.on('reconnected', function () { if (!dom.view.hidden) load(); });
  ZF.on('auth', function () { if (!dom.view.hidden) load(); });

  ZF.views.shares = {
    title: 'Shares',
    enter: function () { load(); },
    leave: function () {}
  };

  ZF.shares = { create: create, showLink: showLink, reload: load };
})(ZF);
