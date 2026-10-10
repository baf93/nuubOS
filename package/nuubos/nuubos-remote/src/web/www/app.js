'use strict';
/* nuubOS Web administration (EPIC-038). Every action goes through the
 * device API (/api), which uses the nuubOS services; nothing here decides
 * what a game, a ROM folder or a valid BIOS is. */
const T = {
  en: { device: 'Device', version: 'Version', games: 'Games', storage: 'Free space', scan: 'Scan for games', upload_roms: 'Upload games', system: 'System', upload: 'Upload', library: 'Library', bios: 'BIOS files', bios_hint: 'nuubOS never includes BIOS files. Upload your own; each file is checked.', bios_file: 'File', delete: 'Delete', confirm_delete: 'Permanently delete "{0}" for every user? Saves are kept.', uploading: 'Uploading {0}…', done: 'Done', failed: 'Failed', scanning: 'Scanning…', extensions: 'File types: {0}', of: '{0} of {1} free', ready: 'Ready', missing: 'Missing', invalid: 'Invalid', optional: 'Optional', unverified: 'Not verifiable', unavailable: 'Unavailable' },
  it: { device: 'Dispositivo', version: 'Versione', games: 'Giochi', storage: 'Spazio libero', scan: 'Cerca giochi', upload_roms: 'Carica giochi', system: 'Sistema', upload: 'Carica', library: 'Libreria', bios: 'File BIOS', bios_hint: 'nuubOS non include mai file BIOS. Carica i tuoi: ogni file viene verificato.', bios_file: 'File', delete: 'Elimina', confirm_delete: 'Eliminare definitivamente "{0}" per tutti gli utenti? I salvataggi restano.', uploading: 'Caricamento di {0}…', done: 'Fatto', failed: 'Non riuscito', scanning: 'Ricerca…', extensions: 'Tipi di file: {0}', of: '{0} liberi su {1}', ready: 'Pronto', missing: 'Mancante', invalid: 'Non valido', optional: 'Facoltativo', unverified: 'Non verificabile', unavailable: 'Non disponibile' },
  de: { device: 'Gerät', version: 'Version', games: 'Spiele', storage: 'Freier Speicher', scan: 'Nach Spielen suchen', upload_roms: 'Spiele hochladen', system: 'System', upload: 'Hochladen', library: 'Bibliothek', bios: 'BIOS-Dateien', bios_hint: 'nuubOS enthält nie BIOS-Dateien. Lade deine eigenen hoch; jede Datei wird geprüft.', bios_file: 'Datei', delete: 'Löschen', confirm_delete: '„{0}“ für alle Benutzer endgültig löschen? Spielstände bleiben erhalten.', uploading: '{0} wird hochgeladen…', done: 'Fertig', failed: 'Fehlgeschlagen', scanning: 'Suche…', extensions: 'Dateitypen: {0}', of: '{0} von {1} frei', ready: 'Bereit', missing: 'Fehlt', invalid: 'Ungültig', optional: 'Optional', unverified: 'Nicht prüfbar', unavailable: 'Nicht verfügbar' },
  es: { device: 'Dispositivo', version: 'Versión', games: 'Juegos', storage: 'Espacio libre', scan: 'Buscar juegos', upload_roms: 'Subir juegos', system: 'Sistema', upload: 'Subir', library: 'Biblioteca', bios: 'Archivos BIOS', bios_hint: 'nuubOS nunca incluye archivos BIOS. Sube los tuyos; se comprueba cada archivo.', bios_file: 'Archivo', delete: 'Eliminar', confirm_delete: '¿Eliminar para siempre «{0}» para todos los usuarios? Se conservan las partidas.', uploading: 'Subiendo {0}…', done: 'Hecho', failed: 'Error', scanning: 'Buscando…', extensions: 'Tipos de archivo: {0}', of: '{0} libres de {1}', ready: 'Listo', missing: 'Falta', invalid: 'No válido', optional: 'Opcional', unverified: 'No verificable', unavailable: 'No disponible' },
  fr: { device: 'Appareil', version: 'Version', games: 'Jeux', storage: 'Espace libre', scan: 'Rechercher des jeux', upload_roms: 'Envoyer des jeux', system: 'Système', upload: 'Envoyer', library: 'Bibliothèque', bios: 'Fichiers BIOS', bios_hint: 'nuubOS n’inclut jamais de fichiers BIOS. Envoyez les vôtres ; chaque fichier est vérifié.', bios_file: 'Fichier', delete: 'Supprimer', confirm_delete: 'Supprimer définitivement « {0} » pour tous les utilisateurs ? Les sauvegardes sont conservées.', uploading: 'Envoi de {0}…', done: 'Terminé', failed: 'Échec', scanning: 'Recherche…', extensions: 'Types de fichiers : {0}', of: '{0} libres sur {1}', ready: 'Prêt', missing: 'Manquant', invalid: 'Non valide', optional: 'Facultatif', unverified: 'Non vérifiable', unavailable: 'Indisponible' },
  nl: { device: 'Apparaat', version: 'Versie', games: 'Spellen', storage: 'Vrije ruimte', scan: 'Spellen zoeken', upload_roms: 'Spellen uploaden', system: 'Systeem', upload: 'Uploaden', library: 'Bibliotheek', bios: 'BIOS-bestanden', bios_hint: 'nuubOS levert nooit BIOS-bestanden mee. Upload je eigen bestanden; elk bestand wordt gecontroleerd.', bios_file: 'Bestand', delete: 'Verwijderen', confirm_delete: '"{0}" definitief verwijderen voor alle gebruikers? Savegames blijven behouden.', uploading: '{0} uploaden…', done: 'Klaar', failed: 'Mislukt', scanning: 'Zoeken…', extensions: 'Bestandstypen: {0}', of: '{0} van {1} vrij', ready: 'Klaar', missing: 'Ontbreekt', invalid: 'Ongeldig', optional: 'Optioneel', unverified: 'Niet controleerbaar', unavailable: 'Niet beschikbaar' },
  pt: { device: 'Dispositivo', version: 'Versão', games: 'Jogos', storage: 'Espaço livre', scan: 'Procurar jogos', upload_roms: 'Enviar jogos', system: 'Sistema', upload: 'Enviar', library: 'Biblioteca', bios: 'Ficheiros BIOS', bios_hint: 'O nuubOS nunca inclui ficheiros BIOS. Envie os seus; cada ficheiro é verificado.', bios_file: 'Ficheiro', delete: 'Eliminar', confirm_delete: 'Eliminar definitivamente "{0}" para todos os utilizadores? Os saves são mantidos.', uploading: 'A enviar {0}…', done: 'Concluído', failed: 'Falhou', scanning: 'A procurar…', extensions: 'Tipos de ficheiro: {0}', of: '{0} livres de {1}', ready: 'Pronto', missing: 'Em falta', invalid: 'Inválido', optional: 'Opcional', unverified: 'Não verificável', unavailable: 'Indisponível' },
};
const lang = (navigator.language || 'en').slice(0, 2);
const L = T[lang] || T.en;
const t = (k, ...a) => (L[k] || T.en[k] || k).replace(/\{(\d)\}/g, (_, i) => a[i]);
const $ = (id) => document.getElementById(id);
document.documentElement.lang = T[lang] ? lang : 'en';
document.querySelectorAll('[data-t]').forEach((e) => { e.textContent = t(e.dataset.t); });

async function api(op, params = {}, opts = {}) {
  const q = new URLSearchParams({ op, ...params });
  const r = await fetch('/api?' + q, { credentials: 'same-origin', ...opts });
  if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || r.status);
  return r.json();
}
const write = { method: 'POST', headers: { 'X-NuubOS': '1' } };
const size = (kb) => kb > 1048576 ? (kb / 1048576).toFixed(1) + ' GB' : (kb / 1024).toFixed(0) + ' MB';
let systems = [];

/* The page wears the active user's Mood (Settings → General → Mood). */
async function loadMood() {
  const m = await api('mood');
  const map = { '--bg': 'background', '--panel': 'surface', '--line': 'border', '--text': 'text', '--dim': 'text-secondary', '--blue': 'accent' };
  for (const [v, k] of Object.entries(map)) if (m[k]) document.documentElement.style.setProperty(v, m[k]);
  if (m['accent-text']) document.documentElement.style.setProperty('--on-accent', m['background'] || '#0d0e10');
}

async function loadStatus() {
  const s = await api('status');
  $('device').textContent = s.device;
  $('version').textContent = s.version || '–';
  $('games').textContent = s.scanning ? t('scanning') : s.games;
  $('storage').textContent = t('of', size(s.free_kb), size(s.total_kb));
}

function fillSystems() {
  for (const id of ['upload-system', 'library-system']) {
    const sel = $(id);
    sel.replaceChildren();
    for (const s of systems) {
      if (id === 'library-system' && !s.games) continue;
      const o = document.createElement('option');
      o.value = s.id; o.textContent = id === 'library-system' ? `${s.name} (${s.games})` : s.name;
      sel.append(o);
    }
  }
  showExtensions();
}

function showExtensions() {
  const s = systems.find((x) => x.id === $('upload-system').value);
  $('upload-exts').textContent = s ? t('extensions', s.extensions.split(',').join(', ')) : '';
}

async function loadSystems() {
  systems = await api('systems');
  fillSystems();
  loadGames();
}

async function loadGames() {
  const list = $('library-games');
  list.replaceChildren();
  const sys = $('library-system').value;
  if (!sys) return;
  for (const g of await api('games', { system: sys })) {
    const li = document.createElement('li');
    const name = document.createElement('span');
    name.textContent = g.available ? g.title : `${g.title} • ${t('unavailable')}`;
    li.append(name);
    if (g.available) {
      const b = document.createElement('button');
      b.className = 'danger'; b.textContent = t('delete');
      b.onclick = async () => {
        if (!confirm(t('confirm_delete', g.title))) return;
        try { await api('delete', { game: g.id }, write); } catch (e) { alert(t('failed')); }
        loadSystems(); loadStatus();
      };
      li.append(b);
    }
    list.append(li);
  }
}

/* Raw PUT body: no multipart parsing on the device; progress via XHR. */
function put(root, path, file, onProgress) {
  return new Promise((resolve, reject) => {
    const x = new XMLHttpRequest();
    x.open('PUT', '/api?' + new URLSearchParams({ op: 'upload', root, path }));
    x.setRequestHeader('X-NuubOS', '1');
    x.upload.onprogress = (e) => e.lengthComputable && onProgress(e.loaded / e.total);
    x.onload = () => (x.status === 200 ? resolve() : reject(new Error(x.status)));
    x.onerror = () => reject(new Error('network'));
    x.send(file);
  });
}

$('upload').onclick = async () => {
  const s = systems.find((x) => x.id === $('upload-system').value);
  const files = [...$('upload-files').files];
  if (!s || !files.length) return;
  const bar = $('upload-progress');
  bar.hidden = false; $('upload').disabled = true;
  try {
    for (const f of files) {
      $('upload-status').textContent = t('uploading', f.name);
      await put('roms', `${s.folder}/${f.name}`, f, (p) => { bar.value = p * 100; });
    }
    $('upload-status').textContent = t('done');
  } catch (e) {
    $('upload-status').textContent = t('failed');
  }
  bar.hidden = true; $('upload').disabled = false;
  loadStatus(); setTimeout(loadSystems, 1500);
};

async function loadBios() {
  const rows = await api('bios');
  const table = $('bios-table');
  const target = $('bios-target');
  table.replaceChildren(); target.replaceChildren();
  for (const r of rows) {
    const tr = document.createElement('tr');
    if (r.type === 'system') {
      const td = document.createElement('td');
      td.className = 'sys'; td.colSpan = 2; td.textContent = r.name;
      const st = document.createElement('td');
      st.className = r.state === 'ready' ? 'ok' : r.state; st.textContent = t(r.state);
      tr.append(td, st);
    } else {
      const p = document.createElement('td'); p.textContent = r.path;
      const d = document.createElement('td'); d.textContent = r.required ? r.description : `${t('optional')} • ${r.description}`;
      const st = document.createElement('td'); st.className = r.state; st.textContent = t(r.state === 'ok' ? 'ready' : r.state);
      tr.append(p, d, st);
      if (r.state !== 'ok') {
        const o = document.createElement('option');
        o.value = r.path; o.textContent = r.path;
        target.append(o);
      }
    }
    table.append(tr);
  }
}

$('bios-upload').onclick = async () => {
  const f = $('bios-file').files[0];
  const path = $('bios-target').value;
  if (!f || !path) return;
  $('bios-status').textContent = t('uploading', f.name);
  try { await put('bios', path, f, () => {}); $('bios-status').textContent = t('done'); }
  catch (e) { $('bios-status').textContent = t('failed'); }
  loadBios();
};

$('scan').onclick = async () => {
  try { await api('scan', {}, write); } catch (e) { /* shown by status */ }
  $('games').textContent = t('scanning');
  setTimeout(() => { loadStatus(); loadSystems(); }, 2000);
};
$('upload-system').onchange = showExtensions;
$('library-system').onchange = loadGames;
loadMood().catch(() => {}); loadStatus(); loadSystems(); loadBios();
