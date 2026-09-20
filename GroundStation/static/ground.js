(() => {
  'use strict';
  const get = id => document.getElementById(id);
  const finite = Number.isFinite, fmt = (v, n = 2) => finite(v) ? v.toFixed(n) : '—';
  const colors = {camera: '#edbf72', radar: '#88c9f2', output: '#98ed83'};
  const key = (kind, generation, id) => `${kind}/${generation}/${id}`;
  const range = p => p ? Math.hypot(p.right, p.forward) : null;
  const bearing = p => p ? Math.atan2(p.right, p.forward) * 180 / Math.PI : null;
  const position = p => p && finite(p.right_m) && finite(p.forward_m) ? {right:p.right_m, forward:p.forward_m} : null;
  let settings = null, latest = null, received = 0, failure = '', identity = '', selected = '';
  let previous = new Map(), histories = new Map(), health = [], events = [], hits = [];
  let requestMs = 0;
  let poseImageToken='';
  const exportSnapshot = () => ({...latest, browser:{snapshot_time_utc:new Date().toISOString(),
    age_since_response_ms:performance.now()-received, diagnostics_error:failure||null}});

  function element(tag, text, cls = '') {
    const node = document.createElement(tag); node.textContent = text; node.className = cls; return node;
  }
  function event(message) {
    events.unshift(`${new Date().toLocaleTimeString()} · ${message}`); events.length = Math.min(events.length, 60);
  }
  function cameraPosition(person, packet) {
    if (!person.observed || person.range_source !== 'monocular_estimate' || !finite(person.x_m) || !finite(person.y_m)) return null;
    const yaw = packet.radar?.config?.yaw_deg;
    if (!finite(yaw)) return null;
    const a = yaw * Math.PI / 180, c = latest.config;
    return {right:person.x_m * Math.cos(a) + person.y_m * Math.sin(a) + c.camera_offset_right_m,
      forward:-person.x_m * Math.sin(a) + person.y_m * Math.cos(a) + c.camera_offset_forward_m};
  }
  function model(now) {
    const empty = {rows:[], outputs:[], cameras:[], radars:[], cameraAge:null, radarAge:null, cameraLive:false, radarLive:false};
    if (!latest || failure) return empty;
    const p = latest.packet, f = p.spatial_people, elapsed = now - received;
    const ca = finite(p.camera_age_ms) ? p.camera_age_ms + elapsed : null;
    const ra = finite(p.radar?.age_ms) ? p.radar.age_ms + elapsed : null;
    const cameraLive = p.camera_connected && finite(ca) && ca <= 750;
    const radarLive = p.radar?.status === 'live' && finite(ra) && ra <= 500;
    const cameras = cameraLive ? p.camera_people || [] : [], radars = radarLive ? p.radar.targets || [] : [];
    const outputs = [];
    for (const original of f.people) {
      if (!finite(original.camera_age_ms) || original.camera_age_ms + elapsed > 750) continue;
      const person = {...original};
      if (person.position_source === 'radar_matched' && (!finite(person.radar_age_ms) || person.radar_age_ms + elapsed > 500)) {
        if (!position(person.fallback_position)) continue;
        Object.assign(person, person.fallback_position, {position_source:'camera_estimate', radar_id:null});
      }
      outputs.push({key:key('C',person.camera_generation,person.camera_id), label:`C${person.camera_id}`,
        kind:person.position_source === 'radar_matched' ? 'green' : 'amber',
        source:person.position_source === 'radar_matched' ? `RADAR · R${person.radar_id}` : 'ESTIMATED',
        pos:position(person), age:person.camera_age_ms+elapsed, radarAge:person.radar_age_ms == null ? null : person.radar_age_ms+elapsed, raw:person});
    }
    for (const radar of f.radar_targets) {
      if (!finite(radar.age_ms) || radar.age_ms + elapsed > 500) continue;
      outputs.push({key:key('R',radar.generation,radar.id),label:`R${radar.id}`,kind:'blue',source:'RADAR ONLY',
        pos:position(radar),age:radar.age_ms+elapsed,radarAge:radar.age_ms+elapsed,raw:radar});
    }
    if(f.tracks_version===1){outputs.length=0;for(const t of f.tracks||[]){
      if(!finite(t.valid_for_ms)||t.valid_for_ms<elapsed)continue;
      const k=t.camera_id!=null?key('C',t.camera_generation,t.camera_id):key('R',t.radar_generation,t.radar_id);
      outputs.push({key:k,label:`P${t.id}`,kind:t.position_source==='radar_matched'?'green':t.position_source==='radar_only'?'blue':'amber',
        source:`${t.position_source.replaceAll('_',' ').toUpperCase()} / ${t.pose_source==='camera'&&t.pose_age_ms+elapsed<=350?'CAMERA POSE':'EST. POSE'}`,
        pos:position(t),age:t.age_ms+elapsed,radarAge:t.position_source==='camera_estimate'?null:t.age_ms+elapsed,raw:t});
    }}
    const rows = cameras.map(c => {
      const k = key('C',p.camera_generation,c.id), out = outputs.find(o => o.key === k);
      const pos = cameraPosition(c,p);
      return {key:k,label:`C${c.id}`,kind:'camera',generation:p.camera_generation,frame:p.camera_frame_id,
        score:c.observed ? c.score : null,pos,age:ca,raw:c,output:out,
        state:!c.observed ? 'Coasting · excluded' : out ? out.source : pos ? 'Estimate · outside body limit' : 'Bearing only · no range'};
    });
    for (const r of radars) {
      const out = outputs.find(o => o.kind === 'green' && o.raw.radar_id === r.id && o.raw.radar_generation === p.radar.generation);
      rows.push({key:key('R',p.radar.generation,r.id),label:`R${r.id}`,kind:'radar',generation:p.radar.generation,frame:p.radar.frame_id,
        score:null,pos:r.observed ? position(r) : null,age:ra,raw:r,output:outputs.find(o => o.key === key('R',p.radar.generation,r.id)),
        state:!r.observed ? 'Coasting · excluded' : out ? `Matched to ${out.label}` : 'Independent · unclassified'});
    }
    return {rows,outputs,cameras,radars,cameraAge:ca,radarAge:ra,cameraLive,radarLive};
  }
  function remember(m, now) {
    const active = new Map();
    for (const row of m.rows) {
      active.set(row.key,row.state);
      if (!previous.has(row.key)) event(`${row.label} appeared · ${row.state}`);
      else if (previous.get(row.key) !== row.state) event(`${row.label} → ${row.state}`);
      const out = row.output, match = out?.kind === 'green' ? out.raw : null;
      const token = `${row.frame}/${out?.source}/${match?.radar_generation}/${match?.radar_frame_id}`;
      let track = histories.get(row.key);
      if (!track) { track = {points:[],token:null,last:now}; histories.set(row.key,track); }
      track.last = now;
      if (track.token !== token) {
        track.token = token;
        track.points.push({t:now,camera:row.kind === 'camera' ? row.pos : null,
          radar:row.kind === 'radar' ? row.pos : match ? position(match) : null,
          output:out?.pos || null,score:row.kind === 'camera' && finite(row.score) ? row.score*100 : null});
        track.points = track.points.filter(point => now-point.t <= 30000).slice(-300);
      }
    }
    for (const k of previous.keys()) if (!active.has(k)) event(`${k.split('/')[0]}${k.split('/')[2]} left / expired`);
    previous = active;
    for (const [k,track] of histories) if (now-track.last > 30000) histories.delete(k);
    while (histories.size > 64) histories.delete(histories.keys().next().value);
  }
  function choose(k) { selected = k; render(); }
  function plot(canvasId, series, now, options = {}) {
    const canvas = get(canvasId), ctx = canvas.getContext('2d'), w = canvas.width, h = canvas.height;
    const left=68, right=w-25, top=30, bottom=h-46;
    ctx.fillStyle='#0c1218'; ctx.fillRect(0,0,w,h); ctx.font='18px ui-monospace, monospace';
    const valid = series.flatMap(s => s.points.filter(p => finite(p.v) && now-p.t <= 30000).map(p => p.v));
    let low=options.low ?? Math.min(0,...valid), high=options.high ?? Math.max(1,...valid);
    if (high-low < .1) high=low+1;
    if (options.high == null) high += (high-low)*.12;
    const px=t=>left+(1-(now-t)/30000)*(right-left), py=v=>bottom-(v-low)/(high-low)*(bottom-top);
    ctx.textAlign='right'; ctx.fillStyle='#8193a4'; ctx.strokeStyle='#24303b'; ctx.lineWidth=1;
    for(let i=0;i<=4;i++) { const v=low+(high-low)*i/4,y=py(v); ctx.beginPath();ctx.moveTo(left,y);ctx.lineTo(right,y);ctx.stroke();ctx.fillText(fmt(v,options.digits ?? 1),left-9,y+6); }
    ctx.textAlign='center';
    for(let t=0;t<=30;t+=10) ctx.fillText(t===0?'now':`−${t}s`,right-(t/30)*(right-left),h-12);
    ctx.save();ctx.beginPath();ctx.rect(left,top,right-left,bottom-top);ctx.clip();
    for(const limit of options.limits || []) {ctx.strokeStyle=limit.color;ctx.setLineDash([8,7]);ctx.beginPath();ctx.moveTo(left,py(limit.v));ctx.lineTo(right,py(limit.v));ctx.stroke();}
    ctx.setLineDash([]);
    for(const s of series) {
      ctx.strokeStyle=s.color;ctx.lineWidth=2.5;ctx.beginPath();let before=null;
      for(const p of s.points) { if(!finite(p.v)||now-p.t>30000){before=null;continue;} const x=px(p.t),y=py(p.v);
        if(before && p.t-before.t<1000)ctx.lineTo(x,y);else ctx.moveTo(x,y);before=p;
      }
      ctx.stroke();
      const last=s.points.at(-1);if(last&&finite(last.v)&&now-last.t<1000){ctx.fillStyle=s.color;ctx.beginPath();ctx.arc(px(last.t),py(last.v),4,0,Math.PI*2);ctx.fill();}
    }
    ctx.restore();
    if(!valid.length){ctx.textAlign='center';ctx.fillStyle='#8193a4';ctx.fillText('Waiting for observations',w/2,h/2);}
  }
  function map(m, now) {
    const c=get('fusion-map'),ctx=c.getContext('2d'),w=c.width,h=c.height,max=Number(get('fusion-range').value);
    const scale=Math.min((w-120)/(2*max),(h-105)/(max+1)),ox=w/2,oy=45+max*scale;
    const xy=p=>[ox+p.right*scale,oy-p.forward*scale];
    ctx.fillStyle='#0c1218';ctx.fillRect(0,0,w,h);ctx.font='18px ui-monospace, monospace';ctx.textAlign='center';ctx.lineWidth=1;
    for(let x=-max;x<=max;x++){ctx.strokeStyle='#24303b';ctx.beginPath();ctx.moveTo(ox+x*scale,45);ctx.lineTo(ox+x*scale,oy+scale);ctx.stroke();ctx.fillStyle='#8193a4';ctx.fillText(`${x}m`,ox+x*scale,h-18);}
    for(let y=-1;y<=max;y++){ctx.beginPath();ctx.moveTo(ox-max*scale,oy-y*scale);ctx.lineTo(ox+max*scale,oy-y*scale);ctx.stroke();ctx.fillText(`${y}m`,30,oy-y*scale+6);}
    ctx.fillStyle='#bccbd8';ctx.fillText('FORWARD ↑ / RIGHT →',ox,24);
    hits=[];
    ctx.save();ctx.beginPath();ctx.rect(ox-max*scale,45,2*max*scale,(max+1)*scale);ctx.clip();
    for(const [k,track] of histories) {
      ctx.strokeStyle=k===selected?'#8193a4':'#33414e';ctx.lineWidth=k===selected?2:1;ctx.beginPath();let last=null;
      for(const p of track.points){const pos=p.output||p.camera||p.radar;if(!pos||now-p.t>8000){last=null;continue;}const[x,y]=xy(pos);if(last&&p.t-last.t<1000)ctx.lineTo(x,y);else ctx.moveTo(x,y);last=p;}ctx.stroke();
    }
    const config=latest?.config, mount=latest?.packet.radar?.config;
    if(config&&finite(mount?.yaw_deg)){
      const origin={right:config.camera_offset_right_m,forward:config.camera_offset_forward_m},a=mount.yaw_deg*Math.PI/180;
      const fov=latest.packet.camera_hfov_deg*Math.PI/180;
      if(finite(fov)){ctx.fillStyle='#edbf720c';ctx.beginPath();ctx.moveTo(...xy(origin));
        for(const angle of [a-fov/2,a+fov/2])ctx.lineTo(...xy({right:origin.right+max*Math.sin(angle),forward:origin.forward+max*Math.cos(angle)}));ctx.closePath();ctx.fill();}
      for(const r of m.rows.filter(r=>r.kind==='camera'&&!r.pos&&r.raw.observed&&finite(r.raw.bearing_deg))){const angle=a+r.raw.bearing_deg*Math.PI/180;
        ctx.strokeStyle=colors.camera;ctx.setLineDash([3,10]);ctx.beginPath();ctx.moveTo(...xy(origin));ctx.lineTo(...xy({right:origin.right+max*Math.sin(angle),forward:origin.forward+max*Math.cos(angle)}));ctx.stroke();ctx.setLineDash([]);}
    }
    for(const r of m.rows){if(!r.pos)continue;const[x,y]=xy(r.pos);ctx.strokeStyle=colors[r.kind];ctx.lineWidth=2;
      if(r.kind==='camera')ctx.strokeRect(x-6,y-6,12,12);else{ctx.beginPath();ctx.moveTo(x-7,y-7);ctx.lineTo(x+7,y+7);ctx.moveTo(x+7,y-7);ctx.lineTo(x-7,y+7);ctx.stroke();}
      ctx.fillStyle=colors[r.kind];ctx.textAlign='left';ctx.fillText(r.label,x+12,y-11);hits.push({x,y,key:r.key});
    }
    for(const o of m.outputs){if(!o.pos)continue;const[x,y]=xy(o.pos);
      if(finite(o.raw.variance_right_m2)&&finite(o.raw.variance_forward_m2)){
        ctx.strokeStyle='#c3d7e344';ctx.lineWidth=1;ctx.beginPath();ctx.ellipse(x,y,
          Math.min(scale*3,2*Math.sqrt(o.raw.variance_right_m2)*scale),Math.min(scale*3,2*Math.sqrt(o.raw.variance_forward_m2)*scale),0,0,Math.PI*2);ctx.stroke();
        if(finite(o.raw.velocity_right_mps)&&finite(o.raw.velocity_forward_mps)){ctx.strokeStyle='#c3d7e399';ctx.beginPath();ctx.moveTo(x,y);ctx.lineTo(x+o.raw.velocity_right_mps*scale*.5,y-o.raw.velocity_forward_mps*scale*.5);ctx.stroke();}
      }
      if(o.kind==='green'){const camera=m.rows.find(r=>r.key===o.key);if(camera?.pos){ctx.strokeStyle='#98ed8377';ctx.setLineDash([5,6]);ctx.beginPath();ctx.moveTo(...xy(camera.pos));ctx.lineTo(x,y);ctx.stroke();ctx.setLineDash([]);}}
      ctx.strokeStyle=o.kind==='green'?colors.output:o.kind==='blue'?colors.radar:colors.camera;ctx.lineWidth=2;ctx.beginPath();ctx.arc(x,y,o.key===selected?13:9,0,Math.PI*2);ctx.stroke();hits.push({x,y,key:o.key});
    }
    ctx.restore();ctx.fillStyle='#d8e4e9';ctx.beginPath();ctx.moveTo(ox,oy-8);ctx.lineTo(ox-7,oy+6);ctx.lineTo(ox+7,oy+6);ctx.closePath();ctx.fill();
  }
  function render() {
    const now=performance.now(),m=model(now);
    if(!latest){get('fusion-status').textContent=failure||'Waiting for sensor telemetry.';return;}
    const p=latest.packet,f=p.spatial_people;
    const pp=p.pose_pipeline||{status:'disabled'},elapsed=now-received;
    const poseFresh=!failure&&pp.status==='live'&&finite(pp.age_ms)&&pp.age_ms+elapsed<=350;
    get('human-pose-status').textContent=`${poseFresh?'LIVE POSE':pp.status.toUpperCase()} · ${pp.accepted??0} bound poses · processing ${fmt(pp.processing_ms,0)} ms · age ${fmt(pp.age_ms==null?null:pp.age_ms+elapsed,0)} ms${pp.error?' · '+pp.error:''}`;
    const img=get('pose-image');img.hidden=!poseFresh;
    const imageToken=`${pp.source_session_id}/${pp.generation}/${pp.frame_id}`;
    if(poseFresh&&imageToken!==poseImageToken){poseImageToken=imageToken;img.src='/pose/snapshot.jpg?frame='+encodeURIComponent(imageToken);}
    get('pose-tracks').replaceChildren(...(failure?[]:(f.tracks||[]).filter(t=>t.valid_for_ms>=elapsed)).map(t=>{
      const speed=f.rig_motion_mode==='left_controller'?'World speed estimated on Quest':`${fmt(Math.hypot(t.velocity_right_mps,t.velocity_forward_mps))} m/s relative to rig`;
      const n=element('p',`P${t.id} · ${t.person_evidence} · ${t.pose_source==='camera'&&t.pose_age_ms+elapsed<=350?'camera joints':'estimated motion'}\n${speed} · height ${fmt(t.height_m)} m (${t.height_source})\n${t.camera_visibility}`,'details');n.style.whiteSpace='pre-line';return n;
    }));
    get('pose-decisions').textContent=[...(f.track_decisions||[]),...(p.radar?.filter_decisions||[])].map(d=>`P${d.id??'?'}: ${d.reason}`).join(' · ')||'No current filter rejections.';
    const rig=p.quest_controller_rig,rigFresh=!failure&&rig&&rig.age_ms+elapsed<=750;
    get('pose-rig').textContent=f.rig_motion_mode==='left_controller'
      ?`LEFT CONTROLLER · ${rigFresh?rig.status:'Waiting for Quest alignment / fresh tracking report'}. Radar floor positions are estimated; camera floor ranging is disabled in this mode.`
      :f.rig_pose_valid===false?`Quest world placement paused: ${p.rig_motion?.motion_alarm?'background motion detected':'rig motion is untracked'}. Relative sensor data remains available.`:'Quest placement assumes the registered rig remains stationary. Re-register after moving it.';
    remember(m,now);
    if(!m.rows.some(r=>r.key===selected))selected=m.rows[0]?.key||'';
    const options=m.rows.map(r=>`${r.key}:${r.label} · ${r.state}`).join('|');
    const select=get('fusion-select');
    if(select.dataset.options!==options){select.replaceChildren(...(m.rows.length?m.rows.map(r=>{const o=element('option',`${r.label} · ${r.state}`);o.value=r.key;return o;}):[Object.assign(element('option','No current contacts'),{value:''})]));select.dataset.options=options;}
    select.value=selected;
    get('fusion-mode').textContent=latest.is_replay?'RECORDED REPLAY':'LIVE INPUT';
    get('fusion-status').textContent=failure||`${m.cameraLive?'CAMERA LIVE':'CAMERA STALE / UNAVAILABLE'} · ${m.radarLive?'RADAR LIVE':'RADAR STALE / UNAVAILABLE'} · ${latest.clients} relay clients · matching ${f.alignment_confirmed?'enabled':'awaiting calibration'}${latest.error?' · '+latest.error:''}`;
    get('fusion-camera-count').textContent=m.cameras.filter(c=>c.observed).length;
    get('fusion-matched-count').textContent=m.outputs.filter(o=>o.kind==='green').length;
    get('fusion-estimated-count').textContent=m.outputs.filter(o=>o.kind==='amber').length;
    get('fusion-radar-count').textContent=m.outputs.filter(o=>o.kind==='blue').length;
    get('fusion-unpositioned-count').textContent=m.rows.filter(r=>r.kind==='camera'&&r.raw.observed&&!r.pos&&!r.output).length;
    const cards=m.outputs.map((o,i)=>{const node=element('div','',`person ${o.kind}${selected===o.key?' selected':''}`);node.onclick=()=>choose(o.key);
      node.append(element('strong',`${o.label} / ${o.source}${i>=8?' · MAP ONLY':''}`),
        element('div',`${fmt(o.pos?.right)} right / ${fmt(o.pos?.forward)} forward m`),
        element('div',`${fmt(range(o.pos))} m range · ${fmt(bearing(o.pos),1)}° bearing`,'details'),
        element('div',`Observation age ${fmt(o.age,0)} ms${o.kind==='blue'?' · unclassified':''}`,'details'),
        element('div',o.raw.sample_key?`Source ${o.raw.sample_key} · ${o.raw.person_evidence}`:o.kind==='blue'?`Radar generation ${o.raw.generation} / frame ${o.raw.frame_id}`:`Camera generation ${o.raw.camera_generation} / frame ${o.raw.camera_frame_id}`,'details'));return node;});
    get('fusion-people').replaceChildren(...cards);get('fusion-empty').hidden=cards.length>0;
    get('fusion-observations').querySelector('tbody').replaceChildren(...m.rows.map(r=>{
      const tr=document.createElement('tr');tr.className=r.key===selected?'selected':'';tr.onclick=()=>choose(r.key);
      const button=element('button',r.label,r.kind==='camera'?'amber':'blue');button.type='button';const first=document.createElement('td');first.append(button);tr.append(first);
      const details=r.kind==='camera'?`box ${(r.raw.box||[]).map(v=>fmt(v,0)).join(', ')} px`:`${fmt(r.raw.radial_speed_mps)} m/s radial`;
      for(const value of [r.state,r.kind==='camera'&&finite(r.score)?`${fmt(r.score*100,1)}%`:'—',r.pos?`${fmt(r.pos.right)} / ${fmt(r.pos.forward)}`:'Unavailable',
        `${fmt(range(r.pos))} m / ${fmt(r.pos?bearing(r.pos):r.raw.bearing_deg,1)}°`,`${fmt(r.age,0)} ms`,`${r.generation} / ${r.frame}`,details])tr.append(element('td',value));return tr;
    }));
    const row=m.rows.find(r=>r.key===selected),metric=get('fusion-metric').value,track=histories.get(selected);
    const value=pos=>!pos?null:metric==='range'?range(pos):pos[metric];
    const series=metric==='score'?[{color:colors.camera,points:(track?.points||[]).map(t=>({t:t.t,v:t.score}))}]:
      ['camera','radar','output'].map(kind=>({color:colors[kind],points:(track?.points||[]).map(t=>({t:t.t,v:value(t[kind])}))}));
    plot('fusion-history',series,now,metric==='score'?{low:0,high:100}:{});
    get('fusion-selection-summary').textContent=row?`${row.label} · ${row.state} · generation ${row.generation} · frame ${row.frame}. Sensor reference coordinates; no measured body height or facing.`:'No fresh contact selected. Historical health data remains below.';
    if(get('fusion-inspector').parentElement.open)get('fusion-inspector').textContent=row?JSON.stringify({observation:row.raw,output_to_quest:row.output?.raw||null},null,2):'No contact selected.';
    if(get('fusion-packet').parentElement.open)get('fusion-packet').textContent=JSON.stringify(exportSnapshot(),null,2);
    get('fusion-events').replaceChildren(...events.map(e=>element('li',e)));
    get('fusion-timing').textContent=`HTTP round trip ${fmt(requestMs,0)} ms · last relay receipt ${fmt(latest.received_age_ms==null?null:latest.received_age_ms+now-received,0)} ms ago · camera frame ${p.camera_frame_id??'—'} / radar frame ${p.radar?.frame_id??'—'}. Sensor-to-laptop transit is not clock-compensated.`;
    plot('fusion-health',['camera','radar'].map(kind=>({color:colors[kind],points:health.map(v=>({t:v.t,v:v[kind]}))})),now,
      {low:0,high:1200,digits:0,limits:[{v:750,color:colors.camera},{v:500,color:colors.radar}]});
    map(m,now);
  }
  async function update() {
    const start=performance.now();
    try {
      const response=await fetch('/diagnostics.json',{cache:'no-store',signal:AbortSignal.timeout(2000)});
      if(!response.ok)throw Error(`Diagnostics unavailable (HTTP ${response.status})`);
      const data=await response.json(),p=data.packet,f=p.spatial_people;
      const nextIdentity=`${p.relay_session_id}/${f.source_session_id}/${f.reference_id}`;
      if(identity && identity!==nextIdentity){histories.clear();health=[];previous.clear();events=[];selected='';event('Source or reference changed · history cleared');}
      identity=nextIdentity;latest=data;received=performance.now();requestMs=received-start;failure='';
      if(!settings){settings=data.config;get('fusion-right').value=settings.camera_offset_right_m;get('fusion-forward').value=settings.camera_offset_forward_m;get('fusion-confirmed').checked=settings.alignment_confirmed;
        get('fusion-rig-mode').value=settings.rig_motion_mode||'stationary';get('fusion-height').value=settings.camera_height_m??'';get('fusion-pitch').value=settings.camera_pitch_deg||0;}
      const m=model(received);health.push({t:received,camera:m.cameraAge,radar:m.radarAge});health=health.filter(v=>received-v.t<=30000).slice(-300);
      render();
    } catch(error){if(!failure)event('Diagnostics connection interrupted');failure=error.message;render();}
    finally{setTimeout(update,200);}
  }
  get('fusion-select').onchange=e=>choose(e.target.value);
  get('fusion-range').onchange=render;get('fusion-metric').onchange=render;
  get('fusion-map').onclick=e=>{const c=e.currentTarget,b=c.getBoundingClientRect(),x=(e.clientX-b.left)*c.width/b.width,y=(e.clientY-b.top)*c.height/b.height;
    const near=hits.map(h=>({...h,d:Math.hypot(x-h.x,y-h.y)})).sort((a,b)=>a.d-b.d)[0];if(near&&near.d<40)choose(near.key);};
  for(const id of ['fusion-inspector','fusion-packet'])get(id).parentElement.addEventListener('toggle',render);
  get('fusion-download').onclick=()=>{if(!latest)return;const url=URL.createObjectURL(new Blob([JSON.stringify(exportSnapshot(),null,2)],{type:'application/json'}));
    const a=element('a','');a.href=url;a.download='sensor-diagnostics.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);};
  get('fusion-form').onsubmit=async e=>{
    e.preventDefault();
    try{if(!settings)throw Error('Wait for the relay configuration first');const value={...settings,camera_offset_right_m:Number(get('fusion-right').value),camera_offset_forward_m:Number(get('fusion-forward').value),alignment_confirmed:get('fusion-confirmed').checked,
      rig_motion_mode:get('fusion-rig-mode').value,camera_height_m:get('fusion-height').value===''?null:Number(get('fusion-height').value),camera_pitch_deg:Number(get('fusion-pitch').value)};
      const response=await fetch('/fusion/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(value)}),result=await response.json();
      if(!response.ok)throw Error(result.error||'Unable to save alignment');settings=result;get('fusion-save').textContent='Saved. If offsets changed, place the sensor reference again in Quest.';
    }catch(error){get('fusion-save').textContent=error.message;}
  };
  setInterval(render,100);update();
})();
