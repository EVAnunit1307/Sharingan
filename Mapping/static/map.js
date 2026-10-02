'use strict';
const $ = id => document.getElementById(id);
let busy = false, remoteSessions = [], localSessions = [], activeId = null, jobBusy = false, learnedReady = false, depthReady = false, loadedRevision = null, completedJob = null, sessionsSignature = null;
let trackingReady=false;
async function api(path, method = 'GET', payload = {}) {
  const response = await fetch(path, {method, headers: method === 'POST' ? {'Content-Type':'application/json'} : {}, body: method === 'POST' ? JSON.stringify(payload) : undefined, signal: AbortSignal.timeout(10000)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}
function notice(message) { $('notice').textContent = message; $('notice').hidden = !message; }
async function action(name) {
  if (busy) return;
  busy = true; $('start').disabled = $('stop').disabled = true; notice('');
  try { await api('/map-api/pi/' + name, 'POST'); } catch (error) { notice(error.message); }
  finally { busy = false; await refresh(); }
}
$('start').onclick = () => action('start'); $('stop').onclick = () => action('stop');
async function build(id, backend = 'standard') {
  try { await api('/map-api/build/' + encodeURIComponent(id), 'POST', {backend}); notice(''); await refresh(); }
  catch (error) { notice(error.message); }
}
function renderSessions() {
  const merged = new Map(remoteSessions.map(s => [s.session_id, {...s, onPi:true}]));
  for (const s of localSessions) merged.set(s.session_id, {...merged.get(s.session_id), ...s, onLaptop:true});
  const signature=JSON.stringify([[...merged.values()],activeId,jobBusy,learnedReady,depthReady,trackingReady]);
  if(signature===sessionsSignature)return;
  sessionsSignature=signature;
  const area = $('sessions'); area.replaceChildren();
  if (!merged.size) { const p=document.createElement('p'); p.className='muted'; p.textContent='No recordings yet. Start a walk when the camera is live.'; area.append(p); return; }
  for (const s of [...merged.values()].sort((a,b)=>b.session_id.localeCompare(a.session_id))) {
    const row=document.createElement('div'); row.className='session';
    const description=document.createElement('div'), title=document.createElement('strong'), detail=document.createElement('small');
    title.textContent=(s.display_label?s.display_label+' · ':s.source==='synthetic_test_only'?'SYNTHETIC TEST · ':'')+s.session_id;
    detail.textContent=`${s.frames_saved || 0} images · ${s.status} · ${s.onLaptop ? 'saved on laptop' : 'saved on Pi'}${s.dropped_frames ? ` · ${s.dropped_frames} dropped` : ''}`;
    description.append(title,detail); const buttons=document.createElement('div'); buttons.className='session-buttons';
    const reconstruct=document.createElement('button'); reconstruct.className='secondary'; reconstruct.textContent=s.has_model?'Rebuild':'Build 3D map';
    reconstruct.disabled=jobBusy || s.session_id===activeId || !['complete','interrupted'].includes(s.status) || s.frames_saved<12;
    reconstruct.onclick=()=>build(s.session_id); buttons.append(reconstruct);
    if(learnedReady){const stronger=document.createElement('button');stronger.className='secondary';stronger.textContent='Experimental matching';stronger.title='Learned matching with a second build to check stability. May recover more views; shape still needs validation.';stronger.disabled=reconstruct.disabled||s.frames_saved>400;stronger.onclick=()=>build(s.session_id,'xfeat');buttons.append(stronger);}
    if(depthReady&&s.has_model){const depth=document.createElement('button');depth.className='secondary';depth.textContent=s.has_depth?'Rebuild AI surfaces':'Add AI surfaces';depth.title='Experimental depth estimates aligned and checked across views. No measured distances or temperature.';depth.disabled=jobBusy||s.session_id===activeId;depth.onclick=()=>build(s.session_id,'depth');buttons.append(depth);}
    if(s.has_model){const view=document.createElement('button');view.textContent='View map';view.onclick=()=>load(s.session_id);buttons.append(view);}
    if(trackingReady&&s.has_calibration){const track=document.createElement('button');track.className='secondary';track.textContent='Test continuous tracking';track.disabled=reconstruct.disabled;track.onclick=()=>build(s.session_id,'orb');buttons.append(track);}
    if(s.has_tracking){const replay=document.createElement('a');replay.textContent='Replay tracking →';replay.href='/map-assets/tracking.html?session='+encodeURIComponent(s.session_id);buttons.append(replay);}
    if(s.has_partial){const partial=document.createElement('a');partial.textContent='View coarse partial scan →';partial.href='/map-assets/tracking.html?source=partial&session='+encodeURIComponent(s.session_id);buttons.append(partial);}
    row.append(description,buttons);area.append(row);
  }
}
let refreshing=false;
let cameraLive=false, previewActive=false, previewGeneration=0;
let previewRequest=null, previewTimer=null, previewExpiryTimer=null, previewUrl=null;
const PREVIEW_MAX_AGE_MS=1000, PREVIEW_INTERVAL_MS=100;
function hidePreview(message){
  $('feed').hidden=true;$('feed').removeAttribute('src');delete $('feed').dataset.frameId;
  if(previewUrl){URL.revokeObjectURL(previewUrl);previewUrl=null;}
  $('camera-empty').hidden=false;$('camera-empty').querySelector('p').textContent=message;
}
function stopPreview(){
  previewActive=false;previewGeneration++;
  clearTimeout(previewTimer);clearTimeout(previewExpiryTimer);
  if(previewRequest)previewRequest.abort();
  hidePreview(document.hidden&&cameraLive?'Preview paused while this page is in the background':'Waiting for the drone camera');
}
async function updatePreview(){
  if(!previewActive||!cameraLive||document.hidden||previewRequest)return;
  const generation=previewGeneration, started=performance.now(), controller=new AbortController();
  previewRequest=controller;
  const timeout=setTimeout(()=>controller.abort(),1500);
  let candidateUrl=null;
  try{
    const response=await fetch('/map-api/pi/preview.jpg?t='+Date.now(),{cache:'no-store',signal:controller.signal});
    if(!response.ok||!response.headers.get('Content-Type')?.startsWith('image/jpeg'))throw new Error('Preview unavailable');
    const ageHeader=response.headers.get('X-Frame-Age-Ms'),frameId=response.headers.get('X-Frame-Id');
    const sourceAge=Number(ageHeader);
    if(ageHeader===null||!Number.isFinite(sourceAge)||sourceAge<0||!frameId)throw new Error('Missing preview timing');
    const blob=await response.blob();
    if(!blob.size||blob.size>8*1024*1024)throw new Error('Invalid preview size');
    const bytes=new Uint8Array(await blob.arrayBuffer());
    if(bytes[0]!==255||bytes[1]!==216||bytes.at(-2)!==255||bytes.at(-1)!==217)throw new Error('Incomplete preview JPEG');
    // Download and decode away from the displayed image. An incomplete stream
    // must never replace the last complete frame with a partially drawn JPEG.
    candidateUrl=URL.createObjectURL(blob);
    const decoded=new Image();decoded.src=candidateUrl;await decoded.decode();
    if(generation!==previewGeneration||!previewActive||!cameraLive||document.hidden)return;
    // Include the whole request/decode duration as a conservative age bound;
    // receiving a delayed response must not make an old frame appear fresh.
    const age=sourceAge+performance.now()-started;
    if(age>=PREVIEW_MAX_AGE_MS)throw new Error('Preview arrived too late');
    const previous=previewUrl;previewUrl=candidateUrl;candidateUrl=null;
    $('feed').src=previewUrl;$('feed').dataset.frameId=frameId;
    $('feed').hidden=false;$('camera-empty').hidden=true;
    if(previous)URL.revokeObjectURL(previous);
    clearTimeout(previewExpiryTimer);
    previewExpiryTimer=setTimeout(()=>{
      if(generation===previewGeneration)hidePreview('Preview delayed — waiting for a fresh frame');
    },PREVIEW_MAX_AGE_MS-age);
  }catch(error){
    if(generation===previewGeneration&&previewActive){
      clearTimeout(previewExpiryTimer);hidePreview('Preview delayed — waiting for a fresh frame');
    }
  }finally{
    clearTimeout(timeout);if(candidateUrl)URL.revokeObjectURL(candidateUrl);
    if(previewRequest===controller)previewRequest=null;
    // Only one request is in flight, including across tab hide/resume cycles.
    if(previewActive&&cameraLive&&!document.hidden){
      clearTimeout(previewTimer);
      previewTimer=setTimeout(updatePreview,Math.max(0,PREVIEW_INTERVAL_MS-(performance.now()-started)));
    }
  }
}
function startPreview(){
  if(document.hidden){hidePreview('Preview paused while this page is in the background');return;}
  if(!cameraLive||previewActive)return;
  previewActive=true;previewGeneration++;updatePreview();
}
document.addEventListener('visibilitychange',()=>{if(document.hidden)stopPreview();else startPreview();});
async function refresh() {
  if(refreshing)return;refreshing=true;
  try {
    const results=await Promise.allSettled([api('/map-api/pi/status'),api('/map-api/pi/sessions'),api('/map-api/local')]);
    const status=results[0];
    if(status.status==='fulfilled' && status.value.enabled){
      const s=status.value;activeId=['recording','saving'].includes(s.state)?s.session_id:null;
      cameraLive=s.camera_live;
      $('connection').textContent=s.camera_live?'Camera live':'Camera unavailable';$('connection').classList.toggle('live',s.camera_live);
      $('frames').textContent=s.frames_saved;$('elapsed').textContent=s.elapsed_seconds;$('quality').textContent=s.state;
      $('start').disabled=busy||!s.camera_live||!!activeId||s.settings_settling;$('stop').disabled=busy||!activeId;
      $('apply-capture').disabled=busy||!s.camera_live||!!activeId||!s.frame_selection;
      $('capture-fps').disabled=$('apply-capture').disabled||!s.sample_fps_max;
      $('capture-fps-note').textContent=s.sample_fps_max?`Currently ${s.sample_fps} saved fps requested; choose a rate and Apply. Actual throughput is checked after recording.`:'Recording-rate control requires the updated Pi camera software.';
      const exposure=s.camera_metadata?.ExposureTime,gain=s.camera_metadata?.AnalogueGain;
      $('capture-detail').textContent=s.error||`${s.sample_fps} images/sec · stops after ${s.max_seconds} sec${s.dropped_frames?` · ${s.dropped_frames} frames dropped`:''}${exposure?` · ${(exposure/1000).toFixed(1)} ms exposure · ${Number(gain).toFixed(1)}× gain`:''}${s.frame_selection==='sharpest'?' · sharper frame selection':''}${s.settings_settling?' · settling…':''}`;
      if(cameraLive){startPreview();}else{stopPreview();}
    }else{
      activeId=null;$('connection').textContent='Pi offline';$('connection').classList.remove('live');$('start').disabled=$('stop').disabled=true;
      cameraLive=false;stopPreview();$('quality').textContent='offline';$('frames').textContent=$('elapsed').textContent='—';
      $('apply-capture').disabled=true;
      $('capture-fps').disabled=true;
      $('capture-fps-note').textContent='Connect the Pi to apply a recording rate.';
      $('capture-detail').textContent=status.status==='rejected'?status.reason.message:(status.value.error||'Enable mapping on the Pi');
    }
    if(results[1].status==='fulfilled')remoteSessions=results[1].value.sessions;else remoteSessions=[];
    if(results[2].status==='fulfilled'){
      const s=results[2].value;localSessions=s.sessions;learnedReady=!!s.learned_backend_ready;depthReady=!!s.depth_backend_ready;jobBusy=['downloading','running'].includes(s.job.state);
      trackingReady=!!s.tracking_backend_ready;
      $('backend').textContent=s.backend_ready?'CPU mapper ready':'Mapper not installed';$('backend').classList.toggle('live',s.backend_ready);$('storage').textContent=s.storage;
      $('job').hidden=s.job.state==='idle';$('job').textContent=s.job.phase||'';$('job').classList.toggle('failed',s.job.state==='failed');
      const jobKey=JSON.stringify([s.job.session_id,s.job.revision,s.job.backend]);
      if(s.job.state==='complete' && jobKey!==completedJob){
        if(s.job.kind==='tracking')notice('Tracking replay is ready. Choose Replay tracking on the recording below.');
        else await load(s.job.session_id,s.job.kind==='depth');completedJob=jobKey;}
    }
    renderSessions();
  }finally{refreshing=false;}
}

// Small dependency-free 3D point viewer. The full-resolution cloud remains in the PLY.
const canvas=$('scene'),ctx=canvas.getContext('2d');
let points=[],inferred=[],cameras=[],livePose=null,loadedId=null,poseRunning=false,center=[0,0,0],radius=1,yaw=.5,pitch=-.35,zoom=1,drag=null;
function fit(){yaw=.5;pitch=-.35;zoom=1;draw();}
function project(p){const x=(p[0]-center[0])/radius,y=(p[1]-center[1])/radius,z=(p[2]-center[2])/radius;
  const a=x*Math.cos(yaw)+z*Math.sin(yaw),b=-x*Math.sin(yaw)+z*Math.cos(yaw);
  const c=y*Math.cos(pitch)-b*Math.sin(pitch),d=y*Math.sin(pitch)+b*Math.cos(pitch);
  const perspective=3.5/(3.5+d);return [canvas.width/2+a*perspective*Math.min(canvas.width,canvas.height)*.35*zoom,canvas.height/2+c*perspective*Math.min(canvas.width,canvas.height)*.35*zoom,d];}
function draw(){const rect=canvas.getBoundingClientRect(),ratio=Math.min(window.devicePixelRatio||1,2);canvas.width=Math.round(rect.width*ratio);canvas.height=Math.round(rect.height*ratio);ctx.clearRect(0,0,canvas.width,canvas.height);
  if(!points.length)return;
  const shown=$('layer-select').value==='inferred'?inferred:$('layer-select').value==='both'?[...inferred,...points]:points;
  const sorted=shown.map(p=>[...project(p),p]).sort((a,b)=>b[2]-a[2]);
  for(const [x,y,z,p] of sorted){if(z<=-3.4)continue;ctx.fillStyle=`rgb(${p[3]},${p[4]},${p[5]})`;ctx.fillRect(x,y,2*ratio,2*ratio);}
  ctx.strokeStyle='#c1ee83';ctx.lineWidth=1.3*ratio;ctx.beginPath();cameras.forEach((c,i)=>{const [x,y]=project(c.position);if(i)ctx.lineTo(x,y);else ctx.moveTo(x,y);});ctx.stroke();
  cameras.forEach(c=>{const [x,y]=project(c.position);ctx.fillStyle='#e4ffbd';ctx.beginPath();ctx.arc(x,y,2.3*ratio,0,2*Math.PI);ctx.fill();});
  if(livePose){const [x,y,z]=project(livePose);if(z>-3.4){ctx.strokeStyle='#ffc56f';ctx.lineWidth=2*ratio;ctx.beginPath();ctx.arc(x,y,8*ratio,0,Math.PI*2);ctx.stroke();ctx.fillStyle='#ffc56f';ctx.font=`${10*ratio}px system-ui`;ctx.fillText('CAMERA ESTIMATE',x+12*ratio,y-10*ratio);}}
}
let loadSequence=0;
async function load(id,showInferred=false){const sequence=++loadSequence;try{const scene=await api('/map-api/model/'+encodeURIComponent(id));
  if(sequence!==loadSequence)return;
  points=scene.points;cameras=scene.cameras;loadedRevision=scene.revision;loadedId=id;livePose=null;
  inferred=[];$('layer-select').value='observed';$('layer-select').disabled=true;$('depth-detail').textContent='AI surfaces are optional estimates; use Add AI surfaces on this recording.';
  try{const layer=await api('/map-api/depth/'+encodeURIComponent(id));if(sequence!==loadSequence)return;
    if(layer.source_revision===scene.revision){inferred=layer.points;$('layer-select').disabled=!inferred.length;
      $('depth-detail').textContent=`AI-INFERRED · ${layer.points_total.toLocaleString()} surface points from ${layer.aligned_frames} aligned views. Checked against reconstructed geometry; physical accuracy unverified. Colours come from the camera, not temperature.`;
      if(showInferred&&inferred.length)$('layer-select').value='inferred';}
  }catch(error){if(sequence!==loadSequence)return;}
  const bounds=[...points,...cameras.map(c=>c.position)];
  if(!bounds.length)throw new Error('This reconstruction has no geometry');
  // Percentile bounds keep distant triangulation outliers from shrinking the whole view.
  center=[0,1,2].map(axis=>{const values=bounds.map(p=>p[axis]).sort((a,b)=>a-b);return (values[Math.floor(values.length*.02)]+values[Math.min(values.length-1,Math.floor(values.length*.98))])/2;});
  const distances=bounds.map(p=>Math.hypot(...p.slice(0,3).map((v,i)=>v-center[i]))).sort((a,b)=>a-b);radius=Math.max(.001,distances[Math.floor(distances.length*.95)]);
  $('model-empty').hidden=true;$('model-title').textContent=(scene.source==='synthetic_test_only'?'SYNTHETIC SOFTWARE TEST · ':scene.experimental?'EXPERIMENTAL XFEAT · ':scene.coverage==='partial'?'PARTIAL RECONSTRUCTION · ':'')+id;
  $('model-detail').textContent=`${scene.registered_images}/${scene.input_images} cameras · ${scene.points_total.toLocaleString()} points · ${scene.components} connected component(s); largest usable component shown${scene.lens_calibration?' · Checkerboard lens estimate applied':''}${scene.coverage==='partial'?' · Much of the walk is missing.':''}`;
  if(scene.quality_warning)$('model-detail').textContent+=' · '+scene.quality_warning;
  fit();
}catch(error){notice(error.message);}}
canvas.onpointerdown=e=>{drag=[e.clientX,e.clientY];canvas.setPointerCapture(e.pointerId);};
canvas.onpointermove=e=>{if(!drag)return;yaw+=(e.clientX-drag[0])*.007;pitch=Math.max(-1.55,Math.min(1.55,pitch+(e.clientY-drag[1])*.007));drag=[e.clientX,e.clientY];draw();};
canvas.onpointerup=canvas.onpointercancel=()=>{drag=null;};canvas.addEventListener('wheel',e=>{e.preventDefault();zoom=Math.max(.2,Math.min(8,zoom*Math.exp(-e.deltaY*.001)));draw();},{passive:false});
$('fit').onclick=fit;$('top').onclick=()=>{pitch=-1.5;draw();};new ResizeObserver(draw).observe(canvas);
$('layer-select').onchange=draw;
$('apply-capture').onclick=async()=>{const selected=$('capture-profile').value;
  const options={selection:['standard','lit20'].includes(selected)?'uniform':'sharpest',exposure_us:selected==='motion10'?10000:['motion20','lit20'].includes(selected)?20000:null,gain:selected==='lit20'?4:8};
  if(!$('capture-fps').disabled)options.fps=Number($('capture-fps').value);
  try{const result=await api('/map-api/pi/configure','POST',options);notice(`Capture settings applied: ${result.sample_fps} saved fps requested. Check brightness before recording.`);await refresh();}catch(error){notice(error.message);}
};
let depthLiveRunning=false,depthLiveBusy=false,depthImageLoading=false,depthExpiryTimer=null;
async function refreshDepth(){
  if(depthLiveBusy)return;
  depthLiveBusy=true;
  try{const s=await api('/map-api/live-depth/status');depthLiveRunning=s.state!=='stopped';
    $('depth-live-toggle').disabled=!s.available;$('depth-live-toggle').textContent=depthLiveRunning?'Stop depth preview':'Start depth preview';
    $('depth-live-status').textContent=s.fresh?`AI ESTIMATE · ${s.inference_ms} ms inference · ${s.fetch_infer_render_ms} ms fetch + inference + render · up to ${s.rate_limit_fps} fps. No measured distance or temperature.`:s.phase||'Optional preview. This does not track the drone or update the saved map.';
    clearTimeout(depthExpiryTimer);
    if(s.fresh&&!document.hidden){if(!depthImageLoading){depthImageLoading=true;$('depth-live-image').src='/map-api/live-depth/preview.jpg?t='+Date.now();}$('depth-live-image').hidden=false;
      depthExpiryTimer=setTimeout(()=>{$('depth-live-image').hidden=true;$('depth-live-status').textContent='Depth preview stale — image hidden';},Math.max(0,(3-s.result_age_seconds)*1000));}
    else{$('depth-live-image').hidden=true;}
  }catch(error){$('depth-live-image').hidden=true;$('depth-live-status').textContent=error.message;}
  finally{depthLiveBusy=false;}
}
$('depth-live-image').onload=()=>{depthImageLoading=false;};$('depth-live-image').onerror=()=>{depthImageLoading=false;$('depth-live-image').hidden=true;};
$('depth-live-toggle').onclick=async()=>{try{await api('/map-api/live-depth/'+(depthLiveRunning?'stop':'start'),'POST');await refreshDepth();}catch(error){notice(error.message);}};
refreshDepth();setInterval(refreshDepth,500);
let posePolling=false,poseExpiryTimer=null;
async function refreshPose(){if(posePolling)return;posePolling=true;try{
  const requestedAt=Date.now();
  const s=await api('/map-api/live-pose/status');poseRunning=s.state!=='stopped';
  $('pose-toggle').disabled=!s.available||!loadedId;$('pose-toggle').textContent=poseRunning?'Stop camera localization':'Locate camera in this map';
  const sameMap=s.session_id===loadedId&&s.source_revision===loadedRevision;
  const previous=JSON.stringify(livePose);livePose=s.fresh&&sameMap?s.position:null;
  clearTimeout(poseExpiryTimer);
  if(livePose)poseExpiryTimer=setTimeout(()=>{livePose=null;draw();$('pose-status').textContent='Tracking stale — position hidden';},Math.max(0,(.75-s.age_seconds)*1000-(Date.now()-requestedAt)));
  $('pose-status').textContent=livePose?`CAMERA ESTIMATE · ${s.inliers} supporting matches · ${s.processing_ms} ms request + processing. Position is uncalibrated; no flight control.`:poseRunning?(sameMap||!s.source_revision?(s.reason||'Tracking lost or stale — position hidden'):'Localization is running in a different map. Stop and restart for this map.'):'Experimental localization in a saved map. Point the camera toward the scanned area.';
  if(JSON.stringify(livePose)!==previous)draw();
}catch(error){livePose=null;draw();$('pose-status').textContent='Localization unavailable — position hidden';}finally{posePolling=false;}}
$('pose-toggle').onclick=async()=>{try{await api('/map-api/live-pose/'+(poseRunning?'stop':'start'),'POST',{session_id:loadedId});await refreshPose();}catch(error){notice(error.message);}};
refreshPose();setInterval(refreshPose,350);
refresh();setInterval(refresh,1800);
