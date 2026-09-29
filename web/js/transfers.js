/* zftpd web — Transfers view: server-side URL downloads and session jobs. */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var api = ZF.api;
  var $ = ZF.byId;

  var dom = {
    view: $('view-transfers'),
    form: $('dl-form'),
    url: $('dl-url'),
    hint: $('dl-url-hint'),
    dest: $('dl-dest'),
    destText: $('dl-dest-text'),
    start: $('dl-start'),
    count: $('dl-count'),
    list: $('dl-list'),
    jobsCount: $('jobs-count'),
    jobsClear: $('jobs-clear'),
    jobsList: $('jobs-list')
  };

  var HINT = dom.hint.textContent;
  var downloads = [];
  var dismissed = {};
  var rows = {};
  var timer = null;
  var visible = false;
  var loaded = false;
  var loadError = null;

  function dest() { return ZF.settings.downloadDest || '/'; }

  function renderDest() {
    dom.destText.textContent = dest();
    dom.dest.title = 'Save to ' + dest() + ' \u2014 click to change';
  }

  /* ── Form ───────────────────────────────────────────────────────────── */

  function setHint(msg, isError) {
    dom.hint.textContent = msg || HINT;
    dom.hint.className = isError ? 'field-error' : 'field-hint';
    dom.url.className = 'input' + (isError ? ' is-invalid' : '');
  }

  function validUrl(v) {
    return /^(https?|ftps?):\/\/[^\s\/?#]+[^\s]*$/i.test(v);
  }

  dom.url.addEventListener('input', function () {
    if (dom.hint.className === 'field-error') setHint(null);
  });

  dom.dest.addEventListener('click', function () {
    ZF.pickFolder({ title: 'Save downloads to', start: dest(), confirmLabel: 'Use this folder' }).then(function (p) {
      if (p) { ZF.setSetting('downloadDest', p); renderDest(); }
    });
  });

  dom.form.addEventListener('submit', function (e) {
    e.preventDefault();
    var url = dom.url.value.trim();
    if (!url) { setHint('Enter a URL.', true); dom.url.focus(); return; }
    if (!validUrl(url)) { setHint('Enter a full http://, https://, ftp:// or ftps:// URL.', true); dom.url.focus(); return; }
    dom.start.disabled = true;
    api.downloadStart(url, dest()).then(function (res) {
      if (res && res.ok === false) throw new Error(res.message || 'Could not start the download');
      dom.url.value = '';
      setHint(null);
      ZF.toast('Download started' + (res && res.name ? ': ' + res.name : ''), { type: 'success' });
      refresh();
    }, function (err) {
      setHint(err.message, true);
    }).then(function () { dom.start.disabled = false; });
  });

  /* ── Server downloads ───────────────────────────────────────────────── */

  function stateOf(d) {
    if (d.error) return /cancel/i.test(d.error) ? 'cancelled' : 'error';
    if (d.done) return 'done';
    if (d.paused) return 'paused';
    return 'active';
  }

  var LABEL = { active: 'Downloading', paused: 'Paused', done: 'Done', error: 'Failed', cancelled: 'Cancelled' };

  function DlRow(d) {
    this.id = d.id;
    this.name = el('div', { class: 'xfer-name' });
    this.status = el('span', { class: 'status' });
    this.sub = el('div', { class: 'xfer-sub' });
    this.bar = el('span');
    this.progress = el('div', { class: 'progress', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100' }, this.bar);
    this.meta = el('div', { class: 'xfer-meta' });
    this.err = el('div', { class: 'xfer-error', role: 'alert' });
    this.actions = el('div', { class: 'xfer-actions' });
    this.key = '';
    this.node = el('div', { class: 'xfer' }, [
      el('div', { class: 'xfer-icon' }, ZF.icon('download')),
      el('div', { class: 'xfer-main' }, [
        el('div', { class: 'xfer-top' }, [this.name, this.status]),
        this.sub, this.progress, this.meta, this.err
      ]),
      this.actions
    ]);
  }

  DlRow.prototype.update = function (d) {
    var st = stateOf(d);
    var total = d.total_size || 0;
    var got = d.downloaded || 0;
    var pct = total > 0 ? ZF.percent(got, total) : (d.progress || 0);
    this.name.textContent = d.name || 'Download';
    this.name.title = d.name || '';
    this.status.className = 'status status-' + st;
    this.status.textContent = LABEL[st];
    this.sub.textContent = d.url || '';
    this.sub.title = d.url || '';

    var cls = 'progress';
    if (st === 'done') { cls += ' is-done'; pct = 100; }
    else if (st === 'error') cls += ' is-error';
    else if (st === 'paused' || st === 'cancelled') cls += ' is-paused';
    else if (!total && !got) cls += ' is-indeterminate';
    this.progress.className = cls;
    this.progress.hidden = st === 'cancelled';
    this.bar.style.width = (cls.indexOf('is-indeterminate') >= 0 ? 35 : pct) + '%';
    this.progress.setAttribute('aria-valuenow', String(Math.round(pct)));

    var parts = [];
    if (st === 'active' || st === 'paused') {
      parts.push(total ? ZF.bytes(got) + ' of ' + ZF.bytes(total) : (got ? ZF.bytes(got) : 'Connecting\u2026'));
      if (st === 'active' && d.speed > 0) {
        parts.push(ZF.rate(d.speed));
        if (total > got) parts.push(ZF.duration((total - got) / d.speed) + ' left');
      }
    } else if (st === 'done') {
      parts.push(ZF.bytes(total || got));
    }
    ZF.clear(this.meta);
    for (var i = 0; i < parts.length; i++) this.meta.appendChild(el('span', { text: parts[i] }));
    this.meta.hidden = !parts.length;
    this.err.textContent = st === 'error' ? d.error : '';
    this.err.hidden = st !== 'error';

    if (st !== this.key) {
      this.key = st;
      ZF.clear(this.actions);
      var id = d.id;
      var self = this;
      var add = function (icon, label, fn) {
        self.actions.appendChild(ZF.button({ icon: icon, aria: label, title: label, kind: 'ghost', size: 'sm', onclick: fn }));
      };
      if (st === 'active' || st === 'paused') {
        add(st === 'paused' ? 'play' : 'pause', st === 'paused' ? 'Resume' : 'Pause', function () { act(api.downloadPause, id); });
        add('x', 'Cancel', function () { act(api.downloadCancel, id); });
      } else {
        add('x', 'Remove from list', function () { dismissed[id] = true; render(); });
      }
    }
  };

  function act(fn, id) {
    fn(id).then(refresh, function (e) { ZF.toastError(e); });
  }

  function render() {
    var list = [];
    for (var i = 0; i < downloads.length; i++) if (!dismissed[downloads[i].id]) list.push(downloads[i]);
    list.reverse();

    var active = 0;
    for (var a = 0; a < list.length; a++) { var s = stateOf(list[a]); if (s === 'active' || s === 'paused') active++; }
    dom.count.textContent = list.length ? (active ? active + ' active' : ZF.plural(list.length, 'download')) : '';

    var seen = {};
    var prev = null;
    for (var j = 0; j < list.length; j++) {
      var d = list[j];
      seen[d.id] = true;
      var row = rows[d.id] || (rows[d.id] = new DlRow(d));
      row.update(d);
      var expected = prev ? prev.nextSibling : dom.list.firstChild;
      if (expected !== row.node) dom.list.insertBefore(row.node, expected);
      prev = row.node;
    }
    for (var id in rows) {
      if (rows.hasOwnProperty(id) && !seen[id]) {
        if (rows[id].node.parentNode) rows[id].node.parentNode.removeChild(rows[id].node);
        delete rows[id];
      }
    }
    var empty = ZF.$('.empty-inline', dom.list);
    if (!list.length) {
      var msg = loadError ? 'Could not load server downloads: ' + loadError : (loaded ? 'No downloads yet. Paste a link above to download a file directly to the console.' : 'Loading\u2026');
      if (!empty) dom.list.appendChild(el('div', { class: 'empty-inline', text: msg }));
      else empty.textContent = msg;
    } else if (empty) {
      dom.list.removeChild(empty);
    }
    ZF.emit('badge');
  }

  function activeCount() {
    var n = 0;
    for (var i = 0; i < downloads.length; i++) {
      var s = stateOf(downloads[i]);
      if (s === 'active' || s === 'paused') n++;
    }
    return n;
  }

  function refresh() {
    return api.downloadStatus().then(function (res) {
      downloads = (res && res.downloads) || [];
      loaded = true;
      loadError = null;
      render();
    }, function (e) {
      loadError = e.message;
      render();
    });
  }

  function schedule() {
    clearTimeout(timer);
    var delay = visible ? 1000 : (activeCount() ? 5000 : 0);
    if (!delay) return;
    timer = setTimeout(function () {
      if (document.hidden || !api.ready()) { schedule(); return; }
      refresh().then(schedule, schedule);
    }, delay);
  }

  /* ── Session jobs ───────────────────────────────────────────────────── */

  var jobList = new ZF.jobs.JobList(dom.jobsList, 'Uploads, copies, moves and extractions you start in Files appear here.');

  function renderJobs() {
    var all = ZF.jobs.all().slice().reverse();
    jobList.sync(all);
    var running = 0, finished = 0;
    for (var i = 0; i < all.length; i++) { if (all[i].isFinished()) finished++; else running++; }
    dom.jobsCount.textContent = running ? running + ' running' : (all.length ? ZF.plural(all.length, 'operation') : '');
    dom.jobsClear.hidden = !finished;
    ZF.emit('badge');
  }

  dom.jobsClear.addEventListener('click', function () { ZF.jobs.clearFinished(); });
  ZF.on('jobs', renderJobs);
  ZF.on('reconnected', function () { refresh().then(schedule); });

  ZF.views.transfers = {
    title: 'Transfers',
    enter: function () {
      visible = true;
      renderDest();
      renderJobs();
      render();
      refresh().then(schedule, schedule);
    },
    leave: function () {
      visible = false;
      schedule();
    }
  };

  ZF.transfers = { activeCount: activeCount, refresh: function () { return refresh().then(schedule); } };

  renderDest();
  renderJobs();
})(ZF);
