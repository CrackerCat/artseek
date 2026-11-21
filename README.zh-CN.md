<p align="center">
  <img src="assets/logo.svg" width="128" alt="artseek" />
</p>

<h1 align="center">artseek</h1>

<p align="center">
  <img src="https://img.shields.io/badge/CI-passing-brightgreen?style=flat-square" alt="CI" />
  <img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="License" />
  <img src="https://img.shields.io/badge/android-arm64--v8a-3DDC84?style=flat-square" alt="Android arm64-v8a" />
</p>

零注入、只读的 Android ART 堆检查工具：不用 Frida、不用 Xposed，直接用 TypeScript 读取运行时活对象。

[English](README.md)

## 安装

```bash
npm install artseek
```

## 用法

```ts
import { createArtseekClient } from 'artseek';

const heap = createArtseekClient({ pkg: 'com.example.app' });

// View 层
const [title, meta] = await Promise.all([
  heap.currentActivity.mTitle,
  heap.view({ text: 'Submit' }).$viewMeta(),
]);

// 堆扫描：列出某个类的全部存活实例（点号类名 / JNI 描述符 / `*` 通配）
const sessions = await heap.scan({ type: '*Session', limit: 32 });
const drafts = await heap.scan({ type: '*OrderModel', includeSubclasses: true })
  .$where({ status: 'draft' })
  .$select({ id: 'orderId', total: 'priceInfo.total' });

// 字符串层：UI 事后才校验的 flag，以及谁持有它
const { hits } = await heap.strsearch({ needle: 'flag{' });
const owners = await heap.object(hits[0].addr).$whoRefs();

await heap.close();
```

## 命令行

```bash
npx artseek instances '*Session' --pkg com.example.app --pretty
npx artseek query '{"root":{"kind":"scan","type":"*Session"},"steps":[{"op":"ref"}],"collection":true}' --pkg com.example.app
```

[docs/cli.zh-CN.md](docs/cli.zh-CN.md)

## 许可

MIT
