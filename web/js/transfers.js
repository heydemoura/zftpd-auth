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
    dumpSection: $('dump-section'),
    dumpList: $('dump-list'),
    pkgSection: $('pkg-section'),
    pkgList: $('pkg-list'),
    orphanSection: $('orphan-section'),
    orphanCount: $('orphan-count'),
    orphanList: $('orphan-list'),
    jobsCount: $('jobs-count'),
    jobsClear: $('jobs-clear'),
    jobsList: $('jobs-list')
  };

  var installTimer = null;

  var confirmPrompted = '';

  function renderInstall(st) {
    if (!dom.pkgSection) return;

    if (st && st.needs_confirm) {
      var key = st.title_id || st.path;
      if (key && key !== confirmPrompted) {
        confirmPrompted = key;
        ZF.confirm({
          title: 'Already installed',
          message: (st.title_id || st.path) +
            ' is already installed. Replace it with this package?',
          confirmLabel: 'Replace',
          danger: true
        }).then(function (ok) {
          if (!ok) return;
          api.pkgInstall(st.path, st.slot === 1 ? 'extended' : 'internal', true)
            .catch(function (err) { ZF.toastError(err, 'Could not install the package'); });
        });
      }
      dom.pkgSection.hidden = true;
      return;
    }

    if (!st || (!st.active && !st.path)) {
      dom.pkgSection.hidden = true;
      return;
    }

    var failed = !!st.error;
    var completed = !st.active && !failed;
    var pct = completed ? 100 : st.length ? Math.max(0, Math.min(100, Math.round((st.transferred / st.length) * 100)))
                        : (st.progress || 0);
    ZF.clear(dom.pkgList);
    dom.pkgList.appendChild(el('div', { class: 'xfer' }, [
      el('div', { class: 'xfer-top' }, [
        el('div', { class: 'xfer-name', text: 'Install ' + (st.title_id || 'package') }),
        el('span', { class: 'status status-' + (failed ? 'error' : completed ? 'done' : 'active'),
          text: failed ? 'Failed' : (completed ? 'Completed' : 'Installing') })
      ]),
      el('div', { class: 'muted', text: st.path || '' }),
      failed ? el('div', { class: 'xfer-error', text: st.detail || ('Installer error: ' + st.error) }) : el('div', { class: 'progress' + (completed ? ' is-done' : ''),
        role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': String(pct) },
        el('span', { style: 'width:' + pct + '%' })),
      failed ? null : el('div', { class: 'xfer-meta', text: st.length
        ? ZF.bytes(st.transferred || 0) + ' of ' + ZF.bytes(st.length) + ' \u00b7 ' + pct + '%'
        : (st.active && st.detail ? st.detail : pct + '%') })
    ]));
    dom.pkgSection.hidden = false;
  }

  function pollInstall() {
    clearTimeout(installTimer);
    /* Hidden behind the build flag, like the rest of the installer: skip the
     * request (the daemon answers 409 for it) and re-check later. */
    if (!ZF.features.pkgInstall) {
      installTimer = setTimeout(pollInstall, 15000);
      return;
    }
    api.installStatus().then(function (st) {
      renderInstall(st);
      installTimer = setTimeout(pollInstall, st && st.active ? 2000 : 10000);
    }, function () {
      installTimer = setTimeout(pollInstall, 15000);
    });
  }

  pollInstall();

  var HINT = dom.hint.textContent;
  var downloads = [];
  var rows = {};
  var timer = null;
  var visible = false;
  var loaded = false;
  var loadError = null;
  var pendingDeletes = {};
  var orphanTimer = null;
  var orphans = [];
  var dumpSample = null;

  function renderDump(st) {
    if (!st || !st.active) { dom.dumpSection.hidden = true; dumpSample = null; return; }
    var done = st.state === 'done' ? st.size : (st.bytes_done || 0);
    var total = st.size || 0;
    var pct = total ? Math.max(0, Math.min(100, ZF.percent(done, total))) : 0;
    var now = Date.now();
    var speed = dumpSample && dumpSample.id === st.id && now > dumpSample.time &&
      done >= dumpSample.bytes && st.state === 'running'
      ? (done - dumpSample.bytes) * 1000 / (now - dumpSample.time) : 0;
    if (!dumpSample || dumpSample.id !== st.id || done !== dumpSample.bytes)
      dumpSample = { id: st.id, bytes: done, time: now };
    var state = st.state || 'running';
    var label = { launching: 'Launching', ready: 'Ready to download', running: 'Dumping',
      done: 'Completed', failed: 'Failed' }[state] || state;
    var barClass = 'progress' + (state === 'done' ? ' is-done' :
      state === 'failed' ? ' is-error' : !total ? ' is-indeterminate' : '');
    var parts = [total ? ZF.bytes(done) + ' of ' + ZF.bytes(total) : 'Preparing files'];
    if (total) parts.push(Math.round(pct) + '%');
    if (speed > 0) {
      parts.push(ZF.rate(speed));
      if (total > done) parts.push(ZF.duration((total - done) / speed) + ' left');
    }
    ZF.clear(dom.dumpList).appendChild(el('div', { class: 'xfer' }, [
      el('div', { class: 'xfer-icon' }, ZF.icon('download')),
      el('div', { class: 'xfer-main' }, [
        el('div', { class: 'xfer-top' }, [
          el('div', { class: 'xfer-name', text: 'Dump ' + (st.title_id || '') }),
          el('span', { class: 'status status-' + (state === 'failed' ? 'error' :
            state === 'done' ? 'done' : 'active'), text: label })
        ]),
        el('div', { class: 'xfer-sub', text: st.to_console ? 'Saving on console' : 'Sending to this PC' }),
        el('div', { class: barClass, role: 'progressbar', 'aria-valuemin': '0',
          'aria-valuemax': '100', 'aria-valuenow': String(Math.round(pct)) },
          el('span', { style: 'width:' + (total ? pct : 35) + '%' })),
        el('div', { class: 'xfer-meta', text: parts.join(' \u00b7 ') }),
        state === 'failed' ? el('div', { class: 'xfer-error', text: st.message || 'Dump failed' }) : null
      ])
    ]));
    dom.dumpSection.hidden = false;
  }

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
    return /^(https?|ftps?):\/\/[^\s\/?#]+[^\s]*$/i.test(v) ||
      /^magnet:\?[^\s]*xt=urn:bt(ih|mh):[a-z0-9]+[^\s]*$/i.test(v);
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
    if (!validUrl(url)) { setHint('Enter a full HTTP, FTP or magnet link.', true); dom.url.focus(); return; }
    dom.start.disabled = true;
    api.downloadStart(url, dest()).then(function (res) {
      if (res && res.ok === false) throw new Error(res.message || 'Could not start the download');
      dom.url.value = '';
      setHint(null);
      ZF.toast((res && res.queued ? 'Queued' : 'Download started') +
        (res && res.name ? ': ' + res.name : ''), { type: 'success' });
      refresh();
    }, function (err) {
      setHint(err.message, true);
    }).then(function () { dom.start.disabled = false; });
  });

  /* ── Server downloads ───────────────────────────────────────────────── */

  function stateOf(d) {
    if (d.cancelled) return 'cancelled';
    if (d.error) return /cancel/i.test(d.error) ? 'cancelled' : 'error';
    if (d.done) return 'done';
    if (d.paused) return 'paused';
    if (d.queued) return 'queued';
    return 'active';
  }

  var LABEL = { active: 'Downloading', queued: 'Queued', paused: 'Paused', done: 'Done', error: 'Failed', cancelled: 'Cancelled' };

  function DlRow(d) {
    this.id = d.id;
    this.name = el('div', { class: 'xfer-name' });
    this.status = el('span', { class: 'status' });
    this.sub = el('div', { class: 'xfer-sub' });
    this.destination = el('div', { class: 'xfer-sub xfer-destination' });
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
        this.sub, this.destination, this.progress, this.meta, this.err
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
    var path = d.dst ? d.dst.replace(/\/$/, '') + '/' + (d.name || '') : '';
    this.destination.textContent = path ? 'Save to ' + path : '';
    this.destination.title = path;
    this.destination.hidden = !path;

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
    if (st === 'queued') {
      parts.push(d.queue_position > 1 ? 'Waiting \u00b7 position ' + d.queue_position : 'Next in queue');
      if (got) parts.push(ZF.bytes(got) + ' already saved');
    } else if (st === 'active' || st === 'paused') {
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

    var actionKey = st + '|' + (d.dst || '') + '|' + (d.name || '');
    if (actionKey !== this.key) {
      this.key = actionKey;
      ZF.clear(this.actions);
      var id = d.id;
      var self = this;
      var add = function (icon, label, fn) {
        self.actions.appendChild(ZF.button({ icon: icon, aria: label, title: label, kind: 'ghost', size: 'sm', onclick: fn }));
      };
      if (st === 'active' || st === 'paused' || st === 'queued') {
        add(st === 'paused' ? 'play' : 'pause', st === 'paused' ? 'Resume' : 'Pause', function () { act(api.downloadPause, id); });
      } else if (st === 'error') {
        add('play', 'Retry download', function () { act(api.downloadRetry, id); });
      }
      if (st !== 'done') {
        add('refresh', 'Start over from zero', function () { deleteDownload(d, true); });
      } else if (d.dst && d.name) {
        add('folder', 'Show in Files', function () {
          ZF.files.reveal(ZF.path.join(d.dst, d.name));
        });
      }
      add('trash', st === 'done' ? 'Remove from list' : 'Delete download and partial file',
        function () { deleteDownload(d, false); });
    }
  };

  function waitUntilRemoved(id, attempts) {
    return api.downloadStatus().then(function (res) {
      var items = (res && res.downloads) || [];
      for (var i = 0; i < items.length; i++) {
        if (items[i].id === id) {
          if (attempts <= 0) throw new Error('Deletion is still in progress. Try again shortly.');
          return new Promise(function (resolve) { setTimeout(resolve, 500); })
            .then(function () { return waitUntilRemoved(id, attempts - 1); });
        }
      }
    });
  }

  function deleteDownload(d, restart) {
    if (pendingDeletes[d.id]) return;
    var complete = stateOf(d) === 'done';
    var question = complete
      ? Promise.resolve(true)
      : ZF.confirm({
          title: restart ? 'Start download from zero?' : 'Delete unfinished download?',
          message: 'This removes the saved partial file for ' + (d.name || 'this download') +
            (restart ? ' and starts the same link again from zero.' : '. You can then start it again from zero.'),
          confirmLabel: restart ? 'Start over' : 'Delete download',
          danger: true
        });
    question.then(function (confirmed) {
      if (!confirmed) return;
      pendingDeletes[d.id] = true;
      return api.downloadDelete(d.id).then(function () {
        if (!restart) {
          ZF.toast(complete ? 'Removed from list' : 'Partial download deleted', { type: 'success' });
          return refresh();
        }
        return waitUntilRemoved(d.id, 120).then(function () {
          return api.downloadStart(d.url, d.dst);
        }).then(function () {
          ZF.toast('Download restarted from zero: ' + (d.name || 'file'), { type: 'success' });
          return refresh();
        });
      }).then(function () {
        delete pendingDeletes[d.id];
      }, function (e) {
        delete pendingDeletes[d.id];
        ZF.toastError(e);
        refresh();
      });
    });
  }

  function act(fn, id) {
    fn(id).then(refresh, function (e) { ZF.toastError(e); });
  }

  function render() {
    var list = downloads.slice().reverse();

    var active = 0;
    for (var a = 0; a < list.length; a++) { var s = stateOf(list[a]); if (s === 'active' || s === 'paused' || s === 'queued') active++; }
    dom.count.textContent = list.length ? (active ? active + ' unfinished' : ZF.plural(list.length, 'download')) : '';

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
      if (s === 'active' || s === 'paused' || s === 'queued') n++;
    }
    return n;
  }

  function refresh() {
    var dumpRequest = api.dumpStatus().then(renderDump, function () {});
    return api.downloadStatus().then(function (res) {
      downloads = (res && res.downloads) || [];
      loaded = true;
      loadError = null;
      render();
    }, function (e) {
      loadError = e.message;
      render();
    }).then(function () { return dumpRequest; });
  }

  function renderOrphans(scanning) {
    dom.orphanSection.hidden = !orphans.length && !scanning;
    dom.orphanCount.textContent = scanning ? 'Scanning storage\u2026' :
      ZF.plural(orphans.length, 'partial file');
    ZF.clear(dom.orphanList);
    for (var i = 0; i < orphans.length; i++) {
      (function (item) {
        var name = ZF.path.base(item.path).replace(/\.zftpd\.part$/, '');
        var folder = ZF.path.parent(item.path) || '/';
        var actions = el('div', { class: 'xfer-actions' }, [
          ZF.button({ icon: 'folder', aria: 'Show in Files', title: 'Show in Files',
            kind: 'ghost', size: 'sm', onclick: function () { ZF.files.reveal(item.path); } }),
          ZF.button({ icon: 'trash', aria: 'Delete partial file', title: 'Delete partial file',
            kind: 'ghost', size: 'sm', onclick: function () {
              ZF.confirm({ title: 'Delete partial download?',
                message: 'Delete ' + item.path + '? These saved bytes cannot be recovered.',
                confirmLabel: 'Delete partial file', danger: true }).then(function (yes) {
                  if (!yes) return;
                  api.downloadOrphanDelete(item.path).then(function () {
                    ZF.toast('Partial file deleted', { type: 'success' });
                    refreshOrphans();
                  }, ZF.toastError);
                });
            } })
        ]);
        dom.orphanList.appendChild(el('div', { class: 'xfer' }, [
          el('div', { class: 'xfer-icon' }, ZF.icon('download')),
          el('div', { class: 'xfer-main' }, [
            el('div', { class: 'xfer-top' }, [
              el('div', { class: 'xfer-name', text: name }),
              el('span', { class: 'status status-paused', text: 'Link missing' })
            ]),
            el('div', { class: 'xfer-sub', text: item.path }),
            el('div', { class: 'xfer-meta', text: ZF.bytes(item.size) +
              ' saved \u00b7 paste the original URL above and choose ' + folder + ' to resume' })
          ]), actions
        ]));
      })(orphans[i]);
    }
  }

  function refreshOrphans() {
    clearTimeout(orphanTimer);
    return api.downloadOrphans().then(function (res) {
      orphans = res.orphans || [];
      renderOrphans(!!res.scanning);
      if (visible) orphanTimer = setTimeout(refreshOrphans, res.scanning ? 2000 : 300000);
    }, function (err) {
      if (visible) orphanTimer = setTimeout(refreshOrphans, 30000);
      ZF.toastError(err);
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
  ZF.on('reconnected', function () { refresh().then(schedule); if (visible) refreshOrphans(); });

  ZF.views.transfers = {
    title: 'Transfers',
    enter: function () {
      visible = true;
      renderDest();
      renderJobs();
      render();
      refresh().then(schedule, schedule);
      refreshOrphans();
    },
    leave: function () {
      visible = false;
      clearTimeout(orphanTimer);
      schedule();
    }
  };

  ZF.transfers = { activeCount: activeCount, refresh: function () { return refresh().then(schedule); } };

  renderDest();
  renderJobs();
})(ZF);
