#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createArtseekClient } from './client.js';
import type { QuerySpec } from './types.js';

const VERSION = '0.1.0';

const HELP_TEXT = `artseek v${VERSION} — Android ART heap & View inspector CLI

Usage:
  artseek <command> [args] [options]

Commands:
  ping                              Check binary & cached ART layout status
  selfcheck                         Validate ART layout against target process
  calibrate                         Force sentinel calibration and update layout.json
  roots                             List live Activities and ViewRootImpl windows
  tree                              Dump View hierarchy tree
  strsearch <needle>                Search live String objects and their owners
  instances <type>                  List live instances matching class name or glob
  dumpobj <addr>                    Dump object fields by hex address
  whorefs <addr>                    Find objects referencing <addr>
  query <json>                      Execute a QuerySpec JSON payload

Options:
  -s, --serial <serial>             ADB device serial
  --pkg <package>                   Target Android package name
  --pid <pid>                       Target process PID
  --freeze                          Pause target threads with SIGSTOP during read
  --binary <path>                   Explicit path to local arm64 artseek binary
  --force-push                      Force re-pushing native binary to device
  --skip-push                       Skip auto-push check
  --on-device                       Run via local su -c instead of adb shell
  --timeout <ms>                    Request timeout in milliseconds
  --limit <n>                       Max results (strsearch, instances, whorefs)
  --depth <n>                       Object dump depth (dumpobj)
  --max-depth <n>                   Max View tree depth (tree)
  --root-addr <addr>                Specific ViewRootImpl address (tree)
  --focused                         Only focused window (tree)
  --schema-only                     Dump schema only (dumpobj)
  --preview                         Include owner preview (strsearch)
  --no-auto-recalibrate             Disable auto-recalibration (selfcheck)
  --pretty                          Pretty-print JSON output
  -v, --version                     Show version
  -h, --help                        Show help
`;

interface ParsedArgs {
  command?: string;
  positionals: string[];
  flags: Record<string, string | boolean>;
}

function parseCliArgs(argv: string[]): ParsedArgs {
  const positionals: string[] = [];
  const flags: Record<string, string | boolean> = {};

  const valueFlags = new Set([
    's',
    'serial',
    'pkg',
    'pid',
    'binary',
    'timeout',
    'limit',
    'depth',
    'max-depth',
    'root-addr',
    'expected-class',
  ]);

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--') {
      positionals.push(...argv.slice(i + 1));
      break;
    }
    if (arg.startsWith('--')) {
      const eq = arg.indexOf('=');
      if (eq !== -1) {
        flags[arg.slice(2, eq)] = arg.slice(eq + 1);
      } else {
        const key = arg.slice(2);
        if (valueFlags.has(key) && i + 1 < argv.length) {
          flags[key] = argv[++i];
        } else {
          flags[key] = true;
        }
      }
    } else if (arg.startsWith('-') && arg.length === 2) {
      const short = arg.slice(1);
      if (valueFlags.has(short) && i + 1 < argv.length) {
        flags[short] = argv[++i];
      } else {
        flags[short] = true;
      }
    } else {
      positionals.push(arg);
    }
  }

  const [command, ...rest] = positionals;
  return { command, positionals: rest, flags };
}

function numFlag(flags: Record<string, string | boolean>, key: string): number | undefined {
  const raw = flags[key];
  if (typeof raw !== 'string') return undefined;
  const n = Number(raw);
  if (!Number.isFinite(n)) {
    throw new Error(`Invalid numeric value for --${key}: ${raw}`);
  }
  return n;
}

function strFlag(flags: Record<string, string | boolean>, ...keys: string[]): string | undefined {
  for (const key of keys) {
    const raw = flags[key];
    if (typeof raw === 'string') return raw;
  }
  return undefined;
}

export async function runCli(argv: string[]): Promise<number> {
  const { command, positionals, flags } = parseCliArgs(argv);

  if (flags.v || flags.version) {
    process.stdout.write(`${VERSION}\n`);
    return 0;
  }
  if (!command || flags.h || flags.help || command === 'help') {
    process.stdout.write(HELP_TEXT);
    return 0;
  }

  const client = createArtseekClient({
    serial: strFlag(flags, 'serial', 's'),
    pkg: strFlag(flags, 'pkg'),
    pid: numFlag(flags, 'pid'),
    freeze: Boolean(flags.freeze),
    binaryPath: strFlag(flags, 'binary'),
    forcePush: Boolean(flags['force-push']),
    skipPush: Boolean(flags['skip-push']),
    onDevice: Boolean(flags['on-device']),
    defaultTimeoutMs: numFlag(flags, 'timeout'),
    oneShot: true,
  });

  try {
    let output: unknown;
    switch (command) {
      case 'ping':
        output = await client.ping();
        break;
      case 'selfcheck':
        output = await client.selfcheck({
          autoRecalibrate: !flags['no-auto-recalibrate'],
        });
        break;
      case 'calibrate':
        output = await client.calibrate();
        break;
      case 'roots':
        output = await client.roots();
        break;
      case 'tree':
        output = await client.tree({
          onlyFocused: Boolean(flags.focused),
          rootAddr: strFlag(flags, 'root-addr'),
          maxDepth: numFlag(flags, 'max-depth'),
        });
        break;
      case 'strsearch': {
        const needle = positionals[0];
        if (!needle) throw new Error('Usage: artseek strsearch <needle> --pkg <pkg>');
        output = await client.strsearch({
          needle,
          limit: numFlag(flags, 'limit'),
          includeOwnerPreview: Boolean(flags.preview),
        });
        break;
      }
      case 'instances': {
        const type = positionals[0];
        if (!type) throw new Error('Usage: artseek instances <type> --pkg <pkg>');
        output = await client.instances({
          type,
          limit: numFlag(flags, 'limit'),
        });
        break;
      }
      case 'dumpobj': {
        const addr = positionals[0];
        if (!addr) throw new Error('Usage: artseek dumpobj <addr> --pkg <pkg>');
        output = await client.dumpobj({
          addr,
          expectedClass: strFlag(flags, 'expected-class'),
          depth: numFlag(flags, 'depth'),
          schemaOnly: Boolean(flags['schema-only']),
        });
        break;
      }
      case 'whorefs': {
        const addr = positionals[0];
        if (!addr) throw new Error('Usage: artseek whorefs <addr> --pkg <pkg>');
        output = await client.whorefs({
          addr,
          limit: numFlag(flags, 'limit'),
        });
        break;
      }
      case 'query': {
        const raw = positionals[0];
        if (!raw) throw new Error("Usage: artseek query '<QuerySpec JSON>' --pkg <pkg>");
        const spec = JSON.parse(raw) as QuerySpec;
        output = await client.rawQuery(spec);
        break;
      }
      default:
        throw new Error(`Unknown command: ${command}. Run "artseek --help" for usage.`);
    }

    const formatted = flags.pretty ? JSON.stringify(output, null, 2) : JSON.stringify(output);
    process.stdout.write(`${formatted}\n`);
    return 0;
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    process.stderr.write(`artseek error: ${message}\n`);
    return 1;
  } finally {
    await client.close();
  }
}

function isDirectInvocation(): boolean {
  if (!process.argv[1]) return false;
  try {
    const selfPath = fs.realpathSync(fileURLToPath(import.meta.url));
    const entryPath = fs.realpathSync(path.resolve(process.argv[1]));
    return selfPath === entryPath;
  } catch {
    return false;
  }
}

if (isDirectInvocation()) {
  runCli(process.argv.slice(2)).then((code) => {
    process.exitCode = code;
  });
}
