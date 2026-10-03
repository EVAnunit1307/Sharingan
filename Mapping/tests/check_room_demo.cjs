// Verify/export the portable example. Requires Playwright + Chromium, no Pi.
// node Mapping/tests/check_room_demo.cjs /new/screenshot/directory
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright');

async function main() {
  const output = process.argv[2];
  if (output) fs.mkdirSync(output); // Preserve previous exports.
  const browser = await chromium.launch({headless: true,
    ...(process.env.CHROME_BIN ? {executablePath: process.env.CHROME_BIN} : {})});
  try {
    const page = await browser.newPage({viewport:{width:1440,height:1180},deviceScaleFactor:1});
    const errors = [], external = [];
    page.on('pageerror',e=>errors.push(e.message));
    page.on('request',r=>{if(/^https?:/.test(r.url())) external.push(r.url());});
    const viewer = process.env.VIEWER_FILE || path.resolve(__dirname,'../examples/room-draft-20261002/index.html');
    await page.goto(pathToFileURL(path.resolve(viewer)).href);
    assert.equal(await page.locator('#trial option').count(),4);
    await page.evaluate(async()=>{
      for(const t of DATA.trials) for(const src of [...t.images,...t.overlays]) {
        const image=new Image(); image.src=src; await image.decode();
        if(!image.naturalWidth||!image.naturalHeight) throw new Error('Invalid bundled image');
      }
    });
    const names = process.env.VIEWER_FILE ? await page.evaluate(()=>DATA.trials.map(t=>t.name))
      : ['room-sweep','room-sweep-alternate','lit-sofa','couch-return'];
    for (let i=0; i<names.length; i++) {
      await page.selectOption('#trial',String(i));
      if(await page.evaluate(i=>DATA.trials[i].pose_conditioned,i))
        assert.match(await page.locator('#provenance').textContent(),/imposed, not independent validation/);
      await page.waitForFunction(()=>{const im=document.querySelector('#image');return im.complete&&im.naturalWidth>0;});
      assert.equal(await page.locator('#views').textContent(),'24');
      const points=Number((await page.locator('#shown').textContent()).replaceAll(',',''));
      assert(points>50000);
      await page.locator('#uncertain').uncheck();
      await page.waitForFunction(n=>Number(document.querySelector('#shown').textContent.replaceAll(',',''))<n,points);
      await page.locator('#uncertain').check();
      await page.locator('#semantic').check();
      await page.waitForFunction(()=>document.querySelector('#image').naturalWidth===504);
      await page.locator('#semantic').uncheck();
      await page.locator('#inspect').click();
      assert(await page.locator('#single').isChecked());
      await page.locator('#single').uncheck();
      await page.locator('#fit').click();
      await page.locator('#scrub').fill('23');
      assert.match(await page.locator('#frame-label').textContent(),/24/);
      await page.locator('#scrub').fill('0');
      await page.waitForTimeout(150);
      if(output) await page.screenshot({path:path.join(output,`${names[i]}.png`),fullPage:true});
    }
    await page.setViewportSize({width:390,height:844});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
    assert.deepEqual(errors,[]);
    assert.deepEqual(external,[],'The bundled viewer must open fully offline');
    console.log('PASS: 4 estimates, all images, uncertainty/labels/single-view/scrub controls, mobile layout, offline loading');
  } finally { await browser.close(); }
}
main().catch(e=>{console.error(e);process.exitCode=1;});
