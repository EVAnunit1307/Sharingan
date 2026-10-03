// node check_scene_relay.cjs /viewer.html /new/render-directory
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {pathToFileURL}=require('node:url');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
(async()=>{
 const [viewer,output]=process.argv.slice(2);if(output)fs.mkdirSync(output);
 const browser=await chromium.launch({headless:true,...(process.env.CHROME_BIN?{executablePath:process.env.CHROME_BIN}:{})});
 try{
  const page=await browser.newPage({viewport:{width:1440,height:1160}}),errors=[],external=[];
  page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(/^https?:/.test(r.url()))external.push(r.url());});
  await page.goto(pathToFileURL(path.resolve(viewer)).href);
  assert.equal(await page.locator('#profile option').count(),3);
  await page.evaluate(async()=>{for(const src of DATA.source.images){const im=new Image();im.src=src;await im.decode();if(!im.naturalWidth)throw Error('Missing image');}});
  for(let i=0;i<3;i++){
   await page.selectOption('#profile',String(i));
   assert.equal(await page.locator('#revision').textContent(),'3 / 3');
   assert.equal(await page.locator('#poses').textContent(),'40');
   assert.equal(await page.locator('#camera-status').textContent(),'Withheld');
   await page.locator('#fit').click();await page.waitForTimeout(100);
   if(output)await page.screenshot({path:path.join(output,['clear','limited','interrupted'][i]+'.png'),fullPage:true});
   await page.locator('#time').fill('0');
   assert.equal(await page.locator('#revision').textContent(),'0 / 3');
   assert(await page.locator('#image').isHidden());assert(await page.locator('#empty').isVisible());
   // Check every display sample, including non-regression and camera withholding.
   await page.evaluate(()=>{let prior=0;for(let i=0;i<result.timeline.length;i++){tick=i;frame();if(scene&&scene.revision<prior)throw Error('Revision regressed');prior=scene?.revision||0;if(document.querySelector('#camera-status').textContent!=='Withheld')throw Error('Stale pose shown current');}});
  }
  await page.locator('#outage').click();
  assert.equal(await page.locator('#profile').inputValue(),'2');
  assert.equal(await page.locator('#revision').textContent(),'1 / 3');
  assert.match(await page.locator('#link').textContent(),/NO RECENT/);
  assert.match(await page.locator('#explanation').textContent(),/last complete map stays/);
  assert(await page.locator('#image').isVisible());await page.waitForTimeout(100);
  if(output)await page.screenshot({path:path.join(output,'outage.png'),fullPage:true});
  await page.locator('#time').fill('338');await page.locator('#play').click();
  await page.waitForFunction(()=>document.querySelector('#time').value==='340'&&document.querySelector('#play').textContent==='Play replay');
  await page.setViewportSize({width:390,height:844});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  if(output)await page.screenshot({path:path.join(output,'mobile.png'),fullPage:true});
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  console.log('PASS: 3 profiles, every timeline sample, causal map display, preserved map on outage, stale camera withheld, source images, playback, mobile and offline');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
