'use strict';
/* Pages of the nuubOS Web administration. Settings mirror the console's
 * categories, sections and row order (docs/dev/ui.md "Settings
 * structure"), with the console's own strings; content pages (Overview,
 * Library, Files, BIOS Files) are what a browser does better: uploads,
 * downloads, large lists. */

const ROMS = '/userdata/roms';
/* Where new games go: libraryd's games_root (the SD2 games card while it
 * can be written, else SD1). */
const gamesRoot = async () => (await get('library')).games_root || ROMS;
const BIOS = '/userdata/bios';
const LANGUAGES = [
  ['en', 'English'],
  ['it', 'Italiano'],
  ['fr', 'Français'],
  ['de', 'Deutsch'],
  ['es', 'Español'],
  ['es-419', 'Español (Latinoamérica)'],
  ['pt', 'Português'],
  ['pt-br', 'Português (Brasil)'],
  ['nl', 'Nederlands'],
  ['pl', 'Polski'],
  ['cs', 'Čeština'],
  ['sv', 'Svenska'],
  ['da', 'Dansk'],
  ['nb', 'Norsk'],
  ['fi', 'Suomi'],
  ['hu', 'Magyar'],
  ['ro', 'Română'],
  ['tr', 'Türkçe'],
  ['el', 'Ελληνικά'],
  ['ru', 'Русский'],
  ['uk', 'Українська'],
  ['id', 'Bahasa Indonesia'],
  ['vi', 'Tiếng Việt'],
  ['th', 'ไทย'],
  ['hi', 'हिन्दी'],
  ['ja', '日本語'],
  ['zh-cn', '简体中文'],
  ['zh-tw', '繁體中文'],
  ['ko', '한국어'],
];

function page(id, group, title, render) {
  PAGES[id] = { group, title, render };
  RAIL.push(id);
}

const on01 = (v) => v === '1' || v === 1 || v === true;
const pct = (v) => v + '%';

/* ----------------------------------------------------------- uploads */

/* Files of a drop (folders included, with their relative folder) or of a
 * file picker. */
async function droppedFiles(dt) {
  const out = [];
  const entries = [...(dt.items || [])].map((i) => i.webkitGetAsEntry && i.webkitGetAsEntry()).filter(Boolean);
  if (!entries.length) return [...dt.files].map((file) => ({ file, rel: '' }));
  const walk = async (entry, rel) => {
    if (entry.isFile) {
      const file = await new Promise((res, rej) => entry.file(res, rej));
      out.push({ file, rel });
    } else if (entry.isDirectory) {
      const reader = entry.createReader();
      let batch;
      do {
        batch = await new Promise((res, rej) => reader.readEntries(res, rej));
        for (const e of batch) await walk(e, rel ? rel + '/' + entry.name : entry.name);
      } while (batch.length);
    }
  };
  for (const e of entries) await walk(e, '');
  return out.filter((x) => !x.file.name.startsWith('.'));
}

/* A drop zone with a multi-file picker and, where folders are taken, a
 * folder picker (its files keep their folder, as in a drop);
 * onFiles([{ file, rel }]). */
function dropZone(title, detail, onFiles, { accept = '', folders = true } = {}) {
  const input = h('input', { type: 'file', multiple: true, accept: accept || null, hidden: true });
  input.addEventListener('change', () => {
    const files = [...input.files].map((file) => ({ file, rel: '' }));
    input.value = '';
    if (files.length) onFiles(files);
  });
  const pick = button(T('choose_files'), () => input.click(), { icon: 'upload', key: 'pick:' + title });
  let dirs = null;
  if (folders) {
    const dirInput = h('input', { type: 'file', multiple: true, webkitdirectory: true, hidden: true });
    dirInput.addEventListener('change', () => {
      const files = [...dirInput.files].map((file) => ({ file, rel: (file.webkitRelativePath || '').split('/').slice(0, -1).join('/') }))
        .filter((x) => !x.file.name.startsWith('.'));
      dirInput.value = '';
      if (files.length) onFiles(files);
    });
    dirs = [button(T('choose_folder'), () => dirInput.click(), { icon: 'folder', key: 'pickdir:' + title }), dirInput];
  }
  const zone = h('div', { class: 'drop' }, icon('upload'),
    h('div', { class: 'dt' }, h('b', { text: title }), h('span', { text: detail })), pick, dirs, input);
  zone.addEventListener('dragover', (e) => { e.preventDefault(); zone.classList.add('over'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('over'));
  zone.addEventListener('drop', async (e) => {
    e.preventDefault();
    zone.classList.remove('over');
    const files = await droppedFiles(e.dataTransfer);
    onFiles(folders ? files : files.map((f) => ({ file: f.file, rel: '' })));
  });
  return zone;
}

async function sendAll(items, dirOf, opts) {
  const map = new Map(items.map((x) => [x.file, x]));
  return uploadFiles(items.map((x) => x.file), (f) => dirOf(map.get(f)), { mkdirs: true, ...opts });
}

/* -------------------------------------------------------- shared bits */

function gameCard(g, from) {
  /* game fields: id system title cover aspect last time available fav year players genre */
  const [id, , title, cover, , , , avail] = g;
  const c = h('button', { class: 'card' + (avail === '0' ? ' na' : ''), type: 'button', data: { key: 'g:' + id }, title,
    on: { click: () => go('library/game/' + id + (from ? '/' + from : '')) } },
  cover ? h('img', { src: img(cover, 320), alt: '', loading: 'lazy' }) : null, h('span', { class: 'ct', text: title }));
  return c;
}

function showcase(key, name, sub, color, previews, glyph, onClick) {
  const b = h('button', { class: 'show', type: 'button', data: { key }, vars: color ? { '--c': color } : null, on: { click: onClick } },
    h('span', { class: 'wash' }),
    previews.length ? h('span', { class: 'fan' }, previews.slice(0, 3).map((p) => h('img', { src: img(p, 192), alt: '', loading: 'lazy' })))
      : glyph ? h('img', { class: 'glyph', src: img(glyph, 192), alt: '' }) : null,
    h('span', { class: 'lab' }, h('b', { text: name }), h('span', { text: sub })));
  return b;
}

const JOB_NAMES = {
  'support-bundle': () => C(595, 'Diagnostic report'),
  'scrape-bulk': () => C(571, 'Getting metadata'),
  'scrape-game': () => C(571, 'Getting metadata'),
  'file-copy': () => C(623, 'Copying files'),
  'file-move': () => C(624, 'Moving files'),
  'sd2-backup': () => C(982, 'Backup'),
  'update-download': () => C(660, 'Downloading update'),
  'update-install': () => C(984, 'Installing update'),
};

function jobRow(j) {
  /* job: id type state progress user step result reason */
  const [id, type, state, progress, , step, result] = j;
  const name = (JOB_NAMES[type] || (() => type))();
  const states = { running: C(91, 'Running'), queued: T('queued'), succeeded: C(92, 'Completed'), failed: C(93, 'Failed'), cancelled: C(596, 'Cancelled') };
  const live = state === 'running' || state === 'queued';
  const p = Number(progress);
  return row({
    key: 'job:' + id, title: name, icon: live ? 'circle-notch' : state === 'succeeded' ? 'check' : 'triangle-exclamation',
    detail: [states[state] || state, step].filter(Boolean).join(' • '),
    value: live && p >= 0 ? p + '%' : '',
    on: live ? () => act('job.cancel', { id }) : state === 'failed' ? () => act('job.retry', { id }) : result && result.startsWith('/userdata/') ? () => { location.href = '/api/download?' + new URLSearchParams({ p: result }); } : null,
    arm: live ? { prompt: C(482, 'Press to cancel') } : null,
  });
}

/* ========================================================= OVERVIEW */

page('overview', 'manage', () => T('overview'), async () => {
  const [sys, lib, emu, jobs, roots] = await Promise.all([get('system'), get('library'), get('emu'), get('jobs'), get('roots').catch(() => ({}))]);
  const nodes = [];
  if (emu.state && emu.state !== 'idle' && emu.game) {
    let d = null;
    try { d = await get('details', { id: emu.game }); } catch (e) { /* unknown game */ }
    const g = d ? rows(d.game)[0] : null;
    nodes.push(h('div', { class: 'now' }, g && g[3] ? h('img', { src: img(g[3], 96), alt: '' }) : null,
      h('div', { class: 'rt' }, h('div', { class: 'kicker', text: C(369, 'Now Playing') }), h('div', { class: 'title', text: g ? g[2] : emu.game }),
        h('div', { class: 'detail', text: d ? d.system_name : emu.system })),
      button(C(422, 'Quit Game'), () => act('emu.quit', {}, { topics: ['emu'] }), { key: 'quit', arm: { prompt: C(191, 'Press again to confirm') } })));
  }
  const battery = Number(sys['battery.percent']);
  const bstate = sys['battery.state'];
  const root = rows(roots.root).find((r) => r[1] === '/userdata') || rows(roots.root)[0];
  const total = root ? Number(root[2]) : 0, free = root ? Number(root[3]) : 0;
  const tile = (label, ic, value, sub, meter) => h('div', { class: 'tile' }, h('div', { class: 'tl' }, icon(ic), label),
    h('div', { class: 'tv', text: value }), meter !== undefined ? h('div', { class: 'meter' }, h('i', { vars: { '--m': meter + '%' } })) : null, h('div', { class: 'ts', text: sub }));
  nodes.push(h('div', { class: 'hero' },
    tile(C(100, 'Battery'), 'battery-full', Number.isNaN(battery) ? '–' : battery + '%',
      bstate === 'charging' ? C(382, 'Charging') : bstate === 'full' ? C(383, 'Fully charged') : C(818, 'Discharging'), Number.isNaN(battery) ? 0 : battery),
    tile(C(19, 'Storage'), 'sd-card', total ? sizeText(free) : '–', total ? C(622, '{0} free', sizeText(free)) + ' / ' + sizeText(total) : '', total ? (1 - free / total) * 100 : 0),
    tile(C(149, 'Games'), 'gamepad', lib.games || '0', lib.scanning === '1' ? C(410, 'Searching for games…') : C(840, 'Systems') + ': ' + rows(lib.system).length),
    tile(C(103, 'Network'), 'wifi', (sys.network || '').split('•').pop().trim() || C(159, 'Not connected'), sys.network ? (sys.network.split('•')[0] || '').trim() : '')));

  const recent = rows(lib.recent);
  nodes.push(section(C(0, 'Recently Played'), 'user'));
  nodes.push(recent.length ? h('div', { class: 'strip' }, recent.map((g) => gameCard(g))) : h('div', { class: 'notice', text: C(715, 'Your adventure log is a blank page. Pick a game and write chapter one!') }));

  const all = rows(jobs.job);
  const live = all.filter((j) => j[2] === 'running' || j[2] === 'queued');
  const past = all.filter((j) => !live.includes(j)).slice(-4).reverse();
  if (all.length) {
    nodes.push(section(T('tasks'), 'device'));
    nodes.push(group([...live, ...past].map(jobRow)));
  }
  return {
    title: T('overview'), kicker: [sys['device.model'], 'nuubOS ' + (sys['nuubos.version'] || '')].filter(Boolean).join(' • '),
    nodes, topics: ['system', 'library', 'emu', 'jobs', 'status'],
    tools: button(T('add_games'), () => go('library'), { icon: 'upload', key: 'add', cls: 'primary' }),
  };
});

/* ========================================================== LIBRARY */

async function systemsRegistry() {
  return (await get('systems')).systems || [];
}

const extOf = (name) => (name.match(/\.([^./]+)$/) || [, ''])[1].toLowerCase();
/* Disc images and multi-disc sets come as an index plus the files it
 * names: a .cue its tracks, a .gdi its tracks, a .ccd its .img/.sub, an
 * .m3u (a playlist: PS1 and other multi-disc games, floppy sets) its discs,
 * which can be indexes again (.m3u -> .cue -> .bin), also in subfolders. */
const DISC_INDEX = ['cue', 'm3u', 'gdi', 'ccd'];
const DISC_TRACKS = ['bin', 'img', 'sub', 'iso', 'raw', 'wav'];

const pathOf = (x) => (x.rel ? x.rel + '/' : '') + x.file.name;

/* dir + a path from an index ('..', '.' and backslashes allowed). */
function joinPath(dir, ref) {
  const out = dir ? dir.split('/') : [];
  for (const p of ref.replace(/\\/g, '/').split('/')) {
    if (!p || p === '.') continue;
    if (p === '..') out.pop(); else out.push(p);
  }
  return out.join('/');
}

/* Paths (lower case, relative to the drop) of the files an index names. */
async function discRefs(x) {
  const ext = extOf(x.file.name);
  const names = [];
  if (ext === 'ccd') {
    const base = x.file.name.replace(/\.[^.]+$/, '');
    names.push(base + '.img', base + '.sub');
  } else if (x.file.size <= 256 * 1024) {
    const text = await x.file.text().catch(() => '');
    for (const raw of text.split(/\r?\n/)) {
      const line = raw.trim();
      let m;
      if (!line) continue;
      if (ext === 'm3u') { if (!line.startsWith('#')) names.push(line.split('|')[0].trim()); }
      else if (ext === 'cue') { if ((m = line.match(/^FILE\s+(?:"([^"]+)"|(\S+))/i))) names.push(m[1] || m[2]); }
      else if (ext === 'gdi' && (m = line.match(/^\d+\s+\d+\s+\d+\s+\d+\s+(?:"([^"]+)"|(\S+))/))) names.push(m[1] || m[2]);
    }
  }
  return names.map((n) => joinPath(x.rel, n).toLowerCase());
}

/* The system of a disc set from the header of its first track or image
 * (the strings libraryd's sniff_system() knows); '' when it does not tell
 * (a .chd is compressed). */
const DISC_MAGIC = [['PLAYSTATION', 'psx'], ['SEGASATURN', 'saturn'], ['SEGADISCSYSTEM', 'segacd'], ['SEGAKATANA', 'dreamcast']];
async function discSystem(files) {
  const track = files.find((x) => ['bin', 'img', 'iso'].includes(extOf(x.file.name)));
  if (!track) return '';
  const head = await track.file.slice(0, 40960).text().catch(() => '');
  const hit = DISC_MAGIC.find(([m]) => head.includes(m));
  return hit ? hit[1] : '';
}

/* Disc sets in a drop: Map(top index -> its files, the index first).
 * Tracks an index does not name follow the first index of their folder. */
async function discSets(items) {
  const byPath = new Map(items.map((x) => [pathOf(x).toLowerCase(), x]));
  const owner = new Map();
  const indexes = items.filter((x) => DISC_INDEX.includes(extOf(x.file.name)));
  for (const ix of indexes)
    for (const p of await discRefs(ix)) {
      const r = byPath.get(p);
      if (r && r !== ix && !owner.has(r)) owner.set(r, ix);
    }
  for (const x of items) {
    if (owner.has(x) || !DISC_TRACKS.includes(extOf(x.file.name))) continue;
    const ix = indexes.find((i) => i.rel === x.rel);
    if (ix) owner.set(x, ix);
  }
  const top = (x) => {
    const seen = new Set([x]);
    while (owner.has(x) && !seen.has(owner.get(x))) { x = owner.get(x); seen.add(x); }
    return x;
  };
  const sets = new Map();
  for (const ix of indexes) if (top(ix) === ix) sets.set(ix, [ix]);
  for (const x of owner.keys()) { const t = top(x); if (sets.has(t)) sets.get(t).push(x); }
  return sets;
}

/* Where dropped games go: a folder named like a system (id, folder, name
 * or alias, at any depth) gives its system, the path below it is kept; a disc set goes as a whole to the one system
 * that takes its index and its discs, else the user picks it once; any
 * other file goes by its type when a single system takes it; the rest is
 * asked once per file type (nuubos-library-import places games copied on
 * the console the same way). Resolves to [{ file, dir }] and the number
 * of files the user chose to skip. */
async function placeGames(items, registry, root) {
  const named = new Map();
  for (const s of registry)
    for (const k of [s.id, s.folder, s.name, ...String(s.aliases || '').split(',')]) if (k) named.set(String(k).toLowerCase(), s);
  const takers = new Map();
  for (const s of registry) for (const e of s.extensions.split(',').filter(Boolean)) takers.set(e, [...(takers.get(e) || []), s]);
  const placed = [], pending = new Map(), loose = [];
  for (const x of items) {
    const parts = x.rel ? x.rel.split('/') : [];
    const at = parts.findIndex((p) => named.has(p.toLowerCase()));
    if (at >= 0) placed.push({ x, sys: named.get(parts[at].toLowerCase()), rel: parts.slice(at + 1).join('/') });
    else loose.push(x);
  }
  const inSet = new Set();
  for (const [ix, files] of await discSets(loose)) {
    files.forEach((x) => inSet.add(x));
    /* Track files (.bin, .img…) do not tell the system; indexes and disc
     * images (.chd, .pbp, .d64…) do. */
    const telling = [...new Set(files.map((x) => extOf(x.file.name)).filter((e) => !DISC_TRACKS.includes(e)))];
    let options = registry.filter((s) => { const ex = s.extensions.split(','); return telling.every((e) => ex.includes(e)); });
    if (options.length > 1) {
      const id = await discSystem(files);
      if (options.some((s) => s.id === id)) options = options.filter((s) => s.id === id);
    }
    if (options.length === 1) { files.forEach((x) => placed.push({ x, sys: options[0], rel: x.rel })); continue; }
    pending.set('disc:' + pathOf(ix), { options: options.length ? options : registry, items: files });
  }
  for (const x of loose) {
    if (inSet.has(x)) continue;
    const ext = extOf(x.file.name);
    const options = takers.get(ext) || [];
    if (options.length === 1) { placed.push({ x, sys: options[0], rel: x.rel }); continue; }
    const key = 'ext:' + ext;
    if (!pending.has(key)) pending.set(key, { options: options.length ? options : registry, items: [] });
    pending.get(key).items.push(x);
  }
  let skipped = 0;
  for (const p of pending.values()) {
    const first = p.items[0];
    const label = first.file.name + (p.items.length > 1 ? ' (+' + (p.items.length - 1) + ')' : '');
    const sorted = [...p.options].sort((a, b) => a.name.localeCompare(b.name, LANG));
    const id = await choose(T('which_system', label), sorted.map((s) => ({ value: s.id, label: s.name })));
    const sys = sorted.find((s) => s.id === id);
    if (!sys) { skipped += p.items.length; continue; }
    for (const x of p.items) placed.push({ x, sys, rel: x.rel });
  }
  return { files: placed.map((p) => ({ file: p.x.file, dir: root + '/' + p.sys.folder + (p.rel ? '/' + p.rel : '') })), skipped };
}

async function addGames(items) {
  if (!items.length) return;
  const { files, skipped } = await placeGames(items, await systemsRegistry(), await gamesRoot());
  if (skipped) toast(T('skipped', skipped), '', true);
  const dirs = new Map(files.map((f) => [f.file, f.dir]));
  await uploadFiles(files.map((f) => f.file), (f) => dirs.get(f), { mkdirs: true, after: 'scan' });
}

/* The search text survives the page's live refreshes. */
let libraryQuery = '';

async function searchResults(q) {
  const words = q.replace(/[|\t]/g, ' ').trim();
  const r = await get('search', { spec: 'q=' + words }, true).catch(() => ({}));
  const list = rows(r.game);
  return [section(gamesText(list.length), null),
    list.length ? h('div', { class: 'grid' }, list.map((g) => gameCard(g))) : empty(C(836, 'No games match these filters'), '')];
}

page('library', 'manage', () => T('library'), async (path) => {
  const [what, arg] = path;
  if (what === 'game') return gamePage(arg, path[2]);
  if (what === 'gamesettings') return settingsPage(arg, path[2], path[3] === 'controls');
  if (what === 'system') return systemPage(arg);
  if (what === 'collection') return collectionPage(arg);
  const lib = await get('library');
  const results = h('div', { class: 'results' });
  const shelves = h('div', { class: 'shelves' });
  const search = h('input', { class: 'input', type: 'search', value: libraryQuery, placeholder: T('search_games'), 'aria-label': T('search_games'), data: { key: 'search' } });
  let timer = null, seq = 0;
  const show = async () => {
    const n = ++seq;
    const q = libraryQuery.trim();
    const nodes = q ? await searchResults(q) : [];
    if (n !== seq) return;
    results.replaceChildren(...nodes);
    shelves.hidden = !!q;
  };
  search.addEventListener('input', () => { libraryQuery = search.value; clearTimeout(timer); timer = setTimeout(show, 250); });
  const systems = rows(lib.system);
  shelves.append(section(C(840, 'Systems'), 'device'),
    systems.length
      ? h('div', { class: 'shelf' }, systems.map((s) => showcase('s:' + s[0], s[1], gamesText(s[2]), s[4], s.slice(6).filter(Boolean), s[5], () => go('library/system/' + s[0]))))
      : empty(C(398, 'No games yet'), C(2, 'Copy your games to the roms folder on the SD card, one folder per system.')),
    section(C(537, 'Collections'), 'user'),
    h('div', { class: 'shelf' },
      rows(lib.collection).map((c) => showcase('c:' + c[0], c[1], gamesText(c[2]), null, c.slice(3).filter(Boolean), null, () => go('library/collection/' + c[0]))),
      h('button', { class: 'show new', type: 'button', data: { key: 'newcol' }, on: { click: newCollection } }, icon('plus'), h('b', { text: C(538, 'New Collection') }))));
  if (libraryQuery.trim()) {
    results.append(...await searchResults(libraryQuery.trim()));
    shelves.hidden = true;
  }
  const nodes = [
    dropZone(T('add_games'), T('add_games_hint'), addGames),
    h('div', { class: 'searchbar' }, h('div', { class: 'wrap' }, icon('magnifying-glass'), search)),
    results, shelves,
  ];
  return { title: T('library'), nodes, topics: ['library'] };
});

async function newCollection(game) {
  const v = await form(C(538, 'New Collection'), [{ name: 'name', label: C(539, 'Collection name') }]);
  if (!v || !v.name.trim()) return null;
  await act('coll.create', { name: v.name.trim() }, { topics: ['library'] });
  if (typeof game === 'string') {
    const lib = await get('library', {}, true);
    const c = rows(lib.collection).find((x) => x[1] === v.name.trim());
    if (c) await act('coll.add', { id: c[0], game });
  }
  return v.name;
}

async function systemPage(id) {
  const [games, registry, lib] = await Promise.all([get('games', { scope: 'system:' + id }), systemsRegistry(), get('library')]);
  const sys = registry.find((s) => s.id === id) || { id, name: id, folder: id, extensions: '' };
  const list = rows(games.game);
  const exts = sys.extensions.split(',').filter(Boolean);
  const nodes = [dropZone(T('upload_to', sys.name), T('file_types', exts.map((e) => '.' + e).join(' ')), (items) => {
    const ok = (x) => exts.includes(extOf(x.file.name)) || DISC_INDEX.includes(extOf(x.file.name)) || DISC_TRACKS.includes(extOf(x.file.name));
    const bad = items.filter((x) => !ok(x));
    if (bad.length) toast(T('not_for_system', bad.length), bad.slice(0, 3).map((x) => x.file.name).join(', '), true, 6000);
    const root = lib.games_root || ROMS;
    sendAll(items.filter(ok), (x) => root + '/' + sys.folder + (x.rel ? '/' + x.rel : ''), { after: 'scan' });
  })];
  nodes.push(list.length ? h('div', { class: 'grid' }, list.map((g) => gameCard(g))) : empty(C(398, 'No games yet'), ''));
  const sysLib = rows(lib.system).find((s) => s[0] === id);
  return { title: sys.name, kicker: gamesText(list.length), back: 'library', nodes, topics: ['library'], tools: sysLib && sysLib[5] ? h('img', { class: 'sysicon', src: img(sysLib[5], 96), alt: '' }) : null };
}

async function collectionPage(id) {
  const [games, lib] = await Promise.all([get('games', { scope: 'collection:' + id }), get('library')]);
  const col = rows(lib.collection).find((c) => c[0] === id);
  const list = rows(games.game);
  const nodes = [
    list.length ? h('div', { class: 'grid' }, list.map((g) => gameCard(g, 'c-' + id))) : empty(C(398, 'No games yet'), ''),
    section(C(849, 'Manage'), 'user'),
    group(
      row({ key: 'rename', title: C(843, 'Rename Collection'), icon: 'pen', on: async () => {
        const v = await form(C(843, 'Rename Collection'), [{ name: 'name', label: C(539, 'Collection name'), value: col ? col[1] : '' }]);
        if (v && v.name.trim()) await act('coll.rename', { id, name: v.name.trim() }, { topics: ['library'] });
      } }),
      row({ key: 'delcol', title: C(839, 'Delete Collection'), icon: 'trash-can', danger: true, detail: C(850, 'Games stay in your library; only the collection goes away'),
        arm: { prompt: C(191, 'Press again to confirm') }, on: async () => {
          await act('coll.delete', { id }, { topics: ['library'] });
          toast(C(848, 'Collection deleted'));
          go('library');
        } })),
  ];
  return { title: col ? col[1] : C(537, 'Collections'), kicker: gamesText(list.length), back: 'library', nodes, topics: ['library'] };
}

async function gamePage(id, from) {
  const d = await get('details', { id }, true);
  const g = rows(d.game)[0] || [];
  const [, system, title, cover, , last, time, avail, , year, players] = g;
  const meta = (k) => d['meta_' + k] || '';
  const genre = meta('genre') || g[11] || '';
  const backTo = from && from.startsWith('c-') ? 'library/collection/' + from.slice(2) : 'library/system/' + system;
  const chips = [];
  if (players || meta('players')) chips.push(h('span', {}, icon('user-group'), meta('players') || players));
  if (meta('rating')) chips.push(h('span', {}, icon('star'), meta('rating')));
  chips.push(h('span', {}, icon('clock'), Number(last) ? agoText(last) + ' • ' + playtimeText(time) : C(405, 'Never played')));
  const members = rows(d.member);
  const inside = members.filter((m) => m[2] === '1').map((m) => m[1]);
  const actions = group(
    row({ key: 'col', title: C(838, 'Add to Collection'), icon: 'layer-group', value: inside.join(', '), chevron: true, on: async () => {
      const opts = members.map((m) => ({ value: m[0], label: m[1], detail: m[2] === '1' ? C(222, 'On') : C(54, 'Off') }));
      opts.push({ value: '__new', label: C(538, 'New Collection') });
      const v = await choose(C(537, 'Collections'), opts);
      if (!v) return;
      if (v === '__new') { const n = await newCollection(id); if (n) toast(C(846, 'Added to {0}', n)); return; }
      const m = members.find((x) => x[0] === v);
      const adding = m[2] !== '1';
      await act(adding ? 'coll.add' : 'coll.remove', { id: v, game: id }, { topics: ['details', 'library'] });
      toast(adding ? C(846, 'Added to {0}', m[1]) : C(847, 'Removed from {0}', m[1]));
    } }),
    row({ key: 'gs', title: C(524, 'Game Settings'), icon: 'sliders', chevron: true, on: () => go('library/gamesettings/' + id + '/' + system) }),
    row({ key: 'meta', title: meta('match') || meta('title') ? C(560, 'Update Metadata') : C(559, 'Get Metadata'), icon: 'rotate', on: async () => {
      await act('scraper.game', { id, mode: meta('match') ? 'force' : '' });
      toast(C(571, 'Getting metadata'), title);
    } }),
    row({ key: 'dl', title: T('download'), icon: 'download', value: sizeText(d.size), disabled: avail === '0' || !d.file,
      on: () => { location.href = '/api/download?' + new URLSearchParams({ p: d.file }); } }),
    row({ key: 'stats', title: C(549, 'Reset Play Statistics'), icon: 'rotate-left', arm: { prompt: C(598, 'Press again to reset') },
      on: () => act('lib.statsreset', { id }, { topics: ['details', 'library'] }) }),
    row({ key: 'del', title: C(544, 'Delete Game'), icon: 'trash-can', danger: true,
      detail: avail === '0' ? C(1318, 'Removes the game, its cover and details from the library for every user. Saves are kept.')
        : d.folder === '1' ? C(1475, "Permanently deletes the game's folder and everything in it for every user. Saves are kept.")
        : C(545, 'Permanently deletes the game file for every user. Saves are kept.'),
      arm: { prompt: C(546, 'Press again to delete permanently') }, on: async () => {
        await act('lib.delete', { id }, { topics: ['library'] });
        toast(C(547, 'Game deleted'), title);
        go(backTo);
      } }));
  const info = [
    [C(558, 'File'), d.path.startsWith('sd2:') ? C(64, 'SD2') + ' • ' + d.path.slice(4) : d.path], [T('added'), dateText(d.added, false)],
    [C(550, 'Sessions'), d.sessions], [C(551, 'Last Played'), Number(last) ? dateText(last) : C(405, 'Never played')],
    [C(552, 'Time Played'), playtimeText(time)], [C(553, 'Developer'), meta('developer')], [C(554, 'Publisher'), meta('publisher')],
    [C(555, 'Release Date'), meta('release')], [C(556, 'Genre'), genre], [C(557, 'Players'), meta('players') || (Number(players) ? players : '')],
  ].filter(([, v]) => v);
  const nodes = [
    h('div', { class: 'details' },
      cover ? h('img', { class: 'cover', src: img(cover, 480), alt: '' }) : h('div', { class: 'cover' }),
      h('div', { class: 'dbody' }, h('div', { class: 'kicker', text: d.system_name || system }), h('h2', { text: title }),
        h('div', { class: 'meta', text: [Number(year) ? year : '', genre, meta('developer')].filter(Boolean).join('  •  ') }),
        h('div', { class: 'chips' }, chips), meta('description') ? h('div', { class: 'desc', text: meta('description').replace(/\\n/g, '\n') }) : null,
        actions)),
    section(C(523, 'Details'), null),
    group(info.map(([k, v]) => row({ key: 'i:' + k, title: k, value: String(v) }))),
  ];
  return { title: C(522, 'Game Details'), back: backTo, nodes, topics: ['library'], backdrop: cover ? img(cover, 96) : null };
}

/* Game Settings (id) or System Settings (id = null, GitHub #12): a key a
 * game leaves unset follows its system, a system's the default. */
async function settingsPage(id, system, controls) {
  const sys = !id;
  const [s, lib] = await Promise.all([sys ? get('system_settings', { system }, true) : get('game_settings', { id, system }, true), get('library')]);
  const sysName = (rows(lib.system).find((x) => x[0] === system) || [])[1] || system;
  const def = (v) => C(528, 'Default ({0})', v);
  const inh = (sv, label, fallback) => (sv ? C(1448, 'System ({0})', label) : def(fallback));
  const aspects = { core: C(529, 'As the game'), '4:3': '4:3', '16:9': '16:9', full: C(530, 'Full Screen'), integer: C(531, 'Integer Scaling'), square: C(1413, 'Square Pixels') };
  const filters = { sharp: C(532, 'Sharp'), smooth: C(533, 'Smooth'), pixel: C(534, 'Pixel Perfect'), crt: C(777, 'CRT Effect') };
  const accuracies = { fast: C(1315, 'Fast'), accurate: C(1316, 'Accurate') };
  const paks = { rumble: C(1340, 'Rumble Pak'), memory: C(1341, 'Controller Pak') };
  const resolutions = { auto: C(52, 'Auto'), '1x': C(1379, '1x'), '2x': C(1380, '2x'), custom: C(315, 'Custom') };
  const frameskips = { off: C(54, 'Off'), auto: C(52, 'Auto'), max: C(1465, 'Auto (up to 3)'), custom: C(315, 'Custom') };
  const cores = (s.cores || '').split(/[ ,]/).filter(Boolean);
  const topic = sys ? 'system_settings' : 'game_settings';
  const set = (key, value) => (sys ? act('system.set', { system, key, value }, { topics: [topic] })
    : act('game.set', { id, system, key, value }, { topics: [topic] }));
  const pick = async (key, title, options, cur) => { const v = await choose(title, options, cur); if (v !== null) await set(key, v); };
  const coreInh = inh(s.system_core, s.system_core, s.core_default);
  const aspectInh = inh(s.system_aspect, aspects[s.system_aspect], C(531, 'Integer Scaling'));
  const filterInh = inh(s.system_filter, filters[s.system_filter], filters[s.filter_default] || '');
  const accInh = inh(s.system_accuracy, accuracies[s.system_accuracy], accuracies[s.accuracy_default] || accuracies.fast);
  const resInh = inh(s.system_resolution, resolutions[s.system_resolution], resolutions[s.resolution_default] || resolutions['1x']);
  const skipInh = inh(s.system_frameskip, frameskips[s.system_frameskip], frameskips[s.frameskip_default] || frameskips.off);
  const pakOptions = s.system_pak
    ? [{ value: '', label: C(1448, 'System ({0})', paks[s.system_pak]) }, { value: 'rumble', label: paks.rumble }, { value: 'memory', label: paks.memory }]
    : [{ value: '', label: paks.rumble }, { value: 'memory', label: paks.memory }];
  const pakValue = s.pak ? paks[s.pak] : s.system_pak ? C(1448, 'System ({0})', paks[s.system_pak]) : paks.rumble;
  /* Controls (user request 2026-10-29): what each control of the console
   * presses on the system's pad; the D-pad is every system's. */
  const here = sys ? 'gaming/systems/' + system : 'library/gamesettings/' + id + '/' + system;
  const list = (v) => (v || '').split(',').filter((p) => p.includes(':')).map((p) => p.split(':'));
  const pad = list(s.pad), own = list(s.remap), sysRemap = list(s.system_remap);
  const DPAD = ['menu_up', 'menu_down', 'menu_left', 'menu_right'];
  const padLabel = (t) => {
    if (t === 'none') return C(173, 'None');
    if (DPAD.includes(t)) return ['↑', '↓', '←', '→'][DPAD.indexOf(t)];
    const x = pad.find((p) => p[0] === t);
    return x && x[1].startsWith('#') ? C(+x[1].slice(1), '') : x && x[1] ? x[1] : controlName(t);
  };
  const padDefault = (c) => (DPAD.includes(c) || pad.some((p) => p[0] === c) ? padLabel(c) : C(173, 'None'));
  const remapOf = (l, c) => (l.find((p) => p[0] === c) || [])[1] || '';
  const remapInh = (c) => inh(remapOf(sysRemap, c), padLabel(remapOf(sysRemap, c)), padDefault(c));
  if (controls) {
    const targets = [...pad.map((p) => p[0]), ...DPAD, 'none'];
    return {
      title: C(1456, 'Controls'), kicker: sys ? sysName : C(524, 'Game Settings'), back: here, topics: [],
      nodes: [group(
        ...CONTROLS.map(([c]) => row({ key: 'ctl:' + c, title: controlName(c), chevron: true,
          value: remapOf(own, c) ? padLabel(remapOf(own, c)) : remapInh(c),
          on: () => pick('remap_' + c, controlName(c), [{ value: '', label: remapInh(c) },
            ...targets.map((t) => ({ value: t, label: DPAD.includes(t) ? controlName(t) : padLabel(t) }))], remapOf(own, c)) })),
        row({ key: 'ctl-reset', title: C(1458, 'Reset Controls'), detail: C(536, 'Changes apply the next time the game starts'), disabled: !own.length,
          arm: { prompt: C(598, 'Press again to reset') }, on: () => set('remap', '') }))],
    };
  }
  const controlsValue = own.length ? C(1463, 'Custom') : sysRemap.length ? C(1448, 'System ({0})', C(1463, 'Custom')) : '';
  return {
    title: sys ? sysName : C(524, 'Game Settings'), kicker: sys ? C(1454, 'System Settings') : '',
    back: sys ? 'gaming/systems' : 'library/game/' + id, topics: [],
    nodes: [group(
      !sys && cores.length ? row({ key: 'sys', title: C(1452, '{0} Settings', sysName), detail: C(1453, 'Settings shared by every game of this system'),
        chevron: true, on: () => go('gaming/systems/' + system) }) : null,
      row({ key: 'core', title: C(525, 'Emulator'), value: s.core_override || coreInh, chevron: true, disabled: cores.length < 2,
        on: () => pick('core', C(525, 'Emulator'), [{ value: '', label: coreInh }, ...cores.map((c) => ({ value: c, label: c }))], s.core_override) }),
      row({ key: 'aspect', title: C(526, 'Aspect Ratio'), value: s.aspect ? aspects[s.aspect] || s.aspect : aspectInh, chevron: true,
        on: () => pick('aspect', C(526, 'Aspect Ratio'), [{ value: '', label: aspectInh }, ...Object.entries(aspects).map(([value, label]) => ({ value, label }))], s.aspect) }),
      row({ key: 'filter', title: C(527, 'Video Filter'), value: s.filter ? filters[s.filter] : filterInh, chevron: true,
        on: () => pick('filter', C(527, 'Video Filter'), [{ value: '', label: filterInh }, ...['sharp', 'smooth', 'pixel'].map((v) => ({ value: v, label: filters[v] }))], s.filter) }),
      s.accuracy_supported === '1' ? row({ key: 'accuracy', title: C(1314, 'Emulation Accuracy'),
        detail: ['dreamcast', 'naomi', 'atomiswave'].includes(system) ? C(1383, 'Fast can draw smoke and glass wrong') : C(1317, 'Choose Accurate if the game misbehaves'),
        value: s.accuracy ? accuracies[s.accuracy] : accInh, chevron: true,
        on: () => pick('accuracy', C(1314, 'Emulation Accuracy'), [{ value: '', label: accInh }, ...Object.entries(accuracies).map(([value, label]) => ({ value, label }))], s.accuracy) }) : null,
      s.pak_supported === '1' ? row({ key: 'pak', title: C(1339, 'Controller Accessory'), detail: C(1342, 'One fits at a time: choose Controller Pak if the game saves on it'),
        value: pakValue, chevron: true, on: () => pick('pak', C(1339, 'Controller Accessory'), pakOptions, s.pak || '') }) : null,
      s.resolution_supported === '1' ? row({ key: 'resolution', title: C(454, 'Resolution'),
        detail: s.resolution_default === 'auto' ? C(1381, 'Auto: 1x on the console, 2x on a TV') : C(1382, '2x is sharper but can slow the game down'),
        value: s.resolution ? resolutions[s.resolution] : resInh, chevron: true,
        on: () => pick('resolution', C(454, 'Resolution'), [{ value: '', label: resInh }, { value: '1x', label: resolutions['1x'] }, { value: '2x', label: resolutions['2x'] }], s.resolution) }) : null,
      s.frameskip_supported === '1' ? row({ key: 'frameskip', title: C(1464, 'Frameskip'), detail: C(1466, 'Auto skips frames only when the game slows down'),
        value: s.frameskip ? frameskips[s.frameskip] : skipInh, chevron: true,
        on: () => pick('frameskip', C(1464, 'Frameskip'), [{ value: '', label: skipInh }, { value: 'off', label: frameskips.off }, { value: 'auto', label: frameskips.auto }, { value: 'max', label: frameskips.max }], s.frameskip || '') }) : null,
      cores.length ? row({ key: 'controls', title: C(1456, 'Controls'), detail: C(1457, 'Choose what each button does in the game'),
        value: controlsValue, chevron: true, on: () => go(here + '/controls') }) : null,
      sys && +s.games > 0 ? row({ key: 'games', title: C(1450, 'Apply to All Games'), detail: C(1449, '{0} games use their own settings', s.games),
        arm: { prompt: C(191, 'Press again to confirm') }, on: () => act('system.reset_games', { system }, { topics: [topic] }) }) : null,
      sys ? row({ key: 'reset', title: C(1451, 'Reset System Settings'), detail: C(536, 'Changes apply the next time the game starts'),
        arm: { prompt: C(598, 'Press again to reset') }, on: () => act('system.reset', { system }, { topics: [topic] }) })
        : row({ key: 'reset', title: C(535, 'Reset Game Settings'), detail: C(536, 'Changes apply the next time the game starts'),
          arm: { prompt: C(598, 'Press again to reset') }, on: () => act('game.reset', { id }, { topics: [topic] }) }))],
  };
}

/* Gaming → System Settings: the systems with games and their emulator. */
async function systemsPage() {
  const lib = await get('library');
  const list = (await Promise.all(rows(lib.system).map(async (x) => {
    const s = await get('system_settings', { system: x[0] }, true).catch(() => ({}));
    return (s.cores || '') ? { id: x[0], name: x[1], core: s.core } : null;
  }))).filter(Boolean);
  return {
    title: C(1454, 'System Settings'), back: 'gaming', topics: ['library'],
    nodes: [list.length ? group(...list.map((x) => row({ key: x.id, title: x.name, value: x.core, chevron: true, on: () => go('gaming/systems/' + x.id) })))
      : empty(C(398, 'No games yet'), '')],
  };
}

/* ============================================================ FILES */

let clipboard = null;

const FILE_ICONS = [[/\.(png|jpe?g|gif|webp|bmp)$/i, 'image'], [/\.(mp3|ogg|flac|wav|m4a|opus)$/i, 'music'], [/\.(mp4|mkv|avi|webm|mov)$/i, 'film'],
  [/\.(zip|7z|tar|gz|rar)$/i, 'box-archive']];

page('files', 'manage', () => C(609, 'Files'), async (path) => {
  if (path[0] === ':shares') return sharesPage();
  if (!path.length) {
    const [r, media] = await Promise.all([get('roots', {}, true), get('media').catch(() => ({}))]);
    const roots = rows(r.root);
    const ejectable = new Set(rows(media.volume).map((v) => v[0]));
    return {
      title: C(609, 'Files'), topics: ['roots'],
      nodes: [group(roots.map((x) => {
        const [name, p, total, free, removable] = x;
        const label = name === 'userdata' ? C(610, 'Internal Storage') : name;
        return h('div', { class: 'rows' }, row({ key: 'r:' + p, title: label, icon: removable === '1' || ejectable.has(name) ? 'hard-drive' : p.startsWith('/run') ? 'server' : 'sd-card',
          detail: Number(total) ? C(622, '{0} free', sizeText(free)) + ' / ' + sizeText(total) : '', chevron: true, on: () => go('files' + p) }));
      }), row({ key: 'shares', title: C(611, 'Network Shares'), icon: 'network-wired', chevron: true, on: () => go('files/:shares') }))],
    };
  }
  const abs = '/' + path.join('/');
  const r = await get('files', { path: abs }, true);
  const entries = rows(r.entry).sort((a, b) => (a[0] === b[0] ? a[3].localeCompare(b[3], LANG, { numeric: true }) : a[0] === 'd' ? -1 : 1));
  const crumbs = h('div', { class: 'crumbs' }, path.map((seg, i) => [i ? '/' : null,
    h('button', { type: 'button', text: i === 0 && seg === 'userdata' ? C(610, 'Internal Storage') : seg, on: { click: () => go('files/' + path.slice(0, i + 1).join('/')) } })]));
  const tools = h('div', { class: 'actions' },
    button(C(612, 'New Folder'), async () => {
      const v = await form(C(612, 'New Folder'), [{ name: 'name', label: C(613, 'Folder name') }]);
      if (v && v.name.trim()) await act('files.mkdir', { dir: abs, name: v.name.trim() });
    }, { icon: 'folder-plus', round: true, key: 'mkdir' }),
    clipboard ? button(C(614, 'Paste Here'), async () => {
      await act(clipboard.op === 'move' ? 'files.move' : 'files.copy', { path: clipboard.path, dir: abs });
      toast(clipboard.op === 'move' ? C(624, 'Moving files') : C(623, 'Copying files'), clipboard.path.split('/').pop());
      clipboard = null;
    }, { icon: 'paste', round: true, key: 'paste' }) : null);
  const nodes = [path.length > 1 ? crumbs : null,
    dropZone(T('upload_files'), T('upload_here'), (items) => sendAll(items, (x) => abs + (x.rel ? '/' + x.rel : '')).then(refresh))];
  if (!entries.length) nodes.push(empty(C(621, 'Empty folder'), ''));
  nodes.push(group(entries.map((e) => {
    const [type, size, mtime, name] = e;
    const p = abs + '/' + name;
    const dir = type === 'd';
    const ic = dir ? 'folder' : (FILE_ICONS.find(([re]) => re.test(name)) || [null, 'file'])[1];
    return row({ key: 'e:' + name, title: name, icon: ic, image: !dir && /\.(png|jpe?g|webp|gif)$/i.test(name) ? img(p, 96) : null,
      detail: dir ? dateText(mtime) : sizeText(size) + ' • ' + dateText(mtime), chevron: dir,
      on: () => (dir ? go('files' + p) : fileMenu(p, name, false)), more: true, path: p, name, dir });
  })));
  /* Every entry: its own menu button (folders open on click). */
  for (const el of $$('.row', nodes[nodes.length - 1])) {
    const name = el.dataset.key.slice(2);
    const p = abs + '/' + name;
    const isDir = entries.find((e) => e[3] === name)?.[0] === 'd';
    const wrap = h('div', { class: 'entry' });
    el.replaceWith(wrap);
    wrap.append(el, button(C(849, 'Manage'), () => fileMenu(p, name, isDir), { icon: 'ellipsis-vertical', round: true, key: 'm:' + name }));
  }
  return { title: path.length === 1 && path[0] === 'userdata' ? C(610, 'Internal Storage') : path[path.length - 1], back: path.length > 1 ? 'files/' + path.slice(0, -1).join('/') : 'files', nodes, tools, topics: [] };
});

async function fileMenu(p, name, dir) {
  const opts = [];
  if (!dir) opts.push({ value: 'download', label: T('download') });
  opts.push({ value: 'copy', label: C(615, 'Copy') }, { value: 'move', label: C(616, 'Move') }, { value: 'rename', label: C(617, 'Rename') }, { value: 'delete', label: C(618, 'Delete') });
  const v = await choose(name, opts);
  if (v === 'download') location.href = '/api/download?' + new URLSearchParams({ p });
  else if (v === 'copy' || v === 'move') { clipboard = { op: v, path: p }; toast(v === 'copy' ? C(615, 'Copy') : C(616, 'Move'), T('paste_hint')); }
  else if (v === 'rename') {
    const f = await form(C(617, 'Rename'), [{ name: 'name', label: C(617, 'Rename'), value: name }]);
    if (f && f.name.trim() && f.name !== name) await act('files.rename', { path: p, name: f.name.trim() });
  } else if (v === 'delete') {
    const ok = await form(C(620, 'Delete "{0}"?', name), [], { ok: C(618, 'Delete') });
    if (ok) await act('files.delete', { path: p });
  }
  refresh();
}

async function sharesPage() {
  const [s, media] = await Promise.all([get('shares', {}, true), get('media', {}, true).catch(() => ({}))]);
  const shares = rows(s.share);
  const vols = rows(media.volume);
  return {
    title: C(611, 'Network Shares'), back: 'files', topics: ['shares'],
    nodes: [
      group(shares.map((x) => {
        const [name, addr, , mounted] = x;
        return row({ key: 'sh:' + name, title: name, detail: addr, icon: 'server', pill: { text: mounted === '1' ? C(198, 'Connected') : C(445, 'Offline'), cls: mounted === '1' ? 'on' : '' },
          on: async () => {
            const v = await choose(name, [
              mounted === '1' ? { value: 'open', label: C(414, 'Open') } : { value: 'mount', label: C(158, 'Connect') },
              mounted === '1' ? { value: 'umount', label: C(169, 'Disconnect') } : null,
              { value: 'remove', label: C(644, 'Remove') }].filter(Boolean));
            if (v === 'open') go('files/run/nuubos/shares/' + name);
            else if (v) await act('share.' + v, { name }).catch(() => toast(C(645, 'The share could not be reached'), '', true));
          } });
      }),
      row({ key: 'addshare', title: C(641, 'Add Share'), icon: 'plus', on: async () => {
        const v = await form(C(641, 'Add Share'), [{ name: 'name', label: C(642, 'Share name') }, { name: 'address', label: C(643, 'Share address (//server/share)'), placeholder: '//server/share' },
          { name: 'user', label: C(285, 'Username') }, { name: 'password', label: C(172, 'Password'), type: 'password' }]);
        if (v) await act('share.add', v);
      } })),
      section(C(625, 'Removable Media'), 'device'),
      group(vols.length ? vols.map((m) => row({ key: 'vol:' + m[0], title: m[0], icon: 'hard-drive', detail: m[1] || '', value: C(626, 'Eject'),
        on: async () => { await act('media.eject', { name: m[0] }, { topics: ['media', 'roots'] }); toast(C(627, 'Safe to remove'), m[0]); } }))
        : row({ key: 'novol', title: C(629, 'No removable media'), disabled: true })),
    ],
  };
}

/* ======================================================== BIOS FILES */

const BIOS_STATES = { ready: () => C(66, 'Ready'), ok: () => C(66, 'Ready'), missing: () => C(577, 'Missing'), invalid: () => C(578, 'Invalid'),
  optional: () => C(579, 'Optional'), unverified: () => C(580, 'Not verifiable') };
const biosPill = (s) => ({ text: (BIOS_STATES[s] || (() => s))(), cls: s === 'ready' || s === 'ok' ? 'on' : s === 'invalid' || s === 'missing' ? 'bad' : '' });

function biosModel(b) {
  const systems = rows(b.system).map((s) => ({ id: s[0], name: s[1], state: s[3], files: [] }));
  for (const f of rows(b.file)) {
    const s = systems.find((x) => x.id === f[0]);
    if (s) s.files.push({ path: f[1], required: f[2] === '1', state: f[3], description: f[4] });
  }
  return systems;
}

/* Dropped BIOS files go where the BIOS Manager expects a file of that
 * name; the manager then checks them (EPIC-033). */
function biosUpload(items, systems) {
  const wanted = new Map();
  for (const s of systems) for (const f of s.files) wanted.set(f.path.split('/').pop().toLowerCase(), f.path);
  const known = items.filter((x) => wanted.has(x.file.name.toLowerCase()));
  const unknown = items.filter((x) => !known.includes(x));
  if (unknown.length) toast(T('bios_unknown', unknown.length), unknown.slice(0, 3).map((x) => x.file.name).join(', '), true, 6000);
  if (!known.length) return;
  const byFile = new Map(known.map((x) => [x.file, wanted.get(x.file.name.toLowerCase())]));
  uploadFiles(known.map((x) => x.file), (f) => { const p = byFile.get(f); const i = p.lastIndexOf('/'); return i > 0 ? BIOS + '/' + p.slice(0, i) : BIOS; },
    { replace: true, mkdirs: true, after: 'bios' }).then(() => { forget('bios'); refresh(); });
}

page('bios', 'manage', () => C(576, 'BIOS Files'), async (path) => {
  const [b, lib] = await Promise.all([get('bios'), get('library')]);
  const systems = biosModel(b);
  const zone = dropZone(T('upload_bios'), C(581, 'Copy BIOS files to the bios folder on the SD card. nuubOS never includes them.'), (items) => biosUpload(items, systems), { folders: false });
  if (path[0]) {
    const s = systems.find((x) => x.id === path[0]);
    if (!s) return { title: C(576, 'BIOS Files'), back: 'bios', nodes: [empty(C(205, 'Unavailable'))] };
    return {
      title: s.name, kicker: C(576, 'BIOS Files'), back: 'bios', topics: ['bios'],
      nodes: [zone, group(s.files.map((f) => row({ key: 'f:' + f.path, title: f.path, detail: f.required ? f.description : C(579, 'Optional') + ' • ' + f.description,
        pill: biosPill(f.state), on: () => {
          const input = h('input', { type: 'file' });
          input.addEventListener('change', () => input.files[0] && biosUpload([{ file: new File([input.files[0]], f.path.split('/').pop()), rel: '' }], systems));
          input.click();
        } })))],
    };
  }
  const mine = new Set(rows(lib.system).map((s) => s[0]));
  const own = systems.filter((s) => mine.has(s.id)), other = systems.filter((s) => !mine.has(s.id));
  const sysRow = (s) => row({ key: 's:' + s.id, title: s.name, detail: C(582, '{0} of {1} present', s.files.filter((f) => f.state === 'ok').length, s.files.length),
    pill: biosPill(s.state), chevron: true, on: () => go('bios/' + s.id) });
  return {
    title: C(576, 'BIOS Files'), topics: ['bios'],
    nodes: [zone, section(C(760, 'YOUR SYSTEMS'), 'device'), group(own.length ? own.map(sysRow) : row({ key: 'none', title: C(398, 'No games yet'), disabled: true })),
      section(C(761, 'OTHER SYSTEMS'), 'device'), group(other.map(sysRow))],
  };
});

/* ========================================================== GENERAL */

async function pickAndAct(title, options, cur, op, key = 'v', topics = []) {
  const v = await choose(title, options, cur);
  if (v !== null) await act(op, { [key]: v }, { topics });
}

page('general', 'settings', () => C(4, 'General'), async (path) => {
  const [users, loc, reg, themes] = await Promise.all([get('users'), get('localization'), get('regional'), get('themes')]);
  const all = rows(users.user);
  const me = all.find((u) => u[0] === users.active);
  if (path[0] === 'users') return usersPage(all);
  if (path[0] === 'profile') return profilePage(path[1] || users.active, all, users, !!path[1]);
  if (path[0] === 'avatar') return avatarPage(path[1] || users.active);
  const theme = rows(themes.list).find((t) => t[0] === themes.selected);
  const auto = on01(reg.automatic_time);
  const many = all.length > 1;
  const nodes = [
    section(C(333, 'PROFILE'), 'user'),
    group(
      row({ key: 'profile', title: C(192, 'Profile'), image: me && me[3] ? img(me[3], 96) : null, value: me ? me[1] : '', chevron: true, on: () => go('general/profile') }),
      row({ key: 'lang', title: C(193, 'Language'), value: (LANGUAGES.find((l) => l[0] === loc.language) || ['', loc.language])[1], chevron: true,
        on: () => pickAndAct(C(193, 'Language'), LANGUAGES.map(([value, label]) => ({ value, label })), loc.language, 'lang.set') }),
      row({ key: 'mood', title: C(194, 'Mood'), value: theme ? theme[1] : C(195, 'Default mood'), chevron: true, on: async () => {
        const v = await choose(C(194, 'Mood'), rows(themes.list).map((t) => ({ value: t[0], label: t[1], detail: t[2] })), themes.selected);
        if (v) { await act('mood.select', { id: v }, { topics: ['themes'] }); loadMood(); }
      } })),
    section(C(334, 'USERS'), 'device'),
    group(
      row({ key: 'users', title: C(287, 'Manage Users'), value: String(all.length), chevron: true, on: () => go('general/users') }),
      row({ key: 'login', title: C(276, 'Startup'), value: users.login_mode === 'default' ? C(278, 'Direct Login') : C(277, 'User Picker'), chevron: true, disabled: !many,
        on: () => pickAndAct(C(276, 'Startup'), [{ value: 'select', label: C(277, 'User Picker'), detail: C(307, 'Show user picker at boot') },
          { value: 'default', label: C(278, 'Direct Login'), detail: C(308, 'Load the default user at boot') }], users.login_mode, 'user.loginmode') }),
      row({ key: 'defuser', title: C(324, 'Boot User'), value: (all.find((u) => u[0] === users.default_user) || [, C(173, 'None')])[1], chevron: true,
        disabled: !many || users.login_mode !== 'default',
        on: () => pickAndAct(C(324, 'Boot User'), all.map((u) => ({ value: u[0], label: u[1] })), users.default_user, 'user.default', 'id') })),
    section(C(335, 'DATE & TIME'), 'device'),
    group(
      row({ key: 'tz', title: C(280, 'Timezone'), value: reg.timezone, chevron: true, on: async () => {
        const z = await get('timezones');
        await pickAndAct(C(280, 'Timezone'), list(z.zone).map((v) => ({ value: v, label: v.replace(/_/g, ' ') })), reg.timezone, 'tz.set');
      } }),
      row({ key: 'auto', title: C(281, 'Automatic Time'), toggle: auto, set: (v) => act('time.auto', { v }) }),
      row({ key: 'date', title: C(326, 'Date'), value: reg.local_date, chevron: !auto, disabled: auto, on: async () => {
        const v = await form(C(326, 'Date'), [{ name: 'd', label: C(326, 'Date'), type: 'date', value: reg.local_date }]);
        if (v && v.d) await act('time.set', { v: v.d + ' ' + reg.local_time.slice(0, 5) });
      } }),
      row({ key: 'time', title: C(327, 'Time'), value: reg.local_time, chevron: !auto, disabled: auto, on: async () => {
        const v = await form(C(327, 'Time'), [{ name: 't', label: C(327, 'Time'), type: 'time', value: reg.local_time }]);
        if (v && v.t) await act('time.set', { v: reg.local_date + ' ' + v.t.slice(0, 5) });
      } }),
      row({ key: 'sync', title: C(282, 'Sync Now'), value: Number(reg.last_sync) ? dateText(reg.last_sync) : C(302, 'Never'), disabled: !auto, on: async () => {
        toast(C(323, 'Syncing…'));
        try { await act('time.sync', {}, { quiet: true }); toast(C(299, 'Time synchronized')); } catch (e) { toast(C(300, 'Time synchronization failed'), '', true); }
      } })),
    section(C(336, 'KEYBOARD'), 'device'),
    group(row({ key: 'kbd', title: C(283, 'Keyboard Layout'), value: reg.keyboard_layout, chevron: true, on: async () => {
      const k = await get('keyboards');
      await pickAndAct(C(283, 'Keyboard Layout'), rows(k.keyboard).map((r) => ({ value: r[0], label: r[1], detail: r[2] })), reg.keyboard_layout, 'kbd.set');
    } })),
  ];
  return { title: C(4, 'General'), nodes, topics: ['users', 'localization', 'regional', 'themes'] };
});

/* Manage Users: every profile, then Add User (console page 25). */
function usersPage(all) {
  return {
    title: C(287, 'Manage Users'), back: 'general', topics: ['users'],
    nodes: [group(
      all.map((u) => row({ key: 'u:' + u[0], title: u[1], image: img(u[3], 96),
        value: u[4] === '1' ? C(304, 'Active') : u[5] === '1' ? C(303, 'Default') : '', chevron: true, on: () => go('general/profile/' + u[0]) })),
      row({ key: 'adduser', title: C(288, 'Add User'), icon: 'plus', on: async () => {
        const v = await form(C(295, 'Create User'), [{ name: 'name', label: C(296, 'Enter username') }]);
        if (v && v.name.trim()) { await act('user.create', { name: v.name.trim() }); toast(C(318, 'User created')); }
      } }))],
  };
}

async function profilePage(id, all, users, fromList) {
  const u = all.find((x) => x[0] === id);
  if (!u) return { title: C(192, 'Profile'), back: 'general', nodes: [empty(C(205, 'Unavailable'))] };
  const active = u[0] === users.active;
  const nodes = [group(
    row({ key: 'name', title: C(285, 'Username'), value: u[1], chevron: true, on: async () => {
      const v = await form(C(284, 'Edit Profile'), [{ name: 'name', label: C(285, 'Username'), value: u[1] }]);
      if (v && v.name.trim()) { await act('user.rename', { id, name: v.name.trim() }); toast(C(320, 'Profile updated')); }
    } }),
    row({ key: 'pic', title: C(286, 'Profile Picture'), detail: C(294, 'Custom pictures from your profile folder'), image: img(u[3], 96), chevron: true, on: () => go('general/avatar/' + id) }),
    active ? null : row({ key: 'switch', title: C(292, 'Switch User'), detail: T('switch_hint'), on: () => act('user.activate', { id }) }),
    active ? null : row({ key: 'del', title: C(289, 'Delete User'), detail: C(305, 'Profile data only; shared games and apps are kept'),
      disabled: all.length < 2, danger: true, arm: { prompt: C(290, 'Press again to delete') },
      on: async () => { await act('user.delete', { id }); toast(C(319, 'User deleted')); go('general/users'); } }),
  )];
  return { title: u[1], kicker: C(192, 'Profile'), back: fromList ? 'general/users' : 'general', nodes, topics: ['users'] };
}

async function avatarPage(id) {
  const a = await get('avatars', { user: id }, true);
  const items = rows(a.avatar);
  const grid = h('div', { class: 'grid avatars' }, items.map((x) => h('button', { class: 'card', type: 'button', data: { key: 'a:' + x[0] }, title: x[1],
    on: { click: async () => { await act('user.avatar', { id, spec: x[0] }); toast(C(320, 'Profile updated')); go('general/profile/' + id); } } },
  h('img', { src: img(x[2], 192), alt: x[1], loading: 'lazy' }))));
  return {
    title: C(293, 'Profile Pictures'), back: 'general/profile/' + id, topics: [],
    nodes: [dropZone(C(315, 'Custom'), C(294, 'Custom pictures from your profile folder') + ' (PNG, JPG)', (files) => {
      const okFiles = files.filter((x) => /\.(png|jpe?g)$/i.test(x.file.name));
      uploadFiles(okFiles.map((x) => x.file), () => `/userdata/users/${id}/profile/avatars`, { mkdirs: true }).then(() => { forget('avatars'); refresh(); });
    }, { accept: '.png,.jpg,.jpeg', folders: false }), grid],
  };
}

/* ===================================================== CONNECTIVITY */

const SIGNAL = (dbm) => (dbm >= -55 ? C(200, 'Excellent') : dbm >= -67 ? C(201, 'Good') : dbm >= -75 ? C(202, 'Fair') : C(203, 'Weak'));
const WIFI_STATES = { connected: () => C(198, 'Connected'), connecting: () => C(254, 'Connecting to {0}…', ''), disconnected: () => C(159, 'Not connected') };

let scanning = null;
function stopScans() {
  if (scanning === 'wifi') act('wifi.scan', { v: 0 }, { quiet: true }).catch(() => {});
  if (scanning === 'bt') act('bt.discover', { v: 0 }, { quiet: true }).catch(() => {});
  scanning = null;
}
window.addEventListener('hashchange', () => {
  const p = routeOf();
  const want = p[0] === 'connectivity' && p[1] === 'wifi' ? 'wifi' : p[0] === 'connectivity' && p[1] === 'bt' ? 'bt' : null;
  if (want !== scanning) stopScans();
});
window.addEventListener('pagehide', stopScans);

async function wifiConnect(n) {
  if (n.security === 'open' || n.security === 'Open' || n.saved) {
    await act('wifi.connect', { ssid: n.ssid, password: '', hidden: false });
  } else {
    const v = await form(n.ssid, [{ name: 'password', label: C(172, 'Password'), type: 'password' }]);
    if (!v) return;
    if (v.password.length < 8) { toast(C(263, 'Password must contain at least 8 characters'), '', true); return; }
    await act('wifi.connect', { ssid: n.ssid, password: v.password, hidden: false });
  }
  toast(C(254, 'Connecting to {0}…', n.ssid));
}

page('connectivity', 'settings', () => C(5, 'Connectivity'), async (path) => {
  if (path[0] === 'wifi') return wifiPage();
  if (path[0] === 'saved') return savedPage();
  if (path[0] === 'network') return networkPage(path[1]);
  if (path[0] === 'bt') return btPage();
  if (path[0] === 'devices') return btDevicesPage();
  if (path[0] === 'keys') return sshKeysPage();
  if (path[0] === 'tailscale') return tailscalePage();
  const [w, b, r, keys, ts] = await Promise.all([get('wifi').catch(() => null), get('bluetooth').catch(() => null), get('remote'), get('ssh_keys').catch(() => ({})),
    get('tailscale').catch(() => ({}))]);
  const addr = r.address || '';
  const nodes = [section(C(156, 'WI-FI'), 'device')];
  if (w) {
    const connected = w.enabled && w.state === 'connected';
    nodes.push(group(
      row({ key: 'wifi', title: C(157, 'Wi-Fi'), toggle: w.enabled, set: (v) => act('wifi.enable', { v }) }),
      row({ key: 'connect', title: C(158, 'Connect'), value: connected ? w.ssid : C(159, 'Not connected'), chevron: w.enabled, disabled: !w.enabled,
        on: () => go('connectivity/wifi') }),
      row({ key: 'saved', title: C(10, 'Saved Networks'), value: w.saved.length + C(225, ' saved'), chevron: true, on: () => go('connectivity/saved') })));
  } else nodes.push(group(row({ key: 'wifiwait', title: C(206, 'Waiting for Wi-Fi service'), disabled: true })));
  nodes.push(section(C(160, 'BLUETOOTH'), 'device'));
  if (b && b.present) {
    const known = b.devices.filter((d) => d.paired);
    nodes.push(group(
      row({ key: 'bt', title: C(161, 'Bluetooth'), toggle: b.powered, set: (v) => act('bt.enable', { v }) }),
      row({ key: 'btnew', title: C(11, 'Connect New Device'), chevron: b.powered, disabled: !b.powered, on: () => go('connectivity/bt') }),
      row({ key: 'btmine', title: C(12, 'My Devices'), value: known.length + C(226, ' devices'), chevron: b.powered, disabled: !b.powered, on: () => go('connectivity/devices') })));
  } else nodes.push(group(row({ key: 'btwait', title: b ? C(205, 'Unavailable') : C(207, 'Waiting for Bluetooth service'), disabled: true })));
  nodes.push(section(C(727, 'SERVICES'), 'device'));
  nodes.push(group(
    row({ key: 'ssh', title: C(600, 'SSH, SCP and SFTP'), detail: addr ? 'root@' + addr : C(601, 'Sign in as root with the device password'), toggle: on01(r.ssh),
      set: (v) => act('remote.set', { svc: 'ssh', v }, { topics: ['remote'] }) }),
    row({ key: 'keys', title: T('ssh_keys'), detail: T('ssh_keys_detail'), value: String(rows(keys.key).length), chevron: true, on: () => go('connectivity/keys') }),
    row({ key: 'smb', title: C(602, 'File Sharing (SMB)'), detail: addr ? '\\\\' + addr + '\\nuubOS' : '', toggle: on01(r.smb),
      set: (v) => act('remote.set', { svc: 'smb', v }, { topics: ['remote'] }) }),
    row({ key: 'web', title: C(603, 'Web Administration'), detail: addr ? 'https://' + addr : '', toggle: on01(r.web),
      arm: { prompt: T('web_off') }, on: () => act('remote.set', { svc: 'web', v: 0 }).then(() => showLogin(T('web_disabled'))) }),
    row({ key: 'tailscale', title: C(1328, 'Tailscale'), chevron: true,
      value: !on01(ts.installed) ? '' : on01(ts.enabled) ? C(222, 'On') : C(54, 'Off'),
      detail: on01(ts.enabled) && ts.state === 'Running' && ts.ip ? ts.ip : tsStatus(ts), on: () => go('connectivity/tailscale') }),
    row({ key: 'pw', title: C(172, 'Password'), value: revealed || '••••-••••-••••',
      detail: revealed ? C(604, 'Only on networks you trust. Never exposed to the Internet.') : C(608, 'Press to show'), on: async () => {
        revealed = revealed ? '' : (await get('password', {}, true)).password;
        setTimeout(() => { revealed = ''; refresh(); }, 30000);
      } }),
    row({ key: 'regen', title: C(606, 'Regenerate Password'), detail: C(607, 'The old password stops working') + ' • ' + T('signs_out'),
      arm: { prompt: C(191, 'Press again to confirm') }, on: async () => { await act('remote.regenerate'); showLogin(T('password_changed')); } })));
  return { title: C(5, 'Connectivity'), nodes, topics: ['wifi', 'bluetooth', 'remote', 'ssh_keys', 'tailscale'] };
});

/* Tailscale (nuubos-tsd): the same rows as the console page (tsui.rs); the
 * sign-in address opens here in a new tab. */
function tsStatus(ts) {
  if (on01(ts.failed)) return C(93, 'Failed');
  if (!on01(ts.installed) || !on01(ts.enabled)) return C(1329, 'Reach this console from anywhere through your tailnet');
  if (!on01(ts.ready)) return C(1025, 'Starting…');
  return { Running: C(198, 'Connected'), NeedsLogin: C(1331, 'Sign in to your tailnet'),
    NeedsMachineAuth: C(1334, 'Waiting for approval in your tailnet'), Stopped: C(159, 'Not connected') }[ts.state] || C(1025, 'Starting…');
}

async function tailscalePage() {
  const ts = await get('tailscale', {}, true);
  const newer = (a, b) => { const p = (v) => (v || '').split('.').map(Number); const x = p(a), y = p(b);
    for (let i = 0; i < Math.max(x.length, y.length); i++) { if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) > (y[i] || 0); } return false; };
  const ready = on01(ts.enabled) && on01(ts.ready);
  const list = [];
  if (ts.job === 'install') {
    list.push(row({ key: 'dl', title: C(1221, 'Downloading {0}%', String(Math.max(0, +ts.progress))), detail: C(619, 'Cancel'), on: () => act('ts.cancel') }));
  } else if (!on01(ts.installed)) {
    list.push(row({ key: 'dl', title: C(653, 'Download'), detail: C(1330, "Tailscale's own app, about 25 MB, downloaded from tailscale.com"), icon: 'download',
      on: () => act('ts.install') }));
  } else {
    list.push(row({ key: 'on', title: C(1328, 'Tailscale'), detail: tsStatus(ts), toggle: on01(ts.enabled), set: (v) => act(v ? 'ts.enable' : 'ts.disable') }));
    if (ready && ts.state === 'Running') {
      list.push(row({ key: 'me', title: ts.name || C(177, 'Address'), value: ts.ip || '',
        detail: C(1336, 'SSH, File Sharing and Web Administration, when on, answer at this address too') }));
      list.push(row({ key: 'out', title: C(567, 'Sign Out'), detail: ts.tailnet || '', arm: { prompt: C(191, 'Press again to confirm') }, on: () => act('ts.logout') }));
    } else if (ready && (ts.state === 'NeedsLogin' || ts.state === 'NeedsMachineAuth')) {
      list.push(row({ key: 'in', title: C(587, 'Sign In'), detail: tsStatus(ts), chevron: true, on: () => act('ts.login') }));
      if (ts.auth_url) list.push(h('div', { class: 'notice' }, h('a', { href: ts.auth_url, target: '_blank', rel: 'noopener noreferrer', text: ts.auth_url })));
    }
    list.push(row({ key: 'up', title: newer(ts.latest, ts.version) ? C(486, 'Update to {0}', ts.latest) : C(487, 'Check for Updates'),
      value: C(485, 'Version') + ' ' + ts.version, detail: ts.latest && ts.latest === ts.version ? C(489, 'Up to date') : '',
      on: () => act(newer(ts.latest, ts.version) ? 'ts.install' : 'ts.check') }));
    list.push(row({ key: 'rm', title: C(644, 'Remove'), detail: C(1335, 'Removes Tailscale and takes this console out of your tailnet'),
      arm: { prompt: C(191, 'Press again to confirm') }, on: () => act('ts.remove') }));
  }
  const nodes = [section(C(1328, 'Tailscale').toUpperCase(), 'device'), group(list)];
  if (ts.error) nodes.unshift(h('div', { class: 'notice err', text: { network: C(494, 'Check the network connection'), space: C(493, 'Not enough free space') }[ts.error] || C(1333, 'Tailscale could not be downloaded') }));
  return { title: C(1328, 'Tailscale'), back: 'connectivity', nodes, topics: ['tailscale'] };
}

let revealed = '';

/* SSH Keys (EPIC-039): public keys that sign in as root over SSH/SFTP
 * besides the password. nuubos-remotectl checks every key. */
async function addSshKeys(texts) {
  let added = 0;
  for (const t of texts) {
    for (const line of t.split(/\r?\n/).map((l) => l.trim()).filter((l) => l && !l.startsWith('#'))) {
      try { await act('ssh.key.add', { key: line }, { topics: ['ssh_keys'] }); added++; } catch (e) { /* reported */ }
    }
  }
  if (added) toast(T('ssh_key_added'));
}

async function sshKeysPage() {
  const k = await get('ssh_keys', {}, true);
  const keys = rows(k.key);
  return {
    title: T('ssh_keys'), back: 'connectivity', topics: ['ssh_keys'],
    nodes: [
      h('div', { class: 'notice', text: T('ssh_keys_hint') }),
      dropZone(T('ssh_key_add'), T('ssh_key_drop'), async (items) => addSshKeys(await Promise.all(items.map((x) => x.file.text()))), { accept: '.pub', folders: false }),
      group(
        keys.map(([fp, type, comment]) => row({ key: 'k:' + fp, title: comment || type, icon: 'lock', detail: type + ' • ' + fp,
          arm: { prompt: T('ssh_key_remove') }, on: async () => { await act('ssh.key.remove', { id: fp }, { topics: ['ssh_keys'] }); toast(T('ssh_key_removed')); } })),
        row({ key: 'paste', title: T('ssh_key_paste'), icon: 'plus', on: async () => {
          const v = await form(T('ssh_key_add'), [{ name: 'key', label: T('ssh_key_public'), placeholder: 'ssh-ed25519 AAAA… user@host' }]);
          if (v && v.key.trim()) await addSshKeys([v.key]);
        } })),
    ],
  };
}

/* Saved Networks: a flat list, each opens the network's page. */
async function savedPage() {
  const w = await get('wifi', {}, true);
  return {
    title: C(10, 'Saved Networks'), back: 'connectivity', topics: ['wifi'],
    nodes: [group(w.saved.length
      ? w.saved.map((n) => row({ key: 's:' + n.ssid, title: n.ssid, icon: 'wifi', detail: n.current ? C(198, 'Connected') : n.security,
        chevron: true, on: () => go('connectivity/network/' + n.ssid) }))
      : row({ key: 'none', title: C(10, 'Saved Networks'), value: '0' + C(225, ' saved'), disabled: true }))],
  };
}

/* My Devices: the paired Bluetooth devices. */
async function btDevicesPage() {
  const b = await get('bluetooth', {}, true);
  const known = b.devices.filter((d) => d.paired);
  return {
    title: C(12, 'My Devices'), back: 'connectivity', topics: ['bluetooth'],
    nodes: [group(known.length ? known.map(btRow) : row({ key: 'none', title: C(12, 'My Devices'), value: '0' + C(226, ' devices'), disabled: true }))],
  };
}

function btRow(d) {
  const kind = d.kind === 'audio' || /audio|headset|headphone/i.test(d.kind) ? 'headphones' : /input|gamepad|joystick/i.test(d.kind) ? 'gamepad' : 'bluetooth-b';
  return row({ key: 'bt:' + d.address, title: d.name || d.address, icon: kind,
    detail: [d.connected ? C(198, 'Connected') : d.paired ? C(442, 'Paired') : '', d.battery >= 0 ? C(789, '{0}%', d.battery) : ''].filter(Boolean).join(' • '),
    pill: d.connected ? { text: C(198, 'Connected'), cls: 'on' } : null,
    on: async () => {
      const opts = d.paired
        ? [d.connected ? { value: 'disconnect', label: C(169, 'Disconnect') } : { value: 'connect', label: C(158, 'Connect') }, { value: 'forget', label: C(170, 'Forget Device') }]
        : [{ value: 'pair', label: C(168, 'Pair & Connect') }, { value: 'pin', label: C(171, 'Pair with PIN') }];
      const v = await choose(d.name || d.address, opts);
      if (v === 'pin') {
        const f = await form(C(171, 'Pair with PIN'), [{ name: 'pin', label: C(474, 'PIN') }]);
        if (f) await act('bt.pin', { address: d.address, pin: f.pin });
      } else if (v) await act('bt.' + v, { address: d.address });
    } });
}

async function wifiPage() {
  const w = await get('wifi', {}, true);
  if (scanning !== 'wifi') { scanning = 'wifi'; act('wifi.scan', { v: 1 }, { quiet: true }).catch(() => {}); }
  const nets = [...w.networks].sort((a, b) => b.current - a.current || b.signal - a.signal);
  return {
    title: C(9, 'Connect to Wi-Fi'), back: 'connectivity', topics: ['wifi'],
    nodes: [h('div', { class: 'notice', text: nets.length ? C(164, 'Searching for nearby networks…') : C(23, 'Scanning nearby networks…') }),
      group(nets.map((n) => row({ key: 'n:' + n.ssid, title: n.ssid, icon: 'wifi', detail: [SIGNAL(n.signal), n.security === 'open' ? C(212, 'Open') : n.security, n.saved ? C(199, 'Saved') : ''].filter(Boolean).join(' • '),
        pill: n.current ? { text: C(198, 'Connected'), cls: 'on' } : null,
        on: n.current ? () => go('connectivity/network/' + n.ssid) : async () => {
          const v = n.saved || n.security === 'open' ? 'connect' : await choose(n.ssid, [{ value: 'connect', label: C(158, 'Connect') }, { value: 'wps', label: C(705, 'Connect with WPS') }]);
          if (v === 'wps') { await act('wifi.wps', { ssid: n.ssid }); toast(C(707, 'Press the WPS button on your router…')); } else if (v) await wifiConnect(n);
        } })),
      row({ key: 'hidden', title: C(162, 'Add Hidden Network'), icon: 'plus', on: async () => {
        const v = await form(C(162, 'Add Hidden Network'), [{ name: 'ssid', label: C(739, 'Network name (SSID)') }, { name: 'password', label: C(172, 'Password'), type: 'password' }]);
        if (!v || !v.ssid.trim()) return;
        await act('wifi.connect', { ssid: v.ssid.trim(), password: v.password, hidden: true });
        toast(C(254, 'Connecting to {0}…', v.ssid.trim()));
      } }))],
  };
}

async function networkPage(ssid) {
  const [w, ip] = await Promise.all([get('wifi', {}, true), get('wifi_ip', { ssid }, true).catch(() => ({}))]);
  const n = [...w.saved, ...w.networks].find((x) => x.ssid === ssid) || { ssid };
  const manual = ip.mode === 'manual';
  const editIp = async () => {
    const v = await form(C(175, 'IPv4'), [
      { name: 'mode', label: C(175, 'IPv4'), value: manual ? 'manual' : 'dhcp', options: [{ value: 'dhcp', label: C(122, 'Automatic') + ' (DHCP)' }, { value: 'manual', label: C(176, 'Manual') }] },
      { name: 'address', label: C(177, 'Address'), value: ip.address || ip.runtime_address || '' },
      { name: 'prefix', label: C(178, 'Subnet Mask') + ' (/0–32)', value: String(ip.prefix || ip.runtime_prefix || 24) },
      { name: 'gateway', label: C(179, 'Gateway'), value: ip.gateway || '' },
      { name: 'dns', label: C(180, 'DNS'), value: ip.dns || '' }]);
    if (!v) return;
    await act('wifi.ip', { ssid, ...v });
    toast(C(261, 'Applying network settings…'));
  };
  return {
    title: ssid, kicker: C(157, 'Wi-Fi'), back: 'connectivity', topics: ['wifi'],
    nodes: [group(
      n.current ? row({ key: 'disc', title: C(169, 'Disconnect'), on: () => act('wifi.disconnect') }) : row({ key: 'conn', title: C(158, 'Connect'), on: () => wifiConnect(n) }),
      n.current || n.saved ? null : row({ key: 'wps', title: C(705, 'Connect with WPS'), on: () => act('wifi.wps', { ssid }) }),
      w.wps ? row({ key: 'wpsc', title: C(706, 'Cancel WPS'), detail: C(707, 'Press the WPS button on your router…'), on: () => act('wifi.wpscancel') }) : null,
      n.saved ? row({ key: 'forget', title: C(182, 'Forget Network'), danger: true, arm: { prompt: C(191, 'Press again to confirm') },
        on: async () => { await act('wifi.forget', { ssid }); go('connectivity'); } }) : null),
    section(C(175, 'IPv4'), 'device'),
    group(
      row({ key: 'mode', title: C(175, 'IPv4'), value: manual ? C(176, 'Manual') : C(122, 'Automatic'), chevron: true, disabled: !n.saved, on: editIp }),
      row({ key: 'addr', title: C(177, 'Address'), value: ip.runtime_address || ip.address || '–' }),
      row({ key: 'mask', title: C(178, 'Subnet Mask'), value: ip.runtime_netmask || ip.netmask || '–' }),
      row({ key: 'gw', title: C(179, 'Gateway'), value: ip.runtime_gateway || ip.gateway || '–' }),
      row({ key: 'dns', title: C(180, 'DNS'), value: ip.runtime_dns || ip.dns || '–' }))],
  };
}

async function btPage() {
  const b = await get('bluetooth', {}, true);
  if (scanning !== 'bt') { scanning = 'bt'; act('bt.discover', { v: 1 }, { quiet: true }).catch(() => {}); }
  const found = b.devices.filter((d) => !d.paired).sort((x, y) => y.rssi - x.rssi);
  return {
    title: C(11, 'Connect New Device'), back: 'connectivity', topics: ['bluetooth'],
    nodes: [h('div', { class: 'notice', text: C(29, 'Scanning nearby Bluetooth devices…') }),
      group(found.length ? found.map(btRow) : row({ key: 'none', title: C(166, 'No new Bluetooth devices found'), disabled: true }))],
  };
}

async function syncPage() {
  const s = await get('sync', {}, true);
  const enabled = on01(s.enabled), ready = on01(s.ready);
  const folders = Object.fromEntries(rows(s.folder).map((f) => [f[0], f]));
  const pending = rows(s.pending), devices = rows(s.device), nearby = rows(s.nearby), offers = rows(s.offer);
  /* Existing folders of the user's other devices (folderid: kind, id, custom). */
  const custom = Object.fromEntries(rows(s.folderid).filter((f) => f[2] === '1').map((f) => [f[0], f[1]]));
  const folderIds = Object.fromEntries(rows(s.folderid).map((f) => [f[0], f[1]]));
  const KINDS3 = [['saves', C(673, 'Saves')], ['states', C(674, 'Save States')], ['screenshots', C(675, 'Screenshots')],
    ['roms', C(1414, 'Games')], ['roms-sd2', C(1416, 'Games on SD2')]];
  const code = (id) => (id || '').slice(0, 7);
  const label = (d) => d[1] || C(1059, 'Device {0}', code(d[0]));
  const syncing = (f) => ['syncing', 'sync-preparing', 'sync-waiting'].includes(f[2]);
  /* The same status line as the console (syncui.rs). */
  const status = on01(s.failed) ? C(677, 'Syncthing stopped unexpectedly')
    : !enabled ? C(670, 'Keeps your saves in sync with your other devices. It is not a backup.')
    : !ready ? C(1025, 'Starting…')
    : !devices.length ? C(1027, 'No devices yet')
    : Object.values(folders).some((f) => f[1] === '1' && f[2] === 'error') ? C(1084, 'Cannot sync')
    : !devices.some((d) => d[2] === '1') ? C(1026, 'Waiting for your devices')
    : Object.values(folders).some((f) => f[1] === '1' && syncing(f)) || devices.some((d) => d[2] === '1' && +d[3] >= 0 && +d[3] < 100) ? C(323, 'Syncing…')
    : C(489, 'Up to date');
  const nodes = [group(row({ key: 'on', title: C(669, 'Syncthing'), detail: status, toggle: enabled, set: (v) => act('sync.enable', { v }) }))];
  if (enabled) {
    nodes.push(group(
      row({ key: 'me', title: C(1037, 'This Console'), value: s.device_name || '', detail: ready ? C(671, "This Console's ID") : C(1025, 'Starting…') }),
      ready ? h('div', { class: 'notice mono', text: s.device_id }) : null,
      row({ key: 'add', title: C(1028, 'Add a Device'), detail: C(1086, 'Device ID'), icon: 'plus', disabled: !ready, on: async () => {
        const v = await form(C(1028, 'Add a Device'), [{ name: 'id', label: C(1086, 'Device ID') }]);
        if (v && v.id.trim()) await act('sync.add', { device: v.id.trim().toUpperCase() });
      } })));
    nodes.push(section(C(1030, 'DEVICES'), 'user'));
    const list = [
      pending.map((p) => row({ key: 'p:' + p[0], title: label(p), detail: C(1032, 'Wants to sync with you'), icon: 'desktop',
        on: async () => {
          const v = await choose(label(p), [{ value: 'accept', label: C(1033, 'Accept') }, { value: 'ignore', label: C(1034, 'Ignore') }], 'accept');
          if (v) await act(v === 'accept' ? 'sync.add' : 'sync.ignore', { device: p[0] });
        } })),
      devices.map((d) => row({ key: 'd:' + d[0], title: label(d), icon: 'desktop', detail: d[4] || code(d[0]),
        pill: { text: d[2] !== '1' ? C(159, 'Not connected') : +d[3] >= 0 && +d[3] < 100 ? C(1039, '{0}% synced', d[3]) : C(198, 'Connected'), cls: d[2] === '1' ? 'on' : '' },
        arm: { prompt: C(191, 'Press again to confirm') }, on: () => act('sync.remove', { device: d[0] }) })),
      nearby.map((n) => row({ key: 'n:' + n[0], title: C(1059, 'Device {0}', code(n[0])), detail: n[1], icon: 'plus',
        on: () => act('sync.add', { device: n[0] }) })),
    ].flat();
    nodes.push(group(list.length ? list : row({ key: 'none', title: C(1027, 'No devices yet'), disabled: true })));
    nodes.push(section(C(1031, 'WHAT TO SYNC'), 'user'), group(
      [['saves', C(673, 'Saves'), ''], ['states', C(674, 'Save States'), ''], ['screenshots', C(675, 'Screenshots'), ''],
        ['library', C(1089, 'Covers & Metadata'), C(1090, 'Shared by every profile on this console')],
        ['history', C(1091, 'Play Activity'), C(1092, 'Time played and Recently Played, added up across your devices')],
        ['roms', C(1414, 'Games'), C(1415, "The ROM folder on this console's card, shared by every profile")],
        ['roms-sd2', C(1416, 'Games on SD2'), C(1417, "The SD2 card's ROM folder; it waits while the card is out")]].map(([k, title, note]) => row({ key: 'f:' + k,
        title,
        detail: folders[k] && folders[k][1] === '1' ? (syncing(folders[k]) ? C(323, 'Syncing…') : folders[k][2] === 'error' ? C(1084, 'Cannot sync') : folderIds[k] ? C(1324, 'Folder ID {0}', folderIds[k]) : note) : note,
        toggle: !!folders[k] && folders[k][1] === '1', set: (v) => act('sync.folder', { kind: k, v }) })),
      /* Linked to another device's folder: back to nuubOS's own. */
      KINDS3.filter(([k]) => custom[k]).map(([k, title]) => row({ key: 'own:' + k, title: C(1346, 'Use the nuubOS Folder'), detail: title + ' · ' + custom[k],
        icon: 'folder', arm: { prompt: C(191, 'Press again to confirm') }, on: () => act('sync.folder_id', { kind: k, id: '' }) })),
      row({ key: 'internet', title: C(1035, 'Sync Away from Home'), detail: C(1036, 'Finds your devices over the Internet; your data still goes straight between them'),
        toggle: on01(s.internet), set: (v) => act('sync.internet', { v }) }),
      row({ key: 'relays', title: C(1087, 'Allow Relays'), detail: C(1088, 'If a direct connection is impossible, the encrypted data may pass through public Syncthing servers'),
        toggle: on01(s.internet) && on01(s.relays), disabled: !on01(s.internet), set: (v) => act('sync.relays', { v }) })));
    /* Folders the other devices share and this console does not sync yet
     * (the same section as the console's page 66). */
    if (offers.length) {
      nodes.push(section(C(1343, 'AVAILABLE FOLDERS'), 'user'), group(offers.map((o) => row({ key: 'o:' + o[0] + ':' + o[2], title: o[1] || o[0],
        value: o[0], icon: 'folder', detail: C(1325, 'Shared by {0}', o[3] || C(1059, 'Device {0}', code(o[2]))),
        on: async () => {
          const kinds = KINDS3.filter(([k]) => !custom[k]).map(([value, label]) => ({ value, label }));
          const v = await choose(C(1344, 'Sync {0} as', o[1] || o[0]), [...kinds, { value: 'decline', label: C(1034, 'Ignore') }], kinds.length ? kinds[0].value : 'decline');
          if (v) await act(v === 'decline' ? 'sync.decline' : 'sync.use', v === 'decline' ? { folder: o[0] } : { folder: o[0], kind: v });
        } }))));
    }
  }
  return { title: C(669, 'Syncthing'), back: 'gaming', nodes, topics: ['sync', 'users'] };
}

/* ================================================ CONTROLLERS & INPUT */

const LIGHT_EFFECTS = [['off', 54, 'Off', 741, 'Lights up only for system signals'], ['solid', 114, 'Solid', 742, 'One steady color'], ['breathe', 115, 'Breathing', 743, 'Slowly fades in and out'],
  ['orbit', 116, 'Orbit', 744, 'A comet of light circles the stick'], ['aurora', 117, 'Aurora', 745, 'Soft shades drift around the ring'], ['spectrum', 118, 'Spectrum', 746, 'Every color, slowly turning'],
  ['battery', 119, 'Battery Level', 747, 'Charge left at a glance, pulses while charging'], ['game', 120, 'Console Colors', 748, 'The color of the system you are playing']];
const LIGHT_COLORS = [['theme', 749, 'Mood Color'], ['3a86ff', 209, 'Blue'], ['00c2e0', 750, 'Cyan'], ['2ee6a6', 751, 'Mint'], ['a6e22e', 752, 'Lime'], ['ffb020', 753, 'Amber'],
  ['ff6a1a', 754, 'Orange'], ['ff2e3e', 755, 'Red'], ['ff3ea5', 756, 'Pink'], ['9b5cff', 757, 'Violet'], ['ffffff', 758, 'White']];
const LIGHT_SIGNALS = [['power', 147, 'Startup & Sleep', 151], ['battery', 100, 'Battery', 152], ['connections', 148, 'Connections', 153], ['games', 149, 'Games', 154], ['tasks', 150, 'Tasks', 155]];
const SIGNAL_DETAILS = { 151: 'Welcome, sleep, wake and power off', 152: 'Low battery, charging and full charge', 153: 'Controllers, headphones, Wi-Fi and USB drives', 154: 'Save states, screenshots and errors', 155: 'Downloads, backups and failures' };

page('controllers', 'settings', () => C(6, 'Controllers & Input'), async (path) => {
  if (path[0] === 'players') return playersPage();
  if (path[0] === 'list') return controllerListPage();
  if (path[0] === 'controller') return controllerPage(path[1]);
  if (path[0] === 'rumble') return rumblePage();
  if (path[0] === 'lighting') return lightingPage();
  const [c, rm, li] = await Promise.all([get('controllers').catch(() => ({ devices: [], players: [] })), get('rumble').catch(() => null), get('lighting').catch(() => null)]);
  const rumble = rm && rm.supported;
  const intensity = rm ? RUMBLE_MODES[rm.mode] || RUMBLE_MODES.light : null;
  const lights = li && li.supported;
  const nodes = [
    section(C(734, 'CONTROLLERS'), 'user'),
    group(
      row({ key: 'players', title: C(13, 'Player Assignment'), value: C(111, 'P1–P8'), chevron: true, on: () => go('controllers/players') }),
      row({ key: 'list', title: C(14, 'Controllers'), value: c.devices.length + C(219, ' available'), chevron: true, on: () => go('controllers/list') })),
    section(lights ? C(735, 'RUMBLE & LIGHTING') : C(899, 'RUMBLE'), 'user'),
    group(
      row({ key: 'rumble', title: C(1402, 'Vibration Intensity'), value: intensity ? C(...intensity) : C(205, 'Unavailable'),
        chevron: !!rm, disabled: !rm, on: () => go('controllers/rumble') }),
      lights ? row({ key: 'light', title: C(18, 'RGB Lighting'), value: C(...(LIGHT_EFFECTS.find((e) => e[0] === li.effect) || [0, 54, 'Off']).slice(1, 3)), chevron: true,
        on: () => go('controllers/lighting') }) : null),
  ];
  return { title: C(6, 'Controllers & Input'), nodes, topics: ['controllers', 'rumble', 'lighting'] };
});

/* Player Assignment: every player slot of the service (P1-P8). */
async function playersPage() {
  const c = await get('controllers', {}, true);
  const label = (d) => (d.connected ? d.name : d.name + C(221, ' • unavailable'));
  return {
    title: C(13, 'Player Assignment'), back: 'controllers', topics: ['controllers'],
    nodes: [group(c.players.map((p) => row({ key: 'p:' + p.player, title: C(220, 'Player ') + p.player,
      value: p.name ? p.name + (p.available ? '' : C(221, ' • unavailable')) : C(122, 'Automatic'), chevron: true,
      on: async () => {
        const v = await choose(C(220, 'Player ') + p.player, [{ value: '', label: C(122, 'Automatic') }, ...c.devices.map((d) => ({ value: d.id, label: label(d) }))], p.id || '');
        if (v !== null) await act('ctl.assign', { player: p.player, id: v, name: (c.devices.find((d) => d.id === v) || {}).name || '' });
      } })))],
  };
}

/* Controllers: every known controller, each with its own page. */
async function controllerListPage() {
  const c = await get('controllers', {}, true);
  return {
    title: C(14, 'Controllers'), back: 'controllers', topics: ['controllers'],
    nodes: [group(c.devices.length ? c.devices.map((d) => row({ key: 'c:' + d.id, title: d.name, icon: 'gamepad',
      detail: [d.kind, d.battery >= 0 ? C(789, '{0}%', d.battery) : ''].filter(Boolean).join(' • '),
      value: d.player > 0 ? 'P' + d.player : '', chevron: true, on: () => go('controllers/controller/' + d.id) }))
      : row({ key: 'none', title: C(14, 'Controllers'), value: '0' + C(219, ' available'), disabled: true }))],
  };
}

async function controllerPage(id) {
  const [c, dz] = await Promise.all([get('controllers'), get('deadzones', { id }, true).catch(() => null)]);
  const d = c.devices.find((x) => x.id === id) || { name: id };
  return {
    title: d.name, kicker: C(14, 'Controllers'), back: 'controllers/list', topics: ['controllers'],
    nodes: [group(
      dz ? row({ key: 'dzl', title: C(123, 'Left Stick Deadzone'), slider: { value: dz.left, min: 0, max: 50, set: (v) => act('ctl.deadzone', { id, stick: 'left', v }, { quiet: true }) } }) : null,
      dz ? row({ key: 'dzr', title: C(124, 'Right Stick Deadzone'), slider: { value: dz.right, min: 0, max: 50, set: (v) => act('ctl.deadzone', { id, stick: 'right', v }, { quiet: true }) } }) : null,
      row({ key: 'reset', title: C(365, 'Restore Default Mapping'), arm: { prompt: C(366, 'Press again to restore defaults') }, on: () => act('ctl.resetmapping', { id }) })),
    h('div', { class: 'notice', text: T('mapping_console') })],
  };
}

/* Internal Rumble (console page 16): the preference and a test. */
/* Vibration Intensity (rumbled, every controller; GitHub #20). */
const RUMBLE_MODES = { off: [54, 'Off'], light: [1403, 'Light'], strong: [1404, 'Strong'] };
const RUMBLE_DETAILS = { light: [1405, 'Skips the faintest vibration some games keep sending'], strong: [1406, 'Every vibration the game asks for'] };

async function rumblePage() {
  const rm = await get('rumble', {}, true);
  const mode = RUMBLE_MODES[rm.mode] ? rm.mode : 'light';
  return {
    title: C(1402, 'Vibration Intensity'), back: 'controllers', topics: ['rumble'],
    nodes: [group(
      row({ key: 'mode', title: C(1402, 'Vibration Intensity'), value: C(...RUMBLE_MODES[mode]), detail: RUMBLE_DETAILS[mode] ? C(...RUMBLE_DETAILS[mode]) : '', chevron: true,
        on: () => pickAndAct(C(1402, 'Vibration Intensity'), Object.entries(RUMBLE_MODES).map(([value, l]) => ({ value, label: C(...l) })), mode, 'rumble.mode', 'v', ['rumble']) }),
      ...(rm.supported ? [row({ key: 'test', title: C(125, 'Test Rumble'), disabled: mode === 'off', on: () => act('rumble.test') })] : []))],
  };
}

async function lightingPage() {
  const li = await get('lighting', {}, true);
  const eff = LIGHT_EFFECTS.find((e) => e[0] === li.effect) || LIGHT_EFFECTS[0];
  const col = LIGHT_COLORS.find((e) => e[0] === li.color);
  const colorful = ['solid', 'breathe', 'orbit'].includes(li.effect);
  return {
    title: C(18, 'RGB Lighting'), back: 'controllers', topics: ['lighting'],
    nodes: [section(C(130, 'LIGHTING'), 'user'), group(
      row({ key: 'eff', title: C(126, 'Effect'), value: C(eff[1], eff[2]), detail: C(eff[3], eff[4]), chevron: true,
        on: () => pickAndAct(C(126, 'Effect'), LIGHT_EFFECTS.map((e) => ({ value: e[0], label: C(e[1], e[2]), detail: C(e[3], e[4]) })), li.effect, 'light.effect') }),
      colorful ? row({ key: 'col', title: C(128, 'Color'), value: col ? C(col[1], col[2]) : '#' + li.color, chevron: true, on: async () => {
        const v = await choose(C(128, 'Color'), [...LIGHT_COLORS.map((c) => ({ value: c[0], label: C(c[1], c[2]) })), { value: '__custom', label: C(759, 'Custom') }], li.color);
        if (v === '__custom') {
          const f = await form(C(759, 'Custom'), [{ name: 'c', label: C(128, 'Color'), type: 'color', value: '#' + (/^[0-9a-f]{6}$/i.test(li.color) ? li.color : '3a86ff') }]);
          if (f) await act('light.color', { v: f.c.replace('#', '').toLowerCase() });
        } else if (v) await act('light.color', { v });
      } }) : null,
      li.effect !== 'off' ? row({ key: 'bri', title: C(127, 'Brightness'), slider: { value: li.brightness, set: (v) => act('light.brightness', { v }, { quiet: true }) } }) : null,
      ['breathe', 'orbit', 'aurora', 'spectrum'].includes(li.effect) ? row({ key: 'spd', title: C(121, 'Speed'), value: C({ slow: 144, normal: 145, fast: 146 }[li.speed] || 145, li.speed), chevron: true,
        on: () => pickAndAct(C(121, 'Speed'), [['slow', 144, 'Slow'], ['normal', 145, 'Normal'], ['fast', 146, 'Fast']].map((s) => ({ value: s[0], label: C(s[1], s[2]) })), li.speed, 'light.speed') }) : null),
    section(C(129, 'SYSTEM SIGNALS'), 'user'),
    group(LIGHT_SIGNALS.map((s, i) => row({ key: 'sig:' + s[0], title: C(s[1], s[2]), detail: C(s[3], SIGNAL_DETAILS[s[3]]), toggle: !!(li.signals & (1 << i)),
      set: (v) => act('light.signal', { category: s[0], v }) })))],
  };
}

/* =========================================================== GAMING */

const HOTKEYS = [['save_state', 417, 'Save State'], ['load_state', 418, 'Load State'], ['slot_next', 779, 'Next Slot'], ['slot_prev', 780, 'Previous Slot'],
  ['fast_forward', 781, 'Fast Forward'], ['quit', 422, 'Quit Game'], ['screenshot', 592, 'Screenshot'],
  ['screen_layout', 1302, 'Screen Layout'], ['stylus', 1307, 'Stylus']];
const CONTROLS = [['menu_confirm', 337, 'Face South'], ['menu_back', 338, 'Face East'], ['face_north', 339, 'Face North'], ['face_west', 340, 'Face West'],
  ['menu_up', 341, 'D-Pad Up'], ['menu_down', 342, 'D-Pad Down'], ['menu_left', 343, 'D-Pad Left'], ['menu_right', 344, 'D-Pad Right'],
  ['l1', 345, 'L1'], ['r1', 346, 'R1'], ['l2', 347, 'L2'], ['r2', 348, 'R2'], ['l3', 349, 'L3'], ['r3', 350, 'R3'], ['settings', 351, 'Start'], ['select', 352, 'Select']];
const OVERLAY = [['fps', 783, 'Frame Rate'], ['cpu', 0, 'CPU'], ['gpu', 0, 'GPU'], ['ram', 0, 'RAM'], ['temp', 784, 'Temperature'], ['power', 785, 'Power Draw'], ['battery', 786, 'Battery'], ['clock', 787, 'Clock']];
const SOURCES = { screenscraper: ['ScreenScraper', 793], thegamesdb: ['TheGamesDB', 794], libretro: ['libretro', 795] };
const SOURCE_DETAILS = { 793: 'Covers, descriptions and ratings in your language. A free ScreenScraper account gives more requests.', 794: 'Covers and descriptions in English. A free private API key gives more requests.', 795: 'Covers only. No account needed.' };
const controlName = (c) => { const x = CONTROLS.find((k) => k[0] === c); return x ? C(x[1], x[2]) : c; };

page('gaming', 'settings', () => C(702, 'Gaming'), async (path) => {
  if (path[0] === 'ra') return raPage();
  if (path[0] === 'hotkeys') return hotkeysPage();
  if (path[0] === 'overlay') return overlayPage();
  if (path[0] === 'sync') return syncPage();
  if (path[0] === 'systems') return path[1] ? settingsPage(null, path[1], path[2] === 'controls') : systemsPage();
  const [emu, ra, ss, lib, jobs, sync] = await Promise.all([get('emu_settings'), get('ra'), get('scraper'), get('library'), get('jobs'),
    get('sync').catch(() => ({}))]);
  const set = (key, value) => act('emu.set', { key, value }, { topics: ['emu_settings'] });
  const busy = rows(jobs.job).some((j) => j[1] === 'scrape-bulk' && (j[2] === 'running' || j[2] === 'queued'));
  const avail = on01(ss.available);
  const src = SOURCES[ss.provider] || SOURCES.screenscraper;
  const scanning = lib.scanning === '1';
  const positions = { top: C(1410, 'Top'), center: C(1411, 'Center'), bottom: C(1412, 'Bottom') };
  const regions = { auto: C(122, 'Automatic'), us: C(1421, 'USA'), eu: C(1422, 'Europe'), jp: C(1423, 'Japan'), wor: C(1424, 'World') };
  const nodes = [section(C(703, 'GAMING'), 'user'), group(
    row({ key: 'ra', title: C(584, 'RetroAchievements'), value: ra.user || C(566, 'Not signed in'), chevron: true, on: () => go('gaming/ra') }),
    row({ key: 'boot', title: C(767, 'Console Boot Screens'), detail: C(768, "Shows the console's start-up logo when its BIOS is available"), toggle: on01(emu.boot_screens), set: (v) => set('boot_screens', v ? '1' : '0') }),
    row({ key: 'slots', title: C(769, 'Save State Slots'), value: emu.slots, chevron: true, on: async () => {
      const v = await choose(C(769, 'Save State Slots'), ['3', '5', '10', '20', '50', '99'].map((n) => ({ value: n, label: n })), emu.slots);
      if (v) await set('slots', v);
    } }),
    row({ key: 'rot', title: C(770, 'Rotate Save Slots'), detail: C(771, 'Each save goes to the next slot, then starts again from slot 0'), toggle: on01(emu.slot_rotation), set: (v) => set('slot_rotation', v ? '1' : '0') }),
    row({ key: 'hk', title: C(772, 'Game Hotkeys'), chevron: true, on: () => go('gaming/hotkeys') }),
    row({ key: 'ov', title: C(774, 'Performance Overlay'), value: on01(emu.overlay) ? C(222, 'On') : C(54, 'Off'), chevron: true, on: () => go('gaming/overlay') }),
    row({ key: 'pos', title: C(1408, 'Picture Position'), detail: C(1409, 'Where the game sits when it does not fill the screen'),
      value: positions[emu.position] || positions.center, chevron: true, on: async () => {
        const v = await choose(C(1408, 'Picture Position'), Object.entries(positions).map(([value, label]) => ({ value, label })), emu.position || 'center');
        if (v) await set('position', v);
      } }),
    row({ key: 'bez', title: C(775, 'Bezels'), detail: C(776, 'Frames the game when it does not fill the screen'), toggle: on01(emu.bezels), set: (v) => set('bezels', v ? '1' : '0') }),
    row({ key: 'crt', title: C(777, 'CRT Effect'), detail: C(778, 'Scanlines for consoles played on TVs'), toggle: on01(emu.crt), set: (v) => set('crt', v ? '1' : '0') }),
    row({ key: 'ramenu', title: C(1391, 'RetroArch in the Quick Menu'), detail: C(1392, "Opens RetroArch's own menu during a game"), toggle: on01(emu.retroarch_menu), set: (v) => set('retroarch_menu', v ? '1' : '0') }),
    row({ key: 'systems', title: C(1454, 'System Settings'), detail: C(1455, 'Emulator and picture for all the games of a system'), chevron: true, on: () => go('gaming/systems') })),
  section(C(704, 'LIBRARY'), 'device'), group(
    row({ key: 'scan', title: C(804, 'Scan for New Games'), detail: C(805, 'Finds games copied to the SD card since the last scan'), busy: scanning,
      value: scanning ? C(410, 'Searching for games…') : '', disabled: scanning, on: () => act('lib.scan') }),
    row({ key: 'unav', title: C(1319, 'Show Unavailable Games'), detail: C(1320, 'Games whose file is missing or on an SD2 card that is not inserted'),
      toggle: on01(lib.show_unavailable), set: (v) => act('lib.unavailable', { v }, { topics: ['library'] }) }),
    row({ key: 'wrap', title: C(1467, 'Wrap Around Rows'), detail: C(1468, 'On Home, going past the last item returns to the first'),
      toggle: on01(lib.wrap_rows), set: (v) => act('lib.wrap', { v }, { topics: ['library'] }) }),
    row({ key: 'src', title: C(792, 'Metadata Source'), value: src[0], detail: C(src[1], SOURCE_DETAILS[src[1]]), chevron: true,
      on: () => pickAndAct(C(792, 'Metadata Source'), Object.entries(SOURCES).map(([value, s]) => ({ value, label: s[0], detail: C(s[1], SOURCE_DETAILS[s[1]]) })), ss.provider, 'scraper.provider') }),
    /* Cover Region (GitHub #10); TheGamesDB has one cover a game. */
    ss.provider !== 'thegamesdb' ? row({ key: 'region', title: C(1419, 'Cover Region'), value: regions[ss.region] || regions.auto, chevron: true,
      detail: C(1420, 'Box art from this region when there is one. Fill Missing Data changes the covers already downloaded.'),
      on: () => pickAndAct(C(1419, 'Cover Region'), Object.entries(regions).map(([value, label]) => ({ value, label })), ss.region || 'auto', 'scraper.region') }) : null,
    ss.provider === 'screenscraper' || !ss.provider ? (ss.user
      ? row({ key: 'ssout', title: C(565, 'ScreenScraper Account'), value: ss.user, detail: C(567, 'Sign Out'), arm: { prompt: C(191, 'Press again to confirm') }, on: () => act('scraper.signout') })
      : row({ key: 'ssin', title: C(565, 'ScreenScraper Account'), value: C(566, 'Not signed in'), detail: C(803, 'Optional: more requests per day'), on: async () => {
        const v = await form(C(565, 'ScreenScraper Account'), [{ name: 'name', label: C(285, 'Username') }, { name: 'password', label: C(172, 'Password'), type: 'password' }]);
        if (v && v.name.trim()) { await act('scraper.account', v); toast(C(588, 'Signed in')); }
      } })) : null,
    ss.provider === 'thegamesdb' ? row({ key: 'tgdb', title: C(796, 'TheGamesDB API Key'), value: ss.apikey ? '•••• ' + ss.apikey : C(797, 'Not set'), detail: C(803, 'Optional: more requests per day'),
      on: async () => { const v = await form(C(796, 'TheGamesDB API Key'), [{ name: 'key', label: C(796, 'TheGamesDB API Key') }]); if (v) await act('scraper.apikey', v); } }) : null,
    row({ key: 'bulk', title: C(568, 'Get Metadata for All Games'), busy, value: busy ? C(571, 'Getting metadata') : '',
      detail: avail ? C(569, 'Uses the Internet only when you ask. Games without metadata keep their file name.') : C(570, 'The metadata service is not available'),
      disabled: !avail || busy, on: () => act('scraper.bulk', { scope: 'all' }, { topics: ['jobs'] }) }),
    row({ key: 'fill', title: C(798, 'Fill Missing Data'), detail: C(799, 'Adds only what is missing, such as covers or descriptions. What is already there is kept.'), disabled: !avail || busy,
      on: async () => {
        const v = await choose(C(798, 'Fill Missing Data'), [['missing', 870, 'Every game with missing data'], ['missing:3600', 871, 'Not tried in the last hour'], ['missing:86400', 872, 'Not tried in the last day'],
          ['missing:604800', 873, 'Not tried in the last week'], ['missing:2592000', 874, 'Not tried in the last month']].map((o) => ({ value: o[0], label: C(o[1], o[2]) })));
        if (v) await act('scraper.bulk', { scope: v }, { topics: ['jobs'] });
      } }),
    ss.provider !== 'libretro' ? row({ key: 'lang', title: C(868, 'Metadata in Your Language'), detail: C(869, 'Gets descriptions and genres in your language; the languages already downloaded stay saved'),
      disabled: !avail || busy, on: () => act('scraper.bulk', { scope: 'lang' }, { topics: ['jobs'] }) }) : null,
    row({ key: 'bios', title: C(576, 'BIOS Files'), detail: C(581, 'Copy BIOS files to the bios folder on the SD card. nuubOS never includes them.'), chevron: true, on: () => go('bios') })),
    /* SYNCTHING (user request 2026-10-12: here, not in Connectivity). */
    section(C(1094, 'SYNCTHING'), 'user'), group(
      row({ key: 'sync', title: C(669, 'Syncthing'), value: on01(sync.enabled) ? C(222, 'On') : C(54, 'Off'),
        detail: C(670, 'Keeps your saves in sync with your other devices. It is not a backup.'), chevron: true, on: () => go('gaming/sync') }))];
  return { title: C(702, 'Gaming'), nodes, topics: ['emu_settings', 'ra', 'scraper', 'library', 'jobs', 'sync'] };
});

async function raPage() {
  const ra = await get('ra', {}, true);
  return {
    title: C(584, 'RetroAchievements'), back: 'gaming', topics: ['ra'],
    nodes: [group(ra.user
      ? [row({ key: 'user', title: C(584, 'RetroAchievements'), value: ra.user, detail: C(588, 'Signed in') }),
        row({ key: 'en', title: C(112, 'Enabled'), toggle: on01(ra.enabled), set: (v) => act('ra.set', { key: 'enabled', v }) }),
        row({ key: 'hc', title: C(585, 'Hardcore Mode'), detail: C(586, 'No save states or fast-forward; achievements count double'), toggle: on01(ra.hardcore), set: async (v) => {
          /* Turning it on first explains what it takes away. */
          if (v && !(await form(C(907, 'Turn On Hardcore Mode?'), [], { text: C(908, 'Achievements are earned as on the original console, with no help. While Hardcore Mode is on, save states and fast-forward are off: every game starts from the beginning and only the game\'s own saves are kept. RetroArch also blocks cheats, rewind and slow motion. You can turn it off at any time.'), ok: C(909, 'Turn On') }))) return;
          await act('ra.set', { key: 'hardcore', v });
        } }),
        /* Offline Achievements (RAOfflineProxy): casual sessions only. */
        row({ key: 'off', title: C(1445, 'Offline Achievements'),
          detail: on01(ra.hardcore) ? C(1447, 'Casual only: off in Hardcore Mode') : C(1446, 'Sent when you are back online'),
          toggle: ra.offline !== '0' && !on01(ra.hardcore), disabled: on01(ra.hardcore), set: (v) => act('ra.set', { key: 'offline', v }) }),
        row({ key: 'out', title: C(567, 'Sign Out'), arm: { prompt: C(191, 'Press again to confirm') }, on: () => act('ra.logout') })]
      : row({ key: 'in', title: C(587, 'Sign In'), detail: C(591, 'Your password is used once and never stored'), on: async () => {
        const v = await form(C(584, 'RetroAchievements'), [{ name: 'name', label: C(285, 'Username') }, { name: 'password', label: C(172, 'Password'), type: 'password' }], { text: C(591, 'Your password is used once and never stored'), ok: C(587, 'Sign In') });
        if (!v || !v.name.trim()) return;
        toast(C(590, 'Signing in…'));
        try { await act('ra.login', v, { quiet: true }); toast(C(588, 'Signed in')); } catch (e) { toast(C(589, 'Sign-in failed'), '', true); }
      } }))],
  };
}

async function hotkeysPage() {
  const emu = await get('emu_settings', {}, true);
  return {
    title: C(772, 'Game Hotkeys'), back: 'gaming', topics: ['emu_settings'],
    nodes: [h('div', { class: 'notice', text: C(773, 'Hold Hotkey and press a button') }), group(HOTKEYS.map(([k, i, f]) => row({ key: k, title: C(i, f), value: controlName(emu['hotkey_' + k]), chevron: true,
      on: async () => {
        const v = await choose(C(i, f), CONTROLS.map((c) => ({ value: c[0], label: C(c[1], c[2]) })), emu['hotkey_' + k]);
        if (v) await act('emu.set', { key: 'hotkey_' + k, value: v }, { topics: ['emu_settings'] });
      } })))],
  };
}

async function overlayPage() {
  const emu = await get('emu_settings', {}, true);
  const shown = (emu.overlay_items || '').split(',').filter(Boolean);
  const on = on01(emu.overlay);
  const setItems = (field, v) => {
    const next = OVERLAY.map((o) => o[0]).filter((f) => (f === field ? v : shown.includes(f)));
    return act('emu.set', { key: 'overlay_items', value: next.join(',') }, { topics: ['emu_settings'] });
  };
  return {
    title: C(774, 'Performance Overlay'), back: 'gaming', topics: ['emu_settings'],
    nodes: [group(row({ key: 'on', title: C(782, 'Show Overlay'), toggle: on, set: (v) => act('emu.set', { key: 'overlay', value: v ? '1' : '0' }, { topics: ['emu_settings'] }) }),
      OVERLAY.map(([f, i, fb]) => row({ key: 'o:' + f, title: i ? C(i, fb) : fb, toggle: shown.includes(f), disabled: !on, set: (v) => setItems(f, v) })))],
  };
}

/* =================================================== DISPLAY & AUDIO */

page('display', 'settings', () => C(7, 'Display & Audio'), async () => {
  const [d, a] = await Promise.all([get('display'), get('audio')]);
  const outputs = { auto: C(122, 'Automatic'), analog: C(332, 'Analog Audio'), bluetooth: C(331, 'Bluetooth Audio'), hdmi: 'HDMI' };
  const sel = { speaker: C(240, 'Speaker'), headphones: C(330, 'Headphones'), bluetooth: C(331, 'Bluetooth Audio'), hdmi: 'HDMI' };
  const vol = a['volume.' + a.selected];
  const nodes = [section(C(728, 'DISPLAY'), 'device'), group(
    d.hdmi ? row({ key: 'bri', title: C(127, 'Brightness'), value: C(132, 'Internal display inactive'), disabled: true })
      : row({ key: 'bri', title: C(127, 'Brightness'), slider: { value: Number(d.brightness) || 60, min: 1, max: 100, set: (v) => act('display.brightness', { v }, { quiet: true }) } }),
    d.temperature_supported ? row({ key: 'temp', title: C(143, 'Color Temperature'), slider: { value: Number(d.temperature) || 6500, min: 3500, max: 7500, step: 250, text: (v) => v + C(224, ' K'),
      set: (v) => act('display.temperature', { v }, { quiet: true }) } }) : null),
  section(C(729, 'AUDIO'), 'device'), group(
    row({ key: 'out', title: C(133, 'Audio Output'), value: (a.mode === 'auto' ? C(122, 'Automatic') + ' • ' : '') + (sel[a.selected] || a.selected), chevron: true, on: () => {
      const opts = [{ value: 'auto', label: outputs.auto }];
      if (a['available.bluetooth'] === '1') opts.push({ value: 'bluetooth', label: outputs.bluetooth });
      opts.push({ value: 'analog', label: outputs.analog });
      if (a['available.hdmi'] === '1') opts.push({ value: 'hdmi', label: outputs.hdmi });
      return pickAndAct(C(133, 'Audio Output'), opts, a.mode, 'audio.output');
    } }),
    vol === 'unsupported' ? row({ key: 'vol', title: C(134, 'Volume'), value: a.selected === 'hdmi' ? C(223, 'TV / Receiver') : C(205, 'Unavailable'), detail: C(273, 'Volume controlled by TV / receiver'), disabled: true })
      : row({ key: 'vol', title: C(134, 'Volume'), slider: { value: Number(vol) || 0, set: (v) => act('audio.volume', { v }, { quiet: true }) } }),
    /* Volume Button Step (GitHub #6): percent per Vol+/Vol- press. */
    row({ key: 'vstep', title: C(1418, 'Volume Button Step'), value: (a['volume.step'] || '1') + '%', chevron: true,
      on: () => pickAndAct(C(1418, 'Volume Button Step'), ['1', '2', '5', '10'].map((v) => ({ value: v, label: v + '%' })), a['volume.step'] || '1', 'audio.step') }),
    row({ key: 'apps', title: C(135, 'Applications'), slider: { value: Number(a['volume.applications']) || 0, set: (v) => act('audio.apps', { v }, { quiet: true }) } }),
    /* Home Music On/Off (user request 2026-10-28), then its volume. */
    row({ key: 'musicon', title: C(139, 'Home Music'), toggle: a['home_music.enabled'] !== '0', set: (v) => act('audio.musicon', { v }) }),
    row({ key: 'music', title: C(1116, 'Music Volume'), disabled: a['home_music.enabled'] === '0',
      slider: { value: Number(a['volume.home_music']) || 0, set: (v) => act('audio.music', { v }, { quiet: true }) } }),
    on01(a['home_music.playing']) ? row({ key: 'skip', title: C(329, 'Skip Track'), detail: a['home_music.track'] || '', on: () => act('audio.musicplay', { v: 'NEXT' }) }) : null,
    row({ key: 'test', title: C(140, 'Test Sound'), value: C(142, 'Play'), detail: T('on_console'), on: () => act('audio.test', { v: 'START' }) })),
  section(C(730, 'SOUNDS'), 'device'), group(
    row({ key: 'sys', title: C(136, 'System Sounds'), slider: { value: Number(a['volume.system']) || 0, set: (v) => act('audio.system', { v }, { quiet: true }) } }),
    row({ key: 'nav', title: C(137, 'Navigation Sounds'), toggle: on01(a['system.navigation_enabled']), set: (v) => act('audio.nav', { v }) }),
    row({ key: 'notif', title: C(851, 'Notification Sounds'), toggle: a['system.notify_enabled'] === undefined ? true : on01(a['system.notify_enabled']), set: (v) => act('audio.notify', { v }) }),
    row({ key: 'pow', title: C(138, 'Power Sounds'), toggle: on01(a['system.power_enabled']), set: (v) => act('audio.power', { v }) }))];
  return { title: C(7, 'Display & Audio'), nodes, topics: ['display', 'audio'] };
});

/* ============================================================ POWER */

const BATTERY_HEALTH = { good: [820, 'Good'], overheat: [821, 'Overheating'], cold: [822, 'Too cold'], dead: [823, 'Worn out'] };

page('power', 'settings', () => C(813, 'Power'), async () => {
  const [s, st, c, b] = await Promise.all([get('system'), get('status'), get('controllers').catch(() => ({ devices: [] })), get('bluetooth').catch(() => ({ devices: [] }))]);
  const health = BATTERY_HEALTH[(st.BATTERY_HEALTH || '').toLowerCase()];
  const bstate = s['battery.state'];
  const accessories = [
    ...c.devices.filter((d) => d.connected && !d.builtin && d.battery >= 0).map((d) => ({ name: d.name, battery: d.battery })),
    ...(b.devices || []).filter((d) => d.connected && d.battery >= 0 && !/input|gamepad/i.test(d.kind)).map((d) => ({ name: d.name, battery: d.battery }))];
  const nodes = [section(C(826, 'ENERGY SAVING'), 'device'), group(
    row({ key: 'prof', title: C(51, 'Battery Saver'), toggle: s['profile.requested'] === 'battery-saver',
      detail: s['profile.effective'] && s['profile.effective'] !== s['profile.requested'] ? C(214, 'Effective: ') + (s['profile.effective'] === 'battery-saver' ? C(51, 'Battery Saver') : C(52, 'Auto')) : '',
      set: (v) => act('system.profile', { v: v ? 'battery-saver' : 'auto' }, { topics: ['system'] }) }),
    row({ key: 'auto', title: C(53, 'Automatic Battery Saver'), value: Number(s['auto_battery.threshold']) ? '≤ ' + s['auto_battery.threshold'] + '%' : C(54, 'Off'), chevron: true,
      on: () => pickAndAct(C(53, 'Automatic Battery Saver'), [0, 10, 15, 20, 25, 30, 40, 50].map((v) => ({ value: String(v), label: v ? '≤ ' + v + '%' : C(54, 'Off') })), s['auto_battery.threshold'], 'system.autobattery') }),
    row({ key: 'sleep', title: C(56, 'Sleep After'), value: Number(s.sleep_after_min) ? s.sleep_after_min + C(215, ' min') : C(54, 'Off'), chevron: true,
      on: () => pickAndAct(C(56, 'Sleep After'), [0, 1, 2, 5, 10, 15, 30, 45, 60, 120].map((v) => ({ value: String(v), label: v ? v + C(215, ' min') : C(54, 'Off') })), s.sleep_after_min, 'system.sleep') }),
    row({ key: 'hypersleep', title: C(1347, 'HyperSleep'), toggle: s.hypersleep !== '0',
      detail: s.hypersleep !== '0' ? C(1348, 'Almost no battery use. Wake with the power button.') : C(1349, 'START and the lid also wake it. Uses more battery.'),
      set: (v) => act('system.hypersleep', { v: v ? '1' : '0' }, { topics: ['system'] }) }),
    row({ key: 'faststartup', title: C(1354, 'Fast Startup'), toggle: s.fast_startup !== '0',
      detail: s.fast_startup !== '0' ? C(1355, 'Experimental: starts about 5 seconds faster, but can make the console freeze or restart by itself.') : C(1356, 'Checks the memory at every start. Can help if the console freezes or restarts by itself.'),
      set: (v) => act('system.faststartup', { v: v ? '1' : '0' }, { topics: ['system'] }) }),
    row({ key: 'powerkey', title: C(1388, 'Press Power to Sleep'), toggle: s.power_key_sleep === '1',
      detail: s.power_key_sleep === '1' ? C(1389, 'Hold the power button for the power menu.') : C(1390, 'The power button opens the power menu.'),
      set: (v) => act('system.powerkeysleep', { v: v ? '1' : '0' }, { topics: ['system'] }) })),
  section(C(814, 'BATTERY'), 'device'), group(
    ...(bstate === 'absent' ? [row({ key: 'charge', title: C(816, 'Charge'), value: C(1301, 'No battery') })] : [
    row({ key: 'charge', title: C(816, 'Charge'), value: s['battery.percent'] + '%', detail: bstate === 'charging' ? C(382, 'Charging') : bstate === 'full' ? C(383, 'Fully charged') : bstate === 'not-charging' ? C(819, 'Not charging') : C(818, 'Discharging') }),
    row({ key: 'health', title: C(817, 'Battery Health'), value: health ? C(health[0], health[1]) : st.BATTERY_HEALTH ? C(824, 'Check the battery') : C(211, 'Unknown') })]),
    ...(s['charge_limit.supported'] === '1' ? [row({ key: 'climit', title: C(900, 'Charge Limit'), value: Number(s.charge_limit) < 100 ? s.charge_limit + '%' : C(54, 'Off'), chevron: true,
      on: () => pickAndAct(C(900, 'Charge Limit'), [80, 85, 90, 95, 100].map((v) => ({ value: String(v), label: v < 100 ? v + '%' : C(54, 'Off') })), s.charge_limit, 'system.chargelimit') })] : [])),
  section(C(815, 'ACCESSORY BATTERIES'), 'device'), group(accessories.length
    ? accessories.map((x, i) => row({ key: 'acc' + i, title: x.name, value: x.battery + '%' }))
    : row({ key: 'noacc', title: C(825, 'No accessories connected'), disabled: true }))];
  return { title: C(813, 'Power'), nodes, topics: ['system', 'status', 'controllers', 'bluetooth'] };
});

/* =========================================================== SYSTEM */

page('system', 'settings', () => C(8, 'System'), async (path) => {
  if (path[0] === 'storage') return storagePage();
  if (path[0] === 'update') return updatePage();
  if (path[0] === 'info') return infoPage();
  if (path[0] === 'diagnostics') return diagnosticsPage();
  if (path[0] === 'backup') return path[1] === 'content' ? backupContentPage() : backupPage();
  if (path[0] === 'recovery') return recoveryPage();
  const [s, rec] = await Promise.all([get('system'), get('recovery').catch(() => ({}))]);
  const nodes = [section(C(732, 'STORAGE & SOFTWARE'), 'device'), group(
    row({ key: 'storage', title: C(19, 'Storage'), value: ['ready', 'readonly'].includes(s['storage.sd2.state']) ? C(912, 'SD1') + ' • ' + C(64, 'SD2') : C(912, 'SD1'), chevron: true, on: () => go('system/storage') }),
    row({ key: 'update', title: C(651, 'Software Update'), value: 'nuubOS ' + (s['nuubos.version'] || ''), chevron: true, on: () => go('system/update') }),
    row({ key: 'info', title: C(20, 'System Information'), chevron: true, on: () => go('system/info') })),
  section(C(733, 'TROUBLESHOOTING'), 'device'), group(
    row({ key: 'diag', title: C(498, 'Diagnostics'), chevron: true, on: () => go('system/diagnostics') }),
    row({ key: 'backup', title: C(982, 'Backup'), chevron: true, on: () => go('system/backup') }),
    row({ key: 'rec', title: C(499, 'Recovery'), value: rec['safe-mode'] === '1' ? C(518, 'Safe Mode') : '', chevron: true, on: () => go('system/recovery') }))];
  return { title: C(8, 'System'), nodes, topics: ['system'] };
});

/* System → Storage (console page: SD1, SD2 and its actions, Removable Media). */
async function storagePage() {
  const s = await get('system', {}, true);
  const n = (k) => Number(s[k]) || 0;
  const space = (free, total) => C(914, '{0} free of {1}').replace('{0}', sizeText(free)).replace('{1}', sizeText(total));
  const st = s['storage.sd2.state'] || '';
  const slot = s['storage.sd2.slot'] === '1';
  const mounted = st === 'ready' || st === 'readonly';
  const sd2 = {
    ready: [space(n('storage.sd2.free'), n('storage.sd2.total')), s['storage.sd2.label'] || C(915, 'Games card')],
    readonly: [C(918, 'Read-only'), C(919, 'The card has errors: games can be played, nothing can be written to it')],
    checking: [C(917, 'Checking…'), ''],
    unreadable: [C(920, 'Unreadable'), C(921, 'No supported file system (exFAT, FAT32, ext4)')],
    ejected: [C(922, 'Ejected'), C(627, 'Safe to remove')],
    ignored: [C(978, 'Not in use'), C(980, 'nuubOS leaves this card alone until you choose to use it')],
  }[st] || [C(916, 'No card'), ''];
  return {
    title: C(19, 'Storage'), back: 'system', topics: ['system', 'jobs'],
    nodes: [group(
      row({ key: 'sd1', title: C(912, 'SD1'), value: space(n('storage.userdata_free'), n('storage.userdata_total')), detail: C(913, 'System and saves') }),
      slot ? row({ key: 'sd2', title: C(64, 'SD2'), value: sd2[0], detail: sd2[1] }) : null,
      slot && mounted ? row({ key: 'eject', title: C(924, 'Eject SD2'), on: () => act('storage.eject') }) : null,
      slot && st === 'ejected' ? row({ key: 'use', title: C(925, 'Use SD2 Again'), on: () => act('storage.use') }) : null,
      slot && st === 'ignored' ? row({ key: 'use', title: C(979, 'Use SD2'), on: () => act('storage.use') }) : null,
      slot && st === 'ready' ? row({ key: 'prep', title: C(926, 'Create Game Folders'), detail: C(927, 'Adds a folder for each system. Nothing is deleted.'),
        on: () => act('storage.prepare', {}, { topics: ['jobs'] }) }) : null,
      slot && (mounted || st === 'unreadable' || st === 'ignored') && s['storage.sd2.cid'] ? row({ key: 'fmt', title: C(928, 'Format SD2'), detail: C(929, 'Erases everything on the card'), danger: true,
        arm: { prompt: C(930, 'Press again • everything on SD2 will be erased') }, on: () => act('storage.format', { cid: s['storage.sd2.cid'] }, { topics: ['jobs'] }) }) : null,
      row({ key: 'media', title: C(625, 'Removable Media'), chevron: true, on: () => go('files/:shares') }))],
  };
}

/* System → Diagnostics: the report, where it goes, and (here) its download. */
async function diagnosticsPage() {
  const jobs = await get('jobs', {}, true);
  const report = rows(jobs.job).filter((j) => j[1] === 'support-bundle').pop();
  const running = report && report[2] === 'running';
  return {
    title: C(498, 'Diagnostics'), back: 'system', topics: ['jobs'],
    nodes: [group(
      row({ key: 'diag', title: C(500, 'Save Diagnostic Report'), value: running ? C(501, 'Saving diagnostic report…') : '', busy: running,
        detail: C(504, 'Useful for getting help. It contains no games, saves, passwords or Wi-Fi keys and is saved in the diagnostics folder.'),
        disabled: running, on: () => act('diag.bundle', {}, { topics: ['jobs'] }) }),
      row({ key: 'out', title: C(521, 'Output Folder'), value: 'USERDATA/diagnostics' }),
      report && report[2] === 'succeeded' && report[6] ? row({ key: 'diagdl', title: T('download_report'), icon: 'download', detail: report[6].split('/').pop(),
        on: () => { location.href = '/api/download?' + new URLSearchParams({ p: report[6] }); } }) : null)],
  };
}

const BACKUP_ITEMS = [['saves', 947, 'Saves', 0, ''], ['states', 948, 'Save States', 0, ''], ['library', 949, 'Game Library', 950, 'Play history and collections'],
  ['screenshots', 951, 'Screenshots & Recordings', 0, ''], ['bios', 576, 'BIOS Files', 0, ''], ['covers', 953, 'Covers & Metadata', 954, 'Downloaded game information; it can take a lot of space']];

/* System → Backup: the console backup, on the destination the user chose. */
async function backupPage() {
  const s = await get('system', {}, true);
  const items = (s['storage.backup.items'] || '').split(',');
  const last = Number(s['storage.backup.last']) || 0;
  const dest = s['storage.backup.dest'] || 'sd1';
  const available = s['storage.backup.available'] === '1';
  const slot = s['storage.sd2.slot'] === '1';
  const destName = (d) => (d === 'sd2' ? C(64, 'SD2') : d === 'usb' ? C(1016, 'USB Drive') : C(912, 'SD1'));
  const missing = dest === 'sd2' ? C(935, 'Insert an SD2 card to back up') : dest === 'usb' ? C(1021, 'Connect a USB drive to back up') : '';
  const on = BACKUP_ITEMS.filter((i) => items.includes(i[0])).length;
  return {
    title: C(982, 'Backup'), back: 'system', topics: ['system', 'jobs'],
    nodes: [group(
      row({ key: 'auto', title: C(69, 'Automatic Backup'), toggle: s['storage.backup.enabled'] === '1',
        detail: available ? (last ? C(933, 'Last backup: {0}', dateText(last)) : C(934, 'Never backed up')) : missing,
        set: (v) => act('storage.backup.enabled', { v: v ? '1' : '0' }) }),
      row({ key: 'dest', title: C(1015, 'Destination'), value: destName(dest), detail: dest === 'sd1' ? C(1022, 'Lost if SD1 is formatted or replaced') : '', chevron: true,
        on: async () => {
          const v = await choose(C(1015, 'Destination'), [...(slot ? ['sd2'] : []), 'sd1', 'usb'].map((d) => ({ value: d, label: destName(d) })), dest);
          if (v !== null && v !== dest) await act('storage.backup.dest', { v });
        } }),
      row({ key: 'content', title: C(936, 'Backup Content'), value: on === BACKUP_ITEMS.length ? C(937, 'All') : C(938, '{0} of {1}', on, BACKUP_ITEMS.length),
        chevron: true, on: () => go('system/backup/content') }),
      s['storage.backup.card'] !== 'conflict' ? row({ key: 'bk', title: C(88, 'Back Up Now'), disabled: !available, on: () => act('storage.backup', {}, { topics: ['jobs'] }) })
        : row({ key: 'conflict', title: C(942, 'Backup of a previous SD1'), detail: C(943, 'Restore it, or keep it aside and start a new backup') }))],
  };
}

async function backupContentPage() {
  const s = await get('system', {}, true);
  const items = (s['storage.backup.items'] || '').split(',');
  return {
    title: C(936, 'Backup Content'), back: 'system/backup', topics: ['system'],
    nodes: [group(
      row({ key: 'prof', title: C(945, 'Profiles & Settings'), detail: C(946, 'Always included. Passwords and accounts are never copied.'), toggle: true, disabled: true }),
      BACKUP_ITEMS.map(([k, id, en, did, den]) => row({ key: 'it-' + k, title: C(id, en), detail: did ? C(did, den) : '', toggle: items.includes(k),
        set: (v) => act('storage.backup.item', { item: k, v: v ? '1' : '0' }) })))],
  };
}

/* Release notes in a scrollable sheet, only when asked for. */
function showNotes(title, lines) {
  openModal(h('div', { class: 'sheet', role: 'dialog', 'aria-label': title }, h('h3', { text: title }),
    h('div', { class: 'opts notes' }, ...lines.map((l) => h('p', { text: '• ' + l }))),
    button(C(239, 'Close'), () => modalClose(null))), () => {});
}

async function updatePage() {
  const u = await get('update', {}, true);
  const available = on01(u.available), downloaded = on01(u.downloaded);
  const channels = [{ value: 'stable', label: C(986, 'Stable') }, { value: 'rc', label: C(987, 'RC') }, { value: 'nightly', label: C(988, 'Nightly') }];
  const channel = channels.find((c) => c.value === u.channel) || channels[0];
  /* Release notes: one manifest line, "\\n" = new line, "\\\\" = backslash. */
  const notes = available ? (u.notes || '').replace(/\\(n|\\)/g, (m, c) => (c === 'n' ? '\n' : '\\'))
    .split('\n').map((l) => l.trim().replace(/^[-*•]\s*/, '')).filter((l) => l) : [];
  return {
    title: C(651, 'Software Update'), back: 'system', topics: ['update', 'jobs'],
    nodes: [group(
      row({ key: 'ver', title: C(485, 'Version'), value: u.build ? u.current + ' • ' + u.build : u.current }),
      u.provider !== 'available' ? row({ key: 'na', title: C(655, 'Updates are not available in this build'), disabled: true }) : [
        row({ key: 'chan', title: C(985, 'Update Channel'), value: channel.label, chevron: true,
          detail: channel.value !== 'stable' ? C(989, 'RC and Nightly get new versions first and may have bugs') : '',
          on: () => pickAndAct(C(985, 'Update Channel'), channels, channel.value, 'update.channel', 'v', ['update']) }),
        row({ key: 'auto', title: C(992, 'Check Automatically'), detail: C(993, 'Once a day, when connected to Wi-Fi'), toggle: on01(u.auto),
          set: (v) => act('update.auto', { v: v ? '1' : '0' }, { topics: ['update'] }) }),
        row({ key: 'check', title: C(487, 'Check for Updates'), value: available ? C(652, 'nuubOS {0} available', u.version) : '',
          on: async () => { toast(C(488, 'Checking…')); await act('update.check', {}, { topics: ['update'] }); const n = await get('update', {}, true); if (!on01(n.available)) toast(C(489, 'Up to date')); } }),
        available && !downloaded ? row({ key: 'dl', title: C(653, 'Download'), on: () => act('update.download', {}, { topics: ['jobs'] }) }) : null,
        available && downloaded && u.apply_battery === 'low' ? row({ key: 'inst', title: C(654, 'Install and Restart'), detail: C(1012, 'Charge the battery above 30% to install'),
          on: () => toast(C(1012, 'Charge the battery above 30% to install')) }) : null,
        available && downloaded && u.apply_battery !== 'low' ? row({ key: 'inst', title: C(654, 'Install and Restart'), detail: u.apply_battery === 'absent' ? C(1353, 'No battery: keep the power cable connected until the update is complete') : C(657, 'Your games, saves and settings are kept'), arm: { prompt: C(191, 'Press again to confirm') },
          on: async () => { await act('update.install', {}, { topics: ['jobs'] }); toast(C(984, 'Installing update'), T('console_restarts'), false, 0); } }) : null,
        notes.length ? row({ key: 'notes', title: C(1005, "What's New"), chevron: true, on: () => showNotes(C(1005, "What's New") + ' • nuubOS ' + u.version, notes) }) : null])],
  };
}

async function infoPage() {
  const s = await get('system', {}, true);
  const up = Number(s.uptime_sec) || 0;
  const info = [
    [C(98, 'Device'), s['device.model']], ['nuubOS', (s['nuubos.version'] || '') + (s['nuubos.build'] ? ' • ' + s['nuubos.build'] : '')], [C(99, 'Kernel'), s.kernel],
    [C(100, 'Battery'), s['battery.percent'] + '%'], [C(101, 'CPU'), (Number(s['cpu.freq_khz']) / 1000).toFixed(0) + ' MHz • ' + (Number(s['temp.cpu_millic']) / 1000).toFixed(1) + ' °C'],
    [C(102, 'GPU'), (Number(s['gpu.freq_hz']) / 1e6).toFixed(0) + ' MHz • ' + (Number(s['temp.gpu_millic']) / 1000).toFixed(1) + ' °C'], [C(103, 'Network'), s.network],
    [C(104, 'RAM'), sizeText((Number(s['mem.total_kib']) - Number(s['mem.available_kib'])) * 1024) + ' / ' + sizeText(Number(s['mem.total_kib']) * 1024)],
    [C(105, 'Uptime'), playtimeText(up)]];
  return { title: C(20, 'System Information'), back: 'system', topics: ['system'], nodes: [group(info.map(([k, v]) => row({ key: k, title: k, value: v || '–' })))] };
}

async function recoveryPage() {
  const r = await get('recovery', {}, true).catch(() => ({}));
  return {
    title: C(499, 'Recovery'), back: 'system', topics: [],
    nodes: [group(
      r['safe-mode'] === '1' ? row({ key: 'safe', title: C(518, 'Safe Mode'), detail: C(519, 'The interface closed unexpectedly several times.') }) : null,
      row({ key: 'repair', title: C(505, 'Repair Settings'), detail: C(512, 'Finds and fixes damaged settings files. Games and saves are not touched.'),
        on: async () => { await act('rec.repair'); toast(C(506, 'Check complete')); } }),
      row({ key: 'ruser', title: C(508, 'Reset User Profile'), detail: C(513, "Resets the current user's preferences, controls and app settings. Saves are kept. The console restarts."),
        danger: true, arm: { prompt: C(516, 'Press again to confirm • the console restarts') }, on: async () => { await act('rec.resetuser'); toast(C(509, 'Profile reset'), T('console_restarts')); } }),
      row({ key: 'factory', title: C(510, 'Factory Reset'), detail: C(514, 'Erases users, settings, Wi-Fi networks and Bluetooth pairings, then starts the initial setup. Games and BIOS are kept.'),
        danger: true, arm: { prompt: C(516, 'Press again to confirm • the console restarts') }, on: async () => { await act('rec.factory', { v: 'settings' }); showLogin(T('console_restarts')); } }),
      row({ key: 'full', title: C(511, 'Full Factory Reset'), detail: C(515, "Like Factory Reset, and also erases every user's saves, states, screenshots and app data. Games and BIOS are kept."),
        danger: true, arm: { prompt: C(516, 'Press again to confirm • the console restarts') }, on: async () => {
          const ok = await form(C(511, 'Full Factory Reset'), [], { text: C(515, "Like Factory Reset, and also erases every user's saves, states, screenshots and app data. Games and BIOS are kept."), ok: C(511, 'Full Factory Reset') });
          if (ok) { await act('rec.factory', { v: 'userdata' }); showLogin(T('console_restarts')); }
        } }))],
  };
}
