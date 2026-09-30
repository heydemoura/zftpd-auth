/* zftpd web — client-side job queue for uploads, copy/move and extraction.
 *
 * Jobs run in lanes. The server allows one copy and one extraction at a time,
 * so each lane is sequential; lanes run in parallel with each other.
 */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var P = ZF.path;
  var api = ZF.api;

  var jobs = [];
  var seq = 0;
  var running = { upload: null, copy: null, extract: null };
  var POLL_MS = 400;

  var ERRNO = {
    1: 'Operation not permitted', 2: 'No such file or directory', 5: 'I/O error',
    13: 'Permission denied', 17: 'Already exists', 18: 'Cross-device link',
    20: 'Not a directory', 21: 'Is a directory', 27: 'File too large',
    28: 'Not enough free space', 30: 'Read-only file system', 36: 'Name too long',
    63: 'Name too long', 66: 'Directory not empty'
  };

  var KIND = {
    upload: { icon: 'upload', verb: 'Uploading', lane: 'upload' },
    copy: { icon: 'copy', verb: 'Copying', lane: 'copy' },
    move: { icon: 'move', verb: 'Moving', lane: 'copy' },
    extract: { icon: 'archive', verb: 'Extracting', lane: 'extract' },
    dump: { icon: 'download', verb: 'Dumping', lane: 'dump' }
  };

  var STATE_LABEL = {
    queued: 'Queued', active: 'In progress', paused: 'Paused',
    done: 'Done', error: 'Failed', cancelled: 'Cancelled'
  };

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  function cancelledError() {
    var e = new Error('Cancelled');
    e.cancelled = true;
    return e;
  }

  /* ── Job model ──────────────────────────────────────────────────────── */

  function Job(spec) {
    this.id = ++seq;
    this.kind = spec.kind;
    this.lane = KIND[spec.kind].lane;
    this.title = spec.title;
    this.detail = spec.detail || '';
    this.dest = spec.dest || null;
    this.state = 'queued';
    this.paused = false;
    this.loaded = 0;
    this.total = spec.total || 0;
    this.fraction = null;
    this.indeterminate = false;
    this.speed = 0;
    this.error = '';
    this.summary = '';
    this.canPause = !!spec.canPause;
    this.canCancel = spec.canCancel !== false;
    this.cancelRequested = false;
    this.interrupted = false;
    this.created = Date.now();
    this.runner = spec.run;
    this.onCancel = null;
    this._sampleT = 0;
    this._sampleB = 0;
  }

  Job.prototype.progress = function (loaded, total) {
    var now = Date.now();
    if (typeof total === 'number' && total > 0) this.total = total;
    if (!this._sampleT) { this._sampleT = now; this._sampleB = loaded; }
    var dt = now - this._sampleT;
    if (dt >= 800) {
      var inst = Math.max(0, (loaded - this._sampleB) * 1000 / dt);
      this.speed = this.speed ? this.speed * 0.6 + inst * 0.4 : inst;
      this._sampleT = now;
      this._sampleB = loaded;
    }
    this.loaded = loaded;
    changed();
  };

  Job.prototype.percent = function () {
    if (this.state === 'done') return 100;
    if (this.fraction !== null) return Math.max(0, Math.min(100, this.fraction * 100));
    return ZF.percent(this.loaded, this.total);
  };

  Job.prototype.isFinished = function () {
    return this.state === 'done' || this.state === 'error' || this.state === 'cancelled';
  };

  Job.prototype.checkCancel = function () {
    if (this.interrupted) throw new Error('zftpd restarted while this operation was running.');
    if (this.cancelRequested) throw cancelledError();
  };

  function add(spec) {
    var job = new Job(spec);
    jobs.push(job);
    pump();
    showTray();
    changed();
    return job;
  }

  function pump() {
    for (var lane in running) {
      if (!running.hasOwnProperty(lane) || running[lane]) continue;
      for (var i = 0; i < jobs.length; i++) {
        if (jobs[i].lane === lane && jobs[i].state === 'queued') { start(jobs[i]); break; }
      }
    }
  }

  function start(job) {
    running[job.lane] = job;
    job.state = 'active';
    job.started = Date.now();
    changed();
    var p;
    try { p = Promise.resolve(job.runner(job)); } catch (e) { p = Promise.reject(e); }
    p.then(function (summary) {
      if (job.cancelRequested) job.state = 'cancelled';
      else if (job.error) job.state = 'error';
      else job.state = 'done';
      if (summary) job.summary = summary;
    }, function (err) {
      if (err && err.cancelled) job.state = 'cancelled';
      else {
        job.state = 'error';
        job.error = (err && err.message) || 'Failed';
      }
    }).then(function () {
      job.ended = Date.now();
      job.speed = 0;
      job.paused = false;
      running[job.lane] = null;
      if (job.dest) ZF.emit('fs-changed', { dirs: job.touched || [job.dest] });
      if (job.state === 'error' && (trayHidden || trayCollapsed)) {
        ZF.toast(job.title + ': ' + job.error, { type: 'error' });
      }
      changed();
      pump();
    });
  }

  function cancel(job) {
    if (job.isFinished()) return;
    job.cancelRequested = true;
    if (job.state === 'queued') {
      job.state = 'cancelled';
      job.ended = Date.now();
      changed();
      return;
    }
    if (job.onCancel) {
      try { job.onCancel(); } catch (e) { /* ignore */ }
    }
    changed();
  }

  function togglePause(job) {
    if (!job.canPause || job.state !== 'active') return;
    api.copyPause().then(function (res) {
      job.paused = !!(res && res.paused);
      changed();
    }, function (e) { ZF.toastError(e, 'Could not pause'); });
  }

  function dismiss(job) {
    if (!job.isFinished()) return;
    if (job.kind === 'dump') dismissedDumpId = job.serverDumpId;
    var i = jobs.indexOf(job);
    if (i >= 0) jobs.splice(i, 1);
    changed();
  }

  function clearFinished() {
    for (var i = jobs.length - 1; i >= 0; i--) if (jobs[i].isFinished()) {
      if (jobs[i].kind === 'dump') dismissedDumpId = jobs[i].serverDumpId;
      jobs.splice(i, 1);
    }
    changed();
  }

  var dismissedDumpId = 0;
  function observeDump(st) {
    if (!st || !st.active || st.id === dismissedDumpId) return;
    var job = null;
    for (var i = 0; i < jobs.length; i++) {
      if (jobs[i].kind === 'dump' && jobs[i].serverDumpId === st.id) { job = jobs[i]; break; }
    }
    if (!job) {
      job = new Job({ kind: 'dump', title: 'Dump ' + (st.title_id || ''), canCancel: true });
      job.serverDumpId = st.id;
      job.state = 'active';
      jobs.push(job);
      showTray();
    }
    job.onCancel = function () {
      /* Errors are ignored: the next poll reports the real state. */
      api.dumpCancel(job.serverDumpId).then(null, function () { /* ignore */ });
    };
    job.title = 'Dump ' + (st.title_id || '');
    job.detail = st.state === 'launching' ? 'Launching game' :
      st.state === 'ready' ? 'Ready to download' :
      st.to_console ? 'Saving on console' : 'Sending to this PC';
    job.indeterminate = !st.size && st.state !== 'done';
    if (st.state === 'running' || st.state === 'done')
      job.progress(st.state === 'done' ? st.size : (st.bytes_done || 0), st.size || 0);
    if (st.state === 'done') { job.state = 'done'; job.summary = 'Dump complete'; }
    else if (st.state === 'failed') {
      if (st.cancelled) { job.state = 'cancelled'; job.error = ''; }
      else { job.state = 'error'; job.error = st.message || 'Dump failed'; }
    } else job.state = 'active';
    changed();
  }

  function activeCount() {
    var n = 0;
    for (var i = 0; i < jobs.length; i++) if (!jobs[i].isFinished()) n++;
    return n;
  }

  function uploading() {
    for (var i = 0; i < jobs.length; i++) {
      if (jobs[i].kind === 'upload' && !jobs[i].isFinished()) return true;
    }
    return false;
  }

  /* Render at most every 200 ms. */
  var renderTimer = null;
  function changed() {
    if (renderTimer) return;
    renderTimer = setTimeout(function () {
      renderTimer = null;
      renderTray();
      ZF.emit('jobs', jobs);
    }, 200);
  }

  /* ── Server-side polling ────────────────────────────────────────────── */

  /* Poll `fetcher` until `isDone(state)`; transient network errors are retried. */
  function poll(job, fetcher, onState, isDone) {
    function step() {
      if (job.interrupted) return Promise.reject(new Error('zftpd restarted while this operation was running.'));
      return fetcher().then(function (st) {
        onState(st);
        if (isDone(st)) return st;
        return sleep(POLL_MS).then(step);
      }, function (err) {
        if (err && err.network) return sleep(1000).then(step);
        throw err;
      });
    }
    return step();
  }

  /* ── Conflict handling ──────────────────────────────────────────────── */

  /** Resolves to 'replace', 'skip' or null (cancelled). */
  function askConflicts(names, dest, existing) {
    var many = names.length > 1;
    var dirs = 0;
    for (var i = 0; i < names.length; i++) if (existing[names[i]] === 'directory') dirs++;
    var effect = dirs === 0 ? (many ? 'Replacing overwrites the existing files.' : 'Replacing overwrites the existing file.')
      : dirs === names.length ? 'Replacing merges the folders: files with the same name are overwritten.'
      : 'Replacing overwrites files and merges folders.';
    return ZF.dialog({
      title: many ? names.length + ' items already exist' : '\u201c' + names[0] + '\u201d already exists',
      content: el('div', null, [
        el('p', { text: 'The destination ' + dest + ' already contains ' + (many ? 'items with these names' : 'an item with this name') + '. ' + effect }),
        many ? el('ul', { class: 'name-list' }, names.slice(0, 8).map(function (n) { return el('li', { text: n }); })
          .concat(names.length > 8 ? [el('li', { class: 'subtle', text: 'and ' + (names.length - 8) + ' more' })] : [])) : null
      ]),
      actions: [
        { id: 'cancel', label: 'Cancel' },
        { id: 'skip', label: many ? 'Skip these' : 'Skip' },
        { id: 'replace', label: 'Replace', kind: 'primary', submit: true }
      ]
    }).promise.then(function (id) {
      if (id === 'replace' || id === 'skip') return id;
      return null;
    });
  }

  function existingNames(dir) {
    return api.list(dir).then(function (res) {
      var map = {};
      var e = res.entries || [];
      for (var i = 0; i < e.length; i++) map[e[i].name] = e[i].type;
      return map;
    });
  }

  /* ── Upload ─────────────────────────────────────────────────────────── */

  function badName(name) {
    return !name || /[\/\\]/.test(name) || name.indexOf('..') >= 0 || name.length > 255;
  }

  /**
   * items: [{ file: File, rel: 'folder/sub/name.ext' }]
   * dirs:  optional relative directory paths (to recreate empty folders)
   */
  function upload(dest, items, dirs) {
    dest = P.norm(dest);
    dirs = dirs || [];
    if (!items.length && !dirs.length) return Promise.resolve(null);

    var rejected = [];
    var ok = [];
    for (var i = 0; i < items.length; i++) {
      var segs = items[i].rel.split('/');
      var bad = false;
      for (var s = 0; s < segs.length; s++) if (badName(segs[s])) bad = true;
      if (bad) rejected.push(items[i].rel); else ok.push(items[i]);
    }
    if (rejected.length) {
      ZF.toast(ZF.plural(rejected.length, 'item') + ' skipped: names containing ".." or slashes are not allowed by the server.', { type: 'warning', timeout: 8000 });
    }
    if (!ok.length && !dirs.length) return Promise.resolve(null);

    return existingNames(dest).then(function (existing) {
      var tops = {};
      var conflicts = [];
      function noteTop(rel) {
        var top = rel.split('/')[0];
        if (!tops[top]) {
          tops[top] = true;
          if (existing.hasOwnProperty(top)) conflicts.push(top);
        }
      }
      for (var k = 0; k < ok.length; k++) noteTop(ok[k].rel);
      for (var d = 0; d < dirs.length; d++) noteTop(dirs[d]);
      if (!conflicts.length) return 'replace';
      return askConflicts(conflicts, dest, existing).then(function (choice) {
        if (choice === 'skip') {
          var skip = {};
          for (var c = 0; c < conflicts.length; c++) skip[conflicts[c]] = true;
          ok = ok.filter(function (it) { return !skip[it.rel.split('/')[0]]; });
          dirs = dirs.filter(function (r) { return !skip[r.split('/')[0]]; });
        }
        return choice;
      });
    }, function () { return 'replace'; }).then(function (choice) {
      if (!choice || (!ok.length && !dirs.length)) return null;
      var total = 0;
      for (var t = 0; t < ok.length; t++) total += ok[t].file.size || 0;
      var topNames = {};
      var topCount = 0;
      for (var u = 0; u < ok.length; u++) {
        var top = ok[u].rel.split('/')[0];
        if (!topNames[top]) { topNames[top] = true; topCount++; }
      }
      var title = ok.length === 1 ? ok[0].rel.split('/').pop()
        : (topCount === 1 ? P.base(Object.keys(topNames)[0]) : ZF.plural(ok.length, 'file'));
      return add({
        kind: 'upload',
        title: title,
        detail: 'to ' + dest,
        dest: dest,
        total: total,
        run: function (job) { return runUpload(job, dest, ok, dirs); }
      });
    });
  }

  function runUpload(job, dest, items, dirs) {
    var doneBytes = 0;
    var failures = [];
    var current = null;
    job.onCancel = function () { if (current) current.abort(); };
    job.touched = [dest];

    var sortedDirs = dirs.slice().sort(function (a, b) { return a.split('/').length - b.split('/').length; });

    function mkdirs(i) {
      if (i >= sortedDirs.length) return Promise.resolve();
      job.checkCancel();
      var rel = sortedDirs[i];
      var parent = rel.lastIndexOf('/') >= 0 ? P.join(dest, rel.slice(0, rel.lastIndexOf('/'))) : dest;
      return api.mkdir(parent, P.base(rel)).then(null, function (e) {
        failures.push(rel + ': ' + e.message);
      }).then(function () { return mkdirs(i + 1); });
    }

    function next(i) {
      if (i >= items.length) return Promise.resolve();
      job.checkCancel();
      var it = items[i];
      var parts = it.rel.split('/');
      var name = parts.pop();
      var dir = parts.length ? P.join(dest, parts.join('/')) : dest;
      job.detail = items.length > 1 ? (i + 1) + ' of ' + items.length + ' \u00b7 ' + it.rel : 'to ' + dest;
      changed();
      var p;
      if (!it.file.size) {
        p = api.createFile(dir, name);
      } else {
        current = api.upload(dir, name, it.file, function (loaded) {
          job.progress(doneBytes + loaded);
        });
        p = current.promise;
      }
      return p.then(function () {
        current = null;
        doneBytes += it.file.size || 0;
        job.progress(doneBytes);
      }, function (e) {
        current = null;
        if (e && e.cancelled) throw e;
        if (job.cancelRequested) throw cancelledError();
        doneBytes += it.file.size || 0;
        failures.push(it.rel + ': ' + e.message);
        if (e && e.offline) throw e;
      }).then(function () { return next(i + 1); });
    }

    return mkdirs(0).then(function () { return next(0); }).then(function () {
      job.detail = 'to ' + dest;
      if (failures.length) {
        job.error = failures.length === 1 ? failures[0]
          : failures.length + ' of ' + items.length + ' failed. First error: ' + failures[0];
        return null;
      }
      return items.length > 1 ? ZF.plural(items.length, 'file') + ' uploaded' : 'Uploaded';
    });
  }

  /* ── Copy / move ────────────────────────────────────────────────────── */

  function copyErrorMessage(st) {
    if (st.error_errno && ERRNO[st.error_errno]) return ERRNO[st.error_errno];
    if (st.error_errno) return 'Copy failed (errno ' + st.error_errno + ')';
    return 'Copy failed';
  }

  /** Returns a reason string when `src` cannot go into `dest`, else null. */
  function transferBlocker(srcs, dest, mode) {
    for (var i = 0; i < srcs.length; i++) {
      if (P.parent(srcs[i]) === dest) {
        return mode === 'move' ? 'The selected items are already in this folder.'
          : 'Choose a different folder: the items are already here.';
      }
      if (P.within(dest, srcs[i])) return 'A folder cannot be ' + (mode === 'move' ? 'moved' : 'copied') + ' into itself.';
    }
    return null;
  }

  /** mode: 'copy' | 'move'. srcs: absolute paths. */
  function transfer(mode, srcs, dest) {
    dest = P.norm(dest);
    var reason = transferBlocker(srcs, dest, mode);
    if (reason) { ZF.toast(reason, { type: 'warning' }); return Promise.resolve(null); }

    return existingNames(dest).then(function (existing) {
      var conflicts = [];
      for (var i = 0; i < srcs.length; i++) if (existing.hasOwnProperty(P.base(srcs[i]))) conflicts.push(P.base(srcs[i]));
      if (!conflicts.length) return srcs;
      return askConflicts(conflicts, dest, existing).then(function (choice) {
        if (!choice) return null;
        if (choice === 'replace') return srcs;
        return srcs.filter(function (s) { return conflicts.indexOf(P.base(s)) < 0; });
      });
    }).then(function (list) {
      if (!list || !list.length) return null;
      return add({
        kind: mode,
        title: list.length === 1 ? P.base(list[0]) : ZF.plural(list.length, 'item'),
        detail: 'to ' + dest,
        dest: dest,
        canPause: true,
        run: function (job) { return runTransfer(job, mode, list, dest); }
      });
    });
  }

  function runTransfer(job, mode, srcs, dest) {
    var doneBytes = 0;
    var failures = [];
    job.touched = [dest];
    for (var t = 0; t < srcs.length; t++) {
      if (mode === 'move' && job.touched.indexOf(P.parent(srcs[t])) < 0) job.touched.push(P.parent(srcs[t]));
    }
    job.onCancel = function () { api.copyCancel().then(null, function () { /* ignore */ }); };

    function one(i) {
      if (i >= srcs.length) return Promise.resolve();
      job.checkCancel();
      var src = srcs[i];
      job.detail = srcs.length > 1 ? (i + 1) + ' of ' + srcs.length + ' \u00b7 ' + P.base(src) : 'to ' + dest;
      changed();
      return api.copy(src, dest, mode === 'move').then(function () {
        return poll(job, api.copyProgress, function (st) {
          job.paused = !!st.paused;
          var cur = st.bytes_copied || 0;
          var tot = st.total_bytes || 0;
          job.fraction = srcs.length > 1 ? (i + (tot ? cur / tot : 0)) / srcs.length : (tot ? cur / tot : 0);
          job.total = doneBytes + tot;
          job.progress(doneBytes + cur);
        }, function (st) { return !st.active; });
      }).then(function (st) {
        doneBytes += st.total_bytes || st.bytes_copied || 0;
        if (job.cancelRequested) throw cancelledError();
        if (st.error) throw new Error(copyErrorMessage(st));
      }).then(null, function (e) {
        if (e && e.cancelled) throw e;
        failures.push(P.base(src) + ': ' + e.message);
        if (e && e.offline) throw e;
      }).then(function () { return one(i + 1); });
    }

    return one(0).then(function () {
      job.detail = 'to ' + dest;
      if (failures.length) {
        job.error = failures.length === 1 ? failures[0]
          : failures.length + ' of ' + srcs.length + ' failed. First error: ' + failures[0];
        return null;
      }
      return mode === 'move' ? 'Moved' : 'Copied';
    });
  }

  /* ── Extract ────────────────────────────────────────────────────────── */

  /** Extracts `archive` into `dest`. When `newFolder` is set, it is created inside `dest` first. */
  function extract(archive, dest, newFolder) {
    dest = P.norm(dest);
    var target = newFolder ? P.join(dest, newFolder) : dest;
    return Promise.resolve(add({
      kind: 'extract',
      title: P.base(archive),
      detail: 'to ' + target,
      dest: target,
      run: function (job) {
        job.touched = [dest, target];
        job.indeterminate = true;
        job.onCancel = function () { api.extractCancel().then(null, function () { /* ignore */ }); };
        var prep = newFolder ? api.mkdir(dest, newFolder) : Promise.resolve();
        return prep.then(function () {
          job.checkCancel();
          return api.extract(archive, target);
        }).then(function () {
          return poll(job, api.extractProgress, function (st) {
            if (st.bytes_extracted > 0 && st.total_bytes > 0) {
              job.indeterminate = false;
              job.progress(st.bytes_extracted, st.total_bytes);
            } else {
              changed();
            }
          }, function (st) { return !st.active; });
        }).then(function (st) {
          if (st.cancelled || job.cancelRequested) throw cancelledError();
          if (st.error) throw new Error(st.error_msg || 'Extraction failed');
          job.indeterminate = false;
          return 'Extracted';
        }, function (e) {
          if (e && e.status === 415) throw new Error('This build can only extract .zip archives.');
          if (e && e.status === 409) throw new Error('Another extraction is already running on the server.');
          throw e;
        });
      }
    }));
  }

  /* Daemon restarted: anything that depends on server state is lost. */
  ZF.on('reconnected', function (info) {
    if (!info || !info.restarted) return;
    var n = 0;
    for (var i = 0; i < jobs.length; i++) {
      if (!jobs[i].isFinished() && jobs[i].kind !== 'upload') { jobs[i].interrupted = true; n++; }
    }
    if (n) changed();
  });

  window.addEventListener('beforeunload', function (e) {
    if (!uploading()) return undefined;
    var msg = 'Uploads are still running. Leaving this page will stop them.';
    e.returnValue = msg;
    return msg;
  });

  /* ── Rendering ──────────────────────────────────────────────────────── */

  function stateOf(job) {
    if (job.state === 'active' && job.paused) return 'paused';
    return job.state;
  }

  function metaParts(job) {
    var parts = [];
    var st = stateOf(job);
    if (job.state === 'done') {
      if (job.summary) parts.push(job.summary);
      if (job.loaded > 0) parts.push(ZF.bytes(job.loaded));
      return parts;
    }
    if (job.isFinished()) return parts;
    if (job.state === 'queued') return ['Waiting'];
    if (job.indeterminate) return ['Working\u2026'];
    if (job.total > 0) parts.push(ZF.bytes(job.loaded) + ' of ' + ZF.bytes(job.total));
    else if (job.loaded > 0) parts.push(ZF.bytes(job.loaded));
    if (st === 'active' && job.speed > 0) {
      parts.push(ZF.rate(job.speed));
      if (job.kind === 'upload' && job.total > job.loaded) parts.push(ZF.duration((job.total - job.loaded) / job.speed) + ' left');
    }
    return parts;
  }

  function Row(job, onAction) {
    this.job = job;
    this.icon = el('div', { class: 'xfer-icon' }, ZF.icon(KIND[job.kind].icon));
    this.name = el('div', { class: 'xfer-name' });
    this.status = el('span', { class: 'status' });
    this.sub = el('div', { class: 'xfer-sub' });
    this.bar = el('span');
    this.progress = el('div', { class: 'progress', role: 'progressbar', 'aria-valuemin': '0', 'aria-valuemax': '100' }, this.bar);
    this.meta = el('div', { class: 'xfer-meta' });
    this.err = el('div', { class: 'xfer-error', role: 'alert' });
    this.actions = el('div', { class: 'xfer-actions' });
    this.actionKey = '';
    this.onAction = onAction;
    this.node = el('div', { class: 'xfer' }, [
      this.icon,
      el('div', { class: 'xfer-main' }, [
        el('div', { class: 'xfer-top' }, [this.name, this.status]),
        this.sub, this.progress, this.meta, this.err
      ]),
      this.actions
    ]);
  }

  Row.prototype.update = function () {
    var job = this.job;
    var st = stateOf(job);
    this.name.textContent = job.title;
    this.name.title = job.title;
    this.status.className = 'status status-' + st;
    this.status.textContent = st === 'active' ? KIND[job.kind].verb : STATE_LABEL[st];
    this.sub.textContent = job.detail;
    this.sub.title = job.detail;

    var pc = job.percent();
    var cls = 'progress';
    if (job.state === 'active' && job.indeterminate) cls += ' is-indeterminate';
    if (job.state === 'done') cls += ' is-done';
    if (job.state === 'error') cls += ' is-error';
    if (st === 'paused' || job.state === 'cancelled' || job.state === 'queued') cls += ' is-paused';
    this.progress.className = cls;
    this.progress.hidden = job.state === 'cancelled' || job.state === 'done';
    this.bar.style.width = (job.indeterminate && job.state === 'active' ? 35 : pc) + '%';
    this.progress.setAttribute('aria-valuenow', String(Math.round(pc)));

    var parts = metaParts(job);
    ZF.clear(this.meta);
    for (var i = 0; i < parts.length; i++) this.meta.appendChild(el('span', { text: parts[i] }));
    this.meta.hidden = !parts.length;
    this.err.textContent = job.state === 'error' ? job.error : '';
    this.err.hidden = job.state !== 'error';

    var key = st + '|' + job.canPause + '|' + !!job.dest;
    if (key !== this.actionKey) {
      this.actionKey = key;
      this.renderActions(st);
    }
  };

  Row.prototype.renderActions = function (st) {
    var job = this.job;
    var self = this;
    ZF.clear(this.actions);
    function btn(icon, label, fn, extra) {
      self.actions.appendChild(ZF.button({ icon: icon, aria: label, title: label, kind: 'ghost', size: 'sm', cls: extra, onclick: fn }));
    }
    if (!job.isFinished()) {
      if (job.canPause && job.state === 'active') {
        btn(st === 'paused' ? 'play' : 'pause', st === 'paused' ? 'Resume' : 'Pause', function () { togglePause(job); });
      }
      if (job.canCancel) btn('x', 'Cancel', function () { cancel(job); });
    } else {
      if (job.dest && job.state !== 'cancelled') {
        btn('folder', 'Show in Files', function () { ZF.emit('open-path', job.dest); });
      }
      btn('x', 'Remove from list', function () { dismiss(job); });
    }
  };

  /** Keeps `container` in sync with `list` without recreating rows. */
  function JobList(container, empty) {
    this.container = container;
    this.rows = {};
    this.empty = empty;
    this.emptyNode = null;
  }

  JobList.prototype.sync = function (list) {
    var seen = {};
    var prev = null;
    for (var i = 0; i < list.length; i++) {
      var job = list[i];
      seen[job.id] = true;
      var row = this.rows[job.id];
      if (!row) row = this.rows[job.id] = new Row(job);
      row.update();
      var expected = prev ? prev.nextSibling : this.container.firstChild;
      if (expected !== row.node) this.container.insertBefore(row.node, expected);
      prev = row.node;
    }
    for (var id in this.rows) {
      if (this.rows.hasOwnProperty(id) && !seen[id]) {
        var n = this.rows[id].node;
        if (n.parentNode) n.parentNode.removeChild(n);
        delete this.rows[id];
      }
    }
    if (this.empty) {
      if (!list.length) {
        if (!this.emptyNode) this.emptyNode = el('div', { class: 'empty-inline', text: this.empty });
        if (this.emptyNode.parentNode !== this.container) this.container.appendChild(this.emptyNode);
      } else if (this.emptyNode && this.emptyNode.parentNode) {
        this.emptyNode.parentNode.removeChild(this.emptyNode);
      }
    }
  };

  /* ── Tray ───────────────────────────────────────────────────────────── */

  var tray = ZF.byId('tray');
  var trayTitle = el('div', { class: 'tray-title' });
  var trayList = el('div', { class: 'tray-list' });
  var trayRows = new JobList(trayList);
  var trayHidden = true;
  var trayCollapsed = false;
  var trayDismissed = {};
  var suppressTray = false;

  var collapseBtn = ZF.button({ icon: 'chevron-down', aria: 'Collapse', kind: 'ghost', size: 'sm', cls: 'tray-toggle' });
  collapseBtn.firstChild.setAttribute('class', 'ic chevron');
  collapseBtn.addEventListener('click', function () {
    trayCollapsed = !trayCollapsed;
    renderTray();
  });
  var closeBtn = ZF.button({
    icon: 'x', aria: 'Close', kind: 'ghost', size: 'sm',
    onclick: function () {
      trayHidden = true;
      for (var i = 0; i < jobs.length; i++) trayDismissed[jobs[i].id] = true;
      renderTray();
    }
  });
  var viewBtn = ZF.button({
    icon: 'transfer', aria: 'Open Transfers', kind: 'ghost', size: 'sm',
    onclick: function () { location.hash = '#/transfers'; }
  });

  tray.appendChild(el('div', { class: 'tray-head' }, [trayTitle, viewBtn, collapseBtn, closeBtn]));
  tray.appendChild(trayList);
  tray.setAttribute('role', 'region');
  tray.setAttribute('aria-label', 'File operations');

  function showTray() {
    trayHidden = false;
    renderTray();
  }

  function renderTray() {
    var visible = [];
    for (var i = 0; i < jobs.length; i++) if (!trayDismissed[jobs[i].id]) visible.push(jobs[i]);
    if (trayHidden || suppressTray || !visible.length) { tray.hidden = true; return; }
    tray.hidden = false;
    tray.className = 'tray' + (trayCollapsed ? ' is-collapsed' : '');
    collapseBtn.setAttribute('aria-label', trayCollapsed ? 'Expand' : 'Collapse');
    collapseBtn.title = trayCollapsed ? 'Expand' : 'Collapse';

    var active = 0, failed = 0;
    var first = null;
    for (var j = 0; j < visible.length; j++) {
      if (!visible[j].isFinished()) { active++; if (!first) first = visible[j]; }
      else if (visible[j].state === 'error') failed++;
    }
    var title;
    if (active === 1) title = KIND[first.kind].verb + ' ' + first.title + (first.indeterminate || first.state !== 'active' ? '' : ' \u00b7 ' + Math.round(first.percent()) + '%');
    else if (active > 1) title = active + ' operations in progress';
    else if (failed) title = ZF.plural(failed, 'operation') + ' failed';
    else title = 'All operations complete';
    trayTitle.textContent = title;
    trayRows.sync(visible.slice().reverse());
  }

  ZF.on('view', function (name) {
    suppressTray = name === 'transfers';
    renderTray();
  });

  ZF.jobs = {
    all: function () { return jobs; },
    activeCount: activeCount,
    upload: upload,
    transfer: transfer,
    transferBlocker: transferBlocker,
    extract: extract,
    observeDump: observeDump,
    cancel: cancel,
    clearFinished: clearFinished,
    JobList: JobList
  };
})(ZF);
