// docs/app.js — 60/120fps Zero-Lag Client Controller for C++20 `minecc.wasm`
(() => {
  'use strict';

  const CURRENT_VERSION = '2.1.0';
  const FUNNY_UPDATE_LINES = [
    'yoooo brooo update (pls update fixes things)',
    'yooo bro update rn (pls update fixes random shit)',
    'brooo new build dropped (pls update unbreaks weird bugs)',
    'ayoo update time (pls update makes decompiler way cleaner)',
    'yo twin update real quick (pls update literally fixes sm stuff)'
  ];

  const DEFAULT_VERSIONS = [
    { id: '26.4-snapshot-5', type: 'snapshot', url: '' },
    { id: '26.4-snapshot-4', type: 'snapshot', url: '' },
    { id: '26.3',            type: 'release',  url: '' },
    { id: '26.2',            type: 'release',  url: '' },
    { id: '26.1',            type: 'release',  url: '' },
    { id: '25w46a',          type: 'snapshot', url: '' },
    { id: '1.21.11',         type: 'release',  url: '' },
    { id: '1.21.4',          type: 'release',  url: '' },
    { id: '1.20.4',          type: 'release',  url: '' }
  ];

  const KW = new Set([
    'package','import','public','private','protected','class','interface','record','enum',
    'final','static','abstract','extends','implements','return','new','void','int','long',
    'boolean','float','double','byte','short','char','var','if','else','for','while',
    'switch','case','default','try','catch','throw','throws','super','this','true','false','null','synchronized'
  ]);

  const state = {
    versions: [...DEFAULT_VERSIONS],
    showSnapshots: false,
    diffMode: false,
    screen: 'home', // 'home' | 'code' | 'diff' | 'disasm'
    classes: [],
    diffItems: [],  // [{ tag: '+'|'-'|'~', cls }]
    activeClass: 'net/minecraft/DetectedVersion',
    lastInspect: null,
    userJarLoaded: false,
    baseJarLoaded: false,
    msgIdx: 0
  };

  // Off-main-thread WebAssembly Worker RPC
  const worker = new Worker('./worker.js');
  let reqSeq = 1;
  const pending = new Map();

  function rpc(cmd, payload = {}, transfer = []) {
    return new Promise((resolve, reject) => {
      const id = reqSeq++;
      pending.set(id, { resolve, reject });
      worker.postMessage({ id, cmd, payload }, transfer);
    });
  }

  worker.onmessage = (ev) => {
    const { id, ok, data, error } = ev.data;
    const p = pending.get(id);
    if (!p) return;
    pending.delete(id);
    if (ok) p.resolve(data);
    else p.reject(new Error(error || 'WASM error'));
  };

  const $ = (id) => document.getElementById(id);

  function setStatus(txt, ms) {
    $('statusText').textContent = txt;
    if (typeof ms === 'number') $('perfText').textContent = `${ms.toFixed(1)} ms`;
  }

  function escHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function highlightJavaLine(line) {
    const trimmed = line.trimStart();
    if (trimmed.startsWith('//')) return `<span class="tok-cm">${escHtml(line)}</span>`;
    let out = '';
    let i = 0;
    const n = line.length;
    while (i < n) {
      const c = line[i];
      if (c === '/' && line[i + 1] === '/') {
        out += `<span class="tok-cm">${escHtml(line.slice(i))}</span>`;
        break;
      }
      if (c === '"') {
        let j = i + 1;
        while (j < n && line[j] !== '"') { if (line[j] === '\\') j++; j++; }
        if (j < n) j++;
        out += `<span class="tok-st">${escHtml(line.slice(i, j))}</span>`;
        i = j;
        continue;
      }
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c === '_' || c === '$') {
        let j = i + 1;
        while (j < n && /[A-Za-z0-9_$]/.test(line[j])) j++;
        const tok = line.slice(i, j);
        if (KW.has(tok)) out += `<span class="tok-kw">${tok}</span>`;
        else if (tok[0] >= 'A' && tok[0] <= 'Z') out += `<span class="tok-ty">${tok}</span>`;
        else if (line[j] === '(') out += `<span class="tok-fn">${tok}</span>`;
        else out += tok;
        i = j;
        continue;
      }
      out += escHtml(c);
      i++;
    }
    return out;
  }

  // Populate Version Dropdowns cleanly respecting `Show Snapshots`
  function renderVersionSelects() {
    const visible = state.versions.filter((v) => state.showSnapshots || v.type === 'release');
    const fill = (sel, preferIdx) => {
      const prev = sel.value;
      sel.innerHTML = '';
      visible.forEach((v) => {
        const opt = document.createElement('option');
        opt.value = v.id;
        opt.textContent = v.type === 'snapshot' ? `${v.id}  (snapshot)` : v.id;
        sel.appendChild(opt);
      });
      if ([...sel.options].some((o) => o.value === prev)) sel.value = prev;
      else if (sel.options[preferIdx]) sel.selectedIndex = preferIdx;
    };
    fill($('selTargetVersion'), 0);
    fill($('selBaseVersion'), Math.min(1, visible.length - 1));
  }

  // Optional Live Mojang Manifest Fetch (works seamlessly on GitHub Pages in real browsers!)
  async function fetchMojangManifest() {
    try {
      const r = await fetch('https://piston-meta.mojang.com/mc/game/version_manifest_v2.json');
      if (!r.ok) return;
      const manifest = await r.json();
      if (Array.isArray(manifest.versions) && manifest.versions.length > 0) {
        const map = new Map();
        for (const v of DEFAULT_VERSIONS) map.set(v.id, v);
        for (const mv of manifest.versions.slice(0, 80)) {
          map.set(mv.id, {
            id: mv.id,
            type: mv.type === 'release' ? 'release' : 'snapshot',
            url: mv.url || ''
          });
        }
        state.versions = [...map.values()];
        renderVersionSelects();
      }
    } catch (_) {
      // Offline / sandboxed environment: clean built-in list stays active with zero errors.
    }
  }

  // Try downloading Mojang `client.jar` directly into WASM if manifest URL exists in user's browser
  async function maybeFetchSelectedVersionJar(versionId, isBase) {
    const entry = state.versions.find((v) => v.id === versionId);
    if (!entry || !entry.url) return false;
    try {
      setStatus(`Fetching ${versionId} metadata from Mojang...`);
      const metaResp = await fetch(entry.url);
      if (!metaResp.ok) return false;
      const meta = await metaResp.json();
      const jarUrl = meta?.downloads?.client?.url;
      if (!jarUrl) return false;
      setStatus(`Downloading ${versionId} client.jar into C++20 WASM memory...`);
      const jarResp = await fetch(jarUrl);
      if (!jarResp.ok) return false;
      const buf = await jarResp.arrayBuffer();
      const t0 = performance.now();
      const out = await rpc('loadJarBytes', { buffer: buf, name: `${versionId}.jar`, isBase }, [buf]);
      if (isBase) state.baseJarLoaded = true;
      else {
        state.userJarLoaded = true;
        state.classes = out.classes;
      }
      setStatus(`Loaded ${versionId}.jar (${out.info.classes} classes)`, performance.now() - t0);
      return true;
    } catch (_) {
      return false;
    }
  }

  function renderClassList() {
    const q = $('txtClassFilter').value.trim().toLowerCase();
    const box = $('classList');
    const frag = document.createDocumentFragment();

    if (state.screen === 'diff' && state.diffItems.length > 0) {
      let count = 0;
      for (const item of state.diffItems) {
        if (q && !item.cls.toLowerCase().includes(q)) continue;
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'tree-item' + (item.cls === state.activeClass ? ' active' : '');
        const shortName = item.cls.split('/').pop();
        const badgeCls = item.tag === '+' ? 'diff-add' : item.tag === '-' ? 'diff-rem' : 'diff-mod';
        btn.innerHTML = `<span class="diff-badge ${badgeCls}">${item.tag}</span><span>${escHtml(shortName)}</span>`;
        btn.onclick = () => selectClass(item.cls, 'diff');
        frag.appendChild(btn);
        if (++count >= 400) break;
      }
      box.replaceChildren(frag);
      return;
    }

    let count = 0;
    for (const cls of state.classes) {
      if (q && !cls.toLowerCase().includes(q)) continue;
      const parts = cls.split('/');
      const shortName = parts.pop();
      const pkg = parts.join('.');
      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'tree-item' + (cls === state.activeClass ? ' active' : '');
      btn.innerHTML = `<span>${escHtml(shortName)}</span><span class="pkg-sub">${escHtml(pkg)}</span>`;
      btn.onclick = () => {
        selectClass(cls, state.screen === 'home' ? 'code' : state.screen);
        setMobilePane('code');
      };
      frag.appendChild(btn);
      if (++count >= 400) break;
    }
    box.replaceChildren(frag);
  }

  function renderCodeText(rawText, isDiff, highlightSym = '') {
    const scroller = $('codeScroller');
    const lines = rawText.split('\n');
    const frag = document.createDocumentFragment();
    let scrollTargetRow = null;

    for (let i = 0; i < lines.length; i++) {
      const ln = lines[i];
      const row = document.createElement('div');
      let cls = 'code-row';
      if (isDiff) {
        if (ln.startsWith('+') && !ln.startsWith('+++')) cls += ' d-add';
        else if (ln.startsWith('-') && !ln.startsWith('---')) cls += ' d-del';
      } else if (highlightSym && ln.includes(highlightSym)) {
        cls += ' hl';
        if (!scrollTargetRow) scrollTargetRow = row;
      }
      row.className = cls;
      const body = isDiff ? escHtml(ln) : highlightJavaLine(ln);
      row.innerHTML = `<span class="ln-num">${i + 1}</span><span class="ln-txt">${body}</span>`;
      frag.appendChild(row);
    }
    scroller.replaceChildren(frag);
    if (scrollTargetRow) scrollTargetRow.scrollIntoView({ block: 'center' });
    else scroller.scrollTop = 0;
  }

  function renderOutline(data) {
    const box = $('outlineList');
    const frag = document.createDocumentFragment();

    const mkHead = (txt) => {
      const d = document.createElement('div');
      d.style.cssText = 'padding:8px 8px 4px;font-size:11px;font-weight:700;color:var(--fg-3);';
      d.textContent = txt;
      return d;
    };

    frag.appendChild(mkHead(`METHODS (${data.methods.length})`));
    for (const m of data.methods) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'tree-item';
      b.innerHTML = `<span class="tok-fn">${escHtml(m.name)}</span><span class="pkg-sub">${escHtml(m.desc)}</span>`;
      b.onclick = () => {
        renderCodeText(data.source, false, m.name);
        setMobilePane('code');
      };
      frag.appendChild(b);
    }

    frag.appendChild(mkHead(`INCOMING XREFS (${data.xrefs.length})`));
    for (const x of data.xrefs) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'tree-item';
      const shortFrom = x.from.split('/').pop();
      b.innerHTML = `<span>${escHtml(shortFrom)}::${escHtml(x.method)}</span><span class="pkg-sub">${escHtml(x.kind)}</span>`;
      b.onclick = () => {
        selectClass(x.from, 'code');
        setMobilePane('code');
      };
      frag.appendChild(b);
    }
    box.replaceChildren(frag);
  }

  async function selectClass(cls, mode = 'code', sym = '') {
    const t0 = performance.now();
    state.activeClass = cls;
    const data = await rpc('inspect', { cls });
    state.lastInspect = data;
    state.activeClass = data.name || cls;

    const targetVer = $('selTargetVersion').value || '26.3';
    const baseVer = $('selBaseVersion').value || '26.2';

    if (mode === 'diff') {
      $('lblActivePath').textContent = `${baseVer} -> ${targetVer}  ·  ${state.activeClass}.java`;
      let patch = '';
      if (state.userJarLoaded || state.baseJarLoaded) {
        const d = await rpc('diffClass', { cls: state.activeClass });
        patch = d.patch;
      } else {
        const before = data.source.replace(/26\.3/g, baseVer);
        const after = data.source.replace(/26\.3/g, targetVer);
        patch = buildCleanDiffPreview(before, after, `${baseVer}/${state.activeClass}.java`, `${targetVer}/${state.activeClass}.java`);
      }
      renderCodeText(patch, true);
    } else if (mode === 'disasm') {
      $('lblActivePath').textContent = `${state.activeClass}.class (JVMS Bytecode)`;
      renderCodeText(data.disasm, false, sym);
    } else {
      $('lblActivePath').textContent = `${state.activeClass}.java`;
      renderCodeText(data.source, false, sym);
    }

    renderOutline(data);
    renderClassList();
    setStatus(`Decompiled ${state.activeClass} in C++20 WASM`, performance.now() - t0);
  }

  function buildCleanDiffPreview(aText, bText, aName, bName) {
    const a = aText.split('\n');
    const b = bText.split('\n');
    const out = [`--- ${aName}`, `+++ ${bName}`, `@@ -1,${a.length} +1,${b.length} @@`];
    const max = Math.max(a.length, b.length);
    for (let i = 0; i < max; i++) {
      if (a[i] === b[i]) out.push(' ' + (a[i] ?? ''));
      else {
        if (a[i] !== undefined) out.push('-' + a[i]);
        if (b[i] !== undefined) out.push('+' + b[i]);
      }
    }
    return out.join('\n');
  }

  async function setScreen(next) {
    state.screen = next;
    document.querySelectorAll('[data-nav]').forEach((b) => b.classList.toggle('active', b.dataset.nav === next));
    document.querySelectorAll('[data-tab]').forEach((b) => b.classList.toggle('active', b.dataset.tab === next));

    if (next === 'home') {
      $('homeView').hidden = false;
      $('studioView').hidden = true;
      updateMobileActive('home');
      return;
    }

    $('homeView').hidden = true;
    $('studioView').hidden = false;
    updateMobileActive('code');

    if (next === 'diff') {
      const sum = await rpc('diffSummary', { limit: 2500 });
      state.diffItems = [
        ...sum.added.map((c) => ({ tag: '+', cls: c })),
        ...sum.modified.map((c) => ({ tag: '~', cls: c })),
        ...sum.removed.map((c) => ({ tag: '-', cls: c }))
      ];
    }
    renderClassList();
    await selectClass(state.activeClass, next);
  }

  function setMobilePane(pane) {
    $('studioView').dataset.mpane = pane;
    updateMobileActive(pane);
  }

  function updateMobileActive(id) {
    document.querySelectorAll('[data-mnav]').forEach((b) => b.classList.toggle('active', b.dataset.mnav === id));
  }

  async function syncWasmOptions() {
    await rpc('setOptions', {
      cfg: $('optCfg').checked,
      lambdas: $('optLambdas').checked,
      generics: $('optGenerics').checked,
      pc: $('optPc').checked
    });
    if (state.screen !== 'home') await selectClass(state.activeClass, state.screen);
  }

  async function handleJarFile(file, isBase) {
    if (!file) return;
    setStatus(`Loading ${file.name} into C++20 WASM...`);
    const t0 = performance.now();
    const buf = await file.arrayBuffer();
    const out = await rpc('loadJarBytes', { buffer: buf, name: file.name, isBase }, [buf]);
    if (isBase) {
      state.baseJarLoaded = true;
      $('lblBaseJar').textContent = file.name;
    } else {
      state.userJarLoaded = true;
      state.classes = out.classes;
      state.activeClass = out.classes[0] || state.activeClass;
      $('lblTargetJar').textContent = file.name;
    }
    setStatus(`${file.name}: ${out.info.classes} classes indexed`, performance.now() - t0);
    if (!isBase && state.screen !== 'home') {
      renderClassList();
      await selectClass(state.activeClass, state.screen);
    }
  }

  // Startup Auto-Updater Check + Smooth Toast Trigger
  function parseSemver(tag) {
    const m = String(tag || '').replace(/^v/i, '').split('.').map((x) => parseInt(x, 10) || 0);
    return (m[0] || 0) * 1e6 + (m[1] || 0) * 1e3 + (m[2] || 0);
  }

  function showUpdateBanner(tag, customSub) {
    const line = FUNNY_UPDATE_LINES[state.msgIdx++ % FUNNY_UPDATE_LINES.length];
    $('updHeadline').textContent = line;
    $('updDetail').textContent = customSub || `v${CURRENT_VERSION} -> ${tag} available on GitHub Releases`;
    $('updateToast').classList.add('visible');
  }

  async function runStartupUpdateCheck(forceDemo = false) {
    try {
      const r = await fetch('https://api.github.com/repos/92mxs21/minecc/releases/latest', {
        headers: { Accept: 'application/vnd.github+json' }
      });
      if (r.ok) {
        const rel = await r.json();
        if (rel.tag_name && parseSemver(rel.tag_name) > parseSemver(CURRENT_VERSION)) {
          showUpdateBanner(rel.tag_name);
          return;
        }
        if (forceDemo) {
          showUpdateBanner(rel.tag_name || 'v2.2.0', `Current v${CURRENT_VERSION} · Latest release ${rel.tag_name || 'v2.1.0'} checked`);
          return;
        }
      } else if (forceDemo) {
        showUpdateBanner('v2.2.0');
      }
    } catch (_) {
      if (forceDemo) showUpdateBanner('v2.2.0');
    }
  }

  // Wire Clean Event Listeners
  $('btnHome').onclick = () => setScreen('home');
  document.querySelectorAll('[data-nav]').forEach((b) => { b.onclick = () => setScreen(b.dataset.nav); });
  document.querySelectorAll('[data-tab]').forEach((b) => { b.onclick = () => setScreen(b.dataset.tab); });

  document.querySelectorAll('[data-mnav]').forEach((b) => {
    b.onclick = () => {
      const m = b.dataset.mnav;
      if (m === 'home') setScreen('home');
      else {
        if (state.screen === 'home') setScreen(state.diffMode ? 'diff' : 'code');
        setMobilePane(m);
      }
    };
  });

  $('chkSnapshots').onchange = (e) => {
    state.showSnapshots = e.target.checked;
    renderVersionSelects();
  };

  $('chkDiffMode').onchange = (e) => {
    state.diffMode = e.target.checked;
    $('diffPanel').classList.toggle('open', state.diffMode);
    $('lblLaunchBtn').textContent = state.diffMode ? 'Open Version Diff' : 'Open Decompiler';
  };

  $('selWasmPreset').onchange = (e) => {
    const v = e.target.value;
    $('optCfg').checked = v !== 'ir';
    $('optLambdas').checked = v === 'full';
    $('optGenerics').checked = v !== 'ir';
    $('optPc').checked = v === 'ir';
    syncWasmOptions();
  };

  ['optCfg', 'optLambdas', 'optGenerics', 'optPc'].forEach((id) => {
    $(id).onchange = syncWasmOptions;
  });

  $('btnPickTargetJar').onclick = () => $('jarFileInput').click();
  $('btnTopOpenJar').onclick = () => $('jarFileInput').click();
  $('btnPickBaseJar').onclick = () => $('baseJarInput').click();

  $('jarFileInput').onchange = (e) => handleJarFile(e.target.files[0], false);
  $('baseJarInput').onchange = (e) => handleJarFile(e.target.files[0], true);

  $('btnLaunchStudio').onclick = async () => {
    if (!state.userJarLoaded) {
      await maybeFetchSelectedVersionJar($('selTargetVersion').value, false);
    }
    if (state.diffMode && !state.baseJarLoaded) {
      await maybeFetchSelectedVersionJar($('selBaseVersion').value, true);
    }
    await setScreen(state.diffMode ? 'diff' : 'code');
  };

  $('txtClassFilter').oninput = () => renderClassList();

  $('btnCheckUpdate').onclick = () => runStartupUpdateCheck(true);
  $('btnCloseUpdate').onclick = () => $('updateToast').classList.remove('visible');
  $('btnDismissUpdate2').onclick = () => $('updateToast').classList.remove('visible');

  // Drag & Drop `.jar` anywhere
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    const f = e.dataTransfer?.files?.[0];
    if (f) handleJarFile(f, false).then(() => setScreen('code'));
  });

  // `Ctrl+K` Quick Class Search Modal
  function openQuickSearch() {
    $('searchModal').hidden = false;
    $('txtQuickSearch').value = '';
    renderQuickSearch('');
    setTimeout(() => $('txtQuickSearch').focus(), 20);
  }
  function renderQuickSearch(q) {
    const low = q.trim().toLowerCase();
    const frag = document.createDocumentFragment();
    let n = 0;
    for (const c of state.classes) {
      if (low && !c.toLowerCase().includes(low)) continue;
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'tree-item';
      b.innerHTML = `<span>${escHtml(c.split('/').pop())}</span><span class="pkg-sub">${escHtml(c)}</span>`;
      b.onclick = () => {
        $('searchModal').hidden = true;
        setScreen('code').then(() => selectClass(c, 'code'));
      };
      frag.appendChild(b);
      if (++n >= 30) break;
    }
    $('quickSearchList').replaceChildren(frag);
  }

  $('btnOpenSearch').onclick = openQuickSearch;
  $('txtQuickSearch').oninput = (e) => renderQuickSearch(e.target.value);
  $('searchModal').onclick = (e) => { if (e.target === $('searchModal')) $('searchModal').hidden = true; };

  window.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
      e.preventDefault();
      openQuickSearch();
    } else if (e.key === 'Escape') {
      $('searchModal').hidden = true;
      $('updateToast').classList.remove('visible');
    }
  });

  // Boot WASM Worker + Live Mojang Versions + Non-Blocking Startup Update Check!
  (async () => {
    renderVersionSelects();
    const t0 = performance.now();
    try {
      const initData = await rpc('init');
      state.classes = initData.classes || [];
      $('wasmBadge').textContent = `C++20 WASM (${(performance.now() - t0).toFixed(0)} ms)`;
      setStatus(`Ready · ${state.classes.length} clean-room classes loaded (drop or select any .jar)`, performance.now() - t0);
    } catch (err) {
      $('wasmBadge').textContent = 'WASM Error';
      setStatus(`WASM init failed: ${err.message}`);
    }
    fetchMojangManifest();
    runStartupUpdateCheck(false);
  })();
})();
