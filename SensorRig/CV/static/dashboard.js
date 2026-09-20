'use strict';
const el = id => document.getElementById(id);
let radar = null, config = null, lastRadarID = -1, trails = new Map(), trialBusy = false;
let cameraClock = {id: null, at: 0, age: 0}, radarClock = {id: null, at: 0, age: 0};
let cameraLive = false, radarLive = false, lastResponse = 0, manifest = null;
let reconnectTimer = null, videoBroken = false;

function badge(id, text, live = false) {
  el(id).textContent = text;
  el(id).className = 'status' + (live ? ' live' : '');
}
function clearCamera(message) {
  cameraLive = false;
  badge('camera-status', message);
  for (const id of ['camera-count', 'camera-fps', 'camera-age']) el(id).textContent = '—';
  el('camera-held').textContent = 'No fresh camera observations';
  el('camera-latency').textContent = 'Inference — ms';
  el('camera-tracks').textContent = 'No fresh camera observations.';
  el('camera-notice').hidden = false;
  el('camera-notice').textContent = message;
}
function clearRadar(message) {
  radarLive = false;
  badge('radar-status', message);
  for (const id of ['radar-count', 'radar-rate', 'radar-age']) el(id).textContent = '—';
  el('targets').textContent = 'No fresh radar positions.';
  radar = null;
  trails.clear();
  draw();
}
function fresh(clock, id, age, limit) {
  const now = performance.now();
  if (clock.id !== id) Object.assign(clock, {id, at: now, age});
  return Number.isFinite(age) && age <= limit && now - clock.at + clock.age <= limit;
}
function updateCamera(d) {
  if (!d.camera_connected || !fresh(cameraClock, d.frame_id, d.frame_age_ms, 750)) {
    clearCamera(d.error ? 'CAMERA ERROR' : 'CAMERA WAITING');
    return;
  }
  cameraLive = true;
  badge('camera-status', 'CAMERA LIVE', true);
  el('camera-notice').hidden = !videoBroken;
  el('camera-count').textContent = d.person_count;
  el('camera-fps').textContent = d.infer_fps.toFixed(1);
  el('camera-age').textContent = Math.round(d.frame_age_ms);
  el('camera-held').textContent = d.coasting_count ? `${d.coasting_count} briefly lost; excluded` : 'Observed in the current camera frame';
  el('camera-latency').textContent = `Inference ${Math.round(d.infer_ms)} ms · capture ${d.capture_fps.toFixed(1)} fps`;
  el('model').textContent = d.model.replaceAll('_', ' ').toUpperCase();
  el('camera-frame').textContent = `CAMERA FRAME ${d.frame_id}`;
  const out = el('camera-tracks');
  out.replaceChildren();
  if (!d.people.length) out.textContent = 'No confirmed people in camera view.';
  for (const person of d.people) {
    const row = document.createElement('div'); row.className = 'person';
    const label = document.createElement('strong');
    label.textContent = `Camera C${person.id} · ${person.observed ? Math.round(person.score * 100) + '% model score' : 'briefly lost'}`;
    const detail = document.createElement('div'); detail.className = 'details';
    detail.textContent = `${person.bearing_deg.toFixed(1)}° bearing · ${person.range_source === 'unavailable' ? 'range unavailable' : '~' + Math.hypot(person.x_m, person.y_m).toFixed(1) + ' m estimated'}`;
    row.append(label, detail); out.append(row);
  }
}
function transform(x, y, c) {
  if (c.invert_x) x = -x;
  const angle = c.yaw_deg * Math.PI / 180;
  return [c.offset_right_m + x * Math.cos(angle) + y * Math.sin(angle), c.offset_forward_m - x * Math.sin(angle) + y * Math.cos(angle)];
}
function draw(){const canvas=el('scope'),ctx=canvas.getContext('2d'),w=canvas.width,h=canvas.height,max=Number(el('zoom').value),scale=Math.min((w-110)/(2*max),(h-130)/max),ox=w/2,oy=h-80;ctx.fillStyle='#0c1218';ctx.fillRect(0,0,w,h);const quest=document.body.classList.contains('quest-view');ctx.font=(quest?'34':'17')+'px monospace';ctx.textAlign='center';ctx.lineWidth=quest?2:1;for(let x=-Math.ceil(max);x<=Math.ceil(max);x++){ctx.strokeStyle='#24303b';ctx.beginPath();ctx.moveTo(ox+x*scale,50);ctx.lineTo(ox+x*scale,oy);ctx.stroke();ctx.fillStyle='#8193a4';ctx.fillText((x>0?'+':'')+x+'m',ox+x*scale,oy+27);}for(let y=0;y<=max;y++){ctx.beginPath();ctx.moveTo(ox-max*scale,oy-y*scale);ctx.lineTo(ox+max*scale,oy-y*scale);ctx.stroke();ctx.fillText(y+'m',38,oy-y*scale+5);}ctx.strokeStyle='#4b6275';ctx.beginPath();ctx.moveTo(ox,45);ctx.lineTo(ox,oy);ctx.stroke();ctx.fillStyle='#a7b9c9';ctx.fillText('FORWARD +Y',ox,28);ctx.fillText('LEFT −X',100,h-15);ctx.fillText('RIGHT +X',w-105,h-15);
const c=radar?.config;if(c){ctx.save();ctx.strokeStyle='#43677f';ctx.setLineDash([8,9]);for(const angle of [-60,60]){const a=(c.yaw_deg+angle)*Math.PI/180;ctx.beginPath();ctx.moveTo(ox+c.offset_right_m*scale,oy-c.offset_forward_m*scale);ctx.lineTo(ox+(c.offset_right_m+max*Math.sin(a))*scale,oy-(c.offset_forward_m+max*Math.cos(a))*scale);ctx.stroke();}ctx.restore();}
ctx.fillStyle='#98ed83';ctx.beginPath();ctx.moveTo(ox,oy-11);ctx.lineTo(ox+9,oy+7);ctx.lineTo(ox-9,oy+7);ctx.closePath();ctx.fill();
if(!radar||radar.status!=='live'){ctx.fillStyle='#edbf72';ctx.fillText('WAITING FOR FRESH RADAR DATA',ox,h/2);return;}
for(const [id,trail] of trails){const visible=trail.filter(p=>performance.now()-p.at<1800);if(!visible.length){trails.delete(id);continue;}trails.set(id,visible);ctx.strokeStyle='#375469';ctx.beginPath();visible.forEach((p,i)=>{const x=ox+p.x*scale,y=oy-p.y*scale;i?ctx.lineTo(x,y):ctx.moveTo(x,y);});ctx.stroke();}
if(el('showRaw').checked){ctx.strokeStyle='#a8aeb8';for(const raw of radar.raw_targets){const[x,y]=transform(raw.x/1000,raw.y/1000,c),px=ox+x*scale,py=oy-y*scale;ctx.beginPath();ctx.moveTo(px-5,py-5);ctx.lineTo(px+5,py+5);ctx.moveTo(px-5,py+5);ctx.lineTo(px+5,py-5);ctx.stroke();}}
ctx.textAlign='left';for(const t of radar.targets){const x=ox+t.right_m*scale,y=oy-t.forward_m*scale;ctx.fillStyle='#88c9f2';ctx.beginPath();ctx.arc(x,y,quest?14:8,0,Math.PI*2);ctx.fill();ctx.fillText('R'+t.id,Math.min(x+(quest?24:13),w-70),Math.max(y-(quest?22:12),45));}}
function fillConfig(c) {
  config = {...c};
  el('yaw').value = c.yaw_deg; el('minimum').value = c.min_range_m;
  el('offsetRight').value = c.offset_right_m; el('offsetForward').value = c.offset_forward_m;
  el('mirror').checked = c.invert_x; el('level').checked = c.mount_level; el('verified').checked = c.mounting_confirmed;
}
function updateRadar(d) {
  if (d.config && !config) fillConfig(d.config);
  const diag = d.diagnostics || {};
  el('diagnostics').textContent = `Firmware ${diag.firmware || 'unknown'} · ${diag.tracking_mode || 'unknown'} targets · zone filter ${diag.zone_filter || 'unknown'}. ${diag.query_error || ''}`;
  if (d.status !== 'live' || !fresh(radarClock, d.frame_id, d.age_ms, 500)) {
    clearRadar(d.status === 'disabled' ? 'RADAR DISABLED' : 'RADAR WAITING');
    return;
  }
  if (radar && JSON.stringify(radar.config) !== JSON.stringify(d.config)) trails.clear();
  radar = d; radarLive = true;
  badge('radar-status', 'RADAR LIVE', true);
  el('radar-count').textContent = d.targets.length;
  el('radar-rate').textContent = d.update_hz.toFixed(1);
  el('radar-age').textContent = Math.round(d.age_ms);
  el('radar-frame').textContent = `RADAR FRAME ${d.frame_id}`;
  el('mountNotice').textContent = d.horizontal_projection_enabled
    ? `${d.reference_origin === 'radar' ? 'Radar origin' : 'Configured drone origin'} · +right / +forward · level sensor assumed. Left/right correction ${d.config.invert_x ? 'on' : 'off'}.`
    : 'Mount unconfirmed: displayed estimates are withheld from drone-position handoff.';
  if (lastRadarID !== d.frame_id) {
    lastRadarID = d.frame_id;
    for (const t of d.targets) {
      const trail = trails.get(t.id) || [];
      trail.push({x: t.right_m, y: t.forward_m, at: performance.now()});
      trails.set(t.id, trail.slice(-25));
    }
    for (const id of trails.keys()) if (!d.targets.some(t => t.id === id)) trails.delete(id);
  }
  const out = el('targets'); out.replaceChildren();
  if (!d.targets.length) out.textContent = 'No confirmed radar targets. This does not prove the room is empty.';
  for (const t of d.targets) {
    const row = document.createElement('div'); row.className = 'target';
    const name = document.createElement('strong'); name.textContent = `Radar R${t.id}`;
    const pos = document.createElement('div'); pos.className = 'coords';
    pos.textContent = `${t.right_m >= 0 ? '+' : ''}${t.right_m.toFixed(2)} right · ${t.forward_m.toFixed(2)} forward`;
    const detail = document.createElement('div'); detail.className = 'details';
    detail.textContent = `${t.range_m.toFixed(2)} m · ${t.bearing_deg.toFixed(1)}° · ${t.radial_speed_mps.toFixed(2)} m/s`;
    row.append(name, pos, detail); out.append(row);
  }
  draw();
}
async function poll() {
  try {
    const response = await fetch('/detections', {cache: 'no-store', signal: AbortSignal.timeout(1800)});
    if (!response.ok) throw Error('HTTP ' + response.status);
    const d = await response.json(); lastResponse = performance.now();
    // Neither sensor's health gates the other sensor's display.
    updateCamera(d); updateRadar(d.radar);
    el('error').textContent = [d.error ? 'Camera: ' + d.error : '', d.radar.error ? 'Radar: ' + d.radar.error : ''].filter(Boolean).join(' · ');
    el('bridge-status').textContent = d.quest.status === 'listening' ? `Bridge listening · ${d.quest.clients} clients` : 'Bridge disabled';
    el('pose-status').textContent = d.quest.pose_status || 'Awaiting rig pose';
  } catch (error) {
    clearCamera('CAMERA CONNECTION LOST'); clearRadar('RADAR CONNECTION LOST');
    el('bridge-status').textContent = 'Connection unavailable';
  } finally { setTimeout(poll, 100); }
}
setInterval(() => {
  const now = performance.now();
  if (cameraLive && now - cameraClock.at + cameraClock.age > 750) clearCamera('CAMERA STALE');
  if (radarLive && now - radarClock.at + radarClock.age > 500) clearRadar('RADAR STALE');
  if (lastResponse && now - lastResponse > 1800) el('bridge-status').textContent = 'Connection unavailable';
}, 100);
const feed = el('feed');
feed.onerror = () => {
  videoBroken = true;
  el('camera-notice').hidden = false; el('camera-notice').textContent = 'Reconnecting camera video…';
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => { reconnectTimer = null; feed.src = '/stream?t=' + Date.now(); }, 1500);
};
feed.onload = () => { videoBroken = false; if (cameraLive) el('camera-notice').hidden = true; };
el('fullscreen').onclick = async () => {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else if (document.documentElement.requestFullscreen) await document.documentElement.requestFullscreen();
    else throw Error('Use the browser’s expand control on this device.');
  } catch (error) { el('display-message').textContent = 'Full screen unavailable. Use the browser’s expand control.'; }
};
document.addEventListener('fullscreenchange', () => { el('fullscreen').textContent = document.fullscreenElement ? 'Exit full screen' : 'Full screen'; });
async function loadManifest() {
  const response = await fetch('/handoff.json', {cache: 'no-store', signal: AbortSignal.timeout(2500)});
  if (!response.ok) throw Error('Connection details unavailable');
  manifest = await response.json();
  el('quest-url').value = manifest.quest_url;
  el('ws-url').textContent = manifest.websocket_url || 'Bridge is disabled';
  return manifest;
}
el('copy-url').onclick = async () => {
  try {
    if (!navigator.clipboard?.writeText) throw Error('Clipboard unavailable');
    await navigator.clipboard.writeText(el('quest-url').value); el('copy-state').textContent = 'Quest address copied.';
  } catch (error) {
    el('quest-url').focus(); el('quest-url').select();
    el('copy-state').textContent = 'Address selected. Copy it or type it into the Quest browser.';
  }
};
el('check-bridge').onclick = async () => {
  const button = el('check-bridge'); button.disabled = true;
  el('bridge-check').textContent = 'Connecting…';
  let socket, timeout;
  try {
    const info = await loadManifest();
    if (!info.websocket_url) throw Error('Start the server with --quest-port 8765.');
    const packet = await new Promise((resolve, reject) => {
      socket = new WebSocket(info.websocket_url);
      timeout = setTimeout(() => reject(Error('No telemetry packet received within 4 seconds.')), 4000);
      socket.onmessage = event => {
        try {
          const value = JSON.parse(event.data);
          if (value.schema_version !== 1 || !Array.isArray(value.drone_relative_radar_targets)) throw Error('Unexpected telemetry schema.');
          resolve(value);
        } catch (error) { reject(error); }
      };
      socket.onerror = () => reject(Error('WebSocket unreachable from this device. Check the network and port.'));
      socket.onclose = () => reject(Error('Connection closed before telemetry arrived.'));
    });
    el('bridge-check').textContent = `Telemetry received on this device · ${packet.camera_connected ? 'camera live' : 'camera unavailable'} · radar ${packet.radar.status}. Headset alignment still requires a physical check.`;
  } catch (error) { el('bridge-check').textContent = error.message; }
  finally { clearTimeout(timeout); if (socket) socket.close(); button.disabled = false; }
};
loadManifest().catch(error => { el('ws-url').textContent = error.message; });
el('zoom').onchange=draw;el('showRaw').onchange=draw;
el('save').onclick=async()=>{try{const candidate={...config,invert_x:el('mirror').checked,mount_level:el('level').checked,mounting_confirmed:el('verified').checked,yaw_deg:Number(el('yaw').value),offset_right_m:Number(el('offsetRight').value),offset_forward_m:Number(el('offsetForward').value),min_range_m:Number(el('minimum').value)};const response=await fetch('/radar/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(candidate)});const d=await response.json();if(!response.ok)throw Error(d.error);fillConfig(d);trails.clear();el('configState').textContent='Mounting saved.';}catch(e){el('configState').textContent=e.message;}};
el('trialLabel').onchange=()=>{const empty=el('trialLabel').value==='empty';for(const id of ['knownRight','knownForward']){el(id).disabled=empty;if(empty)el(id).value='';}};el('trialLabel').onchange();
el('record').onclick=async()=>{try{const body={label:el('trialLabel').value,duration_s:12,right_m:el('knownRight').value===''?null:Number(el('knownRight').value),forward_m:el('knownForward').value===''?null:Number(el('knownForward').value)};const response=await fetch('/radar/trials',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});const d=await response.json();if(!response.ok)throw Error(d.error);el('trialState').textContent='Starting in 5 seconds…';}catch(e){el('trialState').textContent=e.message;}};
async function pollTrials(){try{const response=await fetch('/radar/trials',{cache:'no-store',signal:AbortSignal.timeout(2000)});if(!response.ok)return;const d=await response.json(),c=d.current;trialBusy=['countdown','recording'].includes(c.status);el('record').disabled=trialBusy;el('save').disabled=trialBusy;if(trialBusy)el('trialState').textContent=`${c.status==='countdown'?'Starting in':'Recording · remaining'} ${c.remaining_s}s`;else if(c.status==='error')el('trialState').textContent=c.error;else if(c.status==='complete')el('trialState').textContent='Trial saved. Compare the empty baseline and the two person trials.';const reports=el('reports');reports.replaceChildren();for(const r of d.reports.slice(-3).reverse()){const s=r.summary,row=document.createElement('div');row.className='trial';const fraction=s.target_frame_fraction===null?'no fresh frames':Math.round(s.target_frame_fraction*100)+'% frames with a target';row.textContent=`${r.label.replaceAll('_',' ')}: ${fraction}; ${s.ambiguous_frames} frames with multiple targets. ${s.mean_position_error_m===null?'':`Mean error ${s.mean_position_error_m.toFixed(2)} m on ${s.position_samples} single-target frames.`} ${s.outage_polls?'Radar outages occurred.':''}`;reports.append(row);}}catch(e){}finally{setTimeout(pollTrials,500);}}

draw(); poll(); pollTrials();
