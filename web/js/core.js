/* zftpd web — core helpers.
 * ES5 only: the PS4/PS5 browser does not support ES2015 syntax.
 */
var ZF = window.ZF || {};
window.ZF = ZF;

(function (ZF) {
  'use strict';

  var D = document;
  var SVGNS = 'http://www.w3.org/2000/svg';
  var XLINK = 'http://www.w3.org/1999/xlink';

  ZF.views = ZF.views || {};

  /* ── Events ─────────────────────────────────────────────────────────── */

  var handlers = {};

  ZF.on = function (name, fn) {
    (handlers[name] = handlers[name] || []).push(fn);
  };

  ZF.emit = function (name, data) {
    var list = handlers[name];
    if (!list) return;
    for (var i = 0; i < list.length; i++) {
      try { list[i](data); } catch (e) { if (window.console) console.error(e); }
    }
  };

  /* ── DOM ────────────────────────────────────────────────────────────── */

  ZF.$ = function (sel, root) { return (root || D).querySelector(sel); };
  ZF.$$ = function (sel, root) { return Array.prototype.slice.call((root || D).querySelectorAll(sel)); };
  ZF.byId = function (id) { return D.getElementById(id); };

  /**
   * el('button', { class: 'btn', onclick: fn, text: 'Save' }, [children])
   * Keys: class, text, html, style, on<event>, plus plain attributes.
   * Boolean false / null values are skipped.
   */
  ZF.el = function (tag, props, children) {
    var node = D.createElement(tag);
    var k, v;
    if (props) {
      for (k in props) {
        if (!props.hasOwnProperty(k)) continue;
        v = props[k];
        if (v === null || v === undefined || v === false) continue;
        if (k === 'class') node.className = v;
        else if (k === 'text') node.textContent = v;
        else if (k === 'html') node.innerHTML = v;
        else if (k === 'style') node.style.cssText = v;
        else if (k.indexOf('on') === 0 && typeof v === 'function') node.addEventListener(k.slice(2), v);
        else if (k === 'value' || k === 'checked' || k === 'disabled' || k === 'selected') node[k] = v;
        else node.setAttribute(k, v === true ? '' : String(v));
      }
    }
    ZF.append(node, children);
    return node;
  };

  ZF.append = function (node, children) {
    if (children === null || children === undefined || children === false) return node;
    if (!(children instanceof Array)) children = [children];
    for (var i = 0; i < children.length; i++) {
      var c = children[i];
      if (c === null || c === undefined || c === false) continue;
      node.appendChild(typeof c === 'string' || typeof c === 'number' ? D.createTextNode(String(c)) : c);
    }
    return node;
  };

  ZF.clear = function (node) {
    while (node && node.firstChild) node.removeChild(node.firstChild);
    return node;
  };

  ZF.icon = function (name, cls) {
    var svg = D.createElementNS(SVGNS, 'svg');
    svg.setAttribute('class', 'ic ic-' + name + (cls ? ' ' + cls : ''));
    svg.setAttribute('aria-hidden', 'true');
    svg.setAttribute('focusable', 'false');
    var use = D.createElementNS(SVGNS, 'use');
    use.setAttributeNS(XLINK, 'xlink:href', '#i-' + name);
    svg.appendChild(use);
    return svg;
  };

  ZF.iconHtml = function (name, cls) {
    return '<svg class="ic ic-' + name + (cls ? ' ' + cls : '') + '" aria-hidden="true" focusable="false">' +
      '<use xlink:href="#i-' + name + '"></use></svg>';
  };

  /** Button with optional icon and label. */
  ZF.button = function (o) {
    var cls = 'btn' + (o.kind ? ' btn-' + o.kind : '') + (o.size ? ' btn-' + o.size : '') +
      (o.label ? '' : ' btn-icon') + (o.cls ? ' ' + o.cls : '');
    var b = ZF.el('button', {
      type: o.type || 'button',
      class: cls,
      title: o.title || (o.label ? null : o.aria),
      'aria-label': o.label ? null : (o.aria || o.title),
      disabled: !!o.disabled,
      onclick: o.onclick
    });
    if (o.icon) b.appendChild(ZF.icon(o.icon));
    if (o.label) b.appendChild(ZF.el('span', { text: o.label }));
    return b;
  };

  ZF.esc = function (value) {
    return String(value === null || value === undefined ? '' : value)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  };

  ZF.isTyping = function (target) {
    if (!target) return false;
    var tag = target.tagName;
    return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || target.isContentEditable;
  };

  /* ── Paths ──────────────────────────────────────────────────────────── */

  ZF.path = {
    norm: function (p) {
      p = String(p || '/').replace(/\\/g, '/').replace(/\/{2,}/g, '/');
      if (p.charAt(0) !== '/') p = '/' + p;
      if (p.length > 1 && p.charAt(p.length - 1) === '/') p = p.slice(0, -1);
      return p;
    },
    parent: function (p) {
      p = ZF.path.norm(p);
      if (p === '/') return null;
      var i = p.lastIndexOf('/');
      return i <= 0 ? '/' : p.slice(0, i);
    },
    join: function (dir, name) {
      dir = ZF.path.norm(dir);
      return dir === '/' ? '/' + name : dir + '/' + name;
    },
    base: function (p) {
      p = ZF.path.norm(p);
      return p === '/' ? '/' : p.slice(p.lastIndexOf('/') + 1);
    },
    ext: function (name) {
      var i = String(name || '').lastIndexOf('.');
      return i > 0 ? name.slice(i + 1).toLowerCase() : '';
    },
    segments: function (p) {
      p = ZF.path.norm(p);
      return p === '/' ? [] : p.slice(1).split('/');
    },
    /** True when `child` is `parent` or lives inside it. */
    within: function (child, parent) {
      child = ZF.path.norm(child);
      parent = ZF.path.norm(parent);
      if (parent === '/') return true;
      return child === parent || child.indexOf(parent + '/') === 0;
    }
  };

  /* ── Formatting ─────────────────────────────────────────────────────── */

  ZF.bytes = function (n) {
    if (typeof n !== 'number' || !isFinite(n) || n < 0) return '\u2014';
    if (n < 1024) return n + ' B';
    var units = ['KB', 'MB', 'GB', 'TB', 'PB'];
    var i = -1;
    do { n = n / 1024; i++; } while (n >= 1024 && i < units.length - 1);
    return (n >= 100 ? n.toFixed(0) : n.toFixed(1)) + ' ' + units[i];
  };

  ZF.rate = function (bps) {
    return bps > 0 ? ZF.bytes(bps) + '/s' : '\u2014';
  };

  ZF.duration = function (sec) {
    if (typeof sec !== 'number' || !isFinite(sec) || sec < 0) return '\u2014';
    sec = Math.round(sec);
    var d = Math.floor(sec / 86400);
    var h = Math.floor((sec % 86400) / 3600);
    var m = Math.floor((sec % 3600) / 60);
    var s = sec % 60;
    if (d > 0) return d + 'd ' + h + 'h';
    if (h > 0) return h + 'h ' + m + 'm';
    if (m > 0) return m + 'm ' + s + 's';
    return s + 's';
  };

  ZF.plural = function (n, one, many) {
    return n + ' ' + (n === 1 ? one : (many || one + 's'));
  };

  ZF.number = function (n) {
    return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ',');
  };

  ZF.percent = function (part, total) {
    if (!total || total <= 0) return 0;
    return Math.max(0, Math.min(100, (part / total) * 100));
  };

  /** Case-insensitive natural comparison ("file2" < "file10"). */
  ZF.naturalCompare = function (a, b) {
    a = String(a).toLowerCase();
    b = String(b).toLowerCase();
    var re = /(\d+|\D+)/g;
    var ax = a.match(re) || [];
    var bx = b.match(re) || [];
    var n = Math.min(ax.length, bx.length);
    for (var i = 0; i < n; i++) {
      var x = ax[i], y = bx[i];
      if (x === y) continue;
      var xd = /^\d/.test(x), yd = /^\d/.test(y);
      if (xd && yd) {
        var diff = parseInt(x, 10) - parseInt(y, 10);
        if (diff !== 0) return diff < 0 ? -1 : 1;
        if (x.length !== y.length) return x.length < y.length ? -1 : 1;
      }
      return x < y ? -1 : 1;
    }
    return ax.length === bx.length ? 0 : (ax.length < bx.length ? -1 : 1);
  };

  /* ── File types ─────────────────────────────────────────────────────── */

  var TYPES = [
    { kind: 'image', label: 'Image', icon: 'image', ext: 'png jpg jpeg gif bmp webp svg ico tif tiff heic' },
    { kind: 'video', label: 'Video', icon: 'video', ext: 'mp4 m4v mkv mov avi webm wmv flv' },
    { kind: 'audio', label: 'Audio', icon: 'audio', ext: 'mp3 wav flac aac ogg m4a opus wma at9' },
    { kind: 'archive', label: 'Archive', icon: 'archive', ext: 'zip 7z rar tar gz tgz bz2 xz zst lz4' },
    { kind: 'package', label: 'Game package', icon: 'package', ext: 'pkg fpkg ffpkg' },
    { kind: 'image-disk', label: 'Disk image', icon: 'disc', ext: 'exfat ffpfs iso img pup' },
    { kind: 'text', label: 'Text', icon: 'file-text', ext: 'txt md log ini cfg conf csv tsv nfo lst' },
    { kind: 'code', label: 'Source', icon: 'code', ext: 'c h cc cpp hpp py js ts json xml yml yaml toml html htm css sh lua rs go java php rb mk' },
    { kind: 'binary', label: 'Executable', icon: 'file', ext: 'elf self sprx prx so dll exe' },
    { kind: 'document', label: 'Document', icon: 'file-text', ext: 'pdf doc docx odt rtf' }
  ];
  var EXT_MAP = {};
  (function () {
    for (var i = 0; i < TYPES.length; i++) {
      var list = TYPES[i].ext.split(' ');
      for (var j = 0; j < list.length; j++) EXT_MAP[list[j]] = TYPES[i];
    }
  })();

  ZF.fileType = function (name, isDir) {
    if (isDir) return { kind: 'folder', label: 'Folder', icon: 'folder' };
    var ext = ZF.path.ext(name);
    var t = EXT_MAP[ext];
    if (t) return { kind: t.kind, label: t.label, icon: t.icon, ext: ext };
    return { kind: 'file', label: ext ? ext.toUpperCase() + ' file' : 'File', icon: 'file', ext: ext };
  };

  ZF.isArchive = function (name) { return ZF.fileType(name, false).kind === 'archive'; };
  ZF.isPackage = function (name) { return ZF.fileType(name, false).kind === 'package'; };
  ZF.isGameImage = function (name) {
    var ext = ZF.path.ext(name);
    return ext === 'pkg' || ext === 'fpkg' || ext === 'ffpkg' || ext === 'exfat';
  };

  /* ── Settings ───────────────────────────────────────────────────────── */

  var SETTINGS_KEY = 'zftpd.settings';
  var DEFAULTS = {
    theme: 'system',
    density: 'comfortable',
    showHidden: false,
    showProtected: false,
    startPath: '/',
    downloadDest: '/',
    decryptSelf: false,
    bookmarks: [],
    fanThreshold: 60,
    sortKey: 'name',
    sortDir: 1,
    sidebarCollapsed: false
  };

  function loadSettings() {
    var out = {};
    var stored = {};
    try { stored = JSON.parse(localStorage.getItem(SETTINGS_KEY) || '{}') || {}; } catch (e) { stored = {}; }
    for (var k in DEFAULTS) {
      if (DEFAULTS.hasOwnProperty(k)) out[k] = stored.hasOwnProperty(k) ? stored[k] : DEFAULTS[k];
    }
    if (!(out.bookmarks instanceof Array)) out.bookmarks = [];
    return out;
  }

  ZF.settings = loadSettings();

  ZF.setSetting = function (key, value) {
    ZF.settings[key] = value;
    try { localStorage.setItem(SETTINGS_KEY, JSON.stringify(ZF.settings)); } catch (e) { /* private mode */ }
    ZF.emit('settings', key);
  };

  ZF.resetSettings = function () {
    for (var k in DEFAULTS) {
      if (DEFAULTS.hasOwnProperty(k)) ZF.settings[k] = k === 'bookmarks' ? [] : DEFAULTS[k];
    }
    try { localStorage.removeItem(SETTINGS_KEY); } catch (e) { /* ignore */ }
    ZF.emit('settings', '*');
  };

  /* Root-level system directories that are hidden unless explicitly enabled. */
  var PROTECTED = ['system', 'system_ex', 'system_data', 'preinst', 'preinst2', 'mini-syscore',
    'sandcastle', 'update', 'dev', 'proc', 'sys', 'kern'];

  ZF.isProtected = function (dir, name) {
    return ZF.path.norm(dir) === '/' && PROTECTED.indexOf(name) >= 0;
  };

  ZF.features = { pkgInstall: false };

  /* Facts reported by the daemon in /api/status. `root` is the directory it
   * serves (`-d`): every path the interface shows lives inside it. */
  ZF.server = { root: '/' };
  ZF.rootPath = function () { return ZF.server.root || '/'; };
  ZF.isRoot = function (p) { return ZF.path.norm(p) === ZF.rootPath(); };

  /* ── Misc ───────────────────────────────────────────────────────────── */

  ZF.debounce = function (fn, ms) {
    var t = null;
    return function () {
      var args = arguments, ctx = this;
      clearTimeout(t);
      t = setTimeout(function () { fn.apply(ctx, args); }, ms);
    };
  };

  ZF.copyText = function (text) {
    if (navigator.clipboard && navigator.clipboard.writeText && window.isSecureContext) {
      return navigator.clipboard.writeText(text);
    }
    return new Promise(function (resolve, reject) {
      var ta = D.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.cssText = 'position:fixed;left:-9999px;top:0;';
      D.body.appendChild(ta);
      ta.select();
      var ok = false;
      try { ok = D.execCommand('copy'); } catch (e) { ok = false; }
      D.body.removeChild(ta);
      if (ok) resolve(); else reject(new Error('Copy failed'));
    });
  };

  ZF.triggerDownload = function (url, name) {
    var a = D.createElement('a');
    a.href = url;
    if (name) a.setAttribute('download', name);
    a.style.display = 'none';
    D.body.appendChild(a);
    a.click();
    D.body.removeChild(a);
  };
})(ZF);
