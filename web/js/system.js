/* zftpd web — System view. */
(function (ZF) {
  'use strict';

  var el = ZF.el;
  var api = ZF.api;
  var $ = ZF.byId;

  var dom = {
    updated: $('sys-updated'),
    refresh: $('sys-refresh'),
    stats: $('sys-stats'),
    server: $('sys-server'),
    fanRange: $('fan-range'),
    fanValue: $('fan-value'),
    fanApply: $('fan-apply'),
    netReset: $('net-reset'),
    notifyForm: $('notify-form'),
    notifyText: $('notify-text'),
    procSection: $('sys-proc-section'),
    procCount: $('proc-count'),
    procRefresh: $('proc-refresh'),
    procList: $('proc-list')
  };

  var SUB = dom.updated.textContent;
  var timer = null;
  var visible = false;
  var procs = [];
  var showAllProcs = false;
  var PROC_LIMIT = 25;

  function settle(p) {
    return p.then(function (v) { return { ok: true, value: v }; }, function (e) { return { ok: false, error: e }; });
  }

  /* ── Overview ───────────────────────────────────────────────────────── */

  function card(label, value, unit, meta, pct, levels) {
    var node = el('div', { class: 'stat' }, el('div', { class: 'stat-label', text: label }));
    var v = el('div', { class: 'stat-value' });
    v.appendChild(document.createTextNode(value));
    if (unit) v.appendChild(el('small', { text: unit }));
    node.appendChild(v);
    if (typeof pct === 'number') {
      var lv = levels || [85, 95];
      var cls = pct >= lv[1] ? ' is-danger' : pct >= lv[0] ? ' is-warn' : '';
      node.appendChild(el('div', { class: 'meter' + cls, role: 'meter', 'aria-valuemin': '0', 'aria-valuemax': '100', 'aria-valuenow': String(Math.round(pct)), 'aria-label': label },
        el('span', { style: 'width:' + pct.toFixed(1) + '%' })));
    }
    if (meta) node.appendChild(el('div', { class: 'stat-meta', text: meta }));
    return node;
  }

  function unavailable(label) {
    return card(label, '\u2014', null, 'Not available on this system');
  }

  function splitBytes(n) {
    var s = ZF.bytes(n);
    var i = s.lastIndexOf(' ');
    return i > 0 ? [s.slice(0, i), s.slice(i + 1)] : [s, ''];
  }

  function renderStats(r) {
    ZF.clear(dom.stats);
    var st = r.stats.ok ? r.stats.value : null;
    var ram = r.ram.ok ? r.ram.value : null;
    var sys = r.sys.ok ? r.sys.value : null;

    if (st && st.disk_total > 0) {
      var used = st.disk_total - st.disk_free;
      var pct = ZF.percent(used, st.disk_total);
      var free = splitBytes(st.disk_free);
      dom.stats.appendChild(card('Storage', free[0], free[1] + ' free', Math.round(pct) + '% of ' + ZF.bytes(st.disk_total) + ' used' + (st.disk_path ? ' \u00b7 ' + st.disk_path : ''), pct));
    } else {
      dom.stats.appendChild(unavailable('Storage'));
    }

    if (ram && ram.total > 0) {
      var rp = ZF.percent(ram.used, ram.total);
      var ru = splitBytes(ram.used);
      var extra = [];
      if (ram.cached) extra.push(ZF.bytes(ram.cached) + ' cached');
      if (ram.free) extra.push(ZF.bytes(ram.free) + ' free');
      dom.stats.appendChild(card('Memory', ru[0], ru[1] + ' of ' + ZF.bytes(ram.total), extra.join(' \u00b7 '), rp, [80, 92]));
    } else {
      dom.stats.appendChild(unavailable('Memory'));
    }

    var temp = sys && typeof sys.cpu_temp === 'number' ? sys.cpu_temp : (st && typeof st.cpu_temp === 'number' ? st.cpu_temp : null);
    if (temp !== null && temp > 0) {
      dom.stats.appendChild(card('CPU temperature', String(Math.round(temp)), '\u00b0C',
        'Fan threshold ' + (ZF.settings.fanThreshold || 60) + ' \u00b0C', Math.min(100, temp), [75, 85]));
    } else {
      dom.stats.appendChild(unavailable('CPU temperature'));
    }

    if (sys && sys.uptime_seconds > 0) {
      var since = sys.boot_epoch ? new Date(sys.boot_epoch * 1000) : null;
      dom.stats.appendChild(card('Uptime', ZF.duration(sys.uptime_seconds), null,
        since && !isNaN(since.getTime()) ? 'Since ' + since.toLocaleString() : null));
    } else {
      dom.stats.appendChild(unavailable('Uptime'));
    }
  }

  function renderServer(status) {
    ZF.clear(dom.server);
    function row(k, v) {
      dom.server.appendChild(el('dt', { text: k }));
      dom.server.appendChild(el('dd', null, v));
    }
    var s = status || api.info || {};
    row('Version', s.version ? 'zftpd ' + s.version : '\u2014');
    row('Address', el('span', { class: 'mono', text: location.host }));
    row('Process ID', s.pid ? String(s.pid) : '\u2014');
    row('Instance', el('span', { class: 'mono', text: s.instance_id || '\u2014' }));
    row('Changes from browser', api.csrf() ? 'Allowed' : 'Disabled in this build (read-only)');
  }

  function refresh() {
    clearTimeout(timer);
    return Promise.all([
      settle(api.stats('/')),
      settle(api.ram()),
      settle(api.system()),
      settle(api.status())
    ]).then(function (res) {
      renderStats({ stats: res[0], ram: res[1], sys: res[2] });
      renderServer(res[3].ok ? res[3].value : null);
      var t = new Date();
      dom.updated.textContent = 'Updated ' + t.toLocaleTimeString();
    }).then(schedule, schedule);
  }

  function schedule() {
    clearTimeout(timer);
    if (!visible) return;
    timer = setTimeout(function () {
      if (document.hidden || !api.ready()) { schedule(); return; }
      refresh();
    }, 5000);
  }

  /* ── Maintenance ────────────────────────────────────────────────────── */

  function renderFan() {
    dom.fanValue.textContent = dom.fanRange.value + ' \u00b0C';
  }

  dom.fanRange.value = String(ZF.settings.fanThreshold || 60);
  renderFan();
  dom.fanRange.addEventListener('input', renderFan);
  dom.fanApply.addEventListener('click', function () {
    var v = parseInt(dom.fanRange.value, 10);
    dom.fanApply.disabled = true;
    api.fan(v).then(function (res) {
      var applied = res && typeof res.threshold === 'number' ? res.threshold : v;
      ZF.setSetting('fanThreshold', applied);
      dom.fanRange.value = String(applied);
      renderFan();
      ZF.toast('Fan threshold set to ' + applied + ' \u00b0C', { type: 'success' });
    }, function (e) {
      ZF.toastError(e, 'Could not set the fan threshold');
    }).then(function () { dom.fanApply.disabled = false; });
  });

  dom.netReset.addEventListener('click', function () {
    ZF.confirm({
      title: 'Restart network listeners?',
      message: 'The FTP listening sockets are recreated. Connected FTP clients are disconnected and need to reconnect. This page stays connected.',
      confirmLabel: 'Restart'
    }).then(function (ok) {
      if (!ok) return;
      api.networkReset().then(function (res) {
        ZF.toast((res && res.message) || 'Network listeners restarted', { type: 'success' });
      }, function (e) { ZF.toastError(e, 'Restart failed'); });
    });
  });

  dom.notifyForm.addEventListener('submit', function (e) {
    e.preventDefault();
    var text = dom.notifyText.value.trim();
    if (!text) { dom.notifyText.focus(); return; }
    api.notify(text).then(function () {
      dom.notifyText.value = '';
      ZF.toast('Notification sent to the console', { type: 'success' });
    }, function (err) { ZF.toastError(err, 'Could not send the notification'); });
  });

  /* ── Processes ──────────────────────────────────────────────────────── */

  function loadProcs() {
    return api.processes().then(function (list) {
      procs = list instanceof Array ? list : [];
      renderProcs();
    }, function () {
      procs = [];
      renderProcs();
    });
  }

  function renderProcs() {
    dom.procSection.hidden = !procs.length;
    if (!procs.length) return;
    var list = procs.slice().sort(function (a, b) { return (b.mem_mb || 0) - (a.mem_mb || 0); });
    dom.procCount.textContent = ZF.plural(list.length, 'process', 'processes');
    var shown = showAllProcs ? list : list.slice(0, PROC_LIMIT);

    var tbody = el('tbody');
    for (var i = 0; i < shown.length; i++) {
      (function (p) {
        tbody.appendChild(el('tr', null, [
          el('td', { class: 'col-pid', text: String(p.pid) }),
          el('td', { text: p.name || '', title: p.name || '' }),
          el('td', { class: 'muted', text: p.user || '' }),
          el('td', { class: 'ta-r num', text: typeof p.mem_mb === 'number' ? p.mem_mb.toFixed(1) + ' MB' : '' }),
          el('td', { class: 'muted', text: p.status || '' }),
          el('td', { class: 'ta-r' }, p.killable ? ZF.button({
            label: 'End', size: 'sm', kind: 'ghost', cls: 'btn-danger-text',
            onclick: function () { kill(p); }
          }) : null)
        ]));
      })(shown[i]);
    }
    var table = el('table', { class: 'table proc-table' }, [
      el('colgroup', null, [
        el('col', { style: 'width:84px' }), el('col'), el('col', { style: 'width:120px' }),
        el('col', { style: 'width:110px' }), el('col', { style: 'width:110px' }), el('col', { style: 'width:80px' })
      ]),
      el('thead', null, el('tr', null, [
        el('th', { text: 'PID' }), el('th', { text: 'Name' }), el('th', { text: 'User' }),
        el('th', { class: 'ta-r', text: 'Memory' }), el('th', { text: 'State' }), el('th', null, el('span', { class: 'sr-only', text: 'Actions' }))
      ])),
      tbody
    ]);
    ZF.clear(dom.procList).appendChild(table);
    if (list.length > PROC_LIMIT) {
      dom.procList.appendChild(el('div', { class: 'empty-inline' }, el('button', {
        type: 'button', class: 'link-btn',
        text: showAllProcs ? 'Show top ' + PROC_LIMIT : 'Show all ' + list.length + ' processes',
        onclick: function () { showAllProcs = !showAllProcs; renderProcs(); }
      })));
    }
  }

  function kill(p) {
    ZF.confirm({
      title: 'End \u201c' + p.name + '\u201d?',
      message: 'Process ' + p.pid + ' is terminated immediately. Unsaved data in that process is lost.',
      confirmLabel: 'End process',
      danger: true
    }).then(function (ok) {
      if (!ok) return;
      api.killProcess(p.pid).then(function (res) {
        if (res && res.success === false) throw new Error('The process could not be ended.');
        ZF.toast('Process ' + p.pid + ' ended', { type: 'success' });
        setTimeout(loadProcs, 400);
      }, function (e) { ZF.toastError(e, 'Could not end the process'); });
    });
  }

  dom.refresh.addEventListener('click', function () { refresh(); loadProcs(); });
  dom.procRefresh.addEventListener('click', loadProcs);

  ZF.views.system = {
    title: 'System',
    enter: function () {
      visible = true;
      dom.updated.textContent = SUB;
      renderServer(null);
      refresh();
      loadProcs();
    },
    leave: function () {
      visible = false;
      clearTimeout(timer);
    }
  };
})(ZF);
