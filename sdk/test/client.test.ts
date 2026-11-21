import { describe, expect, it, vi } from 'vitest';
import { ArtseekError, createArtseekClient } from '../src/index.js';
import type { ArtseekRequest, ArtseekResponse, ArtseekTransport } from '../src/types.js';

function createRecordingTransport(
  responder?: (req: ArtseekRequest) => ArtseekResponse<any>,
) {
  const requests: ArtseekRequest[] = [];
  const transport: ArtseekTransport = {
    request: vi.fn(async (req: ArtseekRequest) => {
      requests.push(req);
      if (responder) return responder(req);
      return { ok: true, meta: { elapsedMs: 1 }, data: { op: req.op, params: req.params }, error: null };
    }),
    close: vi.fn(async () => {}),
  };
  return { transport, requests };
}

describe('ArtseekClient direct operations, target scoping & error handling', () => {
  it('supports all direct operations and passes target envelopes accurately', async () => {
    const { transport, requests } = createRecordingTransport();
    const client = createArtseekClient({ transport });

    await client.ping();
    await client.calibrate();
    await client.selfcheck({ autoRecalibrate: false });

    const scoped = client.forTarget({ pkg: 'com.example.target', freeze: true });
    await scoped.roots();
    await scoped.tree({ onlyFocused: true, maxDepth: 8 });
    await scoped.strsearch({ needle: 'hello', limit: 5, includeOwnerPreview: true });
    await scoped.dumpobj({ addr: '0x100', depth: 2 });
    await scoped.whorefs({ addr: '0x100', limit: 10 });
    await scoped.instances({ type: '*Adapter', limit: 50 });
    await scoped.rawQuery({
      root: { kind: 'activity', which: 'current' },
      steps: [{ op: 'ref' }],
      collection: false,
    });
    await scoped.batch([{ id: 'q1', op: 'roots', params: {} }]);
    await scoped.close();

    expect(requests.map((r) => r.op)).toEqual([
      'ping',
      'calibrate',
      'selfcheck',
      'roots',
      'tree',
      'strsearch',
      'dumpobj',
      'whorefs',
      'instances',
      'query',
      'batch',
    ]);

    // ping/calibrate carry no pkg/pid
    expect(requests[0].pkg).toBeUndefined();
    expect(requests[1].pkg).toBeUndefined();

    // scoped calls carry pkg + freeze
    for (const req of requests.slice(3)) {
      expect(req.pkg).toBe('com.example.target');
      expect(req.freeze).toBe(true);
    }
    expect(transport.close).toHaveBeenCalledTimes(1);
  });

  it('throws ArtseekError(INVALID_QUERY) when target pkg/pid is missing', async () => {
    const { transport } = createRecordingTransport();
    const client = createArtseekClient({ transport });

    await expect(client.roots()).rejects.toSatisfy(
      (err: unknown) => err instanceof ArtseekError && err.code === 'INVALID_QUERY',
    );
    await expect(client.currentActivity.mTitle).rejects.toSatisfy(
      (err: unknown) => err instanceof ArtseekError && err.code === 'INVALID_QUERY',
    );
    expect(transport.request).not.toHaveBeenCalled();
  });

  it('surfaces protocol errors as ArtseekError with code, message, and meta', async () => {
    const { transport } = createRecordingTransport((req) => ({
      id: req.id ?? 'req_99',
      ok: false,
      meta: { elapsedMs: 3 },
      data: null,
      error: { code: 'PROCESS_NOT_FOUND', message: 'com.missing.app not running' },
    }));
    const client = createArtseekClient({ transport, pkg: 'com.missing.app' });

    await expect(client.roots()).rejects.toSatisfy((err: unknown) => {
      return (
        err instanceof ArtseekError &&
        err.code === 'PROCESS_NOT_FOUND' &&
        err.message === 'com.missing.app not running' &&
        err.meta?.elapsedMs === 3
      );
    });
  });

  it('does not leak parent pid when forTarget switches to pkg (and vice versa)', async () => {
    const { transport, requests } = createRecordingTransport();
    const base = createArtseekClient({ transport, pid: 1234 });
    const byPkg = base.forTarget({ pkg: 'com.other.app' });

    await byPkg.roots();
    expect(requests[0].pkg).toBe('com.other.app');
    expect(requests[0].pid).toBeUndefined();
  });
});
