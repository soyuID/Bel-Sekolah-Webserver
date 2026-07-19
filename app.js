const DAYS=['Senin','Selasa','Rabu','Kamis','Jumat','Sabtu','Minggu'];
const MONTHS=['Januari','Februari','Maret','April','Mei','Juni','Juli','Agustus','September','Oktober','November','Desember'];
const TODAY_IDX=(new Date().getDay()+6)%7;

let TRACKS=[
  {file:'001.mp3',name:'Bel standar',dur:180},
  {file:'002.mp3',name:'Istirahat panjang',dur:180},
  {file:'003.mp3',name:'Adzan / sholat',dur:180},
  {file:'004.mp3',name:'Bel pulang',dur:180},
  {file:'005.mp3',name:'Peringatan khusus',dur:180},
];

const emptyDay=()=>[];
let schedule={};
DAYS.forEach((_,i)=>{schedule[i]=emptyDay();});

let activeDay=TODAY_IDX;
let playTimer=null,curFile='',elapsed=0,curDur=0;
let ntpSynced=false;
let isRinging=false;
let kegiatanList=[];

// ─── Toast ─────────────────────────────────────
function toast(msg,dur=2000){
  const el=document.getElementById('toast');
  el.textContent=msg;el.classList.add('show');
  clearTimeout(el._t);
  el._t=setTimeout(()=>el.classList.remove('show'),dur);
}

// ─── Fetch helpers ─────────────────────────────
async function get(url){
  try{const r=await fetch(url,{signal:AbortSignal.timeout(4000)});return r;}
  catch(e){return null;}
}
async function post(url,body){
  try{const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body,signal:AbortSignal.timeout(5000)});return r;}
  catch(e){return null;}
}

// ─── Connection status ─────────────────────────
async function checkConn(){
  const r=await get('/status');
  const dot=document.getElementById('connDot');
  const lbl=document.getElementById('connLbl');
  if(r&&r.ok){
    dot.style.background='#1D9E75';dot.style.boxShadow='0 0 4px #1D9E75';
    lbl.textContent='Terhubung';
    try{
      const d=await r.json();
      ntpSynced=d.ntp;
      const wasRinging=isRinging;
      isRinging=d.isRinging||false;
      if(wasRinging!==isRinging) updateStatusRealtime();
    }catch(_){}
  } else {
    dot.style.background='#e24b4a';dot.style.boxShadow='none';
    lbl.textContent='Tidak terhubung';
  }
}

// ─── Poll status bunyi ─────────────────────────
async function pollStatus(){
  const r=await get('/status');
  if(!r||!r.ok) return;
  try{
    const d=await r.json();
    ntpSynced=d.ntp;
    const wasRinging=isRinging;
    isRinging=d.isRinging||false;
    if(wasRinging!==isRinging) updateStatusRealtime();
    // Update indikator amplifier
    const dot=document.getElementById('ampliDot');
    const lbl=document.getElementById('ampliLbl');
    if(dot&&lbl){
      if(d.ampliOn){
        dot.style.background='#1D9E75';dot.style.boxShadow='0 0 4px #1D9E75';
        lbl.textContent='Amplifier ON';
      } else {
        dot.style.background='#4b5563';dot.style.boxShadow='none';
        lbl.textContent='Amplifier OFF';
      }
    }
  }catch(_){}
}

// ─── Load jadwal ───────────────────────────────
async function loadJadwal(){
  const r=await get('/jadwal.json');
  if(!r||!r.ok){useDefaultSchedule();return;}
  try{
    const d=await r.json();
    DAYS.forEach((day,i)=>{
      const arr=d[day];
      if(Array.isArray(arr)){
        schedule[i]=arr.map(e=>({
          time: e.jam||'00:00',
          label: e.kegiatan||'-',
          track: typeof e.audio==='number'?e.audio:parseInt(e.audio)||1,
          active: true,
          done: isDone(i,e.jam)
        }));
      } else schedule[i]=[];
    });
    renderDash();
    toast('Jadwal dimuat dari ESP32');
  }catch(e){useDefaultSchedule();}
}

function isDone(dayIdx,jam){
  if(dayIdx!==TODAY_IDX) return false;
  const now=new Date();
  if(!jam||jam.length<5) return false;
  const [bh,bm]=jam.split(':').map(Number);
  const nowMin=now.getHours()*60+now.getMinutes();
  return bh*60+bm < nowMin-1;
}

function useDefaultSchedule(){
  const def=[
    {time:'07:00',label:'Masuk',track:1,active:true},
    {time:'10:00',label:'Istirahat 1',track:2,active:true},
    {time:'10:15',label:'Masuk lagi',track:1,active:true},
    {time:'12:00',label:'Istirahat 2',track:3,active:true},
    {time:'13:00',label:'Jam siang',track:1,active:true},
    {time:'14:30',label:'Pulang',track:4,active:true},
  ];
  const wknd=[
    {time:'07:30',label:'Masuk',track:1,active:true},
    {time:'10:00',label:'Istirahat',track:2,active:true},
    {time:'12:00',label:'Pulang',track:4,active:true},
  ];
  DAYS.forEach((_,i)=>{
    const base=i<5?def:wknd;
    schedule[i]=base.map(e=>({...e,done:isDone(i,e.time)}));
  });
  renderDash();
}

// ─── Load audio list ───────────────────────────
async function loadAudioList(){
  const r=await get('/audio-list');
  if(!r||!r.ok) return;
  try{
    const d=await r.json();
    if(Array.isArray(d)&&d.length>0){
      TRACKS=d.map((t,i)=>({
        file: t.file||`${String(i+1).padStart(3,'0')}.mp3`,
        name: t.name||t.file,
        dur:  t.dur||180
      }));
    }
  }catch(_){}
}

// ─── Tabs ──────────────────────────────────────
function gotoTab(t){
  document.querySelectorAll('.tab').forEach((b,i)=>b.classList.toggle('active',['dashboard','jadwal','kegiatan','audio','json'][i]===t));
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
  document.getElementById('tab-'+t).classList.add('active');
  if(t==='jadwal'){renderDayTabs();renderEdit();}
  if(t==='dashboard'){renderDash();}
  if(t==='audio'){renderTracks();}
  if(t==='kegiatan'){renderKegiatan();}
  if(t==='json'){renderJSON();}
}

// ─── Clock ─────────────────────────────────────
function pad2(n){return String(n).padStart(2,'0');}

function updateClock(){
  const now=new Date();
  document.getElementById('clockTime').textContent=pad2(now.getHours())+':'+pad2(now.getMinutes())+':'+pad2(now.getSeconds());
  document.getElementById('clockDay').textContent=DAYS[(now.getDay()+6)%7];
  document.getElementById('clockDate').textContent=now.getDate()+' '+MONTHS[now.getMonth()]+' '+now.getFullYear();
  updateCountdown(now);
  if(now.getSeconds()===0){
    const today=schedule[TODAY_IDX]||[];
    today.forEach(e=>{ if(e.active) e.done=isDone(TODAY_IDX,e.time); });
    updateStatusRealtime();
    renderDash();
  }
}

function updateCountdown(now){
  const el=document.getElementById('nCountdown');
  if(!el) return;
  const today=schedule[TODAY_IDX]||[];
  const next=today.find(e=>e.active&&!e.done);
  if(!next){el.textContent='Semua selesai hari ini';return;}
  const [bh,bm]=next.time.split(':').map(Number);
  const diff=bh*60+bm - (now.getHours()*60+now.getMinutes());
  if(diff<=0) el.textContent='Sekarang / lewat';
  else if(diff<60) el.textContent='dalam '+diff+' menit';
  else el.textContent='dalam '+Math.floor(diff/60)+'j '+diff%60+'m';
}

// ─── Status realtime ───────────────────────────
function updateStatusRealtime(){
  const today=schedule[TODAY_IDX]||[];
  const now=new Date();
  const nowMin=now.getHours()*60+now.getMinutes();
  const rows=document.querySelectorAll('#todayTbody tr');
  today.forEach((e,i)=>{
    if(!rows[i]) return;
    const cell=rows[i].querySelector('td:last-child');
    if(!cell) return;
    const [bh,bm]=e.time.split(':').map(Number);
    cell.innerHTML=getBadge(e,isRinging&&Math.abs(bh*60+bm-nowMin)<=1);
  });
  const active=today.filter(e=>e.active);
  const done=today.filter(e=>e.active&&e.done);
  document.getElementById('mDone').textContent=done.length;
  document.getElementById('mBunyi').textContent=isRinging?'1':'0';
  document.getElementById('mLeft').textContent=isRinging?today.filter(e=>e.active&&!e.done).length:active.length-done.length;
  const card=document.getElementById('mBunyiCard');
  card.className='metric'+(isRinging?' metric-ring':'');
}

function getBadge(e,isCurrent){
  if(isCurrent) return '<span class="badge b-ring">Bunyi</span>';
  if(e.done)    return '<span class="badge b-ok">Sudah</span>';
  if(e.active)  return '<span class="badge b-wait">Menunggu</span>';
  return '<span class="badge b-off">Nonaktif</span>';
}

// ─── Dashboard ─────────────────────────────────
function trackName(id){
  const t=TRACKS.find(t=>t.file===String(id).padStart(3,'0')+'.mp3');
  return t?t.name:TRACKS[id-1]?TRACKS[id-1].name:'Track '+id;
}

function renderDash(){
  const today=schedule[TODAY_IDX]||[];
  const now=new Date();
  const nowMin=now.getHours()*60+now.getMinutes();
  const active=today.filter(e=>e.active);
  const done=today.filter(e=>e.active&&e.done);
  document.getElementById('mDone').textContent=done.length;
  document.getElementById('mBunyi').textContent=isRinging?'1':'0';
  document.getElementById('mLeft').textContent=active.length-done.length-(isRinging?1:0);
  document.getElementById('mBunyiCard').className='metric'+(isRinging?' metric-ring':'');
  const next=today.find(e=>e.active&&!e.done);
  if(next){
    document.getElementById('nTime').textContent=next.time;
    document.getElementById('nLabel').textContent=next.label;
    document.getElementById('nTrack').textContent=trackName(next.track);
  } else {
    document.getElementById('nTime').textContent='--:--';
    document.getElementById('nLabel').textContent='Semua selesai';
    document.getElementById('nTrack').textContent='';
  }
  const tbody=document.getElementById('todayTbody');
  tbody.innerHTML='';
  today.forEach(e=>{
    const tr=document.createElement('tr');
    const [bh,bm]=e.time.split(':').map(Number);
    const isCurrent=isRinging&&Math.abs(bh*60+bm-nowMin)<=1;
    tr.innerHTML=`<td style="font-family:var(--mono);font-size:12px">${e.time}</td><td>${e.label}</td><td style="color:var(--text2);font-size:12px">${trackName(e.track)}</td><td>${getBadge(e,isCurrent)}</td>`;
    tbody.appendChild(tr);
  });
}

async function ringNow(){
  const btn=event.target;
  const next=(schedule[TODAY_IDX]||[]).find(e=>e.active&&!e.done);
  const file=next?String(next.track).padStart(3,'0')+'.mp3':'001.mp3';
  btn.textContent='Berbunyi...';btn.disabled=true;
  const r=await get('/play-audio?file='+file);
  if(r&&r.ok) toast('Bel dibunyikan: '+file);
  else toast('Gagal — cek koneksi ke ESP32');
  setTimeout(()=>{btn.textContent='Bunyikan sekarang';btn.disabled=false;},3000);
}

// ─── Jadwal Editor ─────────────────────────────
function renderDayTabs(){
  const dt=document.getElementById('dayTabs');
  const ct=document.getElementById('copyTarget');
  dt.innerHTML='';ct.innerHTML='<option value="">Pilih hari...</option>';
  DAYS.forEach((d,i)=>{
    const btn=document.createElement('button');
    btn.className='day-tab'+(i===activeDay?' active':'')+(i===TODAY_IDX?' today':'');
    btn.textContent=d;
    const cnt=(schedule[i]||[]).filter(e=>e.active).length;
    if(cnt>0){const dot=document.createElement('div');dot.className='day-dot';btn.appendChild(dot);}
    btn.onclick=()=>{activeDay=i;renderDayTabs();renderEdit();};
    dt.appendChild(btn);
    if(i!==activeDay){const opt=document.createElement('option');opt.value=i;opt.textContent=d;ct.appendChild(opt);}
  });
}

function renderEdit(){
  const tbody=document.getElementById('editTbody');
  const entries=schedule[activeDay]||[];
  tbody.innerHTML='';
  if(!entries.length){
    tbody.innerHTML=`<tr><td colspan="5" style="text-align:center;color:var(--text2);padding:1.5rem;font-size:13px">Belum ada jadwal. Klik "+ Tambah bel"</td></tr>`;
    return;
  }
  entries.forEach((e,i)=>{
    const tr=document.createElement('tr');
    const trackOpts=TRACKS.map((t,ti)=>`<option value="${ti+1}"${ti+1===e.track?' selected':''}>${t.name}</option>`).join('');
    const kegOpts=kegiatanList.length
      ? kegiatanList.map(k=>`<option value="${k.nama}"${k.nama===e.label?' selected':''}>${k.nama}</option>`).join('')
      : `<option value="${e.label}" selected>${e.label||'(kosong)'}</option>`;
    tr.innerHTML=`
      <td><input type="time" value="${e.time}" onchange="schedule[${activeDay}][${i}].time=this.value;sortEntries()"></td>
      <td><select onchange="schedule[${activeDay}][${i}].label=this.value">${kegOpts}</select></td>
      <td><select onchange="schedule[${activeDay}][${i}].track=+this.value">${trackOpts}</select></td>
      <td style="text-align:center"><input type="checkbox"${e.active?' checked':''} onchange="schedule[${activeDay}][${i}].active=this.checked"></td>
      <td><button class="btn btn-sm btn-d" onclick="delEntry(${i})">&#x2715;</button></td>`;
    tbody.appendChild(tr);
  });
}

function sortEntries(){schedule[activeDay].sort((a,b)=>a.time.localeCompare(b.time));renderEdit();}
function addEntry(){
  if(!schedule[activeDay]) schedule[activeDay]=[];
  const defLabel=kegiatanList.length?kegiatanList[0].nama:'';
  schedule[activeDay].push({time:'08:00',label:defLabel,track:1,active:true,done:false});
  renderEdit();renderDayTabs();
}
function delEntry(i){schedule[activeDay].splice(i,1);renderEdit();renderDayTabs();}
function copyToDay(){
  const t=document.getElementById('copyTarget').value;if(!t) return;
  const src=JSON.parse(JSON.stringify(schedule[activeDay]));
  src.forEach(e=>e.done=false);schedule[+t]=src;
  renderDayTabs();toast('Jadwal '+DAYS[activeDay]+' disalin ke '+DAYS[+t]);
}
function clearDay(){
  if(!confirm('Hapus semua jadwal '+DAYS[activeDay]+'?')) return;
  schedule[activeDay]=[];renderEdit();renderDayTabs();
}
async function saveAll(){
  const btn=event.target;btn.textContent='Menyimpan...';btn.disabled=true;
  const out={};
  DAYS.forEach((d,i)=>{
    out[d]=(schedule[i]||[]).map(e=>({jam:e.time,kegiatan:e.label,audio:e.track}));
  });
  const r=await post('/save-jadwal',JSON.stringify(out));
  if(r&&r.ok){toast('Jadwal tersimpan di ESP32!',2500);btn.textContent='Tersimpan!';setTimeout(()=>{btn.textContent='Simpan semua';btn.disabled=false;},2000);}
  else{toast('Gagal menyimpan — cek koneksi');btn.textContent='Simpan semua';btn.disabled=false;}
}

// ─── Audio ─────────────────────────────────────
function fmtDur(s){return Math.floor(s/60)+':'+String(s%60).padStart(2,'0');}

function renderTracks(){
  const d=document.getElementById('dfPanel');
  d.innerHTML=TRACKS.map((t,i)=>`
    <div class="df-track">
      <div class="df-num">${String(i+1).padStart(3,'0')}</div>
      <div class="df-name">${t.name}</div>
      <span style="font-size:10px;color:#6b7280;margin-right:6px">${fmtDur(t.dur)}</span>
      <button class="df-play" id="dfp${i}" onclick="playTrack(${i},'${t.file}',${t.dur})">&#9654;</button>
    </div>`).join('');
}

async function playTrack(idx,file,dur){
  if(curFile===file){stopAudio();return;}
  stopT();curFile=file;curDur=dur;elapsed=0;
  document.getElementById('dfp'+idx).innerHTML='&#9632;';
  document.getElementById('dfp'+idx).classList.add('playing');
  document.getElementById('tTot').textContent=fmtDur(dur);
  playTimer=setInterval(()=>{
    elapsed++;
    document.getElementById('progBar').style.width=Math.round(elapsed/dur*100)+'%';
    document.getElementById('tNow').textContent=fmtDur(elapsed);
    if(elapsed>=dur) stopT();
  },1000);
  const r=await get('/play-audio?file='+file);
  if(r&&r.ok) toast('Play: '+file);
  else toast('Gagal play — cek koneksi');
}

function stopT(){
  clearInterval(playTimer);playTimer=null;curFile='';elapsed=0;
  document.querySelectorAll('.df-play').forEach(b=>{b.innerHTML='&#9654;';b.classList.remove('playing');});
  document.getElementById('progBar').style.width='0%';
  document.getElementById('tNow').textContent='0:00';
}

async function stopAudio(){
  stopT();
  const r=await get('/stop-audio');
  if(r&&r.ok) toast('Audio dihentikan');
  else toast('Gagal stop — cek koneksi');
}

async function applyVolume(){
  const btn=event.target;btn.textContent='Menerapkan...';btn.disabled=true;
  const v=document.getElementById('volR').value;
  const r=await get('/volume?val='+v);
  if(r&&r.ok){toast('Volume diterapkan: '+v);btn.textContent='Diterapkan!';}
  else{toast('Gagal — cek koneksi');btn.textContent='Gagal';}
  setTimeout(()=>{btn.textContent='Terapkan volume';btn.disabled=false;},1800);
}

// ─── JSON ──────────────────────────────────────
function renderJSON(){
  const out={};
  DAYS.forEach((d,i)=>{
    out[d]=(schedule[i]||[]).map(e=>({jam:e.time,kegiatan:e.label,audio:e.track}));
  });
  document.getElementById('jsonBox').textContent=JSON.stringify(out,null,2);
}
function cpJSON(){
  navigator.clipboard.writeText(document.getElementById('jsonBox').textContent);
  const b=event.target;b.textContent='Tersalin!';setTimeout(()=>b.textContent='Salin JSON',1200);
}
function dlJSON(){
  const blob=new Blob([document.getElementById('jsonBox').textContent],{type:'application/json'});
  const a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='jadwal.json';a.click();
}
async function pushJSON(){
  const b=event.target;b.textContent='Mengirim...';b.disabled=true;
  const r=await post('/save-jadwal',document.getElementById('jsonBox').textContent);
  if(r&&r.ok){toast('Jadwal berhasil dikirim ke ESP32!',2500);b.textContent='Berhasil!';}
  else{toast('Gagal mengirim — cek koneksi');b.textContent='Gagal';}
  setTimeout(()=>{b.textContent='Kirim ke ESP32';b.disabled=false;},2000);
}

// ─── Kegiatan ──────────────────────────────────
async function loadKegiatan(){
  const r=await get('/kegiatan-list');
  if(!r||!r.ok){kegiatanList=[];return;}
  try{
    const d=await r.json();
    kegiatanList=Array.isArray(d)?d.map(k=>({nama:k.nama||k||''})):[];
  }catch(_){kegiatanList=[];}
}

function renderKegiatan(){
  const tbody=document.getElementById('kegiatanTbody');
  if(!tbody) return;
  tbody.innerHTML='';
  if(!kegiatanList.length){
    tbody.innerHTML=`<tr><td colspan="3" style="text-align:center;color:var(--text2);padding:1.5rem;font-size:13px">Belum ada kegiatan. Klik "+ Tambah"</td></tr>`;
    return;
  }
  kegiatanList.forEach((k,i)=>{
    const tr=document.createElement('tr');
    tr.innerHTML=`
      <td style="text-align:center;color:var(--text2);font-size:12px">${i+1}</td>
      <td><input type="text" value="${k.nama}" placeholder="Nama kegiatan" onchange="kegiatanList[${i}].nama=this.value;renderEdit()"></td>
      <td style="text-align:center"><button class="btn btn-sm btn-d" onclick="delKegiatan(${i})">&#x2715;</button></td>`;
    tbody.appendChild(tr);
  });
}

function addKegiatan(){
  kegiatanList.push({nama:''});
  renderKegiatan();
  const inputs=document.querySelectorAll('#kegiatanTbody input[type=text]');
  if(inputs.length) inputs[inputs.length-1].focus();
}
function delKegiatan(i){kegiatanList.splice(i,1);renderKegiatan();renderEdit();}

async function saveKegiatan(){
  const btn=event.target;
  kegiatanList=kegiatanList.filter(k=>k.nama.trim()!=='');
  renderKegiatan();
  btn.textContent='Menyimpan...';btn.disabled=true;
  const r=await post('/save-kegiatan',JSON.stringify(kegiatanList.map(k=>({nama:k.nama.trim()}))));
  if(r&&r.ok){toast('Kegiatan tersimpan di ESP32!',2500);btn.textContent='Tersimpan!';}
  else{toast('Gagal menyimpan — cek koneksi');btn.textContent='Gagal';}
  setTimeout(()=>{btn.textContent='Simpan ke ESP32';btn.disabled=false;},2000);
}

// ─── Backup & Restore ──────────────────────────
async function downloadBackup(){
  const r=await get('/backup-all');
  if(!r||!r.ok){toast('Gagal mengunduh backup — cek koneksi');return;}
  const blob=await r.blob();
  const now=new Date();
  const ts=now.getFullYear()+pad2(now.getMonth()+1)+pad2(now.getDate())+'_'+pad2(now.getHours())+pad2(now.getMinutes());
  const a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='backup_bel_'+ts+'.json';a.click();
  toast('Backup diunduh: backup_bel_'+ts+'.json',3000);
}

async function restoreBackup(input){
  const file=input.files[0];if(!file) return;
  const statusEl=document.getElementById('restoreStatus');
  statusEl.textContent='Membaca file...';statusEl.style.color='var(--amber-text)';
  try{
    const text=await file.text();
    const parsed=JSON.parse(text);
    if(!parsed.jadwal&&!parsed.kegiatan){statusEl.textContent='Format tidak valid!';statusEl.style.color='var(--red-text)';input.value='';return;}
    statusEl.textContent='Mengirim ke ESP32...';
    const r=await post('/restore-backup',text);
    if(r&&r.ok){
      statusEl.textContent='Berhasil dipulihkan!';statusEl.style.color='var(--green-text)';
      toast('Data berhasil direstore ke ESP32!',3000);
      await loadKegiatan();await loadJadwal();renderDash();renderKegiatan();
      setTimeout(()=>{statusEl.textContent='';},5000);
    } else {statusEl.textContent='Gagal kirim ke ESP32';statusEl.style.color='var(--red-text)';toast('Gagal restore — cek koneksi');}
  }catch(e){statusEl.textContent='File JSON tidak valid';statusEl.style.color='var(--red-text)';}
  input.value='';
}

// ─── Init ──────────────────────────────────────
async function init(){
  await checkConn();
  await loadAudioList();
  await loadKegiatan();
  await loadJadwal();
  renderTracks();
  renderKegiatan();
  updateClock();
  setInterval(updateClock,1000);
  setInterval(checkConn,15000);
  setInterval(pollStatus,5000);
  setInterval(()=>{if(document.getElementById('tab-dashboard').classList.contains('active'))renderDash();},30000);
}

init();
