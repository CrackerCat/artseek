<p align="center">
  <img src="https://raw.githubusercontent.com/std-microblock/artseek/master/assets/logo.svg" width="112" alt="artseek" />
</p>

<h1 align="center">artseek</h1>

<p align="center">
  <a href="https://github.com/std-microblock/artseek/actions/workflows/ci.yml"><img src="https://github.com/std-microblock/artseek/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <img src="https://img.shields.io/badge/license-MIT-blue" alt="License" />
  <img src="https://img.shields.io/badge/android-arm64--v8a-3DDC84" alt="Android arm64-v8a" />
</p>

Zero-injection, read-only Android ART heap inspector. Query live objects from TypeScript
without Frida or Xposed: no agent is loaded into the target process, nothing is patched, and
the only thing the device sees is a short-lived NDJSON request.

[中文说明](https://github.com/std-microblock/artseek/blob/master/README.zh-CN.md)

## Requirements

- Node.js >= 18
- A rooted `arm64-v8a` Android device with `su -c` available
- `adb` on `PATH` (the CLI pushes the bundled native binary to `/data/local/tmp/.artseek/`)

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
npx artseek ping --pkg com.example.app
npx artseek roots --serial <serial>
npx artseek instances '*Session' --pkg com.example.app --pretty
npx artseek strsearch 'sessionToken' --pkg com.example.app --preview
npx artseek query '{"root":{"kind":"scan","type":"*Session"},"steps":[{"op":"ref"}],"collection":true}' \
  --pkg com.example.app
```

Full CLI reference: [docs/cli.md](https://github.com/std-microblock/artseek/blob/master/docs/cli.md).
Query grammar: [protocol.md](https://github.com/std-microblock/artseek/blob/master/protocol/protocol.md).

## What it does not do

- No code injection, no hooking, no `ptrace` attach to the target process
- No writes to app memory or files; everything is read through `/proc/<pid>/mem`
- No cloud service, no telemetry: the SDK talks to your device and nothing else

## License

MIT
