// Small DOM helpers: element builder, modals, menus, toasts.
const $ = (s, r = document) => r.querySelector(s);

function h(tag, attrs = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k.startsWith('on')) el.addEventListener(k.slice(2).toLowerCase(), v);
    else if (k === 'class') el.className = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'value') el.value = v;
    else if (k === 'checked') el.checked = !!v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat(Infinity)) if (k != null && k !== false) el.append(k instanceof Node ? k : String(k));
  return el;
}

const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

function ago(sec) {
  if (!sec) return '';
  const d = Date.now() / 1000 - sec;
  if (d < 60) return 'just now';
  const u = [[31536000, 'y'], [2592000, 'mo'], [604800, 'w'], [86400, 'd'], [3600, 'h'], [60, 'm']];
  for (const [s, n] of u) if (d >= s) return `${Math.floor(d / s)}${n} ago`;
}

// Each modal gets its own overlay so an error box opened from inside a dialog survives that dialog closing.
function modal(title, body, buttons = [], { wide } = {}) {
  const ov = h('div', { class: 'overlay' });
  document.body.append(ov);
  return new Promise(resolve => {
    const close = v => { ov.remove(); document.removeEventListener('keydown', onKey); resolve(v); };
    const onKey = e => { if (e.key === 'Escape' && ov === document.body.lastElementChild) close(null); };
    document.addEventListener('keydown', onKey);
    const m = h('div', { class: 'modal' + (wide ? ' wide' : '') },
      h('header', {}, title, h('button', { class: 'icon', onclick: () => close(null) }, '✕')),
      h('div', { class: 'body' }, body),
      buttons.length ? h('footer', {}, buttons.map(b => h('button', {
        class: b.primary ? 'primary' : b.danger ? 'danger' : '',
        onclick: async () => {
          if (!b.action) return close(b.value);
          try { const v = await b.action(); if (v !== false) close(v ?? b.value); } catch (e) { toast(e.message, true); }
        },
      }, b.label))) : null);
    ov.append(m);
    ov.onclick = e => { if (e.target === ov) close(null); };
    const first = m.querySelector('input, textarea, select');
    if (first) setTimeout(() => first.focus(), 0);
  });
}

const confirmBox = (title, text, label = 'OK', danger) =>
  modal(title, h('p', {}, text), [{ label: 'Cancel', value: false }, { label, value: true, primary: !danger, danger }]);

async function prompt(title, label, value = '') {
  const inp = h('input', { value });
  inp.addEventListener('keydown', e => { if (e.key === 'Enter') e.target.closest('.modal').querySelector('footer .primary').click(); });
  const r = await modal(title, h('label', { class: 'field' }, h('span', {}, label), inp),
    [{ label: 'Cancel' }, { label: 'OK', primary: true, action: () => inp.value }]);
  return r;
}

function menu(x, y, items) {
  const m = $('#menu');
  m.innerHTML = '';
  for (const it of items) {
    if (!it) continue;
    if (it === '-') m.append(h('div', { class: 'sep' }));
    else if (it.header) m.append(h('div', { class: 'mh' }, it.header));
    else if (it.el) m.append(it.el);
    else m.append(h('div', { class: 'mi', onclick: () => { hideMenu(); it.action(); } }, it.label));
  }
  m.hidden = false;
  const r = m.getBoundingClientRect();
  m.style.left = Math.min(x, innerWidth - r.width - 8) + 'px';
  m.style.top = Math.min(y, innerHeight - r.height - 8) + 'px';
  setTimeout(() => document.addEventListener('mousedown', outside), 0);
}
function outside(e) { if (!$('#menu').contains(e.target)) hideMenu(); }
function hideMenu() { $('#menu').hidden = true; document.removeEventListener('mousedown', outside); }
const menuAt = (el, items) => { const r = el.getBoundingClientRect(); menu(r.left, r.bottom + 2, items); };

let logBuf = '';
function status(text, err) {
  const s = $('#status-text');
  s.textContent = text;
  s.style.color = err ? 'var(--del)' : '';
}
function toast(text, err) {
  status(text.split('\n')[0], err);
  logBuf += `\n${err ? 'ERROR: ' : ''}${text}\n`;
  if (err) modal('Error', h('pre', { class: 'log' }, text), [{ label: 'OK', primary: true }]);
}
function showLog() {
  modal('Output', h('pre', { class: 'log' }, logBuf.slice(-50000) || '(empty)'), [{ label: 'Clear', action: () => { logBuf = ''; } }, { label: 'Close', primary: true }], { wide: true });
}

function field(label, input) { return h('label', { class: 'field' }, h('span', {}, label), input); }
