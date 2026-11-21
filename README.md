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

[中文](README.zh-CN.md)

## Install

```bash
npm install artseek
```

## Usage

```ts
import { createArtseekClient } from 'artseek';

const heap = createArtseekClient({ pkg: 'com.example.app' });

// View layer
const [title, meta] = await Promise.all([
  heap.currentActivity.mTitle,
  heap.view({ text: 'Submit' }).$viewMeta(),
]);

// Heap layer: every live instance of a class (dot name, JNI descriptor, or `*` glob)
const sessions = await heap.scan({ type: '*Session', limit: 32 });
const drafts = await heap.scan({ type: '*OrderModel', includeSubclasses: true })
  .$where({ status: 'draft' })
  .$select({ id: 'orderId', total: 'priceInfo.total' });

// String layer: the flag a UI only validates after the fact, and who holds it
const { hits } = await heap.strsearch({ needle: 'flag{' });
const owners = await heap.object(hits[0].addr).$whoRefs();

await heap.close();
```

## CLI

```bash
npx artseek instances '*Session' --pkg com.example.app --pretty
npx artseek query '{"root":{"kind":"scan","type":"*Session"},"steps":[{"op":"ref"}],"collection":true}' --pkg com.example.app
```

see [docs/cli.md](docs/cli.md).
