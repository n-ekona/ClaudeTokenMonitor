// アプリ/ウィンドウアイコンを、ヘッダーのブランドマーク（角丸アクセント地＋白い3本バー）から
// 生成する。SVG を @resvg/resvg-js で PNG 化し、png2icons で .ico / .icns を作る。
// 依存は dev のみ（scripts/package.json）。出力（ico/icns/png）のみリポジトリへコミットする。
//   使い方: cd scripts && npm install && node gen-icons.mjs
import { Resvg } from '@resvg/resvg-js';
import png2icons from 'png2icons';
import { writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');

// ブランドマーク（web/index.html の brand-ico と同図形）。アプリアイコンは静止画像で
// テーマ追従できないため、地をアクセント色（既定 #d97757）で固定し、バーは白。
const ACCENT = '#d97757';
const SVG = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20">
  <rect x="1" y="1" width="18" height="18" rx="4.5" fill="${ACCENT}"/>
  <rect x="5" y="11" width="2.6" height="4.5" rx="0.6" fill="#fff"/>
  <rect x="8.7" y="8.5" width="2.6" height="7" rx="0.6" fill="#fff"/>
  <rect x="12.4" y="6" width="2.6" height="9.5" rx="0.6" fill="#fff"/>
</svg>`;

function renderPng(size) {
  const r = new Resvg(SVG, { fitTo: { mode: 'width', value: size } });
  return r.render().asPng();
}

// 高解像度 PNG を基に ico/icns を作る。linux は 512px PNG をそのまま使う。
const png1024 = renderPng(1024);
const png512 = renderPng(512);

const ico = png2icons.createICO(png1024, png2icons.BILINEAR, 0, false);
const icns = png2icons.createICNS(png1024, png2icons.BILINEAR, 0);
if (!ico || !icns) throw new Error('png2icons failed to produce ico/icns');

const outputs = [
  [resolve(root, 'linux/web/icon.png'), png512],
  [resolve(root, 'windows/desktop/icon.ico'), ico],
  [resolve(root, 'mac/AppIcon.icns'), icns],
];
for (const [path, buf] of outputs) {
  writeFileSync(path, buf);
  console.log(`wrote ${path} (${buf.length} bytes)`);
}
console.log('done');
