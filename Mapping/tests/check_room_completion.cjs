// Offline browser QA and reproducible screenshot export.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {pathToFileURL}=require('node:url');
const {chromium}=require(process.env.PLAYWRIGHT_MODULE||'playwright');
(async()=>{
  const [viewer,output]=process.argv.slice(2);if(!viewer)throw Error('Supply viewer path');
  if(output)fs.mkdirSync(output);
  const browser=await chromium.launch({headless:true,...(process.env.CHROME_BIN?{executablePath:process.env.CHROME_BIN}:{})});
  try{
    const page=await browser.newPage({viewport:{width:1440,height:1180}}),errors=[],external=[];
    page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>{if(/^https?:/.test(r.url()))external.push(r.url());});
    await page.goto(pathToFileURL(path.resolve(viewer)).href);
    assert.equal(await page.locator('#trial option').count(),4);
    await page.evaluate(async()=>{for(const t of DATA.trials)for(const src of [...t.images,...t.overlays]){const im=new Image();im.src=src;await im.decode();if(im.naturalWidth!==504)throw Error('Unexpected bundled image');}});
    const names=await page.evaluate(()=>DATA.trials.map(t=>t.name));
    for(let i=0;i<names.length;i++){
      await page.selectOption('#trial',String(i));
      await page.locator('#semantics').check();await page.locator('#scrub').fill('23');
      assert.match(await page.locator('#frame-label').textContent(),/24 \/ 24/);
      await page.locator('#semantics').uncheck();await page.locator('#scrub').fill('10');
      for(const mode of ['evidence','structure','local','envelope']){
        await page.selectOption('#mode',mode);await page.locator('#above').click();
        await page.waitForFunction(m=>document.querySelector('#mode-note').textContent===notes[m],mode);
        const shown=Number(await page.locator('#filled').textContent());
        if(['evidence','structure'].includes(mode))assert.equal(shown,0);
        await page.locator('#angle').click();
        if(output&&['local','envelope'].includes(mode)){
          await page.waitForTimeout(100);await page.screenshot({path:path.join(output,`${names[i]}-${mode}.png`),fullPage:true});
        }
      }
      await page.locator('#points').uncheck();await page.locator('#points').check();await page.locator('#fit').click();
    }
    await page.setViewportSize({width:390,height:844});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
    console.log('PASS: 4 trials × 4 approaches, all images, controls, mobile sizing, fully offline');
  }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
