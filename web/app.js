(() => {
  const token = new URLSearchParams(location.search).get('token') || '';
  const api = async (url, options = {}) => {
    const response = await fetch(url, { ...options, headers: { 'X-JSFAV-Token': token, ...(options.headers || {}) } });
    if (!response.ok) { let message = `${response.status} ${response.statusText}`; try { message = (await response.json()).error || message; } catch {} throw new Error(message); }
    return response;
  };
  const $ = id => document.getElementById(id);
  let resultData = null, catalog = [], activeFilter = 'all', selectedRow = null;
  function status(text, kind = '') { $('status').textContent = text; $('status').className = `status ${kind}`; }
  async function pick(input, mode) {
    try { const data = await (await api(`/api/v1/pick?kind=${mode}`)).json(); if (data.path) $(input).value = data.path; else if (data.error) status(data.error, 'error'); else if (data.hint) status(data.hint); }
    catch (e) { status(e.message, 'error'); }
  }
  $('pickProject').onclick = () => pick('project', 'folder');
  $('pickCompdb').onclick = () => pick('compdb', 'folder');
  $('pickDeviations').onclick = () => pick('deviations', 'file');
  async function startScan() {
    const projectPath = $('project').value.trim();
    if (!projectPath) { status('Choose a project folder first.', 'error'); $('project').focus(); return; }
    $('scan').disabled = true; $('results').classList.remove('hidden'); status('Starting scan…', 'busy');
    $('resultTitle').textContent = 'Scan in progress'; $('coverageText').textContent = 'The checker is analyzing the selected project.';
    try {
      const payload = { projectPath, compdbPath: $('compdb').value.trim(), configPath: $('deviations').value.trim(), profile: $('profile').value };
      const started = await (await api('/api/v1/scans', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) })).json();
      await poll(started.id);
    } catch (e) { status(e.message, 'error'); $('resultTitle').textContent = 'Scan could not start'; $('coverageText').textContent = e.message; }
    finally { $('scan').disabled = false; }
  }
  async function poll(id) {
    for (;;) {
      const job = await (await api(`/api/v1/scans/${encodeURIComponent(id)}`)).json();
      if (job.state === 'failed') throw new Error(job.error || 'The scan failed.');
      if (job.state === 'completed') { resultData = job.result; showResults(); return; }
      status('Scanning… this may take a few minutes.', 'busy'); await new Promise(resolve => setTimeout(resolve, 900));
    }
  }
  function showResults() {
    const s = resultData.summary, c = resultData.coverage;
    $('resultTitle').textContent = resultData.partial ? 'Scan finished with limited coverage' : 'Scan complete';
    $('coverageText').textContent = `${c.executedAutomatedRules} of ${c.automatedRules} automated rule checks ran · ${s.reviews} review items need human verification${resultData.partial ? ' · Some source analysis was skipped' : ''}`;
    $('countAll').textContent = resultData.results.length; $('countViolation').textContent = s.violations;
    $('countReview').textContent = s.reviews; $('countWarning').textContent = s.coverageWarnings; $('countSuppressed').textContent = s.suppressed;
    $('jsonExport').disabled = $('sarifExport').disabled = false; status(`Finished: ${s.violations} violations, ${s.reviews} review items.`, '');
    activeFilter = 'all'; document.querySelectorAll('.stat').forEach(x => x.classList.toggle('selected', x.dataset.filter === 'all')); renderFindings();
  }
  function visibleResults() {
    const query = $('search').value.toLowerCase().trim();
    return resultData.results.filter(r => {
      const matchesFilter = activeFilter === 'all' || (activeFilter === 'suppressed' ? r.suppressed : r.classification === activeFilter);
      const hay = `${r.ruleId} ${r.message} ${r.evidence} ${r.location?.path || ''}`.toLowerCase();
      return matchesFilter && (!query || hay.includes(query));
    });
  }
  function renderFindings() {
    const list = $('findingList'); list.replaceChildren();
    const rows = visibleResults();
    if (!rows.length) { const p = document.createElement('p'); p.className = 'muted'; p.style.padding = '14px'; p.textContent = 'No findings match this filter.'; list.append(p); return; }
    rows.forEach(r => {
      const b = document.createElement('button'); b.className = 'result-row'; if (selectedRow === r) b.classList.add('active');
      const top = document.createElement('span'); top.className = 'row-top';
      const badge = document.createElement('span'); badge.className = `badge ${r.classification === 'review' ? 'review' : r.classification === 'coverage-warning' ? 'warning' : ''}`; badge.textContent = r.suppressed ? 'Suppressed' : r.classification.replace('-', ' ');
      const rule = document.createElement('span'); rule.className = 'rule-tag'; rule.textContent = r.ruleId ? `AV-${r.ruleId}` : 'Project coverage'; top.append(badge, rule);
      const title = document.createElement('span'); title.className = 'row-title'; title.textContent = r.message;
      const loc = document.createElement('span'); loc.className = 'row-location'; loc.textContent = r.location ? `${r.location.path}:${r.location.line}:${r.location.column}` : 'Project-level item';
      b.append(top, title, loc); b.onclick = () => { selectedRow = r; renderFindings(); renderDetail(r); }; list.append(b);
    });
  }
  function section(parent, title, content) { const div = document.createElement('div'); div.className = 'detail-section'; const h = document.createElement('h4'); h.textContent = title; const p = document.createElement('p'); p.textContent = content; div.append(h, p); parent.append(div); }
  async function renderDetail(r) {
    const d = $('detail'); d.replaceChildren();
    const badge = document.createElement('span'); badge.className = `badge ${r.classification === 'review' ? 'review' : r.classification === 'coverage-warning' ? 'warning' : ''}`; badge.textContent = r.suppressed ? 'Suppressed violation' : r.classification.replace('-', ' '); d.append(badge);
    const title = document.createElement('div'); title.className = 'rule-title'; title.textContent = r.ruleId ? `AV-${r.ruleId} · ${r.message}` : r.message; d.append(title);
    section(d, 'What this means', r.evidence || 'No additional details are available.');
    if (r.location) {
      section(d, 'Location', `${r.location.path}:${r.location.line}:${r.location.column}`);
      const source = document.createElement('button'); source.className = 'textbutton'; source.textContent = 'Show nearby source lines'; source.onclick = async () => {
        source.disabled = true;
        try { const data = await (await api(`/api/v1/source?path=${encodeURIComponent(r.location.path)}&line=${r.location.line}`)).json(); const pre = document.createElement('pre'); pre.className = 'code-context'; pre.textContent = data.lines.map(x => `${String(x.line).padStart(5)} ${x.line === r.location.line ? '›' : ' '} ${x.text}`).join('\n'); d.append(pre); }
        catch (e) { section(d, 'Source unavailable', e.message); } finally { source.remove(); }
      }; d.append(source);
    }
    section(d, 'Profile', r.profile || resultData.profile);
    if (r.suppressed && r.deviation) section(d, 'Approved deviation', `${r.deviation.reason}\nApproval: ${r.deviation.approval}`);
  }
  async function download(format) {
    try { const response = await api(`/api/v1/scans/${encodeURIComponent(resultData.scanId)}/export?format=${format}`); const blob = await response.blob(); const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = `jsfav-results.${format === 'sarif' ? 'sarif' : 'json'}`; a.click(); URL.revokeObjectURL(a.href); }
    catch (e) { status(e.message, 'error'); }
  }
  async function openCatalog() {
    $('catalog').classList.remove('hidden'); if (catalog.length) return;
    try { catalog = (await (await api('/api/v1/rules')).json()).rules; renderCatalog(); } catch (e) { status(e.message, 'error'); }
  }
  function renderCatalog() {
    const q = $('catalogSearch').value.toLowerCase().trim(), list = $('catalogList'); list.replaceChildren();
    catalog.filter(r => `${r.id} ${r.description} ${r.detection}`.toLowerCase().includes(q)).forEach(r => {
      const row = document.createElement('div'); row.className = 'catalog-row';
      const id = document.createElement('strong'); id.textContent = `AV-${r.id}`;
      const desc = document.createElement('span'); desc.textContent = r.description;
      const detect = document.createElement('small'); detect.textContent = `${r.detection} · PDF p.${r.pdf_page}`; row.append(id, desc, detect); list.append(row);
    });
  }
  document.querySelectorAll('.stat').forEach(b => b.onclick = () => { activeFilter = b.dataset.filter; document.querySelectorAll('.stat').forEach(x => x.classList.toggle('selected', x === b)); renderFindings(); });
  $('scan').onclick = startScan; $('search').oninput = renderFindings;
  $('jsonExport').onclick = () => download('json'); $('sarifExport').onclick = () => download('sarif');
  $('showCatalog').onclick = openCatalog; $('closeCatalog').onclick = () => $('catalog').classList.add('hidden'); $('catalogSearch').oninput = renderCatalog;
  api('/api/v1/status').then(r => r.json()).then(x => { if (x.projectPath) $('project').value = x.projectPath; }).catch(e => status(e.message, 'error'));
})();
