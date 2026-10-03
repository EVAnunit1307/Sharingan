// node check_selection_replay.cjs /viewer.html /new/render-directory
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {pathToFileURL}=require('node:url');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
(async()=>{
 const [viewer,output]=process.argv.slice(2);if(output)fs.mkdirSync(output);
 const browser=await chromium.launch({headless:true,...(process.env.CHROME_BIN?{executablePath:process.env.CHROME_BIN}:{})});
 try{
  const page=await browser.newPage({viewport:{width:1440,height:1180}}),errors=[],external=[];
  page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(/^https?:/.test(r.url()))external.push(r.url());});
  await page.goto(pathToFileURL(path.resolve(viewer)).href);
  assert.equal(await page.locator('#trial option').count(),3);
  await page.evaluate(async()=>{for(const t of DATA.trials)for(const src of t.images){const im=new Image();im.src=src;await im.decode();if(im.naturalWidth!==504)throw Error('Unexpected source image width');}});
  for(let i=0;i<3;i++){
   await page.selectOption('#trial',String(i));
   const t=await page.evaluate(i=>({name:DATA.trials[i].name,accepted:DATA.trials[i].accepted_windows,poses:DATA.trials[i].poses.length}),i);
   assert.equal(await page.locator('#accepted').textContent(),`${t.accepted} / 3`);
   assert.equal(Number(await page.locator('#poses').textContent()),t.poses);
   if(t.accepted<3)assert.match(await page.locator('#status').textContent(),/NO CURRENT CAMERA/);
   else assert.match(await page.locator('#status').textContent(),/SECTION JOINED/);
   if(t.name==='quality_coverage')assert.match(await page.locator('#join').textContent(),/No join performed/);
   await page.locator('#fit').click();
   await page.waitForTimeout(100);
   if(output)await page.screenshot({path:path.join(output,t.name+'.png'),fullPage:true});
   await page.locator('#step').fill('0');
   assert.equal(await page.locator('#accepted').textContent(),'1 / 1');
   assert.equal(await page.locator('#poses').textContent(),'24');
   await page.locator('#play').click();
   await page.waitForFunction(()=>document.querySelector('#step').value==='2'&&document.querySelector('#play').textContent==='Play sections');
   assert.equal(Number(await page.locator('#poses').textContent()),t.poses);
  }
  await page.setViewportSize({width:390,height:844});
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  console.log('PASS: three selections, all source images, progressive replay, withheld-position status, controls, mobile, offline');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
