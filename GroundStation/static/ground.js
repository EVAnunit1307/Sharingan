(() => {
  'use strict';
  const get = id => document.getElementById(id);
  let settings = null, last = 0;
  function clear() { get('fusion-people').replaceChildren(); }
  async function update() {
    try {
      const response = await fetch('/fusion.json', {cache: 'no-store', signal: AbortSignal.timeout(1000)});
      if (!response.ok) throw Error('Ground station unavailable');
      const data = await response.json(), frame = data.spatial_people;
      last = performance.now();
      if (!settings) {
        settings = data.config;
        get('fusion-right').value = settings.camera_offset_right_m;
        get('fusion-forward').value = settings.camera_offset_forward_m;
        get('fusion-confirmed').checked = settings.alignment_confirmed;
      }
      get('fusion-mode').textContent = data.is_replay ? 'RECORDED REPLAY' : 'LIVE INPUT';
      get('fusion-status').textContent = `${frame.status} · matching ${frame.alignment_confirmed ? 'enabled' : 'awaiting alignment check'} · ${data.clients} connected clients · ${frame.unmatched_people} unmatched people · ${frame.radar_targets.length} unmatched radar · ${frame.unpositioned_people} without range${data.error ? ' · ' + data.error : ''}`;
      clear();
      for (const person of frame.people) {
        const matched = person.position_source === 'radar_matched';
        const row = document.createElement('div');
        row.className = matched ? 'person green' : 'person amber';
        row.textContent = `C${person.camera_id} · ${matched ? 'RADAR R' + person.radar_id : 'ESTIMATED'} · ${person.right_m.toFixed(2)} m right / ${person.forward_m.toFixed(2)} m forward`;
        get('fusion-people').append(row);
      }
    } catch (error) { clear(); get('fusion-status').textContent = error.message; }
    finally { setTimeout(update, 100); }
  }
  get('fusion-form').onsubmit = async event => {
    event.preventDefault();
    try {
      const value = {...settings, camera_offset_right_m: Number(get('fusion-right').value),
        camera_offset_forward_m: Number(get('fusion-forward').value), alignment_confirmed: get('fusion-confirmed').checked};
      const response = await fetch('/fusion/config', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(value)});
      const result = await response.json();
      if (!response.ok) throw Error(result.error || 'Unable to save alignment');
      settings = result;
      get('fusion-save').textContent = 'Saved. If offsets changed, place the sensor reference again in Quest.';
    } catch (error) { get('fusion-save').textContent = error.message; }
  };
  setInterval(() => { if (last && performance.now() - last > 750) clear(); }, 100);
  update();
})();
