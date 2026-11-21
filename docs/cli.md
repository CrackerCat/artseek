# artseek CLI

A thin Node wrapper around the on-device native binary. Each invocation is one NDJSON
request against `adb shell su -c ...`; nothing is injected into the target process.

```bash
npx artseek <command> [args] [options]     # installed package
node sdk/dist/cli.js <command> ...         # from this monorepo
```

Node.js >= 18. Android device must be `arm64-v8a` and rooted (`su -c` available).

## Device & target resolution

| Flag | Meaning |
| --- | --- |
| `-s, --serial <serial>` | `adb -s <serial>`; omit when only one device is attached |
| `--pkg <package>` | Target package; the binary resolves the PID from `/proc/*/cmdline` |
| `--pid <pid>` | Target PID instead of (or alongside) `--pkg` |
| `--binary <path>` | Local arm64 binary to push; defaults to the bundled / repo build output |
| `--force-push` | Re-push even when size and mtime already match on device |
| `--skip-push` | Assume the binary is already at `/data/local/tmp/.artseek/artseek` |
| `--on-device` | Skip adb and spawn `su -c` locally (running the CLI on the phone itself) |
| `--timeout <ms>` | Per-request timeout (default 30000) |
| `--pretty` | Pretty-print JSON |

The first call pushes the binary to a staging path in `/data/local/tmp`, then `su` moves it
to `/data/local/tmp/.artseek/artseek` with `0700` on both directory and executable.
The ART `layout.json` cache lives next to it and is calibrated on first use.

## Commands

| Command | Returns |
| --- | --- |
| `ping` | Binary version, cache dir, layout status |
| `selfcheck` | Layout validation checks; recalibrates when needed |
| `calibrate` | Forces sentinel calibration, rewrites `layout.json` |
| `roots` | Live Activities + ViewRootImpl windows with sizes and focus |
| `tree` | View hierarchy (`--focused`, `--max-depth`, `--root-addr`) |
| `strsearch <needle>` | Live `java.lang.String` hits + the owners referencing them (`--preview`, `--limit`) |
| `instances <type>` | Live instances of a class: dot name, JNI descriptor, or `*` glob |
| `dumpobj <addr>` | Field dump by address (`--depth`, `--schema-only`) |
| `whorefs <addr>` | Objects that reference `addr` (`--limit`) |
| `query <json>` | Any `QuerySpec`: the full query VM, including heap `scan` |

### Examples

```bash
# Is the tool wired up?
artseek ping --serial <serial>

# What is on screen, and where is the button?
artseek roots --pkg com.example.app
artseek tree --pkg com.example.app --focused --max-depth 12
artseek query '{"root":{"kind":"view","text":"Verify"},"steps":[{"op":"view_meta"}],"collection":false}' \
  --pkg com.example.app

# Heap scan: every live instance of the app's own classes, then read one
artseek instances 'com.example.app.*' --pkg com.example.app --limit 20 --pretty
artseek dumpobj 0x76558f08 --pkg com.example.app --depth 2 --pretty

# Strings and their holders
artseek strsearch 'sessionToken' --pkg com.example.app --preview --limit 10 --pretty
```

`query` prints the whole protocol envelope (`id`, `ok`, `meta`, `data`, `error`) so you can
see `elapsedMs`, `cacheHit` and `recalibrated`; every other command prints `data` only.

## Heap scan from the CLI

Anything you can express with the SDK's `heap.scan(...)` chain is one `query` payload:

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

Steps run on-device, in order, in a single request; only the projection comes back.
On Windows PowerShell wrap the JSON in single quotes and keep double quotes inside — or
put the payload in a file and pass `(Get-Content spec.json -Raw)`.

Available steps: `field`, `index`, `first`, `where`, `parent`, `find_owner`, `crawl`,
`select`, `dump`, `ref`, `view_meta`. Root sources: `activity`, `scan`, `view`, `addr`.
See [protocol.md](../protocol/protocol.md) for the full grammar.

## Defaults worth knowing

| Knob | Default |
| --- | --- |
| `strsearch` limit | 50 |
| `instances` / `scan` limit | 256 |
| `whorefs` limit | 128 |
| `tree` maxDepth | 64 |
| `crawl` depth / maxObjects | 4 / 2000 |
| `dump` depth / maxArrayItems | 1 / 64 |
| `find_owner` maxHops | 6 |

## Exit codes & errors

`0` on success, `1` on failure. Failures print `artseek error: <message>` to stderr and never
mix diagnostics into stdout, so the CLI pipes straight into `jq` or a script.

Protocol error codes you will meet most: `PROCESS_NOT_FOUND` (package not running),
`STALE_REF` (address no longer matches the expected class), `CALIBRATION_FAILED`
(ROM/ART mismatch — retry or `artseek calibrate`), `INVALID_QUERY` (bad `QuerySpec`),
`NOT_FOUND`, `TIMEOUT`, `TRANSPORT_ERROR`, `TRANSPORT_CLOSED`.
