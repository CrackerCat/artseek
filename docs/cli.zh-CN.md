# artseek CLI

设备端原生二进制的一层薄 Node 封装。每次调用就是对 `adb shell su -c ...` 发一次 NDJSON 请求，
不往目标进程注入任何东西。

```bash
npx artseek <命令> [参数] [选项]      # 已安装的包
node sdk/dist/cli.js <命令> ...      # 在本仓库里直接用
```

需要 Node.js >= 18；设备为 `arm64-v8a` 且有 root（可用 `su -c`）。

## 设备与目标解析

| 选项 | 含义 |
| --- | --- |
| `-s, --serial <serial>` | `adb -s <serial>`，只连一台设备时可省略 |
| `--pkg <package>` | 目标包名；二进制从 `/proc/*/cmdline` 反查 PID |
| `--pid <pid>` | 直接指定 PID（可与 `--pkg` 并用） |
| `--binary <path>` | 要推送的本地 arm64 二进制；默认用内置/仓库构建产物 |
| `--force-push` | 设备上 size + mtime 一致时也强制重推 |
| `--skip-push` | 假定设备上 `/data/local/tmp/.artseek/artseek` 已就绪 |
| `--on-device` | 不走 adb，本地 `su -c` 拉起（CLI 直接跑在手机上） |
| `--timeout <ms>` | 单请求超时，默认 30000 |
| `--pretty` | JSON 美化输出 |

首次调用会把二进制推到 `/data/local/tmp` 下的暂存路径，再由 `su` 挪到
`/data/local/tmp/.artseek/artseek`，目录和可执行文件都是 `0700`。
ART 的 `layout.json` 缓存放在旁边，首次使用时自动校准。

## 命令

| 命令 | 输出 |
| --- | --- |
| `ping` | 版本、缓存目录、layout 状态 |
| `selfcheck` | layout 校验项，必要时自动重校准 |
| `calibrate` | 强制用 sentinel 校准并重写 `layout.json` |
| `roots` | 存活 Activity + ViewRootImpl 窗口（尺寸/焦点） |
| `tree` | View 树（`--focused`、`--max-depth`、`--root-addr`） |
| `strsearch <needle>` | 堆里含该子串的 `java.lang.String` 及其持有者（`--preview`、`--limit`） |
| `instances <type>` | 某类的存活实例：点号类名、JNI 描述符或 `*` 通配 |
| `dumpobj <addr>` | 按地址 dump 字段（`--depth`、`--schema-only`） |
| `whorefs <addr>` | 引用了该地址的对象（`--limit`） |
| `query <json>` | 任意 `QuerySpec`，也就是含 heap `scan` 的完整查询 VM |

### 例子

```bash
# 工具链是否就绪
artseek ping --serial <serial>

# 现在屏幕上是啥，按钮在哪
artseek roots --pkg com.example.app
artseek tree --pkg com.example.app --focused --max-depth 12
artseek query '{"root":{"kind":"view","text":"Verify"},"steps":[{"op":"view_meta"}],"collection":false}' \
  --pkg com.example.app

# heap scan：列出 app 自己的全部存活实例，再读其中一个
artseek instances 'com.example.app.*' --pkg com.example.app --limit 20 --pretty
artseek dumpobj 0x76558f08 --pkg com.example.app --depth 2 --pretty

# 字符串 + 谁持有它
artseek strsearch 'sessionToken' --pkg com.example.app --preview --limit 10 --pretty
```

`query` 打印完整协议信封（`id`、`ok`、`meta`、`data`、`error`），能顺便看到
`elapsedMs`、`cacheHit`、`recalibrated`；其余命令只打印 `data`。

## CLI 里做 heap scan

SDK 里 `heap.scan(...)` 链式写法能表达的东西，一个 `query` 载荷就能表达：

```bash
artseek query '{
  "root": { "kind": "scan", "type": "*Session", "includeSubclasses": true, "limit": 64 },
  "steps": [
    { "op": "where", "filter": { "state": "active" } },
    { "op": "select", "shape": { "user": "user.name", "token": "token" } }
  ],
  "collection": true
}' --pkg com.example.app --pretty
```

步骤按顺序在设备端一次跑完，只把投影结果带回来。Windows PowerShell 下用单引号包住 JSON、
内部保留双引号；或者把载荷写进文件，用 `(Get-Content spec.json -Raw)` 传。

可用步骤：`field`、`index`、`first`、`where`、`parent`、`find_owner`、`crawl`、
`select`、`dump`、`ref`、`view_meta`；根来源：`activity`、`scan`、`view`、`addr`。
完整语法见 [protocol.md](../protocol/protocol.md)。

## 值得记住的默认值

| 参数 | 默认 |
| --- | --- |
| `strsearch` limit | 50 |
| `instances` / `scan` limit | 256 |
| `whorefs` limit | 128 |
| `tree` maxDepth | 64 |
| `crawl` depth / maxObjects | 4 / 2000 |
| `dump` depth / maxArrayItems | 1 / 64 |
| `find_owner` maxHops | 6 |

## 退出码与错误

成功 `0`，失败 `1`。失败信息以 `artseek error: <message>` 打到 stderr，stdout 永远只有 JSON，
可以直接管到 `jq` 或脚本里。

常见协议错误码：`PROCESS_NOT_FOUND`（包没在跑）、`STALE_REF`（地址不再匹配预期类）、
`CALIBRATION_FAILED`（ROM/ART 变了，重跑 `artseek calibrate`）、`INVALID_QUERY`（`QuerySpec` 写错）、
`NOT_FOUND`、`TIMEOUT`、`TRANSPORT_ERROR`、`TRANSPORT_CLOSED`。
