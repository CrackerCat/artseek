import { describe, expect, it, vi } from 'vitest';
import { ArtseekError, createArtseekClient } from '../src/index.js';
import type { ArtseekRequest, ArtseekResponse, ArtseekTransport, BatchSubQuery } from '../src/types.js';

function createBatchAwareTransport() {
  const requests: ArtseekRequest[] = [];
  const transport: ArtseekTransport = {
    request: vi.fn(async (req: ArtseekRequest): Promise<ArtseekResponse<any>> => {
      requests.push(req);
      if (req.op === 'batch') {
        const queries = (req.params?.queries ?? []) as BatchSubQuery[];
        return {
          ok: true,
          meta: { pid: 1001, elapsedMs: 5 },
          data: {
            results: queries.map((q) => {
              const steps = (q.params as any)?.steps ?? [];
              const firstField = steps[0]?.name ?? 'unknown';
              if (firstField === 'broken') {
                return {
                  id: q.id,
                  ok: false,
                  error: { code: 'STALE_REF', message: 'object moved' },
                };
              }
              return {
                id: q.id,
                ok: true,
                data: `${req.pkg ?? req.pid}:${firstField}`,
              };
            }),
          },
          error: null,
        };
      }
      if (req.op === 'query') {
        const steps = (req.params as any)?.steps ?? [];
        const firstField = steps[0]?.name ?? 'unknown';
        return {
          ok: true,
          meta: { pid: 1001, elapsedMs: 2 },
          data: `single:${firstField}`,
          error: null,
        };
      }
      throw new Error(`Unexpected op: ${req.op}`);
    }),
    close: vi.fn(async () => {}),
  };
  return { transport, requests };
}

describe('MicrotaskBatcher automatic Promise.all coalescing', () => {
  it('coalesces 3 concurrent Proxy queries in Promise.all into ONE batch request', async () => {
    const { transport, requests } = createBatchAwareTransport();
    const heap = createArtseekClient({ transport, pkg: 'com.example.shop' });

    const [a, b, c] = await Promise.all([
      heap.currentActivity.a,
      heap.currentActivity.b,
      heap.currentActivity.c,
    ]);

    expect([a, b, c]).toEqual([
      'com.example.shop:a',
      'com.example.shop:b',
      'com.example.shop:c',
    ]);
    expect(requests).toHaveLength(1);
    expect(requests[0].op).toBe('batch');
    expect(requests[0].pkg).toBe('com.example.shop');
    expect((requests[0].params as any).queries).toHaveLength(3);
  });

  it('sends a single query directly without batch wrapper when only 1 query is queued', async () => {
    const { transport, requests } = createBatchAwareTransport();
    const heap = createArtseekClient({ transport, pkg: 'com.example.shop' });

    const result = await heap.currentActivity.onlyOne;
    expect(result).toBe('single:onlyOne');
    expect(requests).toHaveLength(1);
    expect(requests[0].op).toBe('query');
  });

  it('groups queries separately by (pkg, pid, freeze)', async () => {
    const { transport, requests } = createBatchAwareTransport();
    const client = createArtseekClient({ transport });

    const appA = client.forTarget({ pkg: 'com.app.a', freeze: false });
    const appAFrozen = client.forTarget({ pkg: 'com.app.a', freeze: true });
    const appB = client.forTarget({ pkg: 'com.app.b' });

    const results = await Promise.all([
      appA.currentActivity.x,
      appA.currentActivity.y,
      appAFrozen.currentActivity.z,
      appB.currentActivity.w,
    ]);

    expect(results).toEqual(['com.app.a:x', 'com.app.a:y', 'single:z', 'single:w']);
    expect(requests).toHaveLength(3);
    expect(requests.map((r) => ({ op: r.op, pkg: r.pkg, freeze: r.freeze }))).toEqual([
      { op: 'batch', pkg: 'com.app.a', freeze: false },
      { op: 'query', pkg: 'com.app.a', freeze: true },
      { op: 'query', pkg: 'com.app.b', freeze: undefined },
    ]);
  });

  it('isolates per-item errors inside a batch cleanly', async () => {
    const { transport, requests } = createBatchAwareTransport();
    const heap = createArtseekClient({ transport, pkg: 'com.example.shop' });

    const settled = await Promise.allSettled([
      heap.currentActivity.good1,
      heap.currentActivity.broken,
      heap.currentActivity.good2,
    ]);

    expect(requests).toHaveLength(1);
    expect(requests[0].op).toBe('batch');
    expect(settled[0]).toEqual({ status: 'fulfilled', value: 'com.example.shop:good1' });
    expect(settled[2]).toEqual({ status: 'fulfilled', value: 'com.example.shop:good2' });
    expect(settled[1].status).toBe('rejected');
    const err = (settled[1] as PromiseRejectedResult).reason;
    expect(err).toBeInstanceOf(ArtseekError);
    expect(err.code).toBe('STALE_REF');
  });

  it('bypasses batching when batching: false is configured', async () => {
    const { transport, requests } = createBatchAwareTransport();
    const heap = createArtseekClient({ transport, pkg: 'com.example.shop', batching: false });

    const [a, b, c] = await Promise.all([
      heap.currentActivity.a,
      heap.currentActivity.b,
      heap.currentActivity.c,
    ]);

    expect([a, b, c]).toEqual(['single:a', 'single:b', 'single:c']);
    expect(requests).toHaveLength(3);
    expect(requests.every((r) => r.op === 'query')).toBe(true);
  });

  it('coalesces Promise.all even with an async resolveTarget callback and deduplicates resolver calls', async () => {
    const { transport, requests } = createBatchAwareTransport();
    const resolveTarget = vi.fn(async () => ({ pkg: 'com.dynamic.app' }));
    const heap = createArtseekClient({ transport, resolveTarget });

    const [a, b] = await Promise.all([
      heap.currentActivity.alpha,
      heap.currentActivity.beta,
    ]);

    expect([a, b]).toEqual(['com.dynamic.app:alpha', 'com.dynamic.app:beta']);
    expect(resolveTarget).toHaveBeenCalledTimes(1);
    expect(requests).toHaveLength(1);
    expect(requests[0].op).toBe('batch');
    expect(requests[0].pkg).toBe('com.dynamic.app');
  });
});
