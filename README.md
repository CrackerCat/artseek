<p align="center">
  <img src="assets/logo.svg" width="128" alt="artseek" />
</p>

<h1 align="center">artseek</h1>

<p align="center">
  <img src="https://img.shields.io/badge/CI-passing-brightgreen?style=flat-square" alt="CI" />
  <img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="License" />
  <img src="https://img.shields.io/badge/android-arm64--v8a-3DDC84?style=flat-square" alt="Android arm64-v8a" />
</p>

Zero-injection, read-only Android ART heap inspector. Query live objects from TypeScript without Frida or Xposed.

## Install

```bash
npm install artseek
```

## Usage

```ts
import { createArtseekClient } from 'artseek';

const heap = createArtseekClient({ pkg: 'com.example.app' });
const [title, meta] = await Promise.all([
  heap.currentActivity.mTitle,
  heap.view({ text: 'Submit' }).$viewMeta(),
]);
await heap.close();
```
