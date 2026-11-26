#!/usr/bin/env node
//
// Renders the app store screenshots: each photo in store/photos/, converted
// by the real settings page (config/index.html) for each watch, then decoded
// from the packed bytes the way the watch draws them.
//
//   node scripts/store-screenshots.js [photo ...]
//
// Writes store/screenshots/<platform>_<n>.png at the watch's screen size;
// the platform prefix is what `pebble publish --screenshots` expects.
// Needs Playwright: npm install --no-save playwright && npx playwright install chromium

'use strict';

const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const ROOT = path.resolve(__dirname, '..');
const OUT = path.join(ROOT, 'store', 'screenshots');
const PLATFORMS = {
  emery: { width: 200, height: 228, round: 0 },
  gabbro: { width: 260, height: 260, round: 1 },
};
// Where the page sends its result; intercepted below, never fetched.
const RETURN_TO = 'http://localhost:9/close?';

async function render(browser, platform, photo) {
  const { width, height, round } = PLATFORMS[platform];
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(String(e)));
  let result = null;
  await page.route(RETURN_TO + '**', (route) => {
    result = JSON.parse(decodeURIComponent(route.request().url().slice(RETURN_TO.length)));
    route.fulfill({ body: 'ok' });
  });
  const url = new URL('file://' + path.join(ROOT, 'config', 'index.html'));
  url.search = new URLSearchParams({ width, height, round, v: 2, return_to: RETURN_TO });
  await page.goto(url.href);
  await page.setInputFiles('#file', photo);
  await page.waitForSelector('#preview:not([hidden])', { timeout: 120000 });
  await page.click('#save');
  await page.waitForFunction(() => document.readyState === 'complete');
  for (let i = 0; i < 50 && !result; i++) await page.waitForTimeout(100);
  if (!result) throw new Error(`${platform} ${photo}: no result from the settings page ${errors.join(' ')}`);

  // Decode as src/c/main.c does: 6-bit rrggbb pixels, packed MSB first, only
  // the visible span of each row; everything else stays black.
  await page.goto('about:blank');
  const png = await page.evaluate(({ data, width, height, round }) => {
    const bin = atob(data.replace(/-/g, '+').replace(/_/g, '/'));
    const canvas = document.createElement('canvas');
    canvas.width = width;
    canvas.height = height;
    const ctx = canvas.getContext('2d');
    const image = ctx.createImageData(width, height);
    const px = image.data;
    for (let i = 3; i < px.length; i += 4) px[i] = 255;
    function rowSpan(y) {
      if (!round) return [0, width];
      const d = Math.min(width, height) + 2;
      const dy = 2 * y + 1 - height;
      const lim = d * d - dy * dy;
      if (lim < 0) return [0, 0];
      let x = 0;
      while (x < (width >> 1) && (2 * x + 1 - width) * (2 * x + 1 - width) > lim) x++;
      return [x, width - x];
    }
    let acc = 0, bits = 0, pos = 0;
    for (let y = 0; y < height; y++) {
      const [x0, x1] = rowSpan(y);
      for (let x = x0; x < x1; x++) {
        while (bits < 6) {
          acc = ((acc << 8) | bin.charCodeAt(pos++)) & 0xffff;
          bits += 8;
        }
        bits -= 6;
        const v = (acc >> bits) & 0x3f;
        const o = (y * width + x) * 4;
        px[o] = ((v >> 4) & 3) * 85;
        px[o + 1] = ((v >> 2) & 3) * 85;
        px[o + 2] = (v & 3) * 85;
      }
    }
    ctx.putImageData(image, 0, 0);
    return canvas.toDataURL('image/png').split(',')[1];
  }, { data: result.ops[0].data, width, height, round });
  await page.close();
  return Buffer.from(png, 'base64');
}

(async () => {
  let photos = process.argv.slice(2);
  if (!photos.length) {
    const dir = path.join(ROOT, 'store', 'photos');
    photos = fs.readdirSync(dir).filter((f) => /\.(jpe?g|png|webp)$/i.test(f)).sort()
      .map((f) => path.join(dir, f));
  }
  fs.mkdirSync(OUT, { recursive: true });
  for (const f of fs.readdirSync(OUT)) {
    if (/^\w+_\d+\.png$/.test(f)) fs.unlinkSync(path.join(OUT, f));
  }
  const browser = await chromium.launch();
  try {
    for (const platform of Object.keys(PLATFORMS)) {
      for (const [i, photo] of photos.entries()) {
        const file = path.join(OUT, `${platform}_${i + 1}.png`);
        fs.writeFileSync(file, await render(browser, platform, photo));
        console.log(path.relative(ROOT, file), '<-', path.relative(ROOT, photo));
      }
    }
  } finally {
    await browser.close();
  }
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
