import {
  spawn,
  execFile,
  type ChildProcessWithoutNullStreams,
} from 'node:child_process';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  ArtseekError,
  type ArtseekRequest,
  type ArtseekResponse,
  type ArtseekTransport,
} from './types.js';

const DEFAULT_REMOTE_DIR = '/data/local/tmp/.artseek';
const DEFAULT_TIMEOUT_MS = 30_000;
const MAX_DIAG_BYTES = 8192;

function appendRolling(buf: string, chunk: string): string {
  const next = buf + chunk;
  return next.length > MAX_DIAG_BYTES ? next.slice(next.length - MAX_DIAG_BYTES) : next;
}

function shellQuoteSingle(value: string): string {
  return `'${value.replace(/'/g, `'\\''`)}'`;
}

function execFileAsync(
  file: string,
  args: string[],
  timeoutMs = 30_000,
): Promise<{ stdout: string; stderr: string }> {
  return new Promise((resolve, reject) => {
    execFile(
      file,
      args,
      { timeout: timeoutMs, maxBuffer: 4 * 1024 * 1024, windowsHide: true },
      (err, stdout, stderr) => {
        if (err) {
          const detail = [stderr?.trim(), stdout?.trim(), err.message]
            .filter(Boolean)
            .join(' | ');
          reject(
            new ArtseekError('TRANSPORT_ERROR', `Command failed (${file} ${args.join(' ')}): ${detail}`, {
              cause: err,
            }),
          );
          return;
        }
        resolve({ stdout: String(stdout ?? ''), stderr: String(stderr ?? '') });
      },
    );
  });
}

/**
 * Resolves the local Android arm64 `artseek` binary path.
 * Checks explicit override first, then bundled `dist/bin/artseek-android-arm64`,
 * then repository xmake release output `../../build/android/arm64-v8a/release/artseek`.
 */
export function resolveDefaultBinaryPath(explicitPath?: string): string | null {
  if (explicitPath) {
    const resolved = path.resolve(explicitPath);
    if (!fs.existsSync(resolved) || !fs.statSync(resolved).isFile()) {
      throw new ArtseekError(
        'TRANSPORT_ERROR',
        `Specified artseek binaryPath does not exist or is not a file: ${resolved}`,
      );
    }
    return resolved;
  }

  const moduleDir = path.dirname(fileURLToPath(import.meta.url));
  const candidates = [
    path.resolve(moduleDir, 'bin/artseek-android-arm64'),
    path.resolve(moduleDir, '../dist/bin/artseek-android-arm64'),
    path.resolve(moduleDir, '../../build/android/arm64-v8a/release/artseek'),
    path.resolve(moduleDir, '../../../build/android/arm64-v8a/release/artseek'),
  ];

  for (const candidate of candidates) {
    try {
      if (fs.existsSync(candidate) && fs.statSync(candidate).isFile()) {
        return candidate;
      }
    } catch {
      // Ignore permission/stat errors while probing candidates.
    }
  }
  return null;
}

// ---------------------------------------------------------------------------
// Generic Persistent NDJSON Stdio Transport
// ---------------------------------------------------------------------------

export interface StdioTransportOptions {
  /** Factory that spawns a fresh child process speaking NDJSON over stdin/stdout. */
  spawnProcess: () => ChildProcessWithoutNullStreams;
  /** Optional async hook run before spawning (e.g. ensure remote binary is pushed). */
  beforeSpawn?: () => Promise<void>;
  /** Default per-request timeout in ms (default: 30000). */
  defaultTimeoutMs?: number;
  /** Auto-close idle persistent child after N ms and transparently respawn on next query. */
  idleTimeoutMs?: number;
  /** When true, spawns a fresh child per request and closes stdin immediately. */
  oneShot?: boolean;
  /** Max automatic reconnect attempts if a persistent child crashes mid-request (default: 1). */
  maxReconnectAttempts?: number;
}

interface PendingRequest {
  id: string;
  payload: ArtseekRequest;
  proc: ChildProcessWithoutNullStreams;
  attempt: number;
  timeoutMs: number;
  timer: NodeJS.Timeout | null;
  resolve: (res: ArtseekResponse<any>) => void;
  reject: (err: Error) => void;
}

/**
 * Reusable persistent NDJSON stdio transport supporting:
 * - Request `id` correlation across multiplexed/pipelined lines
 * - Per-request timeouts
 * - Clean `stderr` diagnostic surfacing
 * - Automatic reconnect if the child process exits unexpectedly
 * - Optional `idleTimeoutMs` auto-close with transparent respawn on next query
 * - Optional `oneShot` mode
 */
export class StdioTransport implements ArtseekTransport {
  private readonly spawnProcess: () => ChildProcessWithoutNullStreams;
  private readonly beforeSpawn?: () => Promise<void>;
  private readonly defaultTimeoutMs: number;
  private readonly idleTimeoutMs: number;
  private readonly oneShot: boolean;
  private readonly maxReconnectAttempts: number;

  private proc: ChildProcessWithoutNullStreams | null = null;
  private startingPromise: Promise<ChildProcessWithoutNullStreams> | null = null;
  private stdoutBuffer = '';
  private stderrBuffer = '';
  private nextId = 0;
  private readonly pending = new Map<string, PendingRequest>();
  private idleTimer: NodeJS.Timeout | null = null;
  private intentionalClosingProcs = new WeakSet<ChildProcessWithoutNullStreams>();
  private closed = false;

  constructor(options: StdioTransportOptions) {
    this.spawnProcess = options.spawnProcess;
    this.beforeSpawn = options.beforeSpawn;
    this.defaultTimeoutMs = options.defaultTimeoutMs ?? DEFAULT_TIMEOUT_MS;
    this.idleTimeoutMs = options.idleTimeoutMs ?? 0;
    this.oneShot = options.oneShot ?? false;
    this.maxReconnectAttempts = options.maxReconnectAttempts ?? 1;
  }

  public get isRunning(): boolean {
    return this.proc !== null && this.proc.exitCode === null && !this.proc.killed;
  }

  public async request<T = unknown>(req: ArtseekRequest): Promise<ArtseekResponse<T>> {
    if (this.closed) {
      throw new ArtseekError('TRANSPORT_CLOSED', 'Artseek transport has been closed');
    }

    const id = req.id !== undefined ? String(req.id) : `req_${++this.nextId}`;
    const payload: ArtseekRequest = req.id !== undefined ? req : { ...req, id };
    const timeoutMs = req.timeoutMs ?? this.defaultTimeoutMs;

    if (this.oneShot) {
      return this.requestOneShot<T>(payload, id, timeoutMs);
    }

    return this.sendPersistent<T>(payload, id, timeoutMs, 0);
  }

  public async close(): Promise<void> {
    if (this.closed) {
      return;
    }
    this.closed = true;
    this.clearIdleTimer();

    const closeErr = new ArtseekError('TRANSPORT_CLOSED', 'Artseek transport closed');
    for (const [id, entry] of this.pending) {
      this.pending.delete(id);
      if (entry.timer) clearTimeout(entry.timer);
      entry.reject(closeErr);
    }

    const proc = this.proc;
    this.proc = null;
    if (proc) {
      await this.terminateProcess(proc);
    }
  }

  private async requestOneShot<T>(
    payload: ArtseekRequest,
    id: string,
    timeoutMs: number,
  ): Promise<ArtseekResponse<T>> {
    if (this.beforeSpawn) {
      await this.beforeSpawn();
    }
    if (this.closed) {
      throw new ArtseekError('TRANSPORT_CLOSED', 'Artseek transport has been closed');
    }

    const proc = this.spawnProcess();
    this.intentionalClosingProcs.add(proc);

    return new Promise<ArtseekResponse<T>>((resolve, reject) => {
      let stdoutBuf = '';
      let stderrBuf = '';
      let settled = false;

      const finish = (fn: () => void) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        fn();
      };

      const timer = setTimeout(() => {
        finish(() => {
          void this.terminateProcess(proc);
          const stderrNote = stderrBuf.trim() ? ` | stderr: ${stderrBuf.trim()}` : '';
          reject(
            new ArtseekError(
              'TIMEOUT',
              `One-shot request ${id} (${payload.op}) timed out after ${timeoutMs}ms${stderrNote}`,
              { requestId: id },
            ),
          );
        });
      }, timeoutMs);

      proc.stdin.on('error', () => {
        // Handled by process close/error events.
      });

      proc.stderr.setEncoding('utf8');
      proc.stderr.on('data', (chunk: string) => {
        stderrBuf = appendRolling(stderrBuf, chunk);
      });

      proc.stdout.setEncoding('utf8');
      proc.stdout.on('data', (chunk: string) => {
        stdoutBuf += chunk;
        let nlIdx: number;
        while ((nlIdx = stdoutBuf.indexOf('\n')) !== -1) {
          const line = stdoutBuf.slice(0, nlIdx).replace(/\r$/, '').trim();
          stdoutBuf = stdoutBuf.slice(nlIdx + 1);
          if (!line) continue;
          const parsed = this.tryParseResponseLine(line);
          if (parsed) {
            finish(() => resolve(parsed as ArtseekResponse<T>));
            return;
          }
          stderrBuf = appendRolling(stderrBuf, `${line}\n`);
        }
      });

      proc.on('error', (err) => {
        finish(() => {
          reject(
            new ArtseekError(
              'TRANSPORT_ERROR',
              `Failed to spawn artseek process: ${err.message}`,
              { requestId: id, cause: err },
            ),
          );
        });
      });

      proc.on('close', (code, signal) => {
        const trailing = stdoutBuf.replace(/\r$/, '').trim();
        if (!settled && trailing) {
          const parsed = this.tryParseResponseLine(trailing);
          if (parsed) {
            finish(() => resolve(parsed as ArtseekResponse<T>));
            return;
          }
          stderrBuf = appendRolling(stderrBuf, `${trailing}\n`);
        }
        finish(() => {
          const stderrNote = stderrBuf.trim() ? ` | stderr: ${stderrBuf.trim()}` : '';
          reject(
            new ArtseekError(
              'TRANSPORT_ERROR',
              `One-shot artseek process exited without response (code=${code}, signal=${signal})${stderrNote}`,
              { requestId: id },
            ),
          );
        });
      });

      try {
        proc.stdin.write(`${JSON.stringify(payload)}\n`);
        proc.stdin.end();
      } catch (err) {
        finish(() => {
          reject(
            new ArtseekError(
              'TRANSPORT_ERROR',
              `Failed to write one-shot request ${id}: ${(err as Error).message}`,
              { requestId: id, cause: err },
            ),
          );
        });
      }
    });
  }

  private async sendPersistent<T>(
    payload: ArtseekRequest,
    id: string,
    timeoutMs: number,
    attempt: number,
  ): Promise<ArtseekResponse<T>> {
    this.clearIdleTimer();
    const proc = await this.ensureProcess();

    if (this.closed) {
      throw new ArtseekError('TRANSPORT_CLOSED', 'Artseek transport has been closed');
    }

    return new Promise<ArtseekResponse<T>>((resolve, reject) => {
      const entry: PendingRequest = {
        id,
        payload,
        proc,
        attempt,
        timeoutMs,
        timer: null,
        resolve,
        reject,
      };

      entry.timer = setTimeout(() => {
        if (!this.pending.has(id)) return;
        this.pending.delete(id);
        const stderrNote = this.stderrBuffer.trim()
          ? ` | stderr: ${this.stderrBuffer.trim()}`
          : '';
        reject(
          new ArtseekError(
            'TIMEOUT',
            `Request ${id} (${payload.op}) timed out after ${timeoutMs}ms${stderrNote}`,
            { requestId: id },
          ),
        );
        this.scheduleIdleCloseIfNeeded();
      }, timeoutMs);

      this.pending.set(id, entry);

      try {
        proc.stdin.write(`${JSON.stringify(payload)}\n`, (writeErr) => {
          if (writeErr && this.pending.get(id) === entry) {
            this.handleWriteFailure(entry, writeErr);
          }
        });
      } catch (err) {
        if (this.pending.get(id) === entry) {
          this.handleWriteFailure(entry, err as Error);
        }
      }
    });
  }

  private handleWriteFailure(entry: PendingRequest, err: Error): void {
    this.pending.delete(entry.id);
    if (entry.timer) clearTimeout(entry.timer);

    if (this.proc === entry.proc) {
      this.proc = null;
    }

    if (!this.closed && entry.attempt < this.maxReconnectAttempts) {
      this.sendPersistent(entry.payload, entry.id, entry.timeoutMs, entry.attempt + 1)
        .then(entry.resolve, entry.reject);
      return;
    }

    const stderrNote = this.stderrBuffer.trim() ? ` | stderr: ${this.stderrBuffer.trim()}` : '';
    entry.reject(
      new ArtseekError(
        'TRANSPORT_ERROR',
        `Failed to write request ${entry.id}: ${err.message}${stderrNote}`,
        { requestId: entry.id, cause: err },
      ),
    );
    this.scheduleIdleCloseIfNeeded();
  }

  private async ensureProcess(): Promise<ChildProcessWithoutNullStreams> {
    if (this.proc && this.proc.exitCode === null && !this.proc.killed && !this.proc.stdin.destroyed) {
      return this.proc;
    }
    if (this.startingPromise) {
      return this.startingPromise;
    }

    this.startingPromise = (async () => {
      if (this.beforeSpawn) {
        await this.beforeSpawn();
      }
      if (this.closed) {
        throw new ArtseekError('TRANSPORT_CLOSED', 'Artseek transport has been closed');
      }

      const proc = this.spawnProcess();
      this.proc = proc;
      this.stdoutBuffer = '';
      this.stderrBuffer = '';

      proc.stdin.on('error', (err) => {
        this.stderrBuffer = appendRolling(this.stderrBuffer, `[stdin error] ${err.message}\n`);
      });

      proc.stderr.setEncoding('utf8');
      proc.stderr.on('data', (chunk: string) => {
        this.stderrBuffer = appendRolling(this.stderrBuffer, chunk);
      });

      proc.stdout.setEncoding('utf8');
      proc.stdout.on('data', (chunk: string) => {
        if (this.proc !== proc) return;
        this.handleStdoutChunk(chunk);
      });

      proc.on('error', (err) => {
        this.handleProcessExit(proc, null, null, err);
      });

      proc.on('close', (code, signal) => {
        this.handleProcessExit(proc, code, signal, null);
      });

      return proc;
    })();

    try {
      return await this.startingPromise;
    } finally {
      this.startingPromise = null;
    }
  }

  private handleStdoutChunk(chunk: string): void {
    this.stdoutBuffer += chunk;
    let nlIdx: number;
    while ((nlIdx = this.stdoutBuffer.indexOf('\n')) !== -1) {
      const rawLine = this.stdoutBuffer.slice(0, nlIdx).replace(/\r$/, '').trim();
      this.stdoutBuffer = this.stdoutBuffer.slice(nlIdx + 1);
      if (!rawLine) continue;

      const parsed = this.tryParseResponseLine(rawLine);
      if (!parsed) {
        this.stderrBuffer = appendRolling(this.stderrBuffer, `[stdout non-json] ${rawLine}\n`);
        continue;
      }

      const idKey = parsed.id !== undefined ? String(parsed.id) : undefined;
      let entry: PendingRequest | undefined;
      if (idKey !== undefined && this.pending.has(idKey)) {
        entry = this.pending.get(idKey);
        this.pending.delete(idKey);
      } else if (idKey === undefined && this.pending.size === 1) {
        const first = this.pending.entries().next().value as [string, PendingRequest] | undefined;
        if (first) {
          this.pending.delete(first[0]);
          entry = first[1];
        }
      }

      if (entry) {
        if (entry.timer) clearTimeout(entry.timer);
        entry.resolve(parsed);
        this.scheduleIdleCloseIfNeeded();
      }
    }
  }

  private handleProcessExit(
    proc: ChildProcessWithoutNullStreams,
    code: number | null,
    signal: NodeJS.Signals | null,
    spawnErr: Error | null,
  ): void {
    const wasCurrent = this.proc === proc;
    if (wasCurrent) {
      // Flush any trailing unterminated stdout line before failing pending entries.
      const trailing = this.stdoutBuffer.replace(/\r$/, '').trim();
      if (trailing) {
        this.stdoutBuffer = '';
        const parsed = this.tryParseResponseLine(trailing);
        if (parsed && parsed.id !== undefined) {
          const key = String(parsed.id);
          const match = this.pending.get(key);
          if (match && match.proc === proc) {
            this.pending.delete(key);
            if (match.timer) clearTimeout(match.timer);
            match.resolve(parsed);
          }
        }
      }
      this.proc = null;
    }

    const intentional = this.closed || this.intentionalClosingProcs.has(proc);
    const affected: PendingRequest[] = [];
    for (const [id, entry] of this.pending) {
      if (entry.proc === proc) {
        this.pending.delete(id);
        if (entry.timer) clearTimeout(entry.timer);
        affected.push(entry);
      }
    }

    if (affected.length === 0) {
      return;
    }

    const stderrSnapshot = this.stderrBuffer.trim();
    const reason = spawnErr
      ? `Artseek process error: ${spawnErr.message}`
      : `Artseek process exited unexpectedly (code=${code}, signal=${signal})`;
    const fullMessage = stderrSnapshot ? `${reason} | stderr: ${stderrSnapshot}` : reason;

    for (const entry of affected) {
      if (!intentional && entry.attempt < this.maxReconnectAttempts) {
        this.sendPersistent(entry.payload, entry.id, entry.timeoutMs, entry.attempt + 1)
          .then(entry.resolve, entry.reject);
      } else {
        entry.reject(
          new ArtseekError(
            this.closed ? 'TRANSPORT_CLOSED' : 'TRANSPORT_ERROR',
            fullMessage,
            { requestId: entry.id, cause: spawnErr ?? undefined },
          ),
        );
      }
    }
  }

  private tryParseResponseLine(line: string): ArtseekResponse<any> | null {
    if (!line.startsWith('{')) {
      return null;
    }
    try {
      const obj = JSON.parse(line);
      if (obj && typeof obj === 'object' && typeof obj.ok === 'boolean') {
        return obj as ArtseekResponse<any>;
      }
      return null;
    } catch {
      return null;
    }
  }

  private clearIdleTimer(): void {
    if (this.idleTimer) {
      clearTimeout(this.idleTimer);
      this.idleTimer = null;
    }
  }

  private scheduleIdleCloseIfNeeded(): void {
    this.clearIdleTimer();
    if (this.closed || this.oneShot || this.idleTimeoutMs <= 0 || this.pending.size > 0 || !this.proc) {
      return;
    }
    this.idleTimer = setTimeout(() => {
      this.idleTimer = null;
      if (this.pending.size === 0 && this.proc) {
        const proc = this.proc;
        this.proc = null;
        void this.terminateProcess(proc);
      }
    }, this.idleTimeoutMs);
    this.idleTimer.unref?.();
  }

  private terminateProcess(proc: ChildProcessWithoutNullStreams): Promise<void> {
    this.intentionalClosingProcs.add(proc);
    if (proc.exitCode !== null) {
      return Promise.resolve();
    }

    return new Promise<void>((resolve) => {
      let done = false;
      const finish = () => {
        if (done) return;
        done = true;
        clearTimeout(killTimer);
        resolve();
      };

      proc.once('close', finish);
      proc.once('exit', finish);

      try {
        if (!proc.stdin.destroyed) {
          proc.stdin.end();
        }
      } catch {
        // Ignore write/end errors while shutting down.
      }

      const killTimer = setTimeout(() => {
        try {
          proc.kill('SIGKILL');
        } catch {
          // Process may have already exited.
        }
        finish();
      }, 1500);
      killTimer.unref?.();
    });
  }
}

// ---------------------------------------------------------------------------
// ADB / On-Device Persistent Stdio Transport
// ---------------------------------------------------------------------------

export interface AdbStdioTransportOptions {
  /** ADB device serial (`adb -s <serial>`). */
  serial?: string;
  /** Path to `adb` executable (default: `'adb'`). */
  adbPath?: string;
  /** Run directly on-device via local `su -c` instead of `adb shell`. */
  onDevice?: boolean;
  /** Root elevation command prefix (default: `'su -c'`). Set `''` to run without `su`. */
  suCommand?: string;
  /** Explicit path to the native Android arm64 `artseek` binary to push. */
  binaryPath?: string;
  /** Remote directory on device (default: `'/data/local/tmp/.artseek'`). */
  remoteDir?: string;
  /** Remote binary path on device (default: `'<remoteDir>/artseek'`). */
  remoteBinaryPath?: string;
  /** Optional `--cache-dir` passed to `artseek`. */
  cacheDir?: string;
  /** Force pushing the binary even if size and mtime match on device. */
  forcePush?: boolean;
  /** Skip automatic binary push/verification step. */
  skipPush?: boolean;
  /** Default per-request timeout in ms (default: 30000). */
  defaultTimeoutMs?: number;
  /** Auto-close idle persistent child after N ms and transparently respawn on next query. */
  idleTimeoutMs?: number;
  /** When true, spawns a fresh process per request instead of keeping a persistent session. */
  oneShot?: boolean;
  /** Max automatic reconnect attempts if child dies unexpectedly mid-request (default: 1). */
  maxReconnectAttempts?: number;
}

/**
 * Persistent NDJSON stdio transport over `adb [-s serial] shell "su -c '/data/local/tmp/.artseek/artseek'"`
 * (or local `su -c` when `onDevice: true`). Automatically ensures the native binary is deployed
 * to `/data/local/tmp/.artseek/artseek` with `0700` directory and binary permissions.
 */
export class AdbStdioTransport implements ArtseekTransport {
  private readonly options: Required<
    Pick<AdbStdioTransportOptions, 'adbPath' | 'onDevice' | 'suCommand' | 'remoteDir' | 'remoteBinaryPath' | 'forcePush' | 'skipPush'>
  > &
    AdbStdioTransportOptions;
  private readonly inner: StdioTransport;
  private binaryEnsured = false;
  private ensurePromise: Promise<void> | null = null;

  constructor(options: AdbStdioTransportOptions = {}) {
    const remoteDir = options.remoteDir ?? DEFAULT_REMOTE_DIR;
    const remoteBinaryPath = options.remoteBinaryPath ?? `${remoteDir}/artseek`;

    this.options = {
      ...options,
      adbPath: options.adbPath ?? 'adb',
      onDevice: options.onDevice ?? false,
      suCommand: options.suCommand ?? 'su -c',
      remoteDir,
      remoteBinaryPath,
      forcePush: options.forcePush ?? false,
      skipPush: options.skipPush ?? false,
    };

    this.inner = new StdioTransport({
      beforeSpawn: () => this.ensureBinary(),
      spawnProcess: () => this.spawnArtseek(),
      defaultTimeoutMs: options.defaultTimeoutMs,
      idleTimeoutMs: options.idleTimeoutMs,
      oneShot: options.oneShot,
      maxReconnectAttempts: options.maxReconnectAttempts,
    });
  }

  public get isRunning(): boolean {
    return this.inner.isRunning;
  }

  public request<T = unknown>(req: ArtseekRequest): Promise<ArtseekResponse<T>> {
    return this.inner.request<T>(req);
  }

  public close(): Promise<void> {
    return this.inner.close();
  }

  /**
   * Ensures the native binary is deployed to `remoteBinaryPath` with `0700`
   * permissions on both directory and executable. Skips push when already
   * present with matching size and mtime unless `forcePush: true`.
   */
  public async ensureBinary(): Promise<void> {
    if (this.options.onDevice || this.options.skipPush || this.binaryEnsured) {
      return;
    }
    if (this.ensurePromise) {
      return this.ensurePromise;
    }

    this.ensurePromise = this.ensureBinaryImpl();
    try {
      await this.ensurePromise;
      this.binaryEnsured = true;
    } finally {
      this.ensurePromise = null;
    }
  }

  private adbArgs(extra: string[]): string[] {
    return this.options.serial ? ['-s', this.options.serial, ...extra] : extra;
  }

  private wrapSu(command: string): string {
    const su = this.options.suCommand.trim();
    if (!su) return command;
    return `${su} ${shellQuoteSingle(command)}`;
  }

  private async statRemoteBinary(): Promise<{ size: number; mtimeSec: number } | null> {
    const statCmd = `stat -c "%s %Y" ${shellQuoteSingle(this.options.remoteBinaryPath)} 2>/dev/null || true`;
    const { stdout } = await execFileAsync(
      this.options.adbPath,
      this.adbArgs(['shell', this.wrapSu(statCmd)]),
      15_000,
    );
    const match = stdout.trim().match(/^(\d+)\s+(\d+)$/);
    if (!match) return null;
    return { size: Number(match[1]), mtimeSec: Number(match[2]) };
  }

  private async ensureBinaryImpl(): Promise<void> {
    const localBinary = resolveDefaultBinaryPath(this.options.binaryPath);
    const remoteStat = this.options.forcePush ? null : await this.statRemoteBinary();

    if (!localBinary) {
      if (remoteStat && remoteStat.size > 0) {
        return;
      }
      throw new ArtseekError(
        'TRANSPORT_ERROR',
        `Native artseek binary not found locally or at ${this.options.remoteBinaryPath}. ` +
          `Build with xmake or specify options.binaryPath.`,
      );
    }

    const localStat = await fsp.stat(localBinary);
    const localSize = localStat.size;
    const localMtimeSec = Math.floor(localStat.mtimeMs / 1000);

    if (
      !this.options.forcePush &&
      remoteStat &&
      remoteStat.size === localSize &&
      Math.abs(remoteStat.mtimeSec - localMtimeSec) <= 2
    ) {
      return;
    }

    // Push to a staging path in /data/local/tmp (writable by shell uid 2000),
    // then atomically move into the 0700 root-owned directory with 0700 mode.
    const stagingPath = `/data/local/tmp/.artseek_upload_${process.pid}_${Date.now()}`;
    await execFileAsync(
      this.options.adbPath,
      this.adbArgs(['push', localBinary, stagingPath]),
      60_000,
    );

    const dirQ = shellQuoteSingle(this.options.remoteDir);
    const binQ = shellQuoteSingle(this.options.remoteBinaryPath);
    const stageQ = shellQuoteSingle(stagingPath);
    const installCmd =
      `mkdir -p ${dirQ} && chmod 700 ${dirQ} && ` +
      `mv -f ${stageQ} ${binQ} && chmod 700 ${binQ}`;

    try {
      await execFileAsync(
        this.options.adbPath,
        this.adbArgs(['shell', this.wrapSu(installCmd)]),
        15_000,
      );
    } catch (err) {
      await execFileAsync(
        this.options.adbPath,
        this.adbArgs(['shell', `rm -f ${stageQ}`]),
        5_000,
      ).catch(() => {});
      throw err;
    }
  }

  private buildInnerCommand(): string {
    const parts = [this.options.remoteBinaryPath];
    if (this.options.cacheDir) {
      parts.push('--cache-dir', this.options.cacheDir);
    }
    return parts.join(' ');
  }

  private spawnArtseek(): ChildProcessWithoutNullStreams {
    const innerCmd = this.buildInnerCommand();

    if (this.options.onDevice) {
      const su = this.options.suCommand.trim();
      if (su) {
        const suParts = su.split(/\s+/);
        const [suBin, ...suFlags] = suParts;
        return spawn(suBin, [...suFlags, innerCmd], {
          stdio: ['pipe', 'pipe', 'pipe'],
          windowsHide: true,
        });
      }
      const args = this.options.cacheDir ? ['--cache-dir', this.options.cacheDir] : [];
      return spawn(this.options.remoteBinaryPath, args, {
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true,
      });
    }

    const remoteShellCmd = this.wrapSu(innerCmd);
    return spawn(this.options.adbPath, this.adbArgs(['shell', remoteShellCmd]), {
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    });
  }
}
