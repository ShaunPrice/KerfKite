// The assistant's workshop. See workshop.hpp.
//
// The Python workshop drove the editor through Playwright; this one speaks the DevTools protocol itself. Where an
// answer depends on how a driver behaves (when a control counts as visible, how a click finds its point, which
// key events a chord makes), the behaviour here is Playwright's as its documentation describes it, so that the
// assistant sees the same editor through either server.
#include "workshop.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "browser.hpp"
#include "http.hpp"
#include "kerfkite/errors.hpp"
#include "os_crypto.hpp"
#include "os_random.hpp"
#include "studio.hpp"

namespace kerfkite {
namespace server {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

const size_t kLimit = 8 * 1024 * 1024;
const size_t kStorageLimit = 32 * 1024 * 1024;
// Only these application documents can become top-level browser pages.
const std::set<std::string> kPages = {"/", "/index.html", "/materials-guide.html", "/help.html"};
// The empty page the saved storage is put back from before the editor opens, served by the workshop itself and
// only while it does that.
const char *const kStoragePage = "/__kerfkite_workshop_storage";
// Each element action has this long to find its control ready, as Playwright's timeout in the Python workshop.
const auto kActionTime = std::chrono::milliseconds(4500);

const char *const kStateScript = R"js(() => ({
  page:document.title, workshop:typeof KerfKiteViews!=='undefined'?KerfKiteViews.current():null,
  project:window.projectMetadata?.name, objects:typeof objects==='undefined'?null:objects.length,
  featureNames:window.CNCSolidExchange?.model().features.map(f=>f.id),
  selection:typeof selected==='number'&&objects[selected]?objects[selected].name:null,
  dialog:document.querySelector('dialog[open]')?.id,
  feedback:['message','cad-command-result','cad-script-feedback','cad-macro-status','solid-status','summary']
    .map(id=>document.getElementById(id)).filter(e=>e&&e.getClientRects().length&&e.textContent)
    .map(e=>[e.id,e.textContent.slice(0,300)]),
  tabs:[...document.querySelectorAll('[role=tab][aria-selected=true]')].map(b=>b.id)
}))js";

// A page's answer as the Python workshop received it from Playwright: undefined (also as a property or an
// array element) is null and keeps its place, a number that is not finite is null, a node is named rather than
// copied; text that is not well-formed UTF-16 has its lone surrogates replaced, so that the answer is JSON.
const char *const kPlain = R"js((value => {
  const seen = new Set();
  const text = s => typeof s.toWellFormed === 'function' ? s.toWellFormed() : s.replace(/[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g, '�');
  const walk = v => {
    if (v === undefined || v === null || typeof v === 'function' || typeof v === 'symbol') return null;
    if (typeof v === 'number') return Number.isFinite(v) ? v : null;
    if (typeof v === 'bigint') return Number(v);
    if (typeof v === 'string') return text(v);
    if (typeof v !== 'object') return v;
    if (typeof Window === 'function' && v instanceof Window) return 'ref: <Window>';
    if (typeof Document === 'function' && v instanceof Document) return 'ref: <Document>';
    if (typeof Node === 'function' && v instanceof Node) return 'ref: <Node>';
    if (v instanceof Date) return Number.isNaN(v.getTime()) ? null : v.toJSON();
    if (v instanceof RegExp || v instanceof Error) return text(String(v));
    if (ArrayBuffer.isView(v) && !(v instanceof DataView)) return Array.from(v, x => walk(x));
    if (seen.has(v)) throw new TypeError('Circular reference detected');
    seen.add(v);
    let out;
    if (Array.isArray(v)) {
      out = Array.from(v, x => walk(x));
    } else {
      const names = Object.keys(v);
      if (!names.length && typeof v.toJSON === 'function') {
        seen.delete(v);
        return walk(v.toJSON());
      }
      out = {};
      for (const name of names) {
        let x;
        try { x = v[name]; } catch (e) { continue; }
        Object.defineProperty(out, text(name), {value: walk(x), enumerable: true, writable: true, configurable: true});
      }
    }
    seen.delete(v);
    return out;
  };
  return walk(value);
}))js";

// The element driver: the checks an action makes before it acts and the edits it makes, called on the element
// (this) with an operation and its argument. Visible means a box with area and no hidden visibility (an element
// of display:contents is visible when a child is); enabled means not disabled natively, by a disabled fieldset
// or optgroup, or by aria-disabled on the element (where its role takes it) or an ancestor; editable means
// enabled and not read-only. Stable means the same box in two animation frames.
const char *const kDriver = R"js(async function(op, arg) {
  const el = this;
  const plain = /*PLAIN*/;
  const tag = e => ((e && e.nodeName) || '').toUpperCase();
  const up = e => e.parentElement || (e.parentNode && e.parentNode.host) || null;
  const view = e => (e.ownerDocument && e.ownerDocument.defaultView) || window;
  const role = e => {
    const given = ((e.getAttribute && e.getAttribute('role')) || '').trim().toLowerCase().split(/\s+/)[0];
    if (given) return given;
    const t = tag(e);
    if (t === 'BUTTON') return 'button';
    if ((t === 'A' || t === 'AREA') && e.hasAttribute('href')) return 'link';
    if (t === 'TEXTAREA') return 'textbox';
    if (t === 'SELECT') return e.multiple || e.size > 1 ? 'listbox' : 'combobox';
    if (t === 'OPTION') return 'option';
    if (t !== 'INPUT') return '';
    const type = (e.type || 'text').toLowerCase();
    if (['button', 'submit', 'reset', 'image'].includes(type)) return 'button';
    if (type === 'checkbox' || type === 'radio') return type;
    if (type === 'range') return 'slider';
    if (type === 'number') return 'spinbutton';
    if (['email', 'tel', 'text', 'url', 'search'].includes(type)) return e.hasAttribute('list') ? 'combobox' : type === 'search' ? 'searchbox' : 'textbox';
    return '';
  };
  const shown = e => {
    const style = view(e).getComputedStyle(e);
    if (!style) return true;
    if (style.display === 'contents') {
      for (let c = e.firstChild; c; c = c.nextSibling) {
        if (c.nodeType === 1 && shown(c)) return true;
        if (c.nodeType === 3) {
          const range = c.ownerDocument.createRange();
          range.selectNode(c);
          const r = range.getBoundingClientRect();
          if (r.width > 0 && r.height > 0) return true;
        }
      }
      return false;
    }
    if (e.checkVisibility && !e.checkVisibility()) return false;
    if (style.visibility !== 'visible') return false;
    const r = e.getBoundingClientRect();
    return r.width > 0 && r.height > 0;
  };
  // The element an action really addresses: a control's own button or checkbox, and a label's control.
  const retarget = (e, how) => {
    if (e && e.nodeType !== 1) e = e.parentElement;
    if (!e) return null;
    if (how === 'none') return e;
    if (!e.matches('input, textarea, select') && !e.isContentEditable)
      e = e.closest(how === 'button-link' ? 'button, [role=button], a, [role=link]' : 'button, [role=button], [role=checkbox], [role=radio]') || e;
    if (how === 'follow-label' && !e.isContentEditable &&
        !e.matches('a, input, textarea, button, select, option, optgroup, [role=link], [role=button], [role=checkbox], [role=switch], [role=radio]')) {
      const label = e.closest('label');
      if (label && label.control) e = label.control;
    }
    return e;
  };
  const disabledRoles = new Set(['application', 'button', 'composite', 'gridcell', 'group', 'input', 'link', 'menuitem', 'scrollbar',
    'separator', 'tab', 'checkbox', 'columnheader', 'combobox', 'grid', 'listbox', 'menu', 'menubar', 'menuitemcheckbox', 'menuitemradio',
    'option', 'radio', 'radiogroup', 'row', 'rowheader', 'searchbox', 'select', 'slider', 'spinbutton', 'switch', 'tablist', 'textbox',
    'toolbar', 'tree', 'treegrid', 'treeitem']);
  const disabled = e => {
    if (['BUTTON', 'INPUT', 'SELECT', 'TEXTAREA', 'OPTION', 'OPTGROUP'].includes(tag(e))) {
      if (e.hasAttribute('disabled')) return true;
      if (tag(e) === 'OPTION' && e.closest('optgroup[disabled]')) return true;
      const set = e.closest('fieldset[disabled]');
      if (set) {
        const legend = set.querySelector(':scope > legend');
        if (!legend || !legend.contains(e)) return true;
      }
    }
    if (!disabledRoles.has(role(e))) return false;
    for (let p = e; p; p = up(p)) {
      const given = ((p.getAttribute && p.getAttribute('aria-disabled')) || '').toLowerCase();
      if (given === 'true') return true;
      if (given === 'false') return false;
    }
    return false;
  };
  const readonlyRoles = new Set(['checkbox', 'combobox', 'grid', 'gridcell', 'listbox', 'radiogroup', 'slider', 'spinbutton', 'textbox',
    'columnheader', 'rowheader', 'searchbox', 'switch', 'treegrid']);
  const readonly = e => {
    if (['INPUT', 'TEXTAREA', 'SELECT'].includes(tag(e))) return e.hasAttribute('readonly');
    if (readonlyRoles.has(role(e))) return (e.getAttribute('aria-readonly') || '').toLowerCase() === 'true';
    if (e.isContentEditable) return false;
    throw new Error('Element is not an <input>, <textarea>, <select> or [contenteditable] and does not have a role allowing [aria-readonly]');
  };
  const inert = e => {
    for (let p = e; p; p = up(p)) if (p.hasAttribute && p.hasAttribute('inert')) return true;
    return false;
  };
  const spaces = t => t.replace(/[\u200b\u00ad]/g, '').trim().replace(/\s+/g, ' ');
  // The next animation frame, or false when the action's time (ms) runs out first.
  const frame = ms => new Promise(done => {
    const timer = setTimeout(() => done(false), Math.max(0, ms));
    requestAnimationFrame(() => { clearTimeout(timer); done(true); });
  });
  const box = e => { const r = e.getBoundingClientRect(); return [r.left, r.top, r.width, r.height].join(' '); };
  const attached = e => { if (!e || !e.isConnected) throw new Error('Element is not attached to the DOM'); return e; };
  const ops = {
    connected: () => el.isConnected,
    state(states) {
      if (!el.isConnected) return 'not attached';
      for (const s of states) {
        if (s === 'visible') {
          if (!shown(el)) return 'not visible';
          continue;
        }
        const t = retarget(el, 'follow-label');
        if (!t || !t.isConnected) return 'not attached';
        if (s === 'non-inert') {
          if (inert(t)) return 'inert';
          continue;
        }
        if (disabled(t)) return 'not enabled';
        if (s === 'editable' && readonly(t)) return 'not editable';
      }
      return 'ok';
    },
    async stable(ms) {
      const until = performance.now() + ms;
      const e = retarget(el, 'no-follow-label');
      if (!(await frame(until - performance.now()))) return 'timeout';
      const before = box(e);
      if (!(await frame(until - performance.now()))) return 'timeout';
      return e.isConnected && before === box(e);
    },
    scroll(how) { el.scrollIntoView({...how, behavior: 'instant'}); return true; },
    viewport: () => [innerWidth, innerHeight],
    hit([x, y]) {
      const target = retarget(el, 'button-link');
      let e = (el.ownerDocument || document).elementFromPoint(x, y);
      while (e && e.shadowRoot) {
        const inner = e.shadowRoot.elementFromPoint(x, y);
        if (!inner || inner === e) break;
        e = inner;
      }
      for (; e; e = up(e)) if (e === target) return true;
      return false;
    },
    fill(value) {
      const e = attached(retarget(el, 'follow-label'));
      if (tag(e) === 'INPUT') {
        const type = e.type.toLowerCase();
        const set = ['color', 'date', 'time', 'datetime-local', 'month', 'range', 'week'];
        const typed = ['', 'email', 'number', 'password', 'search', 'tel', 'text', 'url'];
        if (!set.includes(type) && !typed.includes(type)) throw new Error('Input of type "' + type + '" cannot be filled');
        if (type === 'number' && isNaN(Number(value.trim()))) throw new Error('Cannot type text into input[type=number]');
        if (set.includes(type)) {
          e.focus();
          e.value = value.trim();
          e.dispatchEvent(new Event('input', {bubbles: true, composed: true}));
          e.dispatchEvent(new Event('change', {bubbles: true}));
          return 'done';
        }
      } else if (tag(e) !== 'TEXTAREA' && !e.isContentEditable) {
        throw new Error('Element is not an <input>, <textarea> or [contenteditable] element');
      }
      if (tag(e) === 'INPUT') {
        e.select();
        e.focus();
      } else if (tag(e) === 'TEXTAREA') {
        e.selectionStart = 0;
        e.selectionEnd = e.value.length;
        e.focus();
      } else {
        const range = e.ownerDocument.createRange();
        range.selectNodeContents(e);
        const selection = view(e).getSelection();
        if (selection) {
          selection.removeAllRanges();
          selection.addRange(range);
        }
        e.focus();
      }
      return 'type';
    },
    select(value) {
      const e = attached(retarget(el, 'follow-label'));
      if (tag(e) !== 'SELECT') throw new Error('Element is not a <select> element');
      const option = [...e.options].find(o => o.value === value || o.label === value || spaces(value) === spaces(o.label));
      if (!option) return 'missing';
      if (disabled(option)) return 'disabled';
      e.value = undefined;
      option.selected = true;
      e.dispatchEvent(new Event('input', {bubbles: true, composed: true}));
      e.dispatchEvent(new Event('change', {bubbles: true}));
      return 'done';
    },
    checked() {
      const e = attached(retarget(el, 'follow-label'));
      const radio = tag(e) === 'INPUT' && e.type === 'radio';
      if (tag(e) === 'INPUT' && ['checkbox', 'radio'].includes(e.type)) return {checked: e.checked, radio};
      if (['checkbox', 'menuitemcheckbox', 'option', 'radio', 'switch', 'menuitemradio', 'treeitem'].includes(role(e)))
        return {checked: (e.getAttribute('aria-checked') || '').toLowerCase() === 'true', radio};
      throw new Error('Not a checkbox or radio button');
    },
    focus() {
      attached(el);
      if (el.nodeType !== 1) throw new Error('Node is not an element');
      const active = el.getRootNode().activeElement;
      const was = active === el && !!el.ownerDocument && el.ownerDocument.hasFocus();
      if (el.isContentEditable && !was && active && active.blur) active.blur();
      el.focus();
      el.focus();
      if (!was && tag(el) === 'INPUT') {
        try { el.setSelectionRange(0, 0); } catch (e) {}
      }
      return true;
    },
    upload() {
      const e = attached(retarget(el, 'follow-label'));
      if (tag(e) !== 'INPUT') throw new Error('Node is not an HTMLInputElement');
      if (e.webkitdirectory) throw new Error('[webkitdirectory] input requires passing a path to a directory');
      return e;
    },
    inspect: () => (el=>({text:(el.innerText||el.textContent||'').slice(0,4096),
                    value:el.type==='password'?null:el.value, rect:el.getBoundingClientRect().toJSON(),
                    options:el.tagName==='SELECT'?[...el.options].map(o=>[o.value,o.label]):undefined}))(el),
  };
  const result = await ops[op](arg);
  return op === 'upload' ? result : plain(result);
})js";

// The Python workshop's context blocked service workers.
const char *const kNoServiceWorkers = "if (navigator.serviceWorker) navigator.serviceWorker.register = async () => {};";

// The origin's localStorage and IndexedDB in Playwright's storage-state form: a record's value as it is where JSON
// keeps it, else valueEncoded in Playwright's serialised form (references by id), so that the Python and the C++
// workshop each read the other's saved storage.
const char *const kExportStorage = R"js(async () => {
  const plain = v => v === null || typeof v === 'string' || typeof v === 'boolean' ||
    (typeof v === 'number' && Number.isFinite(v) && !Object.is(v, -0)) ||
    (Array.isArray(v) && v.every(plain)) ||
    (typeof v === 'object' && v !== null && Object.getPrototypeOf(v) === Object.prototype && Object.values(v).every(plain));
  const kinds = [[Int8Array,'i8'],[Uint8Array,'ui8'],[Uint8ClampedArray,'ui8c'],[Int16Array,'i16'],[Uint16Array,'ui16'],
    [Int32Array,'i32'],[Uint32Array,'ui32'],[Float32Array,'f32'],[Float64Array,'f64'],[BigInt64Array,'bi64'],[BigUint64Array,'bui64']];
  const encoded = value => {
    const ids = new Map();
    let last = 0;
    const encode = v => {
      if (v === undefined || typeof v === 'symbol' || typeof v === 'function') return {v: 'undefined'};
      if (v === null) return {v: 'null'};
      if (typeof v === 'number') {
        if (Number.isNaN(v)) return {v: 'NaN'};
        if (v === Infinity) return {v: 'Infinity'};
        if (v === -Infinity) return {v: '-Infinity'};
        if (Object.is(v, -0)) return {v: '-0'};
        return {n: v};
      }
      if (typeof v === 'string') return {s: v};
      if (typeof v === 'boolean') return {b: v};
      if (typeof v === 'bigint') return {bi: v.toString()};
      if (v instanceof Error) return {e: {n: v.name, m: v.message, s: v.stack || ''}};
      if (v instanceof Date) return {d: v.toJSON()};
      if (v instanceof URL) return {u: v.href};
      if (v instanceof RegExp) return {r: {p: v.source, f: v.flags}};
      for (const [type, k] of kinds) if (v instanceof type) {
        let text = '';
        const bytes = new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
        for (let i = 0; i < bytes.length; i++) text += String.fromCharCode(bytes[i]);
        return {ta: {b: btoa(text), k}};
      }
      if (ids.has(v)) return {ref: ids.get(v)};
      const id = ++last;
      ids.set(v, id);
      if (Array.isArray(v)) return {a: v.map(encode), id};
      return {o: Object.keys(v).map(k => ({k, v: encode(v[k])})), id};
    };
    return encode(value);
  };
  const record = (key, value, inline) => {
    const r = {};
    if (!inline) { if (plain(key)) r.key = key; else r.keyEncoded = encoded(key); }
    if (plain(value)) r.value = value; else r.valueEncoded = encoded(value);
    return r;
  };
  const done = r => new Promise((ok, no) => { r.onsuccess = () => ok(r.result); r.onerror = () => no(r.error); });
  const path = p => Array.isArray(p) ? {keyPathArray: p} : (p === null ? {} : {keyPath: p});
  const localStorageItems = [];
  for (let i = 0; i < localStorage.length; i++) {
    const name = localStorage.key(i);
    localStorageItems.push({name, value: localStorage.getItem(name)});
  }
  const databases = [];
  for (const info of await indexedDB.databases()) {
    if (!info.name) continue;
    const db = await done(indexedDB.open(info.name));
    const stores = [];
    for (const name of db.objectStoreNames) {
      const store = db.transaction(name, 'readonly').objectStore(name);
      const [keys, values] = await Promise.all([done(store.getAllKeys()), done(store.getAll())]);
      const inline = store.keyPath !== null;
      stores.push({name, autoIncrement: store.autoIncrement, ...path(store.keyPath), records: values.map((v, i) => record(keys[i], v, inline)),
        indexes: [...store.indexNames].map(n => { const x = store.index(n); return {name: n, ...path(x.keyPath), multiEntry: x.multiEntry, unique: x.unique}; })});
    }
    databases.push({name: info.name, version: db.version, stores});
    db.close();
  }
  return {localStorage: localStorageItems, indexedDB: databases};
})js";

const char *const kRestoreStorage = R"js(async saved => {
  const kinds = {i8: Int8Array, ui8: Uint8Array, ui8c: Uint8ClampedArray, i16: Int16Array, ui16: Uint16Array, i32: Int32Array, ui32: Uint32Array,
    f32: Float32Array, f64: Float64Array, bi64: BigInt64Array, bui64: BigUint64Array};
  const decoded = value => {
    const refs = new Map();
    const decode = e => {
      if (e === null || typeof e !== 'object') return e;
      if ('ref' in e) return refs.get(e.ref);
      if ('v' in e) return {undefined: undefined, null: null, NaN: NaN, Infinity: Infinity, '-Infinity': -Infinity, '-0': -0}[e.v];
      if ('n' in e) return e.n;
      if ('s' in e) return e.s;
      if ('b' in e) return e.b;
      if ('bi' in e) return BigInt(e.bi);
      if ('d' in e) return new Date(e.d);
      if ('u' in e) return new URL(e.u);
      if ('r' in e) return new RegExp(e.r.p, e.r.f);
      if ('e' in e) { const error = new Error(e.e.m); error.name = e.e.n; error.stack = e.e.s; return error; }
      if ('ta' in e) {
        const text = atob(e.ta.b);
        const bytes = new Uint8Array(text.length);
        for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i);
        const Type = kinds[e.ta.k] || Uint8Array;
        return new Type(bytes.buffer, 0, bytes.length / (Type.BYTES_PER_ELEMENT || 1));
      }
      if ('a' in e) { const out = []; refs.set(e.id, out); for (const x of e.a) out.push(decode(x)); return out; }
      if ('o' in e) { const out = {}; refs.set(e.id, out); for (const {k, v} of e.o) out[k] = decode(v); return out; }
      return undefined;
    };
    return decode(value);
  };
  for (const item of saved.localStorage) localStorage.setItem(item.name, item.value);
  for (const database of saved.indexedDB) {
    await new Promise((ok, no) => {
      const open = indexedDB.open(database.name, database.version);
      open.onupgradeneeded = () => {
        const db = open.result;
        for (const s of database.stores || []) {
          if (db.objectStoreNames.contains(s.name)) continue;
          const options = {autoIncrement: !!s.autoIncrement};
          if (s.keyPathArray) options.keyPath = s.keyPathArray; else if (s.keyPath !== undefined && s.keyPath !== null) options.keyPath = s.keyPath;
          const store = db.createObjectStore(s.name, options);
          for (const x of s.indexes || []) store.createIndex(x.name, x.keyPathArray || x.keyPath, {unique: !!x.unique, multiEntry: !!x.multiEntry});
        }
      };
      open.onerror = () => no(open.error);
      open.onsuccess = () => {
        const db = open.result;
        const names = (database.stores || []).map(s => s.name).filter(n => db.objectStoreNames.contains(n));
        if (!names.length) { db.close(); ok(); return; }
        const tx = db.transaction(names, 'readwrite');
        for (const s of database.stores) {
          if (!names.includes(s.name)) continue;
          const store = tx.objectStore(s.name);
          for (const r of s.records || []) {
            const value = 'value' in r ? r.value : decoded(r.valueEncoded);
            if ('key' in r || 'keyEncoded' in r) store.put(value, 'key' in r ? r.key : decoded(r.keyEncoded)); else store.put(value);
          }
        }
        tx.oncomplete = () => { db.close(); ok(); };
        tx.onerror = () => no(tx.error);
      };
    });
  }
  return true;
})js";

const std::string &driver_source() {
    static const std::string source = [] {
        std::string s = kDriver;
        const std::string mark = "/*PLAIN*/";
        s.replace(s.find(mark), mark.size(), kPlain);
        return s;
    }();
    return source;
}

std::string read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path.u8string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string base64_decode(const std::string &text) {
    std::string out;
    unsigned v = 0;
    int bits = 0;
    for (const unsigned char c : text) {
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+') d = 62;
        else if (c == '/') d = 63;
        else continue;
        v = (v << 6) | static_cast<unsigned>(d);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((v >> bits) & 0xff);
        }
    }
    return out;
}

std::string base64_encode(const std::string &bytes) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned w = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) w |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) w |= static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(w >> 18) & 63];
        out += alphabet[(w >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(w >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[w & 63] : '=';
    }
    return out;
}

std::string text_of(const Json *value) { return value && value->is_string() ? value->get<std::string>() : std::string(); }

// The Unicode code points of UTF-8 text (well-formed, as the JSON reader admits it).
std::vector<unsigned> code_points(const std::string &text) {
    std::vector<unsigned> out;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        unsigned code = n == 1 ? c : n == 2 ? (c & 0x1fu) : n == 3 ? (c & 0x0fu) : (c & 0x07u);
        for (size_t k = 1; k < n && i + k < text.size(); ++k) code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3fu);
        out.push_back(code);
        i += n;
    }
    return out;
}

// Python's len() of the text: its code points.
size_t characters(const std::string &text) {
    size_t n = 0;
    for (const char c : text) n += (static_cast<unsigned char>(c) & 0xc0) != 0x80;
    return n;
}

// Whether Python's str.strip() leaves nothing of the text: it is empty or Unicode white space only.
bool blank(const std::string &text) {
    for (const unsigned code : code_points(text)) {
        const bool space = (code >= 0x09 && code <= 0x0d) || (code >= 0x1c && code <= 0x20) || code == 0x85 || code == 0xa0 || code == 0x1680 ||
                           (code >= 0x2000 && code <= 0x200a) || code == 0x2028 || code == 0x2029 || code == 0x202f || code == 0x205f ||
                           code == 0x3000;
        if (!space) return false;
    }
    return true;
}

// The size of compact JSON text as Python's json.dumps writes it, every character past ASCII as an escape.
size_t python_json_size(const std::string &text) {
    size_t n = 0;
    for (const unsigned code : code_points(text)) n += code < 0x80 ? 1 : code < 0x10000 ? 6 : 12;
    return n;
}

// scheme://host:port and the path of a URL the browser requests.
void split_url(const std::string &url, std::string &origin, std::string &path) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        origin = url.substr(0, url.find(':') + 1);
        path.clear();
        return;
    }
    const size_t start = url.find_first_of("/?#", scheme + 3);
    origin = url.substr(0, start);
    if (start == std::string::npos || url[start] != '/') {
        path = "/";
        return;
    }
    const size_t end = url.find_first_of("?#", start);
    path = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The output folder's claim: one editor a folder, while .workshop.lock is held by the workshop that runs. The
// Python workshop held the same lock (flock, or the file's first byte on Windows), so the two exclude each other.
class FolderClaim {
public:
    explicit FolderClaim(const fs::path &path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED first{};
        if (handle_ == INVALID_HANDLE_VALUE || !LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &first)) {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            refuse();
        }
#else
        fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = -1;
            refuse();
        }
#endif
    }
    ~FolderClaim() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    FolderClaim(const FolderClaim &) = delete;
    FolderClaim &operator=(const FolderClaim &) = delete;

private:
    [[noreturn]] static void refuse() {
        throw ValueError("This output directory already has an active editor. Connect to that MCP service or use another directory.");
    }
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

// ---- the keyboard: a US layout, as Playwright types --------------------------------------------------------------

struct KeyDesc {
    std::string key, code, text;
    int keycode = 0;  // the Windows virtual key code, without the key's location
    int location = 0;
};

struct KeyDef {
    const char *code;
    int keycode;
    const char *key;
    const char *shifted;  // the key with Shift held, for keys that type something else then
    int location;         // 1 left, 2 right, 3 numeric keypad
};

const KeyDef kUsKeys[] = {
    {"Escape", 27, "Escape", nullptr, 0}, {"F1", 112, "F1", nullptr, 0}, {"F2", 113, "F2", nullptr, 0}, {"F3", 114, "F3", nullptr, 0},
    {"F4", 115, "F4", nullptr, 0}, {"F5", 116, "F5", nullptr, 0}, {"F6", 117, "F6", nullptr, 0}, {"F7", 118, "F7", nullptr, 0},
    {"F8", 119, "F8", nullptr, 0}, {"F9", 120, "F9", nullptr, 0}, {"F10", 121, "F10", nullptr, 0}, {"F11", 122, "F11", nullptr, 0},
    {"F12", 123, "F12", nullptr, 0}, {"F13", 124, "F13", nullptr, 0}, {"F14", 125, "F14", nullptr, 0}, {"F15", 126, "F15", nullptr, 0},
    {"F16", 127, "F16", nullptr, 0}, {"F17", 128, "F17", nullptr, 0}, {"F18", 129, "F18", nullptr, 0}, {"F19", 130, "F19", nullptr, 0},
    {"F20", 131, "F20", nullptr, 0}, {"F21", 132, "F21", nullptr, 0}, {"F22", 133, "F22", nullptr, 0}, {"F23", 134, "F23", nullptr, 0},
    {"F24", 135, "F24", nullptr, 0}, {"Backquote", 192, "`", "~", 0}, {"Digit1", 49, "1", "!", 0}, {"Digit2", 50, "2", "@", 0},
    {"Digit3", 51, "3", "#", 0}, {"Digit4", 52, "4", "$", 0}, {"Digit5", 53, "5", "%", 0}, {"Digit6", 54, "6", "^", 0},
    {"Digit7", 55, "7", "&", 0}, {"Digit8", 56, "8", "*", 0}, {"Digit9", 57, "9", "(", 0}, {"Digit0", 48, "0", ")", 0},
    {"Minus", 189, "-", "_", 0}, {"Equal", 187, "=", "+", 0}, {"Backslash", 220, "\\", "|", 0}, {"Backspace", 8, "Backspace", nullptr, 0},
    {"Tab", 9, "Tab", nullptr, 0}, {"KeyQ", 81, "q", "Q", 0}, {"KeyW", 87, "w", "W", 0}, {"KeyE", 69, "e", "E", 0},
    {"KeyR", 82, "r", "R", 0}, {"KeyT", 84, "t", "T", 0}, {"KeyY", 89, "y", "Y", 0}, {"KeyU", 85, "u", "U", 0},
    {"KeyI", 73, "i", "I", 0}, {"KeyO", 79, "o", "O", 0}, {"KeyP", 80, "p", "P", 0}, {"BracketLeft", 219, "[", "{", 0},
    {"BracketRight", 221, "]", "}", 0}, {"CapsLock", 20, "CapsLock", nullptr, 0}, {"KeyA", 65, "a", "A", 0}, {"KeyS", 83, "s", "S", 0},
    {"KeyD", 68, "d", "D", 0}, {"KeyF", 70, "f", "F", 0}, {"KeyG", 71, "g", "G", 0}, {"KeyH", 72, "h", "H", 0},
    {"KeyJ", 74, "j", "J", 0}, {"KeyK", 75, "k", "K", 0}, {"KeyL", 76, "l", "L", 0}, {"Semicolon", 186, ";", ":", 0},
    {"Quote", 222, "'", "\"", 0}, {"Enter", 13, "Enter", nullptr, 0}, {"ShiftLeft", 16, "Shift", nullptr, 1}, {"KeyZ", 90, "z", "Z", 0},
    {"KeyX", 88, "x", "X", 0}, {"KeyC", 67, "c", "C", 0}, {"KeyV", 86, "v", "V", 0}, {"KeyB", 66, "b", "B", 0},
    {"KeyN", 78, "n", "N", 0}, {"KeyM", 77, "m", "M", 0}, {"Comma", 188, ",", "<", 0}, {"Period", 190, ".", ">", 0},
    {"Slash", 191, "/", "?", 0}, {"ShiftRight", 16, "Shift", nullptr, 2}, {"ControlLeft", 17, "Control", nullptr, 1},
    {"MetaLeft", 91, "Meta", nullptr, 1}, {"AltLeft", 18, "Alt", nullptr, 1}, {"Space", 32, " ", nullptr, 0},
    {"AltRight", 18, "Alt", nullptr, 2}, {"MetaRight", 92, "Meta", nullptr, 2}, {"ContextMenu", 93, "ContextMenu", nullptr, 0},
    {"ControlRight", 17, "Control", nullptr, 2}, {"PrintScreen", 44, "PrintScreen", nullptr, 0}, {"ScrollLock", 145, "ScrollLock", nullptr, 0},
    {"Pause", 19, "Pause", nullptr, 0}, {"PageUp", 33, "PageUp", nullptr, 0}, {"PageDown", 34, "PageDown", nullptr, 0},
    {"Insert", 45, "Insert", nullptr, 0}, {"Delete", 46, "Delete", nullptr, 0}, {"Home", 36, "Home", nullptr, 0},
    {"End", 35, "End", nullptr, 0}, {"ArrowLeft", 37, "ArrowLeft", nullptr, 0}, {"ArrowUp", 38, "ArrowUp", nullptr, 0},
    {"ArrowRight", 39, "ArrowRight", nullptr, 0}, {"ArrowDown", 40, "ArrowDown", nullptr, 0}, {"NumLock", 144, "NumLock", nullptr, 0},
    {"NumpadDivide", 111, "/", nullptr, 3}, {"NumpadMultiply", 106, "*", nullptr, 3}, {"NumpadSubtract", 109, "-", nullptr, 3},
    {"Numpad7", 36, "Home", "7", 3}, {"Numpad8", 38, "ArrowUp", "8", 3}, {"Numpad9", 33, "PageUp", "9", 3},
    {"Numpad4", 37, "ArrowLeft", "4", 3}, {"Numpad5", 12, "Clear", "5", 3}, {"Numpad6", 39, "ArrowRight", "6", 3},
    {"NumpadAdd", 107, "+", nullptr, 3}, {"Numpad1", 35, "End", "1", 3}, {"Numpad2", 40, "ArrowDown", "2", 3},
    {"Numpad3", 34, "PageDown", "3", 3}, {"Numpad0", 45, "Insert", "0", 3}, {"NumpadEnter", 13, "Enter", nullptr, 3},
    {"AudioVolumeMute", 173, "AudioVolumeMute", nullptr, 0}, {"AudioVolumeDown", 174, "AudioVolumeDown", nullptr, 0},
    {"AudioVolumeUp", 175, "AudioVolumeUp", nullptr, 0}, {"MediaTrackNext", 176, "MediaTrackNext", nullptr, 0},
    {"MediaTrackPrevious", 177, "MediaTrackPrevious", nullptr, 0}, {"MediaStop", 178, "MediaStop", nullptr, 0},
    {"MediaPlayPause", 179, "MediaPlayPause", nullptr, 0},
};

struct KeyEntry {
    KeyDesc plain, shifted;
    bool has_shifted = false;
};

// Key names: each code (KeyA, Digit1, Enter: Shift then types its second character), and as aliases the characters
// the main keys type and Shift, Control, Alt and Meta for their left keys (no second character: Shift+a types a).
struct Layout {
    std::map<std::string, KeyEntry> codes;
    std::map<std::string, KeyDesc> aliases;
    Layout() {
        for (const KeyDef &d : kUsKeys) {
            KeyEntry entry;
            entry.plain.key = d.key;
            entry.plain.code = d.code;
            entry.plain.keycode = d.keycode;
            entry.plain.location = d.location;
            const std::string key = d.key;
            if (key.size() == 1) entry.plain.text = key;
            if (key == "Enter") entry.plain.text = "\r";
            if (d.shifted) {
                entry.has_shifted = true;
                entry.shifted = entry.plain;
                entry.shifted.key = entry.shifted.text = d.shifted;
            }
            codes[d.code] = entry;
            const std::string code = d.code;
            if (code == "ShiftLeft" || code == "ControlLeft" || code == "AltLeft" || code == "MetaLeft") aliases[entry.plain.key] = entry.plain;
            if (code == "Enter") aliases["\n"] = aliases["\r"] = entry.plain;
            if (d.location) continue;
            if (key.size() == 1) aliases[key] = entry.plain;
            if (d.shifted) aliases[d.shifted] = entry.shifted;
        }
    }
};

const Layout &layout() {
    static const Layout l;
    return l;
}

const int kAlt = 1, kControl = 2, kMeta = 4, kShift = 8;

int modifier_bit(const std::string &key) {
    if (key == "Alt") return kAlt;
    if (key == "Control") return kControl;
    if (key == "Meta") return kMeta;
    if (key == "Shift") return kShift;
    return 0;
}

std::string smart_key(const std::string &name) {
#ifdef __APPLE__
    return name == "ControlOrMeta" ? "Meta" : name;
#else
    return name == "ControlOrMeta" ? "Control" : name;
#endif
}

// A chord's keys: split at each + that follows a key, so that "Control++" holds Control and presses +.
std::vector<std::string> chord_keys(const std::string &chord) {
    std::vector<std::string> keys;
    std::string building;
    for (const char c : chord) {
        if (c == '+' && !building.empty()) {
            keys.push_back(building);
            building.clear();
        } else {
            building += c;
        }
    }
    keys.push_back(building);
    return keys;
}

// The editing commands a Mac's standard key bindings attach to a shortcut: without them, headless Chrome on a Mac
// does not edit text for Backspace, the arrows or Command+A.
std::vector<std::string> mac_commands(const std::string &code, int modifiers) {
#ifdef __APPLE__
    static const std::map<std::string, std::vector<std::string>> bindings = {
        {"Backspace", {"deleteBackward"}}, {"Shift+Backspace", {"deleteBackward"}}, {"Alt+Backspace", {"deleteWordBackward"}},
        {"Meta+Backspace", {"deleteToBeginningOfLine"}}, {"Control+Backspace", {"deleteBackwardByDecomposingPreviousCharacter"}},
        {"Delete", {"deleteForward"}}, {"Shift+Delete", {"deleteForward"}}, {"Alt+Delete", {"deleteWordForward"}},
        {"Escape", {"cancelOperation"}}, {"Shift+Escape", {"cancelOperation"}},
        {"ArrowUp", {"moveUp"}}, {"ArrowDown", {"moveDown"}}, {"ArrowLeft", {"moveLeft"}}, {"ArrowRight", {"moveRight"}},
        {"Shift+ArrowUp", {"moveUpAndModifySelection"}}, {"Shift+ArrowDown", {"moveDownAndModifySelection"}},
        {"Shift+ArrowLeft", {"moveLeftAndModifySelection"}}, {"Shift+ArrowRight", {"moveRightAndModifySelection"}},
        {"Alt+ArrowLeft", {"moveWordLeft"}}, {"Alt+ArrowRight", {"moveWordRight"}},
        {"Shift+Alt+ArrowLeft", {"moveWordLeftAndModifySelection"}}, {"Shift+Alt+ArrowRight", {"moveWordRightAndModifySelection"}},
        {"Meta+ArrowUp", {"moveToBeginningOfDocument"}}, {"Meta+ArrowDown", {"moveToEndOfDocument"}},
        {"Meta+ArrowLeft", {"moveToLeftEndOfLine"}}, {"Meta+ArrowRight", {"moveToRightEndOfLine"}},
        {"Shift+Meta+ArrowUp", {"moveToBeginningOfDocumentAndModifySelection"}}, {"Shift+Meta+ArrowDown", {"moveToEndOfDocumentAndModifySelection"}},
        {"Shift+Meta+ArrowLeft", {"moveToLeftEndOfLineAndModifySelection"}}, {"Shift+Meta+ArrowRight", {"moveToRightEndOfLineAndModifySelection"}},
        {"Home", {"scrollToBeginningOfDocument"}}, {"End", {"scrollToEndOfDocument"}}, {"PageUp", {"scrollPageUp"}}, {"PageDown", {"scrollPageDown"}},
        {"Shift+Home", {"moveToBeginningOfDocumentAndModifySelection"}}, {"Shift+End", {"moveToEndOfDocumentAndModifySelection"}},
        {"Shift+PageUp", {"pageUpAndModifySelection"}}, {"Shift+PageDown", {"pageDownAndModifySelection"}},
        {"Control+KeyA", {"moveToBeginningOfParagraph"}}, {"Control+KeyE", {"moveToEndOfParagraph"}}, {"Control+KeyB", {"moveBackward"}},
        {"Control+KeyF", {"moveForward"}}, {"Control+KeyD", {"deleteForward"}}, {"Control+KeyH", {"deleteBackward"}},
        {"Control+KeyK", {"deleteToEndOfParagraph"}}, {"Control+KeyN", {"moveDown"}}, {"Control+KeyP", {"moveUp"}},
        {"Meta+KeyA", {"selectAll"}}, {"Meta+KeyC", {"copy"}}, {"Meta+KeyX", {"cut"}}, {"Meta+KeyV", {"paste"}}, {"Meta+KeyZ", {"undo"}},
        {"Shift+Meta+KeyZ", {"redo"}},
    };
    std::string shortcut;
    if (modifiers & kShift) shortcut += "Shift+";
    if (modifiers & kControl) shortcut += "Control+";
    if (modifiers & kAlt) shortcut += "Alt+";
    if (modifiers & kMeta) shortcut += "Meta+";
    const auto found = bindings.find(shortcut + code);
    return found == bindings.end() ? std::vector<std::string>() : found->second;
#else
    (void)code;
    (void)modifiers;
    return {};
#endif
}

// Playwright's retries of an element action: at once, then after 20, 100, 100 and 500 ms and every 500 ms after,
// until the action's time is spent.
class Attempts {
public:
    Attempts(Clock::time_point deadline, std::string action) : deadline_(deadline), action_(std::move(action)) {}
    void wait(const std::string &why) {
        static const int pause[] = {0, 20, 100, 100, 500};
        const int ms = pause[std::min<size_t>(count_, 4)];
        ++count_;
        if (Clock::now() + std::chrono::milliseconds(ms) >= deadline_)
            throw std::runtime_error(action_ + ": Timeout 4500ms exceeded.\nCall log:\n  - " + why);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    size_t count() const { return count_; }

private:
    Clock::time_point deadline_;
    std::string action_;
    size_t count_ = 0;
};

[[noreturn]] void detached(const std::string &label) { throw std::runtime_error(label + ": Element is not attached to the DOM"); }

}  // namespace

// ---- the engine: the workshop's server, the browser and its pages -----------------------------------------

struct Workshop::Engine {
    struct Pointer {
        double x = 0, y = 0;
        int buttons = 0;
        std::string button = "none";
    };
    struct Page {
        std::string target, session;
        bool ready = false, closed = false;
        Pointer pointer;  // each page has its own mouse, as each Playwright page has
    };
    struct Attached {
        Json params;
        std::string parent;  // the session it was attached through: empty for the browser's own
    };

    const Artifacts &artifacts;
    std::unique_ptr<Studio> studio;
    std::unique_ptr<http::Server> server;
    std::thread serving;
    std::string origin;
    std::unique_ptr<FolderClaim> claim;
    fs::path profile, downloads_dir;
    std::string controls_script, user_agent;
    std::unique_ptr<Browser> browser;
    Browser *driver = nullptr;  // the browser for the reader and setup threads, which may outlive the pointer above by a moment
    std::string context;
    std::atomic<bool> restoring{false};

    std::mutex lock;  // pages, dialogs, downloads, loads, the confirmation and the setup queue
    std::condition_variable changed;
    std::deque<Page> pages;  // a deque, so that a page's place stays put while new pages join
    size_t current = 0;
    Json confirm;  // the answer for the next dialog, or null to dismiss it
    std::vector<Json> dialogs, downloads;
    std::map<std::string, std::string> transfers;  // downloads in progress: guid and suggested name
    std::set<std::string> loaded;                  // session and loader of each document that has loaded
    std::set<std::string> navigating;              // session and frame of each navigation a page asked for, not yet committed
    std::deque<Attached> attached;                 // targets waiting for their setup
    bool stopping = false;
    std::thread setup;

    int modifiers = 0;  // the keyboard's modifiers held during a chord
    std::set<std::string> pressed;

    explicit Engine(const Artifacts &a) : artifacts(a) {}

    ~Engine() {
        {
            std::lock_guard<std::mutex> hold(lock);
            stopping = true;
        }
        changed.notify_all();
        // The browser closes first, so that a setup waiting for it gives up; then nothing uses it any more.
        if (browser) {
            try {
                browser->call("Browser.close", Json::object(), std::string(), 5);
            } catch (...) {
            }
        }
        if (setup.joinable()) setup.join();
        browser.reset();
        if (server) server->stop();
        if (serving.joinable()) serving.join();
        server.reset();
        studio.reset();
        claim.reset();
        std::error_code error;
        if (!profile.empty()) fs::remove_all(profile, error);
    }

    bool our_page_session(const std::string &session) {
        std::lock_guard<std::mutex> hold(lock);
        for (const Page &p : pages)
            if (p.session == session) return true;
        return false;
    }

    // Events, on the browser's reader thread: answered at once, without waiting for the browser.
    void event(const std::string &method, const Json &params, const std::string &session) {
        if (!driver) return;
        if (method == "Fetch.requestPaused") {
            // As the Python workshop's route: only the workstation's own origin, and as documents only its pages.
            const std::string id = text_of(member(params, "requestId"));
            const Json *request = member(params, "request");
            std::string where, path;
            split_url(request ? text_of(member(*request, "url")) : std::string(), where, path);
            const bool document = text_of(member(params, "resourceType")) == "Document";
            Json answer = Json::object();
            answer["requestId"] = id;
            if (where == origin && document && path == kStoragePage && restoring) {
                answer["responseCode"] = 200;
                Json header = Json::object();
                header["name"] = "Content-Type";
                header["value"] = "text/html; charset=utf-8";
                answer["responseHeaders"] = Json::array({header});
                answer["body"] = base64_encode("<!doctype html><title></title>");
                driver->send("Fetch.fulfillRequest", answer, session);
            } else if (where == origin && (!document || kPages.count(path))) {
                driver->send("Fetch.continueRequest", answer, session);
            } else {
                answer["errorReason"] = "Failed";
                driver->send("Fetch.failRequest", answer, session);
            }
            return;
        }
        if (method == "Page.frameRequestedNavigation" || method == "Page.frameNavigated" || method == "Page.navigatedWithinDocument" ||
            method == "Page.frameStoppedLoading" || method == "Page.frameDetached") {
            const Json *frame = member(params, "frame");
            const std::string id = frame ? text_of(member(*frame, "id")) : text_of(member(params, "frameId"));
            std::lock_guard<std::mutex> hold(lock);
            if (method == "Page.frameRequestedNavigation") {
                if (text_of(member(params, "disposition")) == "currentTab") navigating.insert(session + "\n" + id);
            } else {
                navigating.erase(session + "\n" + id);
            }
            changed.notify_all();
            return;
        }
        if (method == "Page.lifecycleEvent") {
            if (text_of(member(params, "name")) != "load") return;
            std::lock_guard<std::mutex> hold(lock);
            loaded.insert(session + "\n" + text_of(member(params, "loaderId")));
            changed.notify_all();
            return;
        }
        if (method == "Page.javascriptDialogOpening") {
            const std::string type = text_of(member(params, "type"));
            std::string message = text_of(member(params, "message"));
            const std::vector<unsigned> points = code_points(message);
            if (points.size() > 500) {
                size_t cut = 0;
                for (size_t n = 0; n < 500; ++n) cut += points[n] < 0x80 ? 1 : points[n] < 0x800 ? 2 : points[n] < 0x10000 ? 3 : 4;
                message.resize(cut);
            }
            Json answer = Json::object();
            {
                std::lock_guard<std::mutex> hold(lock);
                Json record = Json::object();
                record["type"] = type;
                record["message"] = message;
                record["accepted"] = !confirm.is_null();
                dialogs.push_back(record);
                answer["accept"] = !confirm.is_null();
                if (!confirm.is_null() && type == "prompt") answer["promptText"] = confirm;
                confirm = Json();
            }
            driver->send("Page.handleJavaScriptDialog", answer, session);
            return;
        }
        if (method == "Browser.downloadWillBegin") {
            std::lock_guard<std::mutex> hold(lock);
            transfers[text_of(member(params, "guid"))] = text_of(member(params, "suggestedFilename"));
            return;
        }
        if (method == "Browser.downloadProgress") {
            const std::string guid = text_of(member(params, "guid")), progress = text_of(member(params, "state"));
            if (progress != "completed" && progress != "canceled") return;
            std::string name;
            {
                std::lock_guard<std::mutex> hold(lock);
                const auto found = transfers.find(guid);
                if (found == transfers.end()) return;
                name = found->second;
            }
            Json record;
            try {
                const fs::path file = downloads_dir / guid;
                std::error_code error;
                const uintmax_t size = fs::file_size(file, error);
                if (progress != "completed" || error || size > kLimit) throw ValueError("Download failed or exceeds 8 MiB.");
                record = artifacts.create_file(name, read_text(file));
                fs::remove(file, error);
            } catch (const std::exception &e) {
                record = Json::object();
                record["error"] = e.what();
            }
            std::lock_guard<std::mutex> hold(lock);
            downloads.push_back(record);
            transfers.erase(guid);
            changed.notify_all();
            return;
        }
        if (method == "Target.attachedToTarget") {
            std::lock_guard<std::mutex> hold(lock);
            Attached a;
            a.params = params;
            a.parent = session;
            attached.push_back(a);
            changed.notify_all();
            return;
        }
        if (method == "Target.detachedFromTarget" || method == "Inspector.detached" || method == "Inspector.targetCrashed") {
            const std::string gone = method == "Target.detachedFromTarget" ? text_of(member(params, "sessionId")) : session;
            std::lock_guard<std::mutex> hold(lock);
            for (Page &p : pages)
                if (p.session == gone) p.closed = true;
            changed.notify_all();
        }
    }

    static Json fetch_patterns() {
        Json pattern = Json::object();
        pattern["urlPattern"] = "*";
        Json fetch = Json::object();
        fetch["patterns"] = Json::array({pattern});
        return fetch;
    }

    // New targets, set up off the reader thread before they run: a page of the workshop's context gets the
    // controls' script, the request filter and the Python workshop's viewport and media; a worker or frame of
    // one gets the request filter; anything else is let go.
    void set_up() {
        for (;;) {
            Attached item;
            {
                std::unique_lock<std::mutex> hold(lock);
                changed.wait(hold, [&] { return stopping || !attached.empty(); });
                if (stopping) return;
                item = attached.front();
                attached.pop_front();
            }
            const Json &params = item.params;
            const std::string session = text_of(member(params, "sessionId"));
            const Json *info = member(params, "targetInfo");
            const std::string type = info ? text_of(member(*info, "type")) : std::string();
            const std::string owner = info ? text_of(member(*info, "browserContextId")) : std::string();
            const bool ours = owner == context || (!item.parent.empty() && our_page_session(item.parent));
            try {
                if (type == "page" && ours) {
                    {
                        std::lock_guard<std::mutex> hold(lock);
                        Page page;
                        page.target = text_of(member(*info, "targetId"));
                        page.session = session;
                        pages.push_back(page);
                    }
                    // All at once and the run last, as a page opened by another (a new tab) answers some of them only
                    // once it runs.
                    std::vector<std::pair<std::string, Json>> commands;
                    commands.emplace_back("Page.enable", Json::object());
                    Json lifecycle = Json::object();
                    lifecycle["enabled"] = true;
                    commands.emplace_back("Page.setLifecycleEventsEnabled", lifecycle);
                    commands.emplace_back("Runtime.enable", Json::object());
                    Json children = Json::object();
                    children["autoAttach"] = true;
                    children["waitForDebuggerOnStart"] = true;
                    children["flatten"] = true;
                    commands.emplace_back("Target.setAutoAttach", children);
                    for (const std::string &source : {std::string(kNoServiceWorkers), controls_script}) {
                        Json script = Json::object();
                        script["source"] = source;
                        commands.emplace_back("Page.addScriptToEvaluateOnNewDocument", script);
                    }
                    commands.emplace_back("Fetch.enable", fetch_patterns());
                    Json metrics = Json::object();
                    metrics["width"] = 1440;
                    metrics["height"] = 960;
                    metrics["screenWidth"] = 1440;
                    metrics["screenHeight"] = 960;
                    metrics["deviceScaleFactor"] = 1;
                    metrics["mobile"] = false;
                    Json orientation = Json::object();
                    orientation["angle"] = 0;
                    orientation["type"] = "landscapePrimary";
                    metrics["screenOrientation"] = orientation;
                    commands.emplace_back("Emulation.setDeviceMetricsOverride", metrics);
                    Json focus = Json::object();
                    focus["enabled"] = true;
                    commands.emplace_back("Emulation.setFocusEmulationEnabled", focus);
                    Json media = Json::object();
                    media["media"] = "";
                    Json features = Json::array();
                    const std::pair<const char *, const char *> wanted[] = {
                        {"prefers-color-scheme", "light"}, {"prefers-reduced-motion", "no-preference"}, {"forced-colors", "none"}, {"prefers-contrast", "no-preference"}};
                    for (const auto &f : wanted) {
                        Json feature = Json::object();
                        feature["name"] = f.first;
                        feature["value"] = f.second;
                        features.push_back(feature);
                    }
                    media["features"] = features;
                    commands.emplace_back("Emulation.setEmulatedMedia", media);
                    if (!user_agent.empty()) {
                        Json agent = Json::object();
                        agent["userAgent"] = user_agent;
                        commands.emplace_back("Emulation.setUserAgentOverride", agent);
                    }
                    commands.emplace_back("Runtime.runIfWaitingForDebugger", Json::object());
                    driver->call_all(commands, session);
                    std::lock_guard<std::mutex> hold(lock);
                    for (Page &p : pages)
                        if (p.session == session) p.ready = true;
                    changed.notify_all();
                } else if (ours && (type == "worker" || type == "shared_worker" || type == "service_worker" || type == "iframe")) {
                    driver->call_all({{"Fetch.enable", fetch_patterns()}, {"Runtime.runIfWaitingForDebugger", Json::object()}}, session);
                } else {
                    driver->send("Runtime.runIfWaitingForDebugger", Json::object(), session);
                }
            } catch (const std::exception &) {
                std::lock_guard<std::mutex> hold(lock);
                for (Page &p : pages)
                    if (p.session == session) p.closed = true;
                changed.notify_all();
            }
        }
    }

    bool page_alive() {
        std::lock_guard<std::mutex> hold(lock);
        return browser && browser->alive() && current < pages.size() && !pages[current].closed;
    }

    Page &page() {
        std::lock_guard<std::mutex> hold(lock);
        if (current >= pages.size() || pages[current].closed) throw std::runtime_error("The workshop page has closed.");
        return pages[current];
    }

    std::string session() { return page().session; }

    // The open pages, in the order they opened.
    std::vector<std::string> open_sessions() {
        std::lock_guard<std::mutex> hold(lock);
        std::vector<std::string> out;
        for (const Page &p : pages)
            if (p.ready && !p.closed) out.push_back(p.session);
        return out;
    }

    static Json answer(const Json &reply, const std::string &label, bool whole = true) {
        if (const Json *exception = member(reply, "exceptionDetails")) {
            const Json *thrown = member(*exception, "exception");
            std::string text = thrown ? text_of(member(*thrown, "description")) : std::string();
            if (text.empty() && thrown) {
                if (const Json *value = member(*thrown, "value")) text = value->is_string() ? value->get<std::string>() : value->dump();
            }
            if (text.empty()) text = text_of(member(*exception, "text"));
            if (!whole) text = text.substr(0, text.find('\n'));
            throw std::runtime_error(label + ": " + text);
        }
        const Json *result = member(reply, "result");
        const Json *value = result ? member(*result, "value") : nullptr;
        return value ? *value : Json();
    }

    // page.evaluate(function, argument): the function's answer, its promise awaited, as Playwright gave it.
    Json evaluate(const std::string &function, const Json *argument = nullptr, std::string on = std::string(), double seconds = 120) {
        if (on.empty()) on = session();
        Json params = Json::object();
        params["expression"] = "(async () => { const run = (" + function + "); return (" + std::string(kPlain) + ")(await run(" +
                               (argument ? argument->dump() : std::string()) + ")); })()";
        params["awaitPromise"] = true;
        params["returnByValue"] = true;
        params["userGesture"] = true;
        return answer(browser->call("Runtime.evaluate", params, on, seconds), "Page.evaluate");
    }

    // An operation of the element driver on the element.
    Json drive(const std::string &object, const char *op, const Json &arg, const std::string &label, bool by_value = true) {
        Json params = Json::object();
        params["objectId"] = object;
        params["functionDeclaration"] = driver_source();
        Json first = Json::object(), second = Json::object();
        first["value"] = op;
        second["value"] = arg;
        params["arguments"] = Json::array({first, second});
        params["awaitPromise"] = true;
        params["returnByValue"] = by_value;
        params["userGesture"] = true;
        const Json reply = browser->call("Runtime.callFunctionOn", params, session());
        if (by_value) return answer(reply, label, false);
        answer(reply, label, false);
        const Json *result = member(reply, "result");
        return result ? *result : Json();
    }

    // KerfKiteAssistantControls.element(ref), as a handle; ValueError when the control is not there.
    std::string element(const std::string &ref) {
        Json params = Json::object();
        params["expression"] = "KerfKiteAssistantControls.element(" + Json(ref).dump() + ")";
        params["returnByValue"] = false;
        params["userGesture"] = true;
        const Json reply = browser->call("Runtime.evaluate", params, session());
        answer(reply, "Page.evaluate_handle");
        const Json *result = member(reply, "result");
        const std::string object = result ? text_of(member(*result, "objectId")) : std::string();
        if (object.empty() || text_of(member(*result, "subtype")) != "node") {
            if (!object.empty()) release(object);
            throw ValueError("Control changed. Search the controls again.");
        }
        return object;
    }

    void release(const std::string &object) {
        Json params = Json::object();
        params["objectId"] = object;
        try {
            browser->call("Runtime.releaseObject", params, session(), 5);
        } catch (...) {
        }
    }

    // ---- the mouse and the keyboard, as Playwright's ----

    void move(double x, double y, int steps = 1) {
        Pointer &p = page().pointer;
        const double from_x = p.x, from_y = p.y;
        p.x = x;
        p.y = y;
        for (int i = 1; i <= steps; ++i) {
            Json params = Json::object();
            params["type"] = "mouseMoved";
            params["button"] = p.button;
            params["buttons"] = p.buttons;
            params["x"] = from_x + (x - from_x) * (static_cast<double>(i) / steps);
            params["y"] = from_y + (y - from_y) * (static_cast<double>(i) / steps);
            params["modifiers"] = modifiers;
            params["force"] = p.buttons ? 0.5 : 0.0;
            browser->call("Input.dispatchMouseEvent", params, session());
        }
    }

    void mouse_down(int count) {
        Pointer &p = page().pointer;
        p.button = "left";
        p.buttons |= 1;
        Json params = Json::object();
        params["type"] = "mousePressed";
        params["button"] = "left";
        params["buttons"] = p.buttons;
        params["x"] = p.x;
        params["y"] = p.y;
        params["modifiers"] = modifiers;
        params["clickCount"] = count;
        params["force"] = 0.5;
        browser->call("Input.dispatchMouseEvent", params, session());
    }

    void mouse_up(int count) {
        Pointer &p = page().pointer;
        p.button = "none";
        p.buttons &= ~1;
        Json params = Json::object();
        params["type"] = "mouseReleased";
        params["button"] = "left";
        params["buttons"] = p.buttons;
        params["x"] = p.x;
        params["y"] = p.y;
        params["modifiers"] = modifiers;
        params["clickCount"] = count;
        browser->call("Input.dispatchMouseEvent", params, session());
    }

    void wheel(double dx, double dy) {
        const Pointer &p = page().pointer;
        Json params = Json::object();
        params["type"] = "mouseWheel";
        params["x"] = p.x;
        params["y"] = p.y;
        params["modifiers"] = modifiers;
        params["deltaX"] = dx;
        params["deltaY"] = dy;
        browser->call("Input.dispatchMouseEvent", params, session());
    }

    static bool known_key(const std::string &name) {
        const std::string key = smart_key(name);
        return layout().codes.count(key) || layout().aliases.count(key);
    }

    // The key's description with the modifiers held now: Shift types a code's second character, and no text goes
    // with a key while any modifier but Shift is held.
    KeyDesc describe(const std::string &name) const {
        const std::string key = smart_key(name);
        KeyDesc d;
        const auto code = layout().codes.find(key);
        if (code != layout().codes.end()) {
            d = (modifiers & kShift) && code->second.has_shifted ? code->second.shifted : code->second.plain;
        } else {
            d = layout().aliases.at(key);
        }
        if (modifiers != 0 && modifiers != kShift) d.text.clear();
        return d;
    }

    void key_down(const std::string &name) {
        const KeyDesc d = describe(name);
        const bool repeat = pressed.count(d.code) > 0;
        pressed.insert(d.code);
        modifiers |= modifier_bit(d.key);
        Json params = Json::object();
        params["type"] = d.text.empty() ? "rawKeyDown" : "keyDown";
        params["modifiers"] = modifiers;
        params["windowsVirtualKeyCode"] = d.keycode;
        params["code"] = d.code;
        Json commands = Json::array();
        for (const std::string &c : mac_commands(d.code, modifiers)) commands.push_back(c);
        params["commands"] = commands;
        params["key"] = d.key;
        params["text"] = d.text;
        params["unmodifiedText"] = d.text;
        params["autoRepeat"] = repeat;
        params["location"] = d.location;
        params["isKeypad"] = d.location == 3;
        browser->call("Input.dispatchKeyEvent", params, session());
    }

    void key_up(const std::string &name) {
        const KeyDesc d = describe(name);
        modifiers &= ~modifier_bit(d.key);
        pressed.erase(d.code);
        Json params = Json::object();
        params["type"] = "keyUp";
        params["modifiers"] = modifiers;
        params["key"] = d.key;
        params["windowsVirtualKeyCode"] = d.keycode;
        params["code"] = d.code;
        params["location"] = d.location;
        browser->call("Input.dispatchKeyEvent", params, session());
    }

    // keyboard.press(chord): the keys before the last held in order, the last pressed and released, the held keys
    // released in reverse. Own rule: every key is looked up before any is pressed, so that an unknown key leaves
    // none held down (Playwright stops at the unknown key with the earlier ones still held).
    void press(const std::string &chord, const std::string &label) {
        const std::vector<std::string> keys = chord_keys(chord);
        for (const std::string &k : keys)
            if (!known_key(k)) throw std::runtime_error(label + ": Unknown key: \"" + smart_key(k) + "\"");
        modifiers = 0;
        pressed.clear();
        for (size_t i = 0; i + 1 < keys.size(); ++i) key_down(keys[i]);
        key_down(keys.back());
        key_up(keys.back());
        for (size_t i = keys.size() - 1; i-- > 0;) key_up(keys[i]);
    }

    void insert_text(const std::string &text) {
        Json params = Json::object();
        params["text"] = text;
        browser->call("Input.insertText", params, session());
    }

    // ---- element actions, with Playwright's checks and retries ----

    // Before an input that may navigate (a click, a key): navigations asked for earlier no longer count.
    void expect_navigation() {
        const std::string prefix = session() + "\n";
        std::lock_guard<std::mutex> hold(lock);
        for (auto it = navigating.lower_bound(prefix); it != navigating.end() && it->compare(0, prefix.size(), prefix) == 0;) it = navigating.erase(it);
    }

    // After it: a navigation the page asked for meanwhile has committed, as Playwright waits for one, so that the
    // state read next is the new document's. Own rule: one that never commits is waited for until the action's
    // time is spent and then let be, where Playwright reports the timeout.
    void await_navigation(Clock::time_point deadline) {
        const std::string on = session();
        try {
            browser->call("Page.enable", Json::object(), on, 10);  // every event the input caused has arrived
        } catch (const std::exception &) {
        }
        const std::string prefix = on + "\n";
        std::unique_lock<std::mutex> hold(lock);
        changed.wait_until(hold, std::max(deadline, Clock::now() + std::chrono::milliseconds(500)), [&] {
            const auto it = navigating.lower_bound(prefix);
            return it == navigating.end() || it->compare(0, prefix.size(), prefix) != 0;
        });
    }

    // Into view: the protocol's own scroll, or on later attempts of a click the element's scrollIntoView to its
    // end, centre and start in turn. False when the element has no layout box.
    bool scroll(const std::string &object, size_t attempt, const std::string &label) {
        static const char *const blocks[] = {nullptr, "end", "center", "start"};
        if (const char *block = blocks[attempt % 4]) {
            Json how = Json::object();
            how["block"] = block;
            how["inline"] = block;
            drive(object, "scroll", how, label);
            return true;
        }
        Json params = Json::object();
        params["objectId"] = object;
        try {
            browser->call("DOM.scrollIntoViewIfNeeded", params, session());
            return true;
        } catch (const std::exception &e) {
            const std::string message = e.what();
            if (message.find("Node is detached from document") != std::string::npos) detached(label);
            if (message.find("Node does not have a layout object") != std::string::npos) return false;
            throw std::runtime_error(label + ": " + message);
        }
    }

    // The point a click aims at: the middle of the element's first box that shows in the viewport, as Playwright
    // aims.
    bool clickable_point(const std::string &object, double &x, double &y, const std::string &label) {
        Json params = Json::object();
        params["objectId"] = object;
        Json reply;
        try {
            reply = browser->call("DOM.getContentQuads", params, session());
        } catch (const std::exception &e) {
            if (std::string(e.what()).find("Node is detached from document") != std::string::npos) detached(label);
            return false;
        }
        const Json size = drive(object, "viewport", Json(), label);
        const double width = size[0].get<double>(), height = size[1].get<double>();
        const Json *quads = member(reply, "quads");
        if (!quads) return false;
        for (const Json &q : *quads) {
            if (q.size() != 8) continue;
            double px[4], py[4];
            for (size_t i = 0; i < 4; ++i) {
                px[i] = std::min(std::max(q[2 * i].get<double>(), 0.0), width);
                py[i] = std::min(std::max(q[2 * i + 1].get<double>(), 0.0), height);
            }
            double area = 0;
            for (size_t i = 0; i < 4; ++i) area += (px[i] * py[(i + 1) % 4] - px[(i + 1) % 4] * py[i]) / 2;
            if (std::fabs(area) <= 0.99) continue;
            x = y = 0;
            for (size_t i = 0; i < 4; ++i) {
                x += px[i] / 4;
                y += py[i] / 4;
            }
            // Hundredths of a pixel, cut toward zero as Playwright rounds the point.
            x = std::trunc(x * 100) / 100;
            y = std::trunc(y * 100) / 100;
            return true;
        }
        return false;
    }

    // element.click() or dblclick(): stable, then visible, enabled and not inert, scrolled into view, and the
    // element (or its button or link) the one at the point; then the mouse moves there and clicks.
    void click(const std::string &object, int count, Clock::time_point deadline, const std::string &label) {
        Attempts attempts(deadline, label);
        for (;;) {
            if (!drive(object, "connected", Json(), label).get<bool>()) detached(label);
            std::string why = "element is not stable";
            if (stable(object, deadline, label)) {
                const std::string state =
                    drive(object, "state", Json::array({Json("visible"), Json("enabled"), Json("non-inert")}), label).get<std::string>();
                if (state == "not attached") detached(label);
                why = "element is " + state;
                if (state == "ok") {
                    why = "element is not visible";
                    double x = 0, y = 0;
                    if (scroll(object, attempts.count(), label)) {
                        why = "element is outside of the viewport";
                        if (clickable_point(object, x, y, label)) {
                            why = "another element would receive the click";
                            if (drive(object, "hit", Json::array({Json(x), Json(y)}), label).get<bool>()) {
                                move(x, y);
                                for (int i = 1; i <= count; ++i) {
                                    mouse_down(i);
                                    mouse_up(i);
                                }
                                return;
                            }
                        }
                    }
                }
            }
            attempts.wait(why);
        }
    }

    // Whether the element keeps its box over two animation frames, waiting for them no longer than the action
    // may last (Playwright waits for the frames until its timeout).
    bool stable(const std::string &object, Clock::time_point deadline, const std::string &label) {
        const long long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const Json result = drive(object, "stable", Json(std::max(0LL, left)), label);
        if (result.is_string()) throw std::runtime_error(label + ": Timeout 4500ms exceeded.\nCall log:\n  - waiting for element to be stable");
        return result.get<bool>();
    }

    void ready_for(const std::string &object, const Json &states, Clock::time_point deadline, const std::string &label) {
        Attempts attempts(deadline, label);
        for (;;) {
            const std::string state = drive(object, "state", states, label).get<std::string>();
            if (state == "not attached") detached(label);
            if (state == "ok") return;
            attempts.wait("element is " + state);
        }
    }

    // element.fill(value): visible, enabled and editable; a value set at once for date-like inputs, else the
    // text selected and the value typed over it (Delete for no value).
    void fill(const std::string &object, const std::string &value, Clock::time_point deadline) {
        const std::string label = "ElementHandle.fill";
        ready_for(object, Json::array({Json("visible"), Json("enabled"), Json("editable"), Json("non-inert")}), deadline, label);
        if (drive(object, "fill", Json(value), label).get<std::string>() != "type") return;
        if (!value.empty()) insert_text(value);
        else press("Delete", label);
    }

    // element.select_option(value): the first option whose value or label it is, once the list is visible and
    // enabled and has that option enabled.
    void select(const std::string &object, const std::string &value, Clock::time_point deadline) {
        const std::string label = "ElementHandle.select_option";
        Attempts attempts(deadline, label);
        for (;;) {
            const std::string state = drive(object, "state", Json::array({Json("visible"), Json("enabled"), Json("non-inert")}), label).get<std::string>();
            if (state == "not attached") detached(label);
            std::string why = "element is " + state;
            if (state == "ok") {
                const std::string done = drive(object, "select", Json(value), label).get<std::string>();
                if (done == "done") return;
                why = done == "missing" ? "did not find some options" : "option being selected is not enabled";
            }
            attempts.wait(why);
        }
    }

    // element.set_checked(wanted): a click when the state differs (never to clear a radio button), which must
    // then change it.
    void set_checked(const std::string &object, bool wanted, Clock::time_point deadline) {
        const std::string label = "ElementHandle.set_checked";
        const Json now = drive(object, "checked", Json(), label);
        if (now.at("checked").get<bool>() == wanted) return;
        if (!wanted && now.at("radio").get<bool>())
            throw std::runtime_error(label + ": Cannot uncheck radio button. Radio buttons can only be unchecked by selecting another radio button in the same group.");
        click(object, 1, deadline, label);
        if (drive(object, "checked", Json(), label).at("checked").get<bool>() != wanted)
            throw std::runtime_error(label + ": Clicking the checkbox did not change its state");
    }

    // element.scroll_into_view_if_needed(): stable, then scrolled; an element with no layout box is waited for.
    void scroll_for_pointer(const std::string &object, Clock::time_point deadline) {
        const std::string label = "ElementHandle.scroll_into_view_if_needed";
        Attempts attempts(deadline, label);
        for (;;) {
            if (!drive(object, "connected", Json(), label).get<bool>()) detached(label);
            std::string why = "element is not stable";
            if (stable(object, deadline, label)) {
                why = "element is not visible";
                if (scroll(object, 0, label)) return;
            }
            attempts.wait(why);
        }
    }

    // element.bounding_box(): the border box in the viewport, or false when it has none.
    bool bounding_box(const std::string &object, double out[4]) {
        Json params = Json::object();
        params["objectId"] = object;
        Json reply;
        try {
            reply = browser->call("DOM.getBoxModel", params, session());
        } catch (const std::exception &) {
            return false;
        }
        const Json *model = member(reply, "model");
        const Json *border = model ? member(*model, "border") : nullptr;
        if (!border || border->size() != 8) return false;
        double minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;
        for (size_t i = 0; i < 4; ++i) {
            const double x = (*border)[2 * i].get<double>(), y = (*border)[2 * i + 1].get<double>();
            minx = std::min(minx, x);
            maxx = std::max(maxx, x);
            miny = std::min(miny, y);
            maxy = std::max(maxy, y);
        }
        out[0] = minx;
        out[1] = miny;
        out[2] = maxx - minx;
        out[3] = maxy - miny;
        return true;
    }

    void upload(const std::string &object, const std::string &path) {
        const std::string label = "ElementHandle.set_input_files";
        const Json input = drive(object, "upload", Json(), label, false);
        const std::string target = text_of(member(input, "objectId"));
        if (target.empty()) detached(label);
        Json params = Json::object();
        params["objectId"] = target;
        params["files"] = Json::array({Json(path)});
        try {
            browser->call("DOM.setFileInputFiles", params, session());
        } catch (const std::exception &e) {
            release(target);
            throw std::runtime_error(label + ": Protocol error (DOM.setFileInputFiles): " + e.what());
        }
        release(target);
    }

    // The downloads an action started, finished.
    void settle() {
        std::unique_lock<std::mutex> hold(lock);
        changed.wait_for(hold, std::chrono::seconds(20), [&] { return transfers.empty(); });
    }

    // A page on the workshop's origin to read its storage from, the selected one first; empty when none is.
    std::string origin_session() {
        std::vector<std::string> sessions = open_sessions();
        const std::string selected = session();
        std::stable_partition(sessions.begin(), sessions.end(), [&](const std::string &s) { return s == selected; });
        for (const std::string &s : sessions) {
            try {
                if (evaluate("() => location.origin", nullptr, s, 10) == Json(origin)) return s;
            } catch (const std::exception &) {
            }
        }
        return std::string();
    }

    // The editor's storage for this origin, written to the output folder as the Python workshop wrote it (no
    // origin when its storage is empty). With no page on the origin open the saved storage stays as it was.
    void persist() {
        const std::string on = origin_session();
        if (on.empty()) return;
        const Json storage = evaluate(kExportStorage, nullptr, on);
        const Json local = member(storage, "localStorage") ? storage.at("localStorage") : Json::array();
        const Json databases = member(storage, "indexedDB") ? storage.at("indexedDB") : Json::array();
        Json origins = Json::array();
        if (!local.empty() || !databases.empty()) {
            Json origin_state = Json::object();
            origin_state["origin"] = origin;
            origin_state["localStorage"] = local;
            origin_state["indexedDB"] = databases;
            origins.push_back(origin_state);
        }
        Json state = Json::object();
        state["cookies"] = Json::array();
        state["origins"] = origins;
        const std::string encoded = state.dump();
        if (python_json_size(encoded) > kStorageLimit) throw ValueError("Saved browser library exceeds 32 MiB. Export and remove unused library entries.");
        const fs::path target = fs::u8path(artifacts.root()) / ".workshop-storage.json";
        const fs::path staged = fs::u8path(artifacts.root()) / (".workshop-storage-" + hex(random_bytes(6)) + ".tmp");
        {
            std::ofstream out(staged, std::ios::binary | std::ios::trunc);
            out << encoded;
            if (!out) throw std::runtime_error("Cannot save the workshop's storage");
        }
        std::error_code error;
        fs::rename(staged, target, error);
        if (error) {
            fs::remove(staged, error);
            throw std::runtime_error("Cannot save the workshop's storage");
        }
    }

    Json state() {
        Json result = evaluate(kStateScript);
        if (!result.is_object()) result = Json::object();
        const std::vector<std::string> sessions = open_sessions();
        Json listed = Json::array();
        for (size_t i = 0; i < sessions.size(); ++i) {
            Json title;
            try {
                title = evaluate("() => document.title", nullptr, sessions[i], 10);
            } catch (const std::exception &) {
            }
            Json item = Json::object();
            item["index"] = static_cast<long long>(i);
            item["title"] = title.is_string() ? title : Json("");
            listed.push_back(item);
        }
        result["pages"] = listed;
        std::lock_guard<std::mutex> hold(lock);
        Json recent = Json::array();
        for (size_t i = dialogs.size() > 4 ? dialogs.size() - 4 : 0; i < dialogs.size(); ++i) recent.push_back(dialogs[i]);
        result["dialogs"] = recent;
        Json files = Json::array();
        for (size_t i = downloads.size() > 8 ? downloads.size() - 8 : 0; i < downloads.size(); ++i) files.push_back(downloads[i]);
        result["downloads"] = files;
        return result;
    }

    // page.goto(url): the navigation, then the load of its document.
    void navigate(const std::string &url) {
        const std::string on = session();
        Json params = Json::object();
        params["url"] = url;
        const Json reply = browser->call("Page.navigate", params, on, 30);
        const std::string problem = text_of(member(reply, "errorText"));
        if (!problem.empty()) throw std::runtime_error("Page.goto: " + problem + " at " + url);
        const std::string loader = text_of(member(reply, "loaderId"));
        if (loader.empty()) return;
        std::unique_lock<std::mutex> hold(lock);
        const bool done = changed.wait_for(hold, std::chrono::seconds(30), [&] {
            if (loaded.count(on + "\n" + loader)) return true;
            for (const Page &p : pages)
                if (p.session == on && p.closed) return true;
            return false;
        });
        if (!done || !loaded.count(on + "\n" + loader)) throw std::runtime_error("Page.goto: the workshop page did not load " + url);
    }

    // The workshop started: its server, the claim on the folder, the browser with the saved storage put back, and
    // the editor open and ready.
    void start(const WorkshopOptions &options) {
        Options studio_options;
        studio_options.port = 0;
        studio_options.enable_hardware = false;  // the simulator only, as the Python workshop's create_app(allow_hardware=False)
        studio_options.monitor = false;
        studio_options.static_dir = options.static_dir;
        studio_options.step_program = options.step_program;
        studio = std::make_unique<Studio>(studio_options);
        server = std::make_unique<http::Server>("127.0.0.1", 0);
        origin = "http://127.0.0.1:" + std::to_string(server->port());
        Studio *s = studio.get();
        serving = std::thread([this, s] {
            server->serve(
                [s](const http::Request &r) {
                    // The workshop is not the operator's workstation: no phone pairing, no serial ports.
                    if (r.path == "/api/phone-monitor") {
                        const Json data = json_body(r);
                        if (data.is_object() && text_of(member(data, "action")) == "status") {
                            Json off = Json::object();
                            off["enabled"] = false;
                            return http::Response::json(off.dump());
                        }
                        return http::Response::error(403, "Use the operator workstation for physical pairing.");
                    }
                    if (r.path == "/api/ports") return http::Response::json("[]");
                    return s->handle(r);
                },
                [s](const http::Request &r) { return s->body_limit(r); });
        });
        claim = std::make_unique<FolderClaim>(fs::u8path(artifacts.root()) / ".workshop.lock");
        // The saved storage, refused when it is a link or too large (as the Python workshop, a missing file or a
        // dangling link is none) and when it is not storage state.
        Json local = Json::array(), databases = Json::array();
        const fs::path stored = fs::u8path(artifacts.root()) / ".workshop-storage.json";
        std::error_code error;
        if (fs::exists(stored, error)) {
            if (fs::is_symlink(fs::symlink_status(stored, error)) || fs::file_size(stored, error) > kStorageLimit) throw ValueError("Invalid saved workshop storage.");
            const Json state = Json::parse(read_text(stored));
            const Json *origins = member(state, "origins");
            if (!origins || !origins->is_array()) throw ValueError("Invalid saved workshop storage.");
            for (const Json &o : *origins) {
                const Json *items = member(o, "localStorage");
                const Json *dbs = member(o, "indexedDB");
                if (!o.is_object() || (items && !items->is_array()) || (dbs && !dbs->is_array())) throw ValueError("Invalid saved workshop storage.");
                if (items)
                    for (const Json &item : *items) {
                        const Json *name = member(item, "name");
                        const Json *value = member(item, "value");
                        if (!name || !name->is_string() || !value || !value->is_string()) throw ValueError("Invalid saved workshop storage.");
                        local.push_back(item);
                    }
                if (dbs)
                    for (const Json &db : *dbs) {
                        const Json *name = member(db, "name");
                        if (!name || !name->is_string()) throw ValueError("Invalid saved workshop storage.");
                        databases.push_back(db);
                    }
            }
        }
        const std::string program = find_chromium(options.chromium);
        if (program.empty())
            throw ValueError("The workshop needs Chrome, Chromium or Microsoft Edge: install one, or set KERFKITE_MCP_CHROMIUM to such a browser.");
        controls_script = read_text(fs::u8path(options.static_dir) / "mcp-controls.js");
        profile = fs::temp_directory_path() / ("kerfkite-workshop-" + hex(random_bytes(8)));
        downloads_dir = profile / "downloads";
        fs::create_directories(downloads_dir);
        // The switches that shape what the editor sees as the Python workshop's browser had them: popups allowed,
        // no throttling of a page in the background, sRGB colour, and headless with no scroll bars and a fine
        // pointer that hovers. The browser's own sandbox stays on.
        std::vector<std::string> arguments = {"--user-data-dir=" + (profile / "profile").u8string(),
                                              "--no-first-run",
                                              "--no-default-browser-check",
                                              "--no-service-autorun",
                                              "--no-startup-window",
                                              "--disable-field-trial-config",
                                              "--disable-background-networking",
                                              "--disable-background-timer-throttling",
                                              "--disable-backgrounding-occluded-windows",
                                              "--disable-renderer-backgrounding",
                                              "--disable-back-forward-cache",
                                              "--disable-breakpad",
                                              "--disable-client-side-phishing-detection",
                                              "--disable-component-extensions-with-background-pages",
                                              "--disable-component-update",
                                              "--disable-default-apps",
                                              "--disable-dev-shm-usage",
                                              "--disable-extensions",
                                              "--disable-features=Translate,MediaRouter,DialMediaRouteProvider,OptimizationHints,AutoExpandDetailsElement,"
                                              "LazyFrameLoading,PaintHolding,HttpsUpgrades,AvoidUnnecessaryBeforeUnloadCheckSync,DestroyProfileOnBrowserClose,"
                                              "GlobalMediaControls,CertificateTransparencyComponentUpdater,LensOverlay,ThirdPartyStoragePartitioning",
                                              "--allow-pre-commit-input",
                                              "--disable-hang-monitor",
                                              "--disable-ipc-flooding-protection",
                                              "--disable-popup-blocking",
                                              "--disable-prompt-on-repost",
                                              "--disable-search-engine-choice-screen",
                                              "--force-color-profile=srgb",
                                              "--metrics-recording-only",
                                              "--enable-automation",
                                              "--password-store=basic",
                                              "--use-mock-keychain",
                                              "--export-tagged-pdf",
                                              "--edge-skip-compat-layer-relaunch"};
        if (options.show) {
            arguments.push_back("--window-size=1440,960");
        } else {
            for (const char *a : {"--headless", "--hide-scrollbars", "--mute-audio",
                                  "--blink-settings=primaryHoverType=2,availableHoverTypes=2,primaryPointerType=4,availablePointerTypes=4"})
                arguments.push_back(a);
        }
        browser = std::make_unique<Browser>(program, arguments, (profile / "browser.log").u8string(),
                                            [this](const std::string &m, const Json &p, const std::string &s) { event(m, p, s); });
        driver = browser.get();
        setup = std::thread([this] { set_up(); });
        try {
            // The user agent without "Headless", as Playwright reports it.
            user_agent = text_of(member(browser->call("Browser.getVersion", Json::object(), std::string(), 15), "userAgent"));
            const size_t headless = user_agent.find("HeadlessChrome/");
            if (headless != std::string::npos) user_agent.replace(headless, 15, "Chrome/");
            Json auto_attach = Json::object();
            auto_attach["autoAttach"] = true;
            auto_attach["waitForDebuggerOnStart"] = true;
            auto_attach["flatten"] = true;
            browser->call("Target.setAutoAttach", auto_attach);
            Json context_params = Json::object();
            context_params["disposeOnDetach"] = true;
            context = text_of(member(browser->call("Target.createBrowserContext", context_params), "browserContextId"));
            Json downloads_params = Json::object();
            downloads_params["behavior"] = "allowAndName";
            downloads_params["browserContextId"] = context;
            downloads_params["downloadPath"] = downloads_dir.u8string();
            downloads_params["eventsEnabled"] = true;
            browser->call("Browser.setDownloadBehavior", downloads_params);
            Json target = Json::object();
            target["url"] = "about:blank";
            target["browserContextId"] = context;
            browser->call("Target.createTarget", target);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("The workshop's browser did not start: ") + e.what() + "\n" + browser->log_tail());
        }
        {
            std::unique_lock<std::mutex> hold(lock);
            if (!changed.wait_for(hold, std::chrono::seconds(30), [&] { return !pages.empty() && (pages[0].ready || pages[0].closed); }) || pages[0].closed)
                throw std::runtime_error("The workshop's page did not open.\n" + browser->log_tail());
            current = 0;
        }
        if (!local.empty() || !databases.empty()) {
            Json merged = Json::object();
            merged["localStorage"] = local;
            merged["indexedDB"] = databases;
            restoring = true;
            try {
                navigate(origin + kStoragePage);
                evaluate(kRestoreStorage, &merged);
            } catch (...) {
                restoring = false;
                throw;
            }
            restoring = false;
        }
        navigate(origin);
        evaluate("() => window.studioReady");
    }
};

Workshop::Workshop(const Artifacts &artifacts, WorkshopOptions options) : artifacts_(artifacts), options_(std::move(options)) {}

Workshop::~Workshop() = default;

Workshop::Engine &Workshop::started() {
    if (engine_ && engine_->page_alive()) return *engine_;
    engine_.reset();
    auto engine = std::make_unique<Engine>(artifacts_);
    engine->start(options_);
    engine_ = std::move(engine);
    return *engine_;
}

Json Workshop::state() {
    std::lock_guard<std::mutex> hold(lock_);
    return started().state();
}

Json Workshop::controls(const std::string &query, long long offset, long long limit, bool visible_only) {
    if (offset < 0 || limit < 1 || limit > 100) throw ValueError("Use offset \xe2\x89\xa5 0 and limit 1\xe2\x80\x93" "100.");
    std::lock_guard<std::mutex> hold(lock_);
    Json options = Json::object();
    options["query"] = query;
    options["offset"] = offset;
    options["limit"] = limit;
    options["visibleOnly"] = visible_only;
    return started().evaluate("(opts)=>KerfKiteAssistantControls.search(opts)", &options);
}

Json Workshop::action(const std::string &action, const std::string &ref, const std::string &value, const Json &points, const Json &confirm) {
    if (characters(value) > 4000000 || (confirm.is_string() && characters(confirm.get<std::string>()) > 8192))
        throw ValueError("Action value exceeds the input limit.");
    std::lock_guard<std::mutex> hold(lock_);
    Engine &e = started();
    if (action == "page") {
        // Own rule: the index in ASCII digits (Python's str.isdigit() also took other scripts' digits).
        const std::vector<std::string> sessions = e.open_sessions();
        if (value.empty() || value.size() > 9 || !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            std::stoul(value) >= sessions.size())
            throw ValueError("Use a page index from workshop_state.");
        {
            std::lock_guard<std::mutex> pages(e.lock);
            for (size_t i = 0; i < e.pages.size(); ++i)
                if (e.pages[i].session == sessions[std::stoul(value)]) e.current = i;
        }
        return e.state();
    }
    const std::string object = e.element(ref);
    {
        std::lock_guard<std::mutex> dialogs(e.lock);
        e.confirm = confirm.is_string() ? confirm : Json();
        e.dialogs.clear();
    }
    struct Done {
        Engine &e;
        const std::string &object;
        ~Done() {
            {
                std::lock_guard<std::mutex> dialogs(e.lock);
                e.confirm = Json();
            }
            e.release(object);
        }
    } done{e, object};
    const Clock::time_point deadline = Clock::now() + kActionTime;
    if (action == "click" || action == "double_click") {
        e.expect_navigation();
        e.click(object, action == "click" ? 1 : 2, deadline, action == "click" ? "ElementHandle.click" : "ElementHandle.dblclick");
        e.await_navigation(deadline);
    } else if (action == "fill") {
        e.fill(object, value, deadline);
    } else if (action == "select") {
        e.select(object, value, deadline);
    } else if (action == "check") {
        if (value != "true" && value != "false") throw ValueError("Set value to true or false.");
        e.set_checked(object, value == "true", deadline);
    } else if (action == "key") {
        const bool allowed = !value.empty() && value.size() <= 50 && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '/' || c == ' ';
        });
        if (!allowed) throw ValueError("Supply a keyboard key or chord.");
        e.expect_navigation();
        e.drive(object, "focus", Json(), "ElementHandle.press");
        e.press(value, "ElementHandle.press");
        e.await_navigation(deadline);
    } else if (action == "upload") {
        const std::string path = artifacts_.file(value);
        std::error_code error;
        if (fs::file_size(fs::u8path(path), error) > kLimit) throw ValueError("File exceeds 8 MiB.");
        e.upload(object, path);
    } else if (action == "point" || action == "drag" || action == "wheel") {
        const size_t length = action == "point" ? 2 : 4;
        bool ok = points.is_array() && points.size() == length;
        if (ok)
            for (const Json &v : points) ok = ok && v.is_number() && std::fabs(v.get<double>()) <= 10000;
        if (!ok) throw ValueError("Supply local pixel coordinates as indicated in the tool description.");
        e.scroll_for_pointer(object, deadline);
        double b[4];
        if (!e.bounding_box(object, b)) throw ValueError("Open the control\xe2\x80\x99s panel before pointing at it.");
        const auto position = [&](double x, double y, double &px, double &py) {
            if (!(0 <= x && x < b[2] && 0 <= y && y < b[3])) throw ValueError("Position lies outside this control.");
            px = b[0] + x;
            py = b[1] + y;
        };
        double sx, sy;
        position(points[0].get<double>(), points[1].get<double>(), sx, sy);
        if (action == "point") {
            e.move(sx, sy);
            e.mouse_down(1);
            e.mouse_up(1);
        } else if (action == "drag") {
            double ex, ey;
            position(points[2].get<double>(), points[3].get<double>(), ex, ey);
            e.move(sx, sy);
            e.mouse_down(1);
            try {
                e.move(ex, ey, 12);
            } catch (...) {
                e.mouse_up(1);
                throw;
            }
            e.mouse_up(1);
        } else {
            e.move(sx, sy);
            e.wheel(points[2].get<double>(), points[3].get<double>());
        }
    } else if (action == "inspect") {
        return e.drive(object, "inspect", Json(), "ElementHandle.evaluate");
    } else {
        throw ValueError("Unknown workshop action.");
    }
    e.settle();
    e.persist();
    return e.state();
}

Json Workshop::command(const std::string &line) {
    if (blank(line) || line.size() > 65536) throw ValueError("Enter up to 64 KiB of CAD source or commands.");
    std::lock_guard<std::mutex> hold(lock_);
    Engine &e = started();
    const Json argument(line);
    const Json message = e.evaluate("(line)=>KerfKiteCAD.execute(line)", &argument);
    e.persist();
    Json out = Json::object();
    out["message"] = message;
    out["state"] = e.state();
    return out;
}

Json Workshop::macros() {
    std::lock_guard<std::mutex> hold(lock_);
    return started().evaluate("()=>KerfKiteMacros.list()");
}

Json Workshop::save_macro(const Json &entry, bool replace) {
    std::lock_guard<std::mutex> hold(lock_);
    Engine &e = started();
    const Json arguments = Json::array({entry, Json(replace)});
    e.evaluate("([entry,replace])=>KerfKiteMacros.save(entry,replace)", &arguments);
    e.persist();
    Json out = Json::object();
    out["saved"] = entry.at("name");
    return out;
}

Json Workshop::macro(const std::string &name, const Json &parameters) {
    std::lock_guard<std::mutex> hold(lock_);
    Engine &e = started();
    const Json arguments = Json::array({Json(name), parameters});
    const Json message = e.evaluate("([n,p])=>KerfKiteMacros.run(n,p)", &arguments);
    e.persist();
    Json out = Json::object();
    out["message"] = message;
    out["state"] = e.state();
    return out;
}

Json Workshop::exported(const std::string &kind, Json *model) {
    std::lock_guard<std::mutex> hold(lock_);
    Engine &e = started();
    if (kind == "stl") {
        *model = e.evaluate("()=>CNCSolidExchange.model()");
        return Json();
    }
    const Json argument(kind);
    const Json payload = e.evaluate("async kind=>{\n  if(kind==='gcode')await generateDesignPreview();\n  return artifact(kind);\n}", &argument);
    const Json *name = member(payload, "name");
    const Json *content = member(payload, "content");
    if (!name || !name->is_string() || !content || !content->is_string()) throw std::runtime_error("The editor did not export the " + kind);
    return artifacts_.create_file(name->get<std::string>(), content->get<std::string>());
}

Json Workshop::text(long long offset, long long length) {
    if (offset < 0 || length < 1 || length > 12000) throw ValueError("Enter a nonnegative offset and a length of 1\xe2\x80\x93" "12000 characters.");
    std::lock_guard<std::mutex> hold(lock_);
    const Json arguments = Json::array({Json(offset), Json(length)});
    return started().evaluate(R"js(([i,n])=>{
              const text=document.body.innerText;
              return {text:text.slice(i,i+n),offset:i,length:text.length};
            })js",
                              &arguments);
}

Json Workshop::print() {
    std::lock_guard<std::mutex> hold(lock_);
    Engine &e = started();
    // page.pdf(format='A4', print_background=True, prefer_css_page_size=True), as Playwright asks for it.
    Json params = Json::object();
    params["landscape"] = false;
    params["displayHeaderFooter"] = false;
    params["headerTemplate"] = "";
    params["footerTemplate"] = "";
    params["printBackground"] = true;
    params["scale"] = 1;
    params["paperWidth"] = 8.27;
    params["paperHeight"] = 11.7;
    params["marginTop"] = 0;
    params["marginBottom"] = 0;
    params["marginLeft"] = 0;
    params["marginRight"] = 0;
    params["pageRanges"] = "";
    params["preferCSSPageSize"] = true;
    params["generateTaggedPDF"] = false;
    params["generateDocumentOutline"] = false;
    const Json reply = e.browser->call("Page.printToPDF", params, e.session(), 120);
    return artifacts_.create_file("workshop.pdf", base64_decode(text_of(member(reply, "data"))));
}

std::string Workshop::snapshot() {
    std::lock_guard<std::mutex> hold(lock_);
    Engine &e = started();
    // page.screenshot(): the viewport once the fonts have loaded, with the text caret hidden.
    e.evaluate("async () => { await document.fonts.ready; const s = document.createElement('style'); s.id = '__kerfkite_no_caret';"
               " s.textContent = '*{caret-color:transparent !important}'; (document.head || document.documentElement).appendChild(s); return true; }");
    Json params = Json::object();
    params["format"] = "png";
    std::string image;
    try {
        image = base64_decode(text_of(member(e.browser->call("Page.captureScreenshot", params, e.session(), 60), "data")));
    } catch (...) {
        e.evaluate("() => { document.getElementById('__kerfkite_no_caret')?.remove(); return true; }");
        throw;
    }
    e.evaluate("() => { document.getElementById('__kerfkite_no_caret')?.remove(); return true; }");
    return image;
}

// ---- the tools ------------------------------------------------------------------------------------------------

void add_workshop_tools(mcp::Server &server, Workshop &workshop, std::function<Json(const std::string &, const Json &)> design_call) {
    Workshop *w = &workshop;
    const auto value = [](Json v) {
        mcp::Result r;
        r.value = std::move(v);
        return r;
    };
    server.add(declared("workshop_state", [w, value](const Json &, mcp::Exchange &) { return value(w->state()); }));
    server.add(declared("workshop_controls", [w, value](const Json &a, mcp::Exchange &) {
        return value(w->controls(a.at("query").get<std::string>(), a.at("offset").get<long long>(), a.at("limit").get<long long>(),
                                 a.at("visible_only").get<bool>()));
    }));
    server.add(declared("workshop_action", [w, value](const Json &a, mcp::Exchange &) {
        const Json points = a.at("points").is_null() ? Json::array() : a.at("points");
        return value(w->action(a.at("action").get<std::string>(), a.at("ref").get<std::string>(), a.at("value").get<std::string>(), points, a.at("confirm")));
    }));
    server.add(declared("workshop_command", [w, value](const Json &a, mcp::Exchange &) { return value(w->command(a.at("line").get<std::string>())); }));
    server.add(declared("workshop_macros", [w, value](const Json &, mcp::Exchange &) { return value(w->macros()); }));
    server.add(declared("workshop_save_macro", [w, value](const Json &a, mcp::Exchange &) {
        Json entry = Json::object();
        entry["name"] = a.at("name");
        entry["kind"] = a.at("kind");
        entry["source"] = a.at("source");
        entry["parameters"] = a.at("parameters").is_null() ? Json::object() : a.at("parameters");
        return value(w->save_macro(entry, a.at("replace").get<bool>()));
    }));
    server.add(declared("workshop_macro", [w, value](const Json &a, mcp::Exchange &) {
        return value(w->macro(a.at("name").get<std::string>(), a.at("parameters").is_null() ? Json::object() : a.at("parameters")));
    }));
    server.add(declared("workshop_export", [w, value, design_call](const Json &a, mcp::Exchange &) {
        const std::string kind = a.at("kind").get<std::string>();
        Json model;
        Json result = w->exported(kind, &model);
        if (kind == "stl") {
            Json payload = Json::object();
            payload["model"] = model;
            result = design_call("solid-cad/stl", payload);
        }
        return value(result);
    }));
    server.add(declared("workshop_text", [w, value](const Json &a, mcp::Exchange &) {
        return value(w->text(a.at("offset").get<long long>(), a.at("length").get<long long>()));
    }));
    server.add(declared("workshop_print", [w, value](const Json &, mcp::Exchange &) { return value(w->print()); }));
    server.add(declared("workshop_snapshot", [w](const Json &, mcp::Exchange &) {
        mcp::Result r;
        r.image = w->snapshot();
        return r;
    }));
}

}  // namespace server
}  // namespace kerfkite
