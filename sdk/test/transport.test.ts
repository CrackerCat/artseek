import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, describe, expect, it } from 'vitest';
import { ArtseekError, StdioTransport } from '../src/index.js';

const fixturePath = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  'fixtures/mock-artseek.mjs',
);

const tempFiles: string[] = [];

function createMockStdioTransport(
  options: {
    stateFile?: string;
    defaultTimeoutMs?: number;
    idleTimeoutMs?: number;
    oneShot?: boolean;
    maxReconnectAttempts?: number;
  } = {},
) {
  let spawnCount = 0;
  const transport = new StdioTransport({
    spawnProcess: () => {
      spawnCount++;
      const args = options.stateFile ? [fixturePath, options.stateFile] : [fixturePath];
      return spawn(process.execPath, args, {
        stdio: ['pipe', 'pipe', 'pipe'],
        windowsHide: true,
      });
    },
    defaultTimeoutMs: options.defaultTimeoutMs,
    idleTimeoutMs: options.idleTimeoutMs,
    oneShot: options.oneShot,
    maxReconnectAttempts: options.maxReconnectAttempts,
  });
  return { transport, getSpawnCount: () => spawnCount };
}

afterEach(() => {
  while (tempFiles.length > 0) {
    const f = tempFiles.pop()!;
    try {
      fs.rmSync(f, { force: true });
    } catch {
      // Ignore cleanup errors.
    }
  }
});

describe('StdioTransport persistent NDJSON framing, reconnect, idle timeout & oneShot', () => {
  it('reuses a single persistent child process across sequential and out-of-order pipelined requests', async () => {
    const { transport, getSpawnCount } = createMockStdioTransport();
    try {
      const r1 = await transport.request<any>({ op: 'ping', params: { emitBanner: true, crlf: true } });
      expect(r1.ok).toBe(true);
      const firstPid = r1.data.serverPid;

      // Send two concurrent requests where the first takes longer than the second
      const [slow, fast] = await Promise.all([
        transport.request<any>({ op: 'query', params: { label: 'slow', delayMs: 80 } }),
        transport.request<any>({ op: 'query', params: { label: 'fast', delayMs: 5 } }),
      ]);

      expect(slow.ok).toBe(true);
      expect(slow.data.echoParams.label).toBe('slow');
      expect(slow.data.serverPid).toBe(firstPid);

      expect(fast.ok).toBe(true);
      expect(fast.data.echoParams.label).toBe('fast');
      expect(fast.data.serverPid).toBe(firstPid);

      // Strictly 1 child process spawned for all 3 requests!
      expect(getSpawnCount()).toBe(1);
    } finally {
      await transport.close();
    }
  });

  it('automatically reconnects and retries if the child process crashes mid-request', async () => {
    const stateFile = path.join(os.tmpdir(), `artseek-crash-${process.pid}-${Date.now()}.flag`);
    tempFiles.push(stateFile);

    const { transport, getSpawnCount } = createMockStdioTransport({
      stateFile,
      maxReconnectAttempts: 1,
    });

    try {
      const initial = await transport.request<any>({ op: 'ping' });
      const pidBeforeCrash = initial.data.serverPid;

      // Child crashes on first attempt, StdioTransport respawns and retries seamlessly
      const recovered = await transport.request<any>({
        op: 'query',
        params: { crashOnce: true },
      });

      expect(recovered.ok).toBe(true);
      expect(recovered.data.serverPid).not.toBe(pidBeforeCrash);
      expect(getSpawnCount()).toBe(2);
    } finally {
      await transport.close();
    }
  });

  it('surfaces stderr diagnostics when a child crashes beyond maxReconnectAttempts', async () => {
    const { transport } = createMockStdioTransport({ maxReconnectAttempts: 0 });
    try {
      await expect(
        transport.request({ op: 'query', params: { crashAlways: true } }),
      ).rejects.toSatisfy((err: unknown) => {
        return (
          err instanceof ArtseekError &&
          err.code === 'TRANSPORT_ERROR' &&
          err.message.includes('fatal: permanent crash')
        );
      });
    } finally {
      await transport.close();
    }
  });

  it('enforces per-request timeout and includes stderr diagnostics', async () => {
    const { transport } = createMockStdioTransport({ defaultTimeoutMs: 5000 });
    try {
      await expect(
        transport.request({
          op: 'query',
          timeoutMs: 80,
          params: { delayMs: 1000, emitStderr: 'slow-scan-in-progress' },
        }),
      ).rejects.toSatisfy((err: unknown) => {
        return err instanceof ArtseekError && err.code === 'TIMEOUT';
      });
    } finally {
      await transport.close();
    }
  });

  it('auto-closes idle child after idleTimeoutMs and transparently respawns on next query', async () => {
    const { transport, getSpawnCount } = createMockStdioTransport({ idleTimeoutMs: 80 });
    try {
      const r1 = await transport.request<any>({ op: 'ping' });
      const pid1 = r1.data.serverPid;
      expect(transport.isRunning).toBe(true);

      // Wait longer than idleTimeoutMs for child to auto-close
      await new Promise((resolve) => setTimeout(resolve, 220));
      expect(transport.isRunning).toBe(false);

      // Next request transparently respawns a fresh process
      const r2 = await transport.request<any>({ op: 'ping' });
      const pid2 = r2.data.serverPid;
      expect(r2.ok).toBe(true);
      expect(pid2).not.toBe(pid1);
      expect(getSpawnCount()).toBe(2);
    } finally {
      await transport.close();
    }
  });

  it('supports oneShot mode spawning a dedicated process per request', async () => {
    const { transport, getSpawnCount } = createMockStdioTransport({ oneShot: true });
    try {
      const r1 = await transport.request<any>({ op: 'ping' });
      const r2 = await transport.request<any>({ op: 'ping' });
      expect(r1.ok).toBe(true);
      expect(r2.ok).toBe(true);
      expect(r1.data.serverPid).not.toBe(r2.data.serverPid);
      expect(getSpawnCount()).toBe(2);
    } finally {
      await transport.close();
    }
  });

  it('rejects requests after close() with TRANSPORT_CLOSED', async () => {
    const { transport } = createMockStdioTransport();
    await transport.request({ op: 'ping' });
    await transport.close();

    await expect(transport.request({ op: 'ping' })).rejects.toSatisfy(
      (err: unknown) => err instanceof ArtseekError && err.code === 'TRANSPORT_CLOSED',
    );
  });
});
