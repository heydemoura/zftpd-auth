/* zftpd web — shared UI primitives: toasts, dialogs, menus, folder picker. */
(function (ZF) {
  'use strict';

  var el = ZF.el;

  /* ── Toasts ─────────────────────────────────────────────────────────── */

  var TOAST_ICONS = { success: 'check-circle', error: 'x-circle', warning: 'alert', info: 'info' };
  var MAX_TOASTS = 4;

  /**
   * ZF.toast('Saved', { type: 'success', action: { label: 'Undo', fn: fn }, timeout: 4000 })
   */
  ZF.toast = function (message, opts) {
    opts = opts || {};
    var type = opts.type || 'info';
    var host = ZF.byId('toasts');
    var node;
    var timer = null;

    function dismiss() {
      if (!node || !node.parentNode) return;
      clearTimeout(timer);
      node.className += ' is-leaving';
      setTimeout(function () { if (node.parentNode) node.parentNode.removeChild(node); }, 160);
    }

    node = el('div', { class: 'toast toast-' + type, role: type === 'error' ? 'alert' : 'status' }, [
      ZF.icon(TOAST_ICONS[type] || 'info'),
      el('span', { class: 'toast-text', text: message }),
      opts.action ? el('button', {
        class: 'toast-action', type: 'button', text: opts.action.label,
        onclick: function () { dismiss(); opts.action.fn(); }
      }) : null,
      el('button', { class: 'toast-close', type: 'button', 'aria-label': 'Dismiss', onclick: dismiss }, ZF.icon('x'))
    ]);
    node.addEventListener('mouseenter', function () { clearTimeout(timer); });
    node.addEventListener('mouseleave', function () { arm(); });

    function arm() {
      clearTimeout(timer);
      var t = opts.timeout !== undefined ? opts.timeout : (type === 'error' ? 8000 : 4000);
      if (t > 0) timer = setTimeout(dismiss, t);
    }

    while (host.children.length >= MAX_TOASTS) host.removeChild(host.firstChild);
    host.appendChild(node);
    arm();
    return { dismiss: dismiss };
  };

  ZF.toastError = function (err, prefix) {
    var msg = (err && err.message) ? err.message : String(err || 'Something went wrong');
    ZF.toast(prefix ? prefix + ': ' + msg : msg, { type: 'error' });
  };

  /* ── Dialogs ────────────────────────────────────────────────────────── */

  var stack = [];

  function focusables(root) {
    return ZF.$$('button:not([disabled]), [href], input:not([disabled]):not([type="hidden"]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])', root)
      .filter(function (n) { return n.offsetWidth > 0 || n.offsetHeight > 0; });
  }

  document.addEventListener('keydown', function (e) {
    if (!stack.length || menuState) return;
    var top = stack[stack.length - 1];
    if (e.key === 'Escape' || e.key === 'Esc') {
      e.preventDefault();
      if (!top.busy) top.close(null);
    } else if (e.key === 'Tab') {
      var list = focusables(top.el);
      if (!list.length) { e.preventDefault(); return; }
      var first = list[0], last = list[list.length - 1];
      if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
      else if (!top.el.contains(document.activeElement)) { e.preventDefault(); first.focus(); }
    }
  }, true);

  /**
   * ZF.dialog({
   *   title, content (node | string), size: 'md' | 'lg',
   *   actions: [{ id, label, kind: 'primary' | 'danger', submit: true }],
   *   footLeft: node,
   *   onOpen(d), onAction(id, d) -> false | Promise   (false / resolved false keeps the dialog open)
   * }) -> d  with d.promise resolving to the action id (null when dismissed)
   */
  ZF.dialog = function (o) {
    var d = { busy: false, buttons: {} };
    var resolveFn;
    d.promise = new Promise(function (resolve) { resolveFn = resolve; });

    var returnFocus = document.activeElement;
    var titleId = 'dlg-t-' + (stack.length + 1) + '-' + Date.now();

    d.body = el('div', { class: 'dialog-body' });
    ZF.append(d.body, typeof o.content === 'string' ? el('p', { text: o.content }) : o.content);

    var foot = el('div', { class: 'dialog-foot' });
    if (o.footLeft) {
      o.footLeft.className += ' foot-left';
      foot.appendChild(o.footLeft);
    }
    var actions = o.actions || [{ id: 'ok', label: 'OK', kind: 'primary', submit: true }];
    for (var i = 0; i < actions.length; i++) {
      (function (a) {
        var b = el('button', {
          type: a.submit ? 'submit' : 'button',
          class: 'btn' + (a.kind ? ' btn-' + a.kind : ''),
          text: a.label,
          disabled: !!a.disabled,
          onclick: a.submit ? null : function () { run(a.id); }
        });
        d.buttons[a.id] = b;
        foot.appendChild(b);
      })(actions[i]);
    }
    var submitAction = null;
    for (var j = 0; j < actions.length; j++) if (actions[j].submit) submitAction = actions[j].id;

    d.el = el('form', {
      class: 'dialog' + (o.size ? ' dialog-' + o.size : ''),
      role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': titleId,
      tabindex: '-1', novalidate: true,
      onsubmit: function (e) { e.preventDefault(); if (submitAction) run(submitAction); }
    }, [
      el('div', { class: 'dialog-head' }, [
        el('h2', { class: 'dialog-title', id: titleId, text: o.title }),
        el('button', {
          type: 'button', class: 'btn btn-ghost btn-icon btn-sm dialog-close', 'aria-label': 'Close',
          onclick: function () { if (!d.busy) d.close(null); }
        }, ZF.icon('x'))
      ]),
      d.body,
      foot
    ]);

    var overlay = el('div', { class: 'overlay' }, d.el);
    var downOnOverlay = false;
    overlay.addEventListener('mousedown', function (e) { downOnOverlay = e.target === overlay; });
    overlay.addEventListener('click', function (e) {
      if (e.target === overlay && downOnOverlay && !d.busy && o.dismissible !== false) d.close(null);
    });

    d.setBusy = function (busy) {
      d.busy = busy;
      for (var k in d.buttons) if (d.buttons.hasOwnProperty(k)) d.buttons[k].disabled = busy;
    };

    d.close = function (result) {
      if (d.closed) return;
      d.closed = true;
      closeMenu();
      var idx = stack.indexOf(d);
      if (idx >= 0) stack.splice(idx, 1);
      if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
      if (o.onClose) o.onClose(result);
      resolveFn(result === undefined ? null : result);
      if (returnFocus && returnFocus.focus && document.body.contains(returnFocus)) {
        try { returnFocus.focus(); } catch (e) { /* ignore */ }
      }
    };

    function run(id) {
      if (d.busy) return;
      var res = o.onAction ? o.onAction(id, d) : undefined;
      if (res === false) return;
      if (res && typeof res.then === 'function') {
        d.setBusy(true);
        res.then(function (v) {
          d.setBusy(false);
          if (v !== false) d.close(id);
        }, function (err) {
          d.setBusy(false);
          if (err) ZF.toastError(err);
        });
        return;
      }
      d.close(id);
    }
    d.run = run;

    stack.push(d);
    document.body.appendChild(overlay);
    if (o.onOpen) o.onOpen(d);
    if (!d.el.contains(document.activeElement)) {
      var auto = ZF.$('[autofocus]', d.el) || focusables(d.body)[0] ||
        (submitAction && d.buttons[submitAction]) || d.el;
      try { auto.focus(); } catch (e) { /* ignore */ }
    }
    return d;
  };

  ZF.dialogOpen = function () { return stack.length > 0; };

  /** Resolves true when confirmed. */
  ZF.confirm = function (o) {
    var content = el('div');
    if (o.message) content.appendChild(el('p', { text: o.message }));
    if (o.names && o.names.length) {
      var ul = el('ul', { class: 'name-list' });
      var max = 8;
      for (var i = 0; i < Math.min(o.names.length, max); i++) ul.appendChild(el('li', { text: o.names[i] }));
      if (o.names.length > max) ul.appendChild(el('li', { class: 'subtle', text: 'and ' + (o.names.length - max) + ' more' }));
      content.appendChild(ul);
    }
    if (o.note) {
      content.appendChild(el('div', { class: 'note' + (o.noteKind ? ' note-' + o.noteKind : '') }, [
        ZF.icon(o.noteKind === 'danger' || o.noteKind === 'warning' ? 'alert' : 'info'),
        el('span', { text: o.note })
      ]));
    }
    return ZF.dialog({
      title: o.title,
      content: content,
      actions: [
        { id: 'cancel', label: o.cancelLabel || 'Cancel' },
        { id: 'ok', label: o.confirmLabel || 'OK', kind: o.danger ? 'danger' : 'primary', submit: true }
      ],
      onOpen: function (d) { d.buttons[o.danger ? 'cancel' : 'ok'].focus(); }
    }).promise.then(function (id) { return id === 'ok'; });
  };

  /**
   * Text prompt. `submit(value)` may return a Promise; a rejection is shown
   * inline and keeps the dialog open. Resolves to the value, or null.
   */
  ZF.prompt = function (o) {
    var input = el('input', {
      class: 'input', type: 'text', value: o.value || '', spellcheck: 'false', autocomplete: 'off',
      placeholder: o.placeholder || null, autofocus: true, id: 'prompt-input'
    });
    var error = el('div', { class: 'field-error', role: 'alert' });
    var field = el('div', { class: 'field' }, [
      o.label ? el('label', { class: 'field-label', for: 'prompt-input', text: o.label }) : null,
      input,
      error,
      o.hint ? el('div', { class: 'field-hint', text: o.hint }) : null
    ]);
    var value = null;

    function check() {
      var msg = o.validate ? o.validate(input.value) : null;
      error.textContent = msg || '';
      input.className = 'input' + (msg ? ' is-invalid' : '');
      return !msg;
    }
    input.addEventListener('input', function () {
      if (error.textContent) check();
    });

    return ZF.dialog({
      title: o.title,
      content: field,
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'ok', label: o.confirmLabel || 'OK', kind: 'primary', submit: true }
      ],
      onOpen: function () {
        input.focus();
        if (o.selectBase && o.value) {
          var dot = o.value.lastIndexOf('.');
          try { input.setSelectionRange(0, dot > 0 ? dot : o.value.length); } catch (e) { input.select(); }
        } else {
          input.select();
        }
      },
      onAction: function (id) {
        if (id !== 'ok') return true;
        if (!check()) { input.focus(); return false; }
        value = input.value;
        if (!o.submit) return true;
        return o.submit(value).then(function () { return true; }, function (err) {
          error.textContent = (err && err.message) || 'Failed';
          input.className = 'input is-invalid';
          input.focus();
          return false;
        });
      }
    }).promise.then(function (id) { return id === 'ok' ? value : null; });
  };

  /** Validation shared by every "name" prompt. */
  ZF.validateName = function (name, existing, current) {
    name = String(name);
    if (!name.trim()) return 'Enter a name.';
    if (name !== name.trim()) return 'Names cannot start or end with a space.';
    if (/[\/\\]/.test(name)) return 'Names cannot contain / or \\.';
    if (name === '.' || name.indexOf('..') >= 0) return 'Names cannot contain "..".';
    if (/[\x00-\x1f]/.test(name)) return 'Names cannot contain control characters.';
    if (name.length > 255) return 'Names must be 255 characters or fewer.';
    if (existing && name !== current) {
      var lower = name.toLowerCase();
      for (var i = 0; i < existing.length; i++) {
        if (existing[i] === name) return 'An item with this name already exists.';
        if (existing[i].toLowerCase() === lower && existing[i] !== current) return 'An item with this name already exists.';
      }
    }
    return null;
  };

  /* ── Breadcrumbs (shared by the file view and folder picker) ────────── */

  ZF.renderCrumbs = function (host, path, onNavigate, maxVisible) {
    ZF.clear(host);
    var segs = ZF.path.segments(path);
    var items = [{ label: '/', path: '/', root: true }];
    var acc = '';
    for (var i = 0; i < segs.length; i++) {
      acc += '/' + segs[i];
      items.push({ label: segs[i], path: acc });
    }
    var limit = maxVisible || 5;
    var hidden = items.length > limit ? items.slice(1, items.length - (limit - 2)) : [];
    var visible = hidden.length ? [items[0]].concat(items.slice(items.length - (limit - 2))) : items;

    for (var k = 0; k < visible.length; k++) {
      (function (it, isLast) {
        if (k > 0) host.appendChild(ZF.icon('chevron-right', 'crumb-sep'));
        if (k === 1 && hidden.length) {
          var more = el('button', { type: 'button', class: 'crumb', title: 'Show hidden path segments', 'aria-label': 'Show more' }, el('span', { text: '\u2026' }));
          more.addEventListener('click', function (e) {
            e.stopPropagation();
            ZF.menu(hidden.map(function (h) {
              return { label: h.label, icon: 'folder', onclick: function () { onNavigate(h.path); } };
            }), more);
          });
          host.appendChild(more);
          host.appendChild(ZF.icon('chevron-right', 'crumb-sep'));
        }
        var b = el('button', {
          type: 'button', class: 'crumb' + (isLast ? ' is-last' : ''),
          title: it.path, 'aria-current': isLast ? 'location' : null
        }, it.root ? (isLast ? [ZF.icon('drive'), el('span', { class: 'crumb-root-label', text: 'Root' })] : ZF.icon('drive')) : el('span', { text: it.label }));
        if (it.root && !isLast) b.setAttribute('aria-label', 'Root');
        b.addEventListener('click', function (e) { e.stopPropagation(); onNavigate(it.path); });
        host.appendChild(b);
      })(visible[k], k === visible.length - 1);
    }
  };

  /* ── Folder picker ──────────────────────────────────────────────────── */

  /**
   * ZF.pickFolder({ title, start, confirmLabel, check(path) -> error string | null })
   * Resolves to the chosen folder path, or null.
   */
  ZF.pickFolder = function (o) {
    var current = ZF.path.norm(o.start || '/');
    var crumbs = el('div', { class: 'crumbs' });
    var list = el('div', { class: 'picker-list', role: 'listbox', 'aria-label': 'Folders' });
    var dest = el('div', { class: 'picker-dest' });
    var err = el('div', { class: 'field-hint picker-note', role: 'status' });
    var names = [];
    var chosen = null; /* file scelto, in modalità files */
    var seq = 0;
    var dialog;

    var newBtn = ZF.button({ icon: 'folder-plus', label: 'New folder', kind: 'ghost' });
    newBtn.addEventListener('click', function () {
      ZF.prompt({
        title: 'New folder', label: 'Name', value: '', confirmLabel: 'Create',
        validate: function (v) { return ZF.validateName(v, names); },
        submit: function (v) { return ZF.api.mkdir(current, v); }
      }).then(function (name) {
        if (name) load(ZF.path.join(current, name));
      });
    });

    function update() {
      ZF.clear(dest);
      dest.appendChild(document.createTextNode(o.files ? 'Package: ' : 'Destination: '));
      dest.appendChild(el('strong', { text: o.files ? (chosen || current) : current }));
      var msg = o.check ? o.check(current) : null;
      if (o.files && !chosen) msg = msg || 'Choose a package';
      err.textContent = msg || '';
      if (dialog) dialog.buttons.ok.disabled = !!msg;
    }

    function load(path) {
      var my = ++seq;
      current = ZF.path.norm(path);
      ZF.renderCrumbs(crumbs, current, load, 4);
      update();
      ZF.clear(list).appendChild(el('div', { class: 'picker-empty', text: 'Loading\u2026' }));
      ZF.api.list(current).then(function (res) {
        if (my !== seq) return;
        ZF.clear(list);
        names = [];
        var dirs = [];
        var files = [];
        var entries = res.entries || [];
        for (var i = 0; i < entries.length; i++) {
          names.push(entries[i].name);
          if (entries[i].type !== 'directory') {
            if (o.files && (!o.filter || o.filter(entries[i].name))) files.push(entries[i].name);
            continue;
          }
          if (!ZF.settings.showHidden && entries[i].name.charAt(0) === '.') continue;
          if (!ZF.settings.showProtected && ZF.isProtected(current, entries[i].name)) continue;
          dirs.push(entries[i].name);
        }
        dirs.sort(ZF.naturalCompare);
        files.sort(ZF.naturalCompare);
        if (!dirs.length && !files.length) {
          list.appendChild(el('div', { class: 'picker-empty', text: o.files ? 'Nothing to choose here' : 'No subfolders' }));
          return;
        }
        for (var j = 0; j < dirs.length; j++) {
          (function (name) {
            list.appendChild(el('button', {
              type: 'button', class: 'picker-row', role: 'option',
              onclick: function () { load(ZF.path.join(current, name)); }
            }, [ZF.icon('folder'), el('span', { text: name }), ZF.icon('chevron-right', 'ic-go')]));
          })(dirs[j]);
        }
        if (o.files) {
          for (var k = 0; k < files.length; k++) {
            (function (name) {
              var full = ZF.path.join(current, name);
              list.appendChild(el('button', {
                type: 'button', class: 'picker-row' + (chosen === full ? ' is-selected' : ''),
                role: 'option',
                onclick: function () { chosen = full; update(); }
              }, [ZF.icon('file'), el('span', { text: name })]));
            })(files[k]);
          }
        }
      }, function (e) {
        if (my !== seq) return;
        ZF.clear(list).appendChild(el('div', { class: 'picker-empty text-danger', text: e.message }));
      });
    }

    var content = el('div', null, [
      el('div', { class: 'picker' }, [crumbs, list]),
      el('div', { class: 'picker-foot' }, [dest]),
      err
    ]);

    dialog = ZF.dialog({
      title: o.title || 'Choose a folder',
      size: 'md',
      content: content,
      footLeft: newBtn,
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'ok', label: o.confirmLabel || 'Select', kind: 'primary', submit: true }
      ],
      onAction: function (id) {
        if (id === 'ok' && o.files && !chosen) return false;
        if (id === 'ok' && o.check && o.check(current)) return false;
        return true;
      }
    });
    load(current);
    return dialog.promise.then(function (id) {
      if (id !== 'ok') return null;
      return o.files ? chosen : current;
    });
  };

  /* ── Menus ──────────────────────────────────────────────────────────── */

  var menuState = null;

  function closeMenu(restoreFocus) {
    if (!menuState) return;
    var s = menuState;
    menuState = null;
    if (s.node.parentNode) s.node.parentNode.removeChild(s.node);
    document.removeEventListener('mousedown', s.onDown, true);
    document.removeEventListener('touchstart', s.onDown, true);
    window.removeEventListener('resize', s.onClose);
    window.removeEventListener('blur', s.onClose);
    document.removeEventListener('scroll', s.onScroll, true);
    if (s.anchor && s.anchor.setAttribute) s.anchor.setAttribute('aria-expanded', 'false');
    if (restoreFocus && s.returnFocus && s.returnFocus.focus) {
      try { s.returnFocus.focus(); } catch (e) { /* ignore */ }
    }
    if (s.onClose2) s.onClose2();
  }
  ZF.closeMenu = closeMenu;
  ZF.menuOpen = function () { return !!menuState; };

  /**
   * ZF.menu(items, anchorElement | { x, y }, { onClose })
   * item: { label, icon, hint, danger, disabled, checked, onclick } or '-'
   */
  ZF.menu = function (items, at, opts) {
    closeMenu();
    opts = opts || {};
    var node = el('div', { class: 'menu', role: 'menu', tabindex: '-1' });
    var buttons = [];
    var lastWasSep = true;

    for (var i = 0; i < items.length; i++) {
      var it = items[i];
      if (!it) continue;
      if (it === '-') {
        if (!lastWasSep) node.appendChild(el('div', { class: 'menu-sep', role: 'separator' }));
        lastWasSep = true;
        continue;
      }
      lastWasSep = false;
      (function (item) {
        var b = el('button', {
          type: 'button', role: item.checked !== undefined ? 'menuitemradio' : 'menuitem',
          class: 'menu-item' + (item.danger ? ' is-danger' : ''),
          disabled: !!item.disabled, tabindex: '-1',
          'aria-checked': item.checked !== undefined ? String(!!item.checked) : null
        }, [
          item.icon ? ZF.icon(item.icon) : null,
          el('span', { text: item.label }),
          item.hint ? el('span', { class: 'menu-hint', text: item.hint }) : null,
          item.checked ? el('span', { class: 'menu-check' }, ZF.icon('check')) : null
        ]);
        b.addEventListener('click', function () {
          closeMenu(true);
          if (item.onclick) item.onclick();
        });
        b.addEventListener('mousemove', function () { setActive(buttons.indexOf(b)); });
        node.appendChild(b);
        if (!item.disabled) buttons.push(b);
      })(it);
    }
    if (node.lastChild && node.lastChild.className === 'menu-sep') node.removeChild(node.lastChild);

    var active = -1;
    function setActive(idx) {
      if (active >= 0 && buttons[active]) buttons[active].className = buttons[active].className.replace(' is-active', '');
      active = idx;
      if (active >= 0 && buttons[active]) {
        buttons[active].className += ' is-active';
        buttons[active].focus();
      }
    }

    node.addEventListener('keydown', function (e) {
      var k = e.key;
      if (k === 'ArrowDown' || k === 'Down') { e.preventDefault(); setActive((active + 1) % buttons.length); }
      else if (k === 'ArrowUp' || k === 'Up') { e.preventDefault(); setActive(active <= 0 ? buttons.length - 1 : active - 1); }
      else if (k === 'Home') { e.preventDefault(); setActive(0); }
      else if (k === 'End') { e.preventDefault(); setActive(buttons.length - 1); }
      else if (k === 'Escape' || k === 'Esc') { e.preventDefault(); e.stopPropagation(); closeMenu(true); }
      else if (k === 'Tab') { e.preventDefault(); closeMenu(true); }
      else if (k.length === 1 && /\S/.test(k)) {
        var ch = k.toLowerCase();
        for (var n = 1; n <= buttons.length; n++) {
          var idx = (active + n) % buttons.length;
          if ((buttons[idx].textContent || '').toLowerCase().charAt(0) === ch) { setActive(idx); break; }
        }
      }
    });

    document.body.appendChild(node);

    var anchor = at && at.nodeType === 1 ? at : null;
    var x, y;
    var vw = window.innerWidth, vh = window.innerHeight;
    var mw = node.offsetWidth, mh = node.offsetHeight;
    if (anchor) {
      var r = anchor.getBoundingClientRect();
      x = opts.alignLeft ? r.left : r.right - mw;
      y = r.bottom + 4;
      if (x < 8) x = r.left;
      if (y + mh > vh - 8) y = Math.max(8, r.top - mh - 4);
      anchor.setAttribute('aria-expanded', 'true');
    } else {
      x = at.x; y = at.y;
      if (x + mw > vw - 8) x = Math.max(8, x - mw);
      if (y + mh > vh - 8) y = Math.max(8, vh - mh - 8);
    }
    x = Math.max(8, Math.min(x, vw - mw - 8));
    node.style.left = Math.round(x) + 'px';
    node.style.top = Math.round(y) + 'px';
    if (mh > vh - 16) { node.style.maxHeight = (vh - 16) + 'px'; node.style.overflowY = 'auto'; }

    menuState = {
      node: node,
      anchor: anchor,
      returnFocus: opts.returnFocus || anchor || document.activeElement,
      onClose2: opts.onClose,
      onDown: function (e) {
        if (node.contains(e.target)) return;
        if (anchor && anchor.contains(e.target)) { e.stopPropagation(); e.preventDefault(); closeMenu(); return; }
        closeMenu();
      },
      onClose: function () { closeMenu(); },
      onScroll: function (e) { if (!node.contains(e.target)) closeMenu(); }
    };
    setTimeout(function () {
      if (!menuState || menuState.node !== node) return;
      document.addEventListener('mousedown', menuState.onDown, true);
      document.addEventListener('touchstart', menuState.onDown, true);
      window.addEventListener('resize', menuState.onClose);
      window.addEventListener('blur', menuState.onClose);
      document.addEventListener('scroll', menuState.onScroll, true);
    }, 0);

    if (opts.keyboard === false) node.focus();
    else setActive(buttons.length ? 0 : -1);
    if (!buttons.length) node.focus();
  };
})(ZF);
