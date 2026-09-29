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

  function render() {
    ZF.clear(body);
    var info = ZF.api.info || {};

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
      setting('Save downloads to', 'Default folder for downloads started from a URL.', pathButton('downloadDest', 'Save downloads to'))
    ]));

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

  ZF.views.settings = {
    title: 'Settings',
    enter: render,
    leave: function () {}
  };
  ZF.showShortcuts = shortcuts;
})(ZF);
