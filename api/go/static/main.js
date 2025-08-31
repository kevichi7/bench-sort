async function api(path, opts={}){
  const hdr = opts.headers || {};
  const key = localStorage.getItem('apiKey') || '';
  if (key) hdr['X-API-Key'] = key;
  // Only attach JSON content-type for bodies
  const headers = {...hdr};
  if (opts.body && !headers['Content-Type']) headers['Content-Type'] = 'application/json';
  const res = await fetch(path, {...opts, headers});
  const ctype = res.headers.get('content-type') || '';
  if (!res.ok) {
    try {
      const data = ctype.includes('application/json') ? await res.json() : await res.text();
      const msg = typeof data === 'string' ? data : (data.error || JSON.stringify(data));
      throw new Error(`${res.status} ${res.statusText}: ${msg}`);
    } catch(e) {
      throw new Error(`${res.status} ${res.statusText}`);
    }
  }
  return ctype.includes('application/json') ? res.json() : res.text();
}

function parseSize(input){
  const v = String(input || '').trim().toLowerCase();
  if (!v) return 0;
  // 1e6, 2e7, 5E5
  if (/^\d+(?:\.\d+)?(?:e[+\-]?\d+)?$/.test(v)) return Math.floor(Number(v));
  // 10k, 2m, 1.5g (decimal)
  const m = v.match(/^(\d+(?:\.\d+)?)([kmg])$/);
  if (m) {
    const n = Number(m[1]);
    const mult = m[2]==='k'?1e3: m[2]==='m'?1e6: 1e9;
    return Math.floor(n * mult);
  }
  // Fallback integer
  return parseInt(v, 10) || 0;
}

function qs(id){return document.getElementById(id)}

async function init(){
  let lastRows = [];
  let currentSort = { by: 'median_ms', dir: 'asc' };
  let currentBaseline = '';
  let useLogScale = false;
  let lastReq = null;
  let pinned = [];
  const pinColors = ['#ff9f40','#4bc0c0','#9966ff','#ff6384','#36a2eb','#a3e635','#f59e0b','#22c55e'];
  const pinColor = (i)=> pinColors[i % pinColors.length];

  // API key UI
  qs('apiKey').value = localStorage.getItem('apiKey') || '';
  qs('saveKey').onclick = ()=>{ localStorage.setItem('apiKey', qs('apiKey').value.trim()); alert('Saved'); };

  // Limits/mode
  try{
    const lim = await api('/limits');
    qs('limits').textContent = JSON.stringify(lim, null, 2);
    const ltab = qs('limitsTable');
    if (ltab) {
      const rows = [
        ['Max N', lim.max_n],
        ['Max Repeats', lim.max_repeats],
        ['Max Threads', lim.max_threads],
        ['Max Jobs', lim.max_jobs],
        ['Workers', lim.workers],
        ['Timeout (ms)', lim.timeout_ms],
        ['Rate r/min', lim.rate_r_per_min],
        ['Rate burst', lim.rate_burst],
        ['Trust X-Forwarded-For', String(!!lim.trust_xff)],
        ['Mode', lim.mode],
        ['DB Enabled', String(!!lim.db_enabled)],
      ];
      ltab.querySelector('tbody').innerHTML = rows
        .map(([k,v])=>`<tr><th>${k}</th><td>${v}</td></tr>`)
        .join('');
    }
    qs('mode').textContent = `mode: ${lim.mode}${lim.db_enabled?' + db':''}`;
  }catch(e){ qs('limits').textContent = `Failed to load limits: ${e.message}` }

  // Meta for selects (+ attempt to include default plugins if available)
  // Load /meta without implicit plugins for maximum compatibility
  let meta;
  try { meta = await api('/meta'); }
  catch(e){ qs('runOut').textContent = `Failed to load /meta: ${e.message}`; return; }
  for(const t of meta.types){ const o=document.createElement('option'); o.value=o.textContent=t; qs('selType').append(o); qs('jobType').append(o.cloneNode(true)); }
  for(const d of meta.dists){ const o=document.createElement('option'); o.value=o.textContent=d; qs('selDist').append(o); qs('jobDist').append(o.cloneNode(true)); }
  qs('selType').value = 'i32'; qs('jobType').value='i32'; qs('selDist').value='runs'; qs('jobDist').value='runs';
  // Populate algos per type
  function classifyAlgo(name){ return /(par|parallel)/i.test(name) ? 'par' : 'seq'; }
  function populateAlgosForType(type){
    const list = (meta.algos && meta.algos[type]) ? meta.algos[type] : [];
    const seq = list.filter(n=>classifyAlgo(n)==='seq');
    const par = list.filter(n=>classifyAlgo(n)==='par');
    const mksel = (id, arr) => { const el = qs(id); if(!el) return; el.innerHTML=''; for(const name of arr){ const o=document.createElement('option'); o.value=o.textContent=name; el.append(o); } };
    mksel('selAlgosSeq', seq);
    mksel('selAlgosPar', par);
  }
  function populateJobAlgosForType(type){
    const list = (meta.algos && meta.algos[type]) ? meta.algos[type] : [];
    const seq = list.filter(n=>classifyAlgo(n)==='seq');
    const par = list.filter(n=>classifyAlgo(n)==='par');
    const mksel = (id, arr) => { const el = qs(id); if(!el) return; el.innerHTML=''; for(const name of arr){ const o=document.createElement('option'); o.value=o.textContent=name; el.append(o); } };
    mksel('jobAlgosSeq', seq);
    mksel('jobAlgosPar', par);
  }
  populateAlgosForType(qs('selType').value);
  populateJobAlgosForType(qs('jobType').value);
  qs('selType').addEventListener('change', ()=>populateAlgosForType(qs('selType').value));
  qs('jobType').addEventListener('change', ()=>populateJobAlgosForType(qs('jobType').value));
  // Better multi-select UX: toggle selection on click
  function enableMultiToggle(sel){
    if (!sel || !sel.multiple) return;
    sel.addEventListener('mousedown', (e)=>{
      const t = e.target;
      if (t && t.tagName === 'OPTION') { e.preventDefault(); t.selected = !t.selected; }
    });
  }
  enableMultiToggle(qs('selAlgosSeq'));
  enableMultiToggle(qs('selAlgosPar'));
  enableMultiToggle(qs('jobAlgosSeq'));
  enableMultiToggle(qs('jobAlgosPar'));

  // Select All / Clear handlers
  function setAll(selId, state){ const sel = qs(selId); if(!sel) return; Array.from(sel.options).forEach(o=>o.selected = !!state); }
  const btnSelAllRun = qs('btnSelAllRun'); if (btnSelAllRun) { btnSelAllRun.onclick = ()=> { setAll('selAlgosSeq', true); setAll('selAlgosPar', true); }; }
  const btnClearRun = qs('btnClearRun'); if (btnClearRun) btnClearRun.onclick = ()=> { setAll('selAlgosSeq', false); setAll('selAlgosPar', false); };
  const btnSelAllJob = qs('btnSelAllJob'); if (btnSelAllJob) { btnSelAllJob.onclick = ()=> { setAll('jobAlgosSeq', true); setAll('jobAlgosPar', true); }; }
  const btnClearJob = qs('btnClearJob'); if (btnClearJob) btnClearJob.onclick = ()=> { setAll('jobAlgosSeq', false); setAll('jobAlgosPar', false); };

  // Run sync
  let chart;
  // Initialize pins legend
  renderPinsLegend();
  function sortRows(rows, by, dir){
    const arr = [...rows];
    const cmp = (a,b) => {
      const av = a[by];
      const bv = b[by];
      if (by === 'algo') return String(av).localeCompare(String(bv));
      return (av - bv);
    };
    arr.sort(cmp);
    if (dir === 'desc') arr.reverse();
    return arr;
  }

  function updateBaselineSelect(rows){
    const sel = qs('selBaseline'); if (!sel) return;
    const prev = sel.value || currentBaseline || '';
    const names = rows.map(r=>r.algo);
    sel.innerHTML = '<option value="">None</option>' + names.map(n=>`<option value="${n}">${n}</option>`).join('');
    const wanted = names.includes(prev) ? prev : '';
    sel.value = wanted;
    currentBaseline = wanted;
  }

  function withBaseline(rows){
    if (!currentBaseline) return rows.map(r=>({...r, _delta_pct: null, _speedup: null}));
    const base = rows.find(r=>r.algo===currentBaseline);
    if (!base) return rows.map(r=>({...r, _delta_pct: null, _speedup: null}));
    const b = base.median_ms;
    return rows.map(r=>{
      const delta = (r.median_ms - b) / (b || 1);
      const speed = (b && r.median_ms) ? (b / r.median_ms) : null;
      return {...r, _delta_pct: delta, _speedup: speed};
    });
  }

  const mkTable = (rows) => {
    const thead = qs('runTable').querySelector('thead');
    const tbody = qs('runTable').querySelector('tbody');
    thead.innerHTML = '<tr><th>Algo</th><th>Median (ms)</th><th>Mean</th><th>Min</th><th>Max</th><th>Stddev</th><th>Δ vs base</th></tr>';
    const sorted = sortRows(rows, currentSort.by, currentSort.dir);
    const aug = withBaseline(sorted);
    tbody.innerHTML = aug.map((r,i)=>{
      let deltaStr = '-';
      if (r._delta_pct != null) {
        const pct = r._delta_pct * 100;
        deltaStr = (pct>=0?'+':'') + pct.toFixed(1) + '%';
      }
      return `<tr class="${i===0&&currentSort.by==='median_ms'&&currentSort.dir==='asc'?'best':''}"><td>${r.algo}</td><td>${r.median_ms.toFixed(3)}</td><td>${r.mean_ms.toFixed(3)}</td><td>${r.min_ms.toFixed(3)}</td><td>${r.max_ms.toFixed(3)}</td><td>${r.stddev_ms.toFixed(3)}</td><td>${deltaStr}</td></tr>`;
    }).join('');
  };
  const mkChart = (rows) => {
    const ctx = qs('runChart');
    const sorted = sortRows(rows, currentSort.by, currentSort.dir);
    const aug = withBaseline(sorted);
    const labels = aug.map(r=>r.algo);
    const med = aug.map(r=>r.median_ms);
    const ranges = aug.map(r=>[r.min_ms, r.max_ms]);
    const bestIdx = (currentSort.by==='median_ms' && currentSort.dir==='asc' && sorted.length) ? 0 : -1;
    if (chart) chart.destroy();
    const medColors = labels.map((_,i)=>{
      const r = aug[i];
      if (r.algo === currentBaseline) return '#ffcc00'; // baseline highlight
      if (r._delta_pct == null) return (i===bestIdx ? '#4f8cff' : '#cbd3e0');
      return r._delta_pct <= 0 ? '#33c46a' : '#e26d6d'; // faster = green, slower = red
    });
    const rangeBg = labels.map((_,i)=> i===bestIdx ? 'rgba(79,140,255,0.22)' : 'rgba(90,100,130,0.16)');
    const rangeBorder = labels.map((_,i)=> i===bestIdx ? 'rgba(79,140,255,0.55)' : 'rgba(90,100,130,0.35)');
    const ds = [
      {
        label: 'Min..Max',
        type: 'bar',
        data: ranges,
        backgroundColor: rangeBg,
        borderColor: rangeBorder,
        borderWidth: 1,
        order: 0,
        barPercentage: 0.9,
        categoryPercentage: 0.9,
      },
      {
        label: 'Median (ms)',
        type: 'line',
        data: med,
        showLine: false,
        pointBackgroundColor: medColors,
        pointBorderColor: medColors,
        pointRadius: 4,
        pointHoverRadius: 5,
        order: 1,
      }
    ];
    // Add pinned runs as additional point datasets aligned by algo label
    for (let i=0;i<pinned.length;i++){
      const p = pinned[i];
      const map = new Map(p.rows.map(r=>[r.algo, r.median_ms]));
      const pdata = labels.map(name => (map.has(name) ? map.get(name) : null));
      const col = pinColor(i);
      ds.push({
        label: `Pinned ${i+1}${p.label?`: ${p.label}`:''}`,
        type: 'line',
        data: pdata,
        showLine: false,
        pointBackgroundColor: col,
        pointBorderColor: col,
        pointRadius: 3,
        pointHoverRadius: 4,
        order: 2,
      });
    }

    chart = new Chart(ctx, {
      type: 'bar',
      data: { labels, datasets: ds },
      options: {
        responsive: true,
        maintainAspectRatio: true,
        aspectRatio: 2,
        plugins: {
          legend: { display: true, labels: { color: '#e6e6e6' } },
          tooltip: {
            callbacks: {
              label: (ctx) => {
                const dsLabel = ctx.dataset.label || '';
                const v = ctx.raw;
                if (Array.isArray(v)) {
                  const [a,b] = v;
                  return `${dsLabel}: ${a.toFixed(3)} – ${b.toFixed(3)} ms`;
                }
                const r = aug[ctx.dataIndex];
                const baseNote = (r && r._delta_pct!=null) ? ` (Δ ${r._delta_pct>=0?'+':''}${(r._delta_pct*100).toFixed(1)}%)` : '';
                return `${dsLabel}: ${Number(v).toFixed(3)} ms${baseNote}`;
              }
            }
          }
        },
        scales: {
          y: { type: useLogScale ? 'logarithmic' : 'linear', beginAtZero: false, ticks: { color: '#e6e6e6' }, grid: { color: '#1f2433' } },
          x: { ticks: { color: '#e6e6e6' }, grid: { display:false } }
        }
      }
    });
  };

  function renderAll(){ if (!lastRows.length) return; mkTable(lastRows); mkChart(lastRows); }

  function renderPinsLegend(){
    const el = qs('pinsLegend'); if (!el) return;
    if (!pinned.length){ el.textContent = ''; return; }
    el.innerHTML = pinned.map((p,i)=>{
      const col = pinColor(i);
      const safe = (p.label||'').replace(/[&<>]/g, s=>({"&":"&amp;","<":"&lt;",">":"&gt;"}[s]));
      return `<span class="pin-badge"><span class="pin-dot" style="background:${col}"></span>${safe||('Pinned '+(i+1))}<button class="pin-remove" data-pin="${i}">×</button></span>`;
    }).join(' ');
    el.querySelectorAll('.pin-remove').forEach(btn=>{
      btn.addEventListener('click', (e)=>{ const idx = parseInt(e.currentTarget.getAttribute('data-pin'),10); if(!isNaN(idx)){ pinned.splice(idx,1); renderPinsLegend(); renderAll(); } });
    });
  }

  qs('btnRun').onclick = async () => {
    const body = {
      N: parseSize(qs('inpN').value),
      dist: qs('selDist').value, type: qs('selType').value,
      repeats: parseInt(qs('inpRep').value||'0',10),
      threads: parseInt(qs('inpThreads').value||'0',10),
      assert_sorted: qs('chkAssert').checked,
    };
    // Advanced
    const warmup = parseInt(qs('inpWarmup').value||'0',10); if (warmup>0) body.warmup = warmup;
    const seedStr = (qs('inpSeed').value||'').trim(); if (seedStr!=='') { const sv = Number(seedStr); if (!Number.isNaN(sv)) body.seed = Math.floor(sv); }
    const toMs = parseInt(qs('inpTimeout').value||'0',10); if (toMs>0) body.timeout_ms = toMs;
    const pluginsStr = (qs('inpPlugins').value||'').trim(); if (pluginsStr) { body.plugins = pluginsStr.split(',').map(s=>s.trim()).filter(Boolean); }
    const pp = parseInt(qs('inpPartialPct').value||'',10); if (!Number.isNaN(pp)) body.partial_shuffle_pct = pp;
    const dv = parseInt(qs('inpDupValues').value||'',10); if (!Number.isNaN(dv)) body.dup_values = dv;
    const zs = parseFloat(qs('inpZipfS').value||''); if (!Number.isNaN(zs)) body.zipf_s = zs;
    const ra = parseFloat(qs('inpRunsAlpha').value||''); if (!Number.isNaN(ra)) body.runs_alpha = ra;
    const sb = parseInt(qs('inpStaggerBlock').value||'',10); if (!Number.isNaN(sb)) body.stagger_block = sb;
    if ((qs('selBaseline').value||'') !== '') body.baseline = qs('selBaseline').value;
    lastReq = body;
    const sels = [qs('selAlgosSeq'), qs('selAlgosPar')];
    const algos = sels.flatMap(sel=> sel ? Array.from(sel.options).filter(o=>o.selected).map(o=>o.value) : []);
    if (algos.length) body.algos = algos;
    qs('btnRun').disabled = true; qs('btnRun').textContent = 'Running...';
    try{
      const rows = await api('/run', {method:'POST', body: JSON.stringify(body)});
      lastRows = rows;
      updateBaselineSelect(rows);
      renderAll(); renderPinsLegend();
      qs('runOut').textContent = JSON.stringify(rows, null, 2);
    }catch(e){ qs('runOut').textContent = e.message }
    finally{ qs('btnRun').disabled = false; qs('btnRun').textContent = 'Run'; }
  };

  // Jobs async

  let currentJobId = '';
  qs('btnSubmit').onclick = async () => {
    const body = {
      N: parseSize(qs('jobN').value), dist: qs('jobDist').value, type: qs('jobType').value,
      repeats: parseInt(qs('jobRep').value||'0',10)
    };
    const jthreads = parseInt(qs('jobThreads').value||'0',10); if (!Number.isNaN(jthreads)) body.threads = jthreads;
    const jwarm = parseInt(qs('jobWarmup').value||'0',10); if (jwarm>0) body.warmup = jwarm;
    const jseedStr = (qs('jobSeed').value||'').trim(); if (jseedStr!=='') { const sv = Number(jseedStr); if (!Number.isNaN(sv)) body.seed = Math.floor(sv); }
    const jto = parseInt(qs('jobTimeout').value||'0',10); if (jto>0) body.timeout_ms = jto;
    const jpluginsStr = (qs('jobPlugins').value||'').trim(); if (jpluginsStr) { body.plugins = jpluginsStr.split(',').map(s=>s.trim()).filter(Boolean); }
    const jpp = parseInt(qs('jobPartialPct').value||'',10); if (!Number.isNaN(jpp)) body.partial_shuffle_pct = jpp;
    const jdv = parseInt(qs('jobDupValues').value||'',10); if (!Number.isNaN(jdv)) body.dup_values = jdv;
    const jzs = parseFloat(qs('jobZipfS').value||''); if (!Number.isNaN(jzs)) body.zipf_s = jzs;
    const jra = parseFloat(qs('jobRunsAlpha').value||''); if (!Number.isNaN(jra)) body.runs_alpha = jra;
    const jsb = parseInt(qs('jobStaggerBlock').value||'',10); if (!Number.isNaN(jsb)) body.stagger_block = jsb;
    if ((qs('selBaseline').value||'') !== '') body.baseline = qs('selBaseline').value;
    lastReq = body;
    const sels = [qs('jobAlgosSeq'), qs('jobAlgosPar')];
    const algos = sels.flatMap(sel=> sel ? Array.from(sel.selectedOptions).map(o=>o.value) : []);
    if (algos.length) body.algos = algos;
    try{
      const r = await api('/jobs', {method:'POST', body: JSON.stringify(body)});
      currentJobId = r.job_id || '';
      qs('jobOut').textContent = JSON.stringify(r, null, 2);
      if (currentJobId) { const s = qs('jobStatus'); if(s) s.textContent = 'Job submitted. Polling...'; pollJob(currentJobId); }
    }catch(e){
      const msg = /401/.test(e.message) ? 'Unauthorized: set a valid API key above.' : e.message;
      qs('jobOut').textContent = msg;
    }
  };
  async function pollJob(id){
    if (!id) return;
    try{
      const r = await api('/jobs/'+encodeURIComponent(id));
      qs('jobOut').textContent = JSON.stringify(r, null, 2);
      const s = qs('jobStatus'); if (s) s.textContent = `Status: ${r.status}`;
      if (r.status === 'done' && r.result) {
        let res = r.result;
        if (typeof res === 'string') { try { res = JSON.parse(res); } catch {} }
        if (Array.isArray(res)) { lastRows = res; updateBaselineSelect(res); renderAll(); renderPinsLegend(); }
      }
      if (r.status === 'pending' || r.status === 'running') {
        setTimeout(()=>pollJob(id), 700);
      }
    }catch(e){ qs('jobOut').textContent = e.message }
  }

  qs('btnPoll').onclick = async () => { if(currentJobId) { const s = qs('jobStatus'); if(s) s.textContent='Polling...'; pollJob(currentJobId); } else { const s=qs('jobStatus'); if(s) s.textContent='No job to poll.' } };
  qs('btnCancel').onclick = async () => {
    if(!currentJobId){ const s=qs('jobStatus'); if(s) s.textContent='No job to cancel.'; return; }
    try{ const r = await api('/jobs/'+encodeURIComponent(currentJobId)+'/cancel', {method:'POST'}); qs('jobOut').textContent = JSON.stringify(r, null, 2); const s=qs('jobStatus'); if(s) s.textContent='Cancel requested.' }catch(e){ qs('jobOut').textContent = e.message }
  };
  // Pinning controls
  const btnPin = qs('btnPin');
  if (btnPin) btnPin.addEventListener('click', ()=>{
    if (!lastRows.length) return;
    const labelParts = [];
    if (lastReq) {
      if (lastReq.N) labelParts.push(`N=${lastReq.N}`);
      if (lastReq.type) labelParts.push(lastReq.type);
      if (lastReq.dist) labelParts.push(lastReq.dist);
      if (lastReq.repeats) labelParts.push(`r=${lastReq.repeats}`);
      if (lastReq.threads) labelParts.push(`t=${lastReq.threads}`);
    }
    const label = labelParts.join(', ');
    const copy = lastRows.map(r=>({algo:r.algo, median_ms:r.median_ms}));
    pinned.push({ label, rows: copy });
    if (pinned.length > 8) pinned.shift();
    renderPinsLegend();
    renderAll();
  });
  const btnClearPins = qs('btnClearPins');
  if (btnClearPins) btnClearPins.addEventListener('click', ()=>{ pinned = []; renderPinsLegend(); renderAll(); });
  // Toolbar controls
  const selSortBy = qs('selSortBy');
  const selSortDir = qs('selSortDir');
  const selBaseline = qs('selBaseline');
  const chkLogScale = qs('chkLogScale');
  if (selSortBy) selSortBy.addEventListener('change', ()=>{ currentSort.by = selSortBy.value; renderAll(); });
  if (selSortDir) selSortDir.addEventListener('change', ()=>{ currentSort.dir = selSortDir.value; renderAll(); });
  if (selBaseline) selBaseline.addEventListener('change', ()=>{ currentBaseline = selBaseline.value || ''; renderAll(); });
  if (chkLogScale) chkLogScale.addEventListener('change', ()=>{ useLogScale = chkLogScale.checked; renderAll(); });

  function getSortedAugRows(){
    const sorted = sortRows(lastRows, currentSort.by, currentSort.dir);
    return withBaseline(sorted);
  }

  // Export CSV
  const btnCSV = qs('btnExportCSV');
  if (btnCSV) btnCSV.addEventListener('click', () => {
    if (!lastRows.length) return;
    const aug = getSortedAugRows();
    const header = ['algo','median_ms','mean_ms','min_ms','max_ms','stddev_ms','delta_pct','speedup'];
    const lines = [header.join(',')].concat(aug.map(r=>[
      r.algo,
      r.median_ms.toFixed(6),
      r.mean_ms.toFixed(6),
      r.min_ms.toFixed(6),
      r.max_ms.toFixed(6),
      r.stddev_ms.toFixed(6),
      r._delta_pct==null?'':(r._delta_pct.toFixed(6)),
      r._speedup==null?'':(r._speedup.toFixed(6)),
    ].map(x=>String(x)).join(',')));
    const blob = new Blob([lines.join('\n')+'\n'], {type:'text/csv'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'sortbench_results.csv';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(()=>URL.revokeObjectURL(a.href), 5000);
  });

  // Save PNG
  const btnPNG = qs('btnSavePNG');
  if (btnPNG) btnPNG.addEventListener('click', () => {
    if (!chart) return;
    const url = chart.toBase64Image('image/png', 1);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'sortbench_chart.png';
    document.body.appendChild(a); a.click(); a.remove();
  });
}

init();
