/* zftpd web — Settings view. Preferences live in localStorage. */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var body = ZF.byId('settings-body');

  function section(title, rows) {
    return el('section', { class: 'section' }, [
      el('div', { class: 'section-head' }, el('h2', { class: 'section-title', text: title })),
      el('div', { class: 'card settings-card' }, rows)
    ]);
  }

  function setting(title, desc, control, descClass) {
    var id = 'set-' + title.toLowerCase().replace(/[^a-z0-9]+/g, '-');
    var node = el('div', { class: 'setting' }, [
      el('div', { class: 'setting-text' }, [
        el('div', { class: 'setting-title', id: id, text: title }),
        desc ? el('div', { class: 'setting-desc' + (descClass ? ' ' + descClass : ''), text: desc }) : null
      ]),
      el('div', { class: 'setting-control' }, control)
    ]);
    if (control && control.className === 'seg') control.setAttribute('aria-labelledby', id);
    return node;
  }

  function segmented(key, options) {
    var box = el('div', { class: 'seg', role: 'group' });
    for (var i = 0; i < options.length; i++) {
      (function (o) {
        var on = ZF.settings[key] === o.value;
        box.appendChild(el('button', {
          type: 'button', class: on ? 'is-active' : '', text: o.label, 'aria-pressed': on ? 'true' : 'false',
          onclick: function () { ZF.setSetting(key, o.value); render(); }
        }));
      })(options[i]);
    }
    return box;
  }

  function toggle(key, label) {
    var input = el('input', {
      type: 'checkbox', checked: !!ZF.settings[key], 'aria-label': label,
      onchange: function () { ZF.setSetting(key, input.checked); }
    });
    return el('label', { class: 'switch' }, [input, el('span')]);
  }

  function pathButton(key, title) {
    var b = el('button', { type: 'button', class: 'btn path-btn setting-path', title: 'Change folder' }, [
      ZF.icon('folder'),
      el('span', { class: 'truncate', text: ZF.settings[key] || '/' })
    ]);
    b.addEventListener('click', function () {
      ZF.pickFolder({ title: title, start: ZF.settings[key] || '/', confirmLabel: 'Use this folder' }).then(function (p) {
        if (p) { ZF.setSetting(key, p); render(); }
      });
    });
    return b;
  }

  /* ── DNS filter ─────────────────────────────────────────────────────────
   * The filter runs in the daemon, so this switch mirrors /api/dns/status
   * instead of a local preference, and every flip is a POST that the payload
   * remembers across reinjections.
   */

  var dnsState = { known: false, running: false, enabled: true, resolver: false, masks: 0, exceptions: 0 };

  function loadDns() {
    fetch('/api/dns/status', { credentials: 'same-origin' }).then(function (r) {
      return r.json();
    }).then(function (j) {
      if (!j || j.ok !== true) return;
      var changed = !dnsState.known || dnsState.running !== !!j.running ||
        dnsState.enabled !== !!j.enabled || dnsState.resolver !== !!j.resolver;
      dnsState = {
        known: true, running: !!j.running, enabled: !!j.enabled, resolver: !!j.resolver,
        masks: j.masks || 0, exceptions: j.exceptions || 0
      };
      if (changed) render();
    }).catch(function () { /* Console unreachable: keep the last known state. */ });
  }

  function setDns(enable) {
    fetch(enable ? '/api/dns/enable' : '/api/dns/disable', {
      method: 'POST', credentials: 'same-origin',
      headers: { 'X-CSRF-Token': ZF.api.csrf() }
    }).then(function (r) {
      return r.json();
    }).then(function (j) {
      if (!j) return;
      ZF.toast(j.message || (enable ? 'DNS filter enabled' : 'DNS filter disabled'),
        { type: j.ok ? 'success' : 'error' });
      loadDns();
    }).catch(function () {
      ZF.toast('Could not reach the console', { type: 'error' });
      loadDns();
    });
  }

  function dnsSwitch() {
    var input = el('input', {
      type: 'checkbox', checked: !!dnsState.running, disabled: !!dnsState.resolver || !dnsState.known,
      'aria-label': 'Block Sony CDN (DNS)',
      onchange: function () { setDns(input.checked); }
    });
    return el('label', { class: 'switch' }, [input, el('span')]);
  }

  function dnsStatusText() {
    if (!dnsState.known) return 'Checking…';
    if (dnsState.resolver) return 'another resolver already serves this console';
    if (!dnsState.running) return dnsState.enabled ? 'Not running — port 53 is busy' : 'Disabled';
    return dnsState.masks + ' Sony masks blackholed · ' + dnsState.exceptions + ' exceptions forwarded';
  }

  function shortcuts() {
    var list = [
      ['Open item', 'Enter'], ['Parent folder', 'Backspace or Alt+\u2191'], ['Back / forward', 'Alt+\u2190 / Alt+\u2192'],
      ['Move selection', '\u2191 \u2193, Home, End'], ['Extend selection', 'Shift+\u2191 \u2193, Shift+click'],
      ['Toggle item', 'Space or Ctrl+click'], ['Select all', 'Ctrl+A'], ['Clear selection', 'Esc'],
      ['Rename', 'F2'], ['Delete', 'Delete'], ['Copy / cut / paste', 'Ctrl+C / Ctrl+X / Ctrl+V'],
      ['Filter this folder', '/'], ['Edit path', 'Ctrl+L'], ['Actions menu', 'Shift+F10 or right-click'],
      ['Jump to item', 'Type its name'], ['Collapse sidebar', 'Ctrl+B']
    ];
    var dl = el('dl', { class: 'kv' });
    for (var i = 0; i < list.length; i++) {
      dl.appendChild(el('dt', { text: list[i][0] }));
      dl.appendChild(el('dd', null, el('span', { class: 'mono', text: list[i][1] })));
    }
    ZF.dialog({ title: 'Keyboard shortcuts', size: 'md', content: dl, actions: [{ id: 'ok', label: 'Close', kind: 'primary', submit: true }] });
  }

  /* ── Account, users and folder rules ──────────────────────────────────
   * Users and the allow-list live in the daemon (see /api/auth/*).  The
   * sections are rendered from a cached copy and refreshed after each change.
   */

  var users = null;      /* [{login, role}] or null until loaded */
  var folders = null;    /* [path] or null until loaded */
  var adminError = null;

  function loadAdmin() {
    if (!ZF.auth.isAdmin()) return;
    Promise.all([ZF.api.users(), ZF.api.accessFolders()]).then(function (res) {
      users = (res[0] && res[0].users) || [];
      folders = (res[1] && res[1].folders) || [];
      adminError = null;
      if (!ZF.byId('view-settings').hidden) render();
    }, function (err) {
      adminError = err.message;
      if (!ZF.byId('view-settings').hidden) render();
    });
  }

  function passwordField(id, label, autocomplete) {
    var input = el('input', { class: 'input', id: id, type: 'password', autocomplete: autocomplete || 'new-password' });
    return { input: input, node: el('div', { class: 'field' }, [el('label', { class: 'field-label', for: id, text: label }), input]) };
  }

  function changePassword() {
    var cur = passwordField('pw-current', 'Current password', 'current-password');
    var next = passwordField('pw-new', 'New password');
    var again = passwordField('pw-again', 'Repeat new password');
    var error = el('div', { class: 'field-error', role: 'alert' });
    var content = el('div', { class: 'pw-form' }, [cur.node, next.node, again.node, error,
      el('div', { class: 'field-hint', text: 'At least 6 characters.' })]);
    ZF.dialog({
      title: 'Change password',
      content: content,
      actions: [{ id: 'cancel', label: 'Cancel' }, { id: 'ok', label: 'Change password', kind: 'primary', submit: true }],
      onOpen: function () { cur.input.focus(); },
      onAction: function (id) {
        if (id !== 'ok') return true;
        if (next.input.value.length < 6) { error.textContent = 'The new password needs at least 6 characters.'; next.input.focus(); return false; }
        if (next.input.value !== again.input.value) { error.textContent = 'The new passwords do not match.'; again.input.focus(); return false; }
        error.textContent = '';
        return ZF.api.changePassword(cur.input.value, next.input.value).then(function () {
          ZF.toast('Password changed', { type: 'success' });
          return true;
        }, function (err) { error.textContent = err.message; cur.input.focus(); return false; });
      }
    });
  }

  function roleSelect(value) {
    var sel = el('select', { class: 'input', 'aria-label': 'Role' }, [
      el('option', { value: 'user', text: 'User' }),
      el('option', { value: 'admin', text: 'Administrator' })
    ]);
    sel.value = value || 'user';
    return sel;
  }

  function addUser() {
    var login = el('input', { class: 'input', id: 'nu-login', type: 'text', autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false', maxlength: '32' });
    var pw = passwordField('nu-pw', 'Password');
    var role = roleSelect('user');
    var error = el('div', { class: 'field-error', role: 'alert' });
    var content = el('div', { class: 'pw-form' }, [
      el('div', { class: 'field' }, [el('label', { class: 'field-label', for: 'nu-login', text: 'Login' }), login,
        el('div', { class: 'field-hint', text: 'Letters, digits and . _ - @ only.' })]),
      pw.node,
      el('div', { class: 'field' }, [el('label', { class: 'field-label', text: 'Role' }), role,
        el('div', { class: 'field-hint', text: 'Users only see the folders listed under “Folders for users” and cannot create shares. Administrators can do everything.' })]),
      error
    ]);
    ZF.dialog({
      title: 'Add user',
      content: content,
      actions: [{ id: 'cancel', label: 'Cancel' }, { id: 'ok', label: 'Add user', kind: 'primary', submit: true }],
      onOpen: function () { login.focus(); },
      onAction: function (id) {
        if (id !== 'ok') return true;
        if (!/^[A-Za-z0-9._@-]{1,32}$/.test(login.value)) { error.textContent = 'Enter a valid login.'; login.focus(); return false; }
        if (pw.input.value.length < 6) { error.textContent = 'The password needs at least 6 characters.'; pw.input.focus(); return false; }
        error.textContent = '';
        return ZF.api.userAdd(login.value, pw.input.value, role.value).then(function (res) {
          users = (res && res.users) || users;
          ZF.toast('User ' + login.value + ' added', { type: 'success' });
          render();
          return true;
        }, function (err) { error.textContent = err.message; return false; });
      }
    });
  }

  function editUser(u) {
    var self = u.login === ZF.auth.user();
    var role = roleSelect(u.role);
    role.disabled = self;
    var pw = passwordField('eu-pw', 'New password');
    var error = el('div', { class: 'field-error', role: 'alert' });
    var content = el('div', { class: 'pw-form' }, [
      el('div', { class: 'field' }, [el('label', { class: 'field-label', text: 'Role' }), role,
        self ? el('div', { class: 'field-hint', text: 'Your own role can only be changed by another administrator.' }) : null]),
      pw.node,
      el('div', { class: 'field-hint', text: 'Leave the password empty to keep the current one. Setting a new one signs that user out everywhere.' }),
      error
    ]);
    ZF.dialog({
      title: 'Edit ' + u.login,
      content: content,
      actions: [{ id: 'cancel', label: 'Cancel' }, { id: 'ok', label: 'Save', kind: 'primary', submit: true }],
      onAction: function (id) {
        if (id !== 'ok') return true;
        var fields = {};
        if (!self && role.value !== u.role) fields.role = role.value;
        if (pw.input.value) {
          if (pw.input.value.length < 6) { error.textContent = 'The password needs at least 6 characters.'; pw.input.focus(); return false; }
          fields.password = pw.input.value;
        }
        if (!fields.role && !fields.password) return true;
        error.textContent = '';
        return ZF.api.userUpdate(u.login, fields).then(function (res) {
          users = (res && res.users) || users;
          ZF.toast('User ' + u.login + ' updated', { type: 'success' });
          render();
          return true;
        }, function (err) { error.textContent = err.message; return false; });
      }
    });
  }

  function deleteUser(u) {
    ZF.confirm({
      title: 'Remove ' + u.login + '?',
      message: 'The account is deleted and its sessions end immediately.',
      confirmLabel: 'Remove user', danger: true
    }).then(function (ok) {
      if (!ok) return;
      ZF.api.userDelete(u.login).then(function (res) {
        users = (res && res.users) || users;
        ZF.toast('User ' + u.login + ' removed', { type: 'success' });
        render();
      }, ZF.toastError);
    });
  }

  function usersCard() {
    if (adminError) return el('div', { class: 'empty-inline', text: adminError });
    if (users === null) return el('div', { class: 'empty-inline', text: 'Loading…' });
    var tbody = el('tbody');
    for (var i = 0; i < users.length; i++) {
      (function (u) {
        var self = u.login === ZF.auth.user();
        tbody.appendChild(el('tr', null, [
          el('td', { class: 'col-login', text: u.login + (self ? ' (you)' : '') }),
          el('td', null, el('span', { class: 'role-pill' + (u.role === 'admin' ? ' is-admin' : ''), text: u.role === 'admin' ? 'Administrator' : 'User' })),
          el('td', { class: 'col-actions' }, el('div', { class: 'row-actions' }, [
            ZF.button({ icon: 'pencil', aria: 'Edit ' + u.login, title: 'Edit', kind: 'ghost', size: 'sm', onclick: function () { editUser(u); } }),
            ZF.button({ icon: 'trash', aria: 'Remove ' + u.login, title: 'Remove', kind: 'ghost', size: 'sm', cls: 'btn-danger-text', disabled: self, onclick: function () { deleteUser(u); } })
          ]))
        ]));
      })(users[i]);
    }
    var table = el('table', { class: 'table users-table' }, [
      el('colgroup', null, [el('col'), el('col', { style: 'width:140px' }), el('col', { style: 'width:90px' })]),
      el('thead', null, el('tr', null, [el('th', { text: 'Login' }), el('th', { text: 'Role' }), el('th', null, el('span', { class: 'sr-only', text: 'Actions' }))])),
      tbody
    ]);
    return el('div', null, [
      users.length ? table : el('div', { class: 'empty-inline', text: 'No accounts yet.' }),
      el('div', { class: 'card-foot' }, [
        ZF.button({ icon: 'plus', label: 'Add user', size: 'sm', onclick: addUser })
      ])
    ]);
  }

  function saveFolders(list) {
    return ZF.api.accessSet(list).then(function (res) {
      folders = (res && res.folders) || list;
      render();
    }, function (err) { ZF.toastError(err); render(); });
  }

  function foldersCard() {
    if (adminError) return el('div', { class: 'empty-inline', text: adminError });
    if (folders === null) return el('div', { class: 'empty-inline', text: 'Loading…' });
    var list = el('ul', { class: 'folder-list' });
    for (var i = 0; i < folders.length; i++) {
      (function (f) {
        list.appendChild(el('li', { class: 'folder-item' }, [
          ZF.icon('folder'),
          el('span', { class: 'folder-item-path', text: f, title: f }),
          ZF.button({ icon: 'x', aria: 'Remove ' + f, title: 'Remove', kind: 'ghost', size: 'sm', onclick: function () {
            var next = [];
            for (var k = 0; k < folders.length; k++) if (folders[k] !== f) next.push(folders[k]);
            saveFolders(next);
          } })
        ]));
      })(folders[i]);
    }
    return el('div', null, [
      folders.length ? list : el('div', { class: 'empty-inline', text: 'No folders yet: accounts with the User role cannot open anything until a folder is added here.' }),
      el('div', { class: 'card-foot' }, [
        ZF.button({ icon: 'folder-plus', label: 'Add folder', size: 'sm', onclick: function () {
          ZF.pickFolder({ title: 'Folder for users', start: ZF.files.current() || '/', confirmLabel: 'Allow this folder' }).then(function (p) {
            if (!p) return;
            for (var k = 0; k < folders.length; k++) if (folders[k] === p) return;
            saveFolders(folders.concat([p]));
          });
        } })
      ])
    ]);
  }

  function accessSections() {
    var out = [];
    var enabled = ZF.auth.enabled();
    if (enabled && ZF.auth.loggedIn()) {
      out.push(section('Account', [
        setting('Signed in as', ZF.auth.role() === 'admin' ? 'Administrator' : 'User', el('span', { class: 'mono', text: ZF.auth.user() })),
        setting('Password', 'Change the password of this account.', ZF.button({ label: 'Change password', size: 'sm', onclick: changePassword }))
      ]));
    }
    if (!ZF.auth.isAdmin()) return out;
    out.push(section('Users', [
      el('div', { class: 'setting' }, el('div', { class: 'setting-text' }, [
        el('div', { class: 'setting-title', text: enabled ? 'Login required' : 'Login gate off' }),
        el('div', { class: 'setting-desc', text: enabled
          ? 'Everyone must sign in to use this interface. Share links stay public.'
          : 'Anyone on the network can use this interface. Start zftpd with ZFTPD_ADMIN_PASSWORD set to require a login; accounts created here are kept for when it is on.' })
      ])),
      usersCard()
    ]));
    out.push(section('Folders for users', [
      el('div', { class: 'setting' }, el('div', { class: 'setting-text' }, [
        el('div', { class: 'setting-title', text: 'Allowed folders' }),
        el('div', { class: 'setting-desc', text: 'Accounts with the User role can only browse, download and change files inside these folders (and their subfolders). Administrators are never restricted.' })
      ])),
      foldersCard()
    ]));
    return out;
  }

  function render() {
    ZF.clear(body);
    var info = ZF.api.info || {};
    var admin = ZF.auth.isAdmin();
    if (admin) loadDns();
    if (admin && users === null && !adminError) loadAdmin();

    var access = accessSections();
    for (var a = 0; a < access.length; a++) body.appendChild(access[a]);

    body.appendChild(section('Appearance', [
      setting('Theme', 'System follows your device setting.', segmented('theme', [
        { value: 'system', label: 'System' }, { value: 'light', label: 'Light' }, { value: 'dark', label: 'Dark' }
      ])),
      setting('Density', 'Compact fits more rows in the file list.', segmented('density', [
        { value: 'comfortable', label: 'Comfortable' }, { value: 'compact', label: 'Compact' }
      ]))
    ]));

    body.appendChild(section('Files', [
      setting('Show hidden files', 'Items whose name starts with a dot.', toggle('showHidden', 'Show hidden files')),
      setting('Show system folders', 'Console folders at the root, such as system and preinst. Changing them can make the console unusable.',
        toggle('showProtected', 'Show system folders'), 'text-warning'),
      setting('Start folder', 'The folder Files shows when you open zftpd.', pathButton('startPath', 'Start folder'))
    ]));

    body.appendChild(section('Downloads', [
      setting('Save downloads to', 'Default folder for downloads started from a URL.', pathButton('downloadDest', 'Save downloads to')),
      setting('Decrypt protected files', 'Console .self files download as the decrypted executable instead of the encrypted container. Other files are always sent untouched.',
        toggle('decryptSelf', 'Decrypt protected files'))
    ]));

    if (admin) {
      var dnsRows = [
        setting('Block Sony CDN (DNS)',
          'Answers the Sony content-delivery names with 0.0.0.0 so the console stops downloading from them. Point this console\'s DNS server at this address to use it.',
          dnsSwitch()),
        setting('Filter state', dnsStatusText(), el('span', { class: 'mono', text: dnsState.running ? 'Active' : 'Off' }))
      ];
      body.appendChild(section('Network', dnsRows));
    }

    body.appendChild(section('About', [
      about(info),
      setting('Keyboard shortcuts', 'Every shortcut available in Files.',
        ZF.button({ label: 'Show shortcuts', size: 'sm', onclick: shortcuts })),
      setting('Reset preferences', 'Restores the defaults on this page and removes your Places.',
        ZF.button({
          label: 'Reset', size: 'sm', cls: 'btn-danger-text', onclick: function () {
            ZF.confirm({ title: 'Reset preferences?', message: 'Theme, file options, folders and Places go back to their defaults.', confirmLabel: 'Reset', danger: true })
              .then(function (ok) { if (ok) { ZF.resetSettings(); render(); ZF.toast('Preferences reset', { type: 'success' }); } });
          }
        }))
    ]));

    body.appendChild(credits());
  }

  /* ── About and credits ──────────────────────────────────────────────── */

  var REPO = 'https://github.com/seregonwar/zftpd';
  var DONATE = 'https://www.seregonwar.com/donations';

  function extLink(href, icon, label, cls) {
    return el('a', { class: 'btn btn-sm' + (cls ? ' ' + cls : ''), href: href, target: '_blank', rel: 'noopener noreferrer' }, [
      ZF.icon(icon), el('span', { text: label })
    ]);
  }

  function iconLink(href, icon, label) {
    return el('a', { class: 'btn btn-sm btn-icon', href: href, target: '_blank', rel: 'noopener noreferrer', title: label, 'aria-label': label }, ZF.icon(icon));
  }

  function about(info) {
    return el('div', { class: 'about' }, [
      el('img', { class: 'about-mark', src: 'img/zftpd-mark.png', width: '48', height: '48', alt: '' }),
      el('div', { class: 'about-text' }, [
        el('div', { class: 'about-name' }, [
          el('span', { text: 'zftpd' }),
          info.version ? el('span', { class: 'about-version', text: 'v' + info.version }) : null
        ]),
        el('div', { class: 'about-tagline', text: 'Zero-copy FTP daemon for PS4, PS5, Linux and macOS.' })
      ]),
      el('div', { class: 'about-links' }, [
        extLink(REPO, 'github', 'Source code'),
        extLink(REPO + '/issues', 'issue', 'Report an issue')
      ])
    ]);
  }

  function person(o) {
    var avatar = el('div', { class: 'avatar', 'aria-hidden': 'true' },
      o.photo ? el('img', { src: o.photo, width: '48', height: '48', alt: '' }) : el('span', { text: o.initials }));
    return el('div', { class: 'person' }, [
      avatar,
      el('div', { class: 'person-main' }, [
        el('div', { class: 'person-name', text: o.name }),
        el('div', { class: 'person-role', text: o.role })
      ]),
      o.links ? el('div', { class: 'person-links' }, o.links) : null
    ]);
  }

  function thanks(name, what) {
    return el('li', null, [
      el('span', { class: 'thanks-name', text: name }),
      el('span', { class: 'thanks-what', text: what })
    ]);
  }

  function credits() {
    return el('section', { class: 'section' }, [
      el('div', { class: 'section-head' }, el('h2', { class: 'section-title', text: 'Credits' })),
      el('div', { class: 'card' }, [
        person({
          photo: 'img/seregonwar.jpg', name: 'SeregonWar', role: 'Creator and maintainer',
          links: [
            iconLink('https://github.com/seregonwar', 'github', 'SeregonWar on GitHub'),
            iconLink('https://x.com/SeregonWar', 'brand-x', 'SeregonWar on X'),
            extLink(DONATE, 'heart', 'Donate')
          ]
        }),
        person({ initials: 'MC', name: 'M///Class', role: 'Tester' })
      ]),
      el('div', { class: 'section-head section-head-sub' }, el('h3', { class: 'section-title', text: 'Acknowledgements' })),
      el('div', { class: 'card' }, [
        el('ul', { class: 'thanks' }, [
          thanks('hippie68', 'PS4 FTP reference implementation'),
          thanks('John T\u00f6rnblom', 'PS5 payload framework'),
          thanks('Drakmor', 'Inspiration for the CPFR, CPTO and COPY implementation'),
          thanks('PlayStation homebrew community', 'Testing and feedback')
        ]),
        el('div', { class: 'credits-foot' }, [
          el('span', { text: 'Everyone who reported issues or contributed code.' }),
          el('a', { href: REPO + '/graphs/contributors', target: '_blank', rel: 'noopener noreferrer', text: 'All contributors' })
        ])
      ]),
      el('p', { class: 'section-note', text: 'zftpd is released under the MIT License. \u00a9 2026 Seregon.' })
    ]);
  }

  ZF.on('status', function () { if (!ZF.byId('view-settings').hidden) render(); });
  ZF.on('auth', function () {
    users = null;
    folders = null;
    adminError = null;
    if (!ZF.byId('view-settings').hidden) render();
  });

  ZF.views.settings = {
    title: 'Settings',
    enter: function () { if (ZF.auth.isAdmin()) loadAdmin(); render(); },
    leave: function () {}
  };
  ZF.showShortcuts = shortcuts;
})(ZF);
