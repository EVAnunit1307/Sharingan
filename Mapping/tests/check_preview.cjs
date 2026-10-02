// Browser regression for complete-frame preview, expiry and cancellation.
// Run with Playwright installed: node Mapping/tests/check_preview.cjs
// PLAYWRIGHT_MODULE and CHROME_BIN can select an existing local installation.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

async function main() {
  const browser = await chromium.launch({headless:true,
    ...(process.env.CHROME_BIN ? {executablePath:process.env.CHROME_BIN} : {})});
  let server;
  try {
    const fixture = await browser.newPage();
    const encoded = await fixture.evaluate(() => {
      const canvas = document.createElement('canvas'); canvas.width=64; canvas.height=48;
      const ctx=canvas.getContext('2d'); ctx.fillStyle='#20a060'; ctx.fillRect(0,0,64,24);
      ctx.fillStyle='#3060e0'; ctx.fillRect(0,24,64,24);
      return canvas.toDataURL('image/jpeg').split(',')[1];
    });
    await fixture.close();
    const jpeg=Buffer.from(encoded,'base64'), pending=new Set(), errors=[];
    let mode='hold', requests=0, active=0, maximumActive=0;
    const assets=new Map([
      ['/map','index.html'], ['/map-assets/map.js','map.js'], ['/map-assets/map.css','map.css'],
    ]);
    server=http.createServer((req,res) => {
      const url=new URL(req.url,'http://localhost');
      res.setHeader('Cache-Control','no-store');
      if(assets.has(url.pathname)) {
        const file=assets.get(url.pathname);
        res.setHeader('Content-Type',file.endsWith('.js')?'text/javascript':file.endsWith('.css')?'text/css':'text/html');
        return res.end(fs.readFileSync(path.join(__dirname,'../static',file)));
      }
      if(url.pathname==='/map-api/pi/preview.jpg') {
        requests++; active++; maximumActive=Math.max(maximumActive,active);
        res.once('close',()=>{active--;pending.delete(res);});
        res.setHeader('Content-Type','image/jpeg');
        res.setHeader('X-Frame-Id',String(requests));
        res.setHeader('X-Frame-Age-Ms',mode==='stale'?'1500':'5');
        const body=mode==='truncated'?jpeg.subarray(0,jpeg.length-2):jpeg;
        res.setHeader('Content-Length',body.length);
        if(mode==='hold') {res.write(body.subarray(0,body.length>>1)); pending.add(res);}
        else res.end(body);
        return;
      }
      res.setHeader('Content-Type','application/json');
      if(url.pathname==='/map-api/pi/status') return res.end(JSON.stringify({enabled:true,camera_live:true,
        state:'ready',frames_saved:0,elapsed_seconds:0,max_seconds:120,sample_fps:12,sample_fps_max:24,
        frame_selection:'uniform',camera_metadata:{ExposureTime:20000,AnalogueGain:8}}));
      if(url.pathname==='/map-api/pi/sessions') return res.end('{"sessions":[]}');
      if(url.pathname==='/map-api/local') return res.end('{"sessions":[],"job":{"state":"idle"},"storage":"test","backend_ready":true}');
      if(url.pathname.includes('/live-')) return res.end('{"state":"stopped","available":false,"fresh":false}');
      res.statusCode=404; res.end('{"error":"Not found"}');
    });
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    const page=await browser.newPage(); page.on('pageerror',e=>errors.push(e.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/map`,{waitUntil:'domcontentloaded'});
    await page.waitForFunction(()=>document.querySelector('#connection').textContent==='Camera live');
    await page.waitForTimeout(200);
    assert.equal(requests,1,'One request while a frame is incomplete');
    assert(await page.locator('#feed').isHidden(),'Do not show a partial download');
    mode='good'; for(const res of pending) res.end(jpeg.subarray(jpeg.length>>1));
    await page.waitForFunction(()=>{const e=document.querySelector('#feed');return !e.hidden&&e.complete&&e.naturalWidth===64;});
    const pixel=await page.locator('#feed').evaluate(image=>{
      const c=document.createElement('canvas');c.width=64;c.height=48;
      const ctx=c.getContext('2d');ctx.drawImage(image,0,0);return [...ctx.getImageData(32,40,1,1).data];
    });
    assert(pixel[2]>150 && pixel[0]<80,'The bottom of the frame must be decoded');
    mode='hold';
    await page.waitForFunction(()=>document.querySelector('#feed').hidden,{},{timeout:2500});
    assert.match(await page.locator('#camera-empty p').textContent(),/delayed/);
    await page.evaluate(()=>{
      Object.defineProperty(document,'hidden',{configurable:true,get:()=>true});
      document.dispatchEvent(new Event('visibilitychange'));
    });
    await page.waitForTimeout(100); const count=requests;
    for(const res of pending) if(!res.destroyed) res.end(jpeg.subarray(jpeg.length>>1));
    await page.waitForTimeout(250);
    assert.equal(requests,count,'No background preview requests');
    assert(await page.locator('#feed').isHidden(),'A late response cannot reveal a paused preview');
    mode='good';
    await page.evaluate(()=>{Object.defineProperty(document,'hidden',{configurable:true,get:()=>false});document.dispatchEvent(new Event('visibilitychange'));});
    await page.waitForFunction(()=>!document.querySelector('#feed').hidden);
    for(const bad of ['stale','truncated']) {
      mode=bad;
      await page.waitForFunction(()=>document.querySelector('#feed').hidden);
      mode='good';
      await page.waitForFunction(()=>{const e=document.querySelector('#feed');return !e.hidden&&e.complete&&e.naturalHeight===48;});
    }
    assert.equal(maximumActive,1,'Never queue concurrent frame downloads');
    assert.deepEqual(errors,[]);
    await page.close();
    console.log('PASS: partial download, full frame, expiry, hidden tab, late response, stale metadata, truncated JPEG and recovery');
  } finally {
    await browser.close();
    if(server) {server.closeAllConnections();await new Promise(resolve=>server.close(resolve));}
  }
}
main().catch(error=>{console.error(error);process.exitCode=1;});
