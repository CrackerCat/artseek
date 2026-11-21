import {
  ArtseekError,
  type ArtseekOp,
  type ArtseekRequest,
  type ArtseekTransport,
  type BatchResult,
  type BatchSubQuery,
  type TargetSpec,
} from './types.js';

type BatchableOp = Exclude<ArtseekOp, 'batch'>;

interface QueuedCall {
  op: BatchableOp;
  params: Record<string, unknown>;
  timeoutMs?: number;
  resolve: (value: any) => void;
  reject: (err: Error) => void;
}

interface BatchBucket {
  target: TargetSpec;
  items: QueuedCall[];
}

function makeBucketKey(target: TargetSpec): string {
  return JSON.stringify([
    target.pkg ?? null,
    target.pid ?? null,
    Boolean(target.freeze),
    target.madvise ?? true,
  ]);
}

function buildTargetEnvelope(target: TargetSpec, timeoutMs?: number): Partial<ArtseekRequest> {
  const envelope: Partial<ArtseekRequest> = {};
  if (target.pid !== undefined) envelope.pid = target.pid;
  if (target.pkg !== undefined) envelope.pkg = target.pkg;
  if (target.freeze !== undefined) envelope.freeze = target.freeze;
  if (target.madvise !== undefined) envelope.madvise = target.madvise;
  if (timeoutMs !== undefined) envelope.timeoutMs = timeoutMs;
  return envelope;
}

/**
 * Coalesces multiple Proxy queries targeting the same `(pkg, pid, freeze, madvise)`
 * queued in the same microtask turn into a single `{ op: 'batch', params: { queries } }`
 * request. Single-item turns bypass the batch wrapper and send `{ op, params }` directly.
 */
export class MicrotaskBatcher {
  private readonly transport: ArtseekTransport;
  private readonly buckets = new Map<string, BatchBucket>();

  constructor(transport: ArtseekTransport) {
    this.transport = transport;
  }

  public enqueue<T = unknown>(
    target: TargetSpec,
    op: BatchableOp,
    params: Record<string, unknown>,
    batching = true,
  ): Promise<T> {
    if (!batching) {
      return this.sendSingle<T>(target, op, params, target.timeoutMs);
    }

    const key = makeBucketKey(target);
    let bucket = this.buckets.get(key);
    const shouldSchedule = !bucket;

    if (!bucket) {
      bucket = {
        target: {
          pid: target.pid,
          pkg: target.pkg,
          freeze: target.freeze,
          madvise: target.madvise,
        },
        items: [],
      };
      this.buckets.set(key, bucket);
    }

    const promise = new Promise<T>((resolve, reject) => {
      bucket!.items.push({
        op,
        params,
        timeoutMs: target.timeoutMs,
        resolve,
        reject,
      });
    });

    if (shouldSchedule) {
      queueMicrotask(() => {
        void this.flush(key);
      });
    }

    return promise;
  }

  private async sendSingle<T>(
    target: TargetSpec,
    op: BatchableOp,
    params: Record<string, unknown>,
    timeoutMs?: number,
  ): Promise<T> {
    const res = await this.transport.request<T>({
      op,
      ...buildTargetEnvelope(target, timeoutMs),
      params,
    });
    if (!res.ok) {
      throw new ArtseekError(
        res.error?.code ?? 'INTERNAL_ERROR',
        res.error?.message ?? `artseek ${op} request failed`,
        { meta: res.meta, requestId: res.id },
      );
    }
    return res.data;
  }

  private async flush(key: string): Promise<void> {
    const bucket = this.buckets.get(key);
    if (!bucket) return;
    this.buckets.delete(key);

    const { target, items } = bucket;
    if (items.length === 0) return;

    // Single query in this tick: send directly without unnecessary batch wrapper.
    if (items.length === 1) {
      const only = items[0];
      try {
        const data = await this.sendSingle(target, only.op, only.params, only.timeoutMs);
        only.resolve(data);
      } catch (err) {
        only.reject(err as Error);
      }
      return;
    }

    let maxTimeout: number | undefined;
    for (const item of items) {
      if (item.timeoutMs !== undefined) {
        maxTimeout = maxTimeout === undefined ? item.timeoutMs : Math.max(maxTimeout, item.timeoutMs);
      }
    }

    const queries: BatchSubQuery[] = items.map((item, idx) => ({
      id: `b_${idx}`,
      op: item.op,
      params: item.params,
    }));

    try {
      const res = await this.transport.request<BatchResult>({
        op: 'batch',
        ...buildTargetEnvelope(target, maxTimeout),
        params: { queries },
      });

      if (!res.ok) {
        const err = new ArtseekError(
          res.error?.code ?? 'INTERNAL_ERROR',
          res.error?.message ?? 'artseek batch request failed',
          { meta: res.meta, requestId: res.id },
        );
        for (const item of items) {
          item.reject(err);
        }
        return;
      }

      const rawResults = Array.isArray(res.data?.results) ? res.data.results : [];
      const byId = new Map<string, (typeof rawResults)[number]>();
      for (const entry of rawResults) {
        if (entry?.id !== undefined) {
          byId.set(String(entry.id), entry);
        }
      }

      for (let i = 0; i < items.length; i++) {
        const item = items[i];
        const subId = queries[i].id!;
        const sub = byId.get(subId) ?? rawResults[i];
        if (!sub) {
          item.reject(
            new ArtseekError(
              'INTERNAL_ERROR',
              `Missing batch sub-result for index ${i} (${subId})`,
              { meta: res.meta, requestId: res.id },
            ),
          );
          continue;
        }
        if (sub.ok) {
          item.resolve(sub.data);
        } else {
          item.reject(
            new ArtseekError(
              sub.error?.code ?? 'INTERNAL_ERROR',
              sub.error?.message ?? `Batch sub-query ${subId} (${item.op}) failed`,
              { meta: res.meta, requestId: res.id },
            ),
          );
        }
      }
    } catch (err) {
      const error = err instanceof Error ? err : new Error(String(err));
      for (const item of items) {
        item.reject(error);
      }
    }
  }
}
