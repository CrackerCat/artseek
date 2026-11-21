import { MicrotaskBatcher } from './batcher.js';
import { createHeapProxy, type HeapProxyExecutor } from './proxy.js';
import { AdbStdioTransport, type AdbStdioTransportOptions } from './transport.js';
import {
  ArtseekError,
  type ActivityInfo,
  type ArtseekOp,
  type ArtseekRequest,
  type ArtseekResponse,
  type ArtseekTransport,
  type BatchResult,
  type BatchSubQuery,
  type CalibrateResult,
  type DumpObjParams,
  type HeapExtensionFn,
  type HeapExtensionsMap,
  type HeapProxy,
  type InstancesParams,
  type InstancesResult,
  type PingResult,
  type QuerySpec,
  type RootsResult,
  type ScanOptions,
  type SelfcheckParams,
  type SelfcheckResult,
  type StrSearchParams,
  type StrSearchResult,
  type TargetSpec,
  type TreeParams,
  type TreeResult,
  type ViewSelector,
  type WhoRefsParams,
  type WhoRefsResult,
} from './types.js';

export type TargetResolver = () => TargetSpec | Promise<TargetSpec>;

export interface ArtseekClientOptions extends AdbStdioTransportOptions {
  /** Custom transport implementation (overrides automatic `AdbStdioTransport`). */
  transport?: ArtseekTransport;
  /** Default target Android package name. */
  pkg?: string;
  /** Default target process PID. */
  pid?: number;
  /** Default SIGSTOP freeze behavior during reads (default: `false`). */
  freeze?: boolean;
  /** Default `process_madvise(MADV_DONTNEED)` cleanup (default: `true`). */
  madvise?: boolean;
  /** Default per-request timeout in milliseconds. */
  timeoutMs?: number;
  /** Optional dynamic resolver invoked when no explicit `pkg`/`pid` is bound. */
  resolveTarget?: TargetResolver;
  /** Enable automatic microtask batching for HeapProxy queries (default: `true`). */
  batching?: boolean;
  /** Custom `$`-prefixed HeapProxy extension methods. */
  extensions?: HeapExtensionsMap;
}

interface InternalClientState {
  transport: ArtseekTransport;
  batcher: MicrotaskBatcher;
  defaultTarget: TargetSpec;
  resolveTarget?: TargetResolver;
  batching: boolean;
  extensions: Record<string, HeapExtensionFn>;
}

function mergeTargets(base: TargetSpec, override?: TargetSpec): TargetSpec {
  if (!override) return { ...base };
  const merged: TargetSpec = { ...base };
  if (override.pid !== undefined) merged.pid = override.pid;
  if (override.pkg !== undefined) merged.pkg = override.pkg;
  if (override.freeze !== undefined) merged.freeze = override.freeze;
  if (override.madvise !== undefined) merged.madvise = override.madvise;
  if (override.timeoutMs !== undefined) merged.timeoutMs = override.timeoutMs;
  return merged;
}

function validateAndCopyExtensions(
  extensions?: HeapExtensionsMap,
): Record<string, HeapExtensionFn> {
  const result: Record<string, HeapExtensionFn> = {};
  if (!extensions) return result;
  for (const [key, fn] of Object.entries(extensions)) {
    if (!key.startsWith('$')) {
      throw new ArtseekError(
        'INVALID_QUERY',
        `HeapProxy extension name must start with "$", got "${key}"`,
      );
    }
    if (typeof fn !== 'function') {
      throw new ArtseekError(
        'INVALID_QUERY',
        `HeapProxy extension "${key}" must be a function`,
      );
    }
    result[key] = fn;
  }
  return result;
}

function isPromiseLike<T>(value: unknown): value is PromiseLike<T> {
  return (
    value !== null &&
    (typeof value === 'object' || typeof value === 'function') &&
    typeof (value as PromiseLike<T>).then === 'function'
  );
}

export class ArtseekClient {
  public readonly transport: ArtseekTransport;
  private readonly batcher: MicrotaskBatcher;
  private readonly defaultTarget: TargetSpec;
  private readonly resolveTarget?: TargetResolver;
  private readonly batching: boolean;
  private extensions: Record<string, HeapExtensionFn>;
  private inFlightResolveTarget: Promise<TargetSpec> | null = null;
  private readonly proxyExecutor: HeapProxyExecutor;

  constructor(options: ArtseekClientOptions = {}, internal?: InternalClientState) {
    if (internal) {
      this.transport = internal.transport;
      this.batcher = internal.batcher;
      this.defaultTarget = { ...internal.defaultTarget };
      this.resolveTarget = internal.resolveTarget;
      this.batching = internal.batching;
      this.extensions = { ...internal.extensions };
    } else {
      const transport = options.transport ?? new AdbStdioTransport(options);
      this.transport = transport;
      this.batcher = new MicrotaskBatcher(transport);
      this.defaultTarget = {};
      if (options.pkg !== undefined) this.defaultTarget.pkg = options.pkg;
      if (options.pid !== undefined) this.defaultTarget.pid = options.pid;
      if (options.freeze !== undefined) this.defaultTarget.freeze = options.freeze;
      if (options.madvise !== undefined) this.defaultTarget.madvise = options.madvise;
      if (options.timeoutMs !== undefined) this.defaultTarget.timeoutMs = options.timeoutMs;
      this.resolveTarget = options.resolveTarget;
      this.batching = options.batching ?? true;
      this.extensions = validateAndCopyExtensions(options.extensions);
    }

    this.proxyExecutor = {
      executeQuery: <T>(spec: QuerySpec) =>
        this.enqueueForProxy<T>('query', spec as unknown as Record<string, unknown>),
      executeOp: <T>(op: Exclude<ArtseekOp, 'batch'>, params: Record<string, unknown>) =>
        this.enqueueForProxy<T>(op, params),
    };
  }

  /**
   * Returns a lightweight target-scoped client sharing the underlying transport
   * and microtask batcher, pre-bound to `{ pid, pkg, freeze, madvise, timeoutMs }`.
   */
  public forTarget(target: TargetSpec): ArtseekClient {
    const merged = mergeTargets(this.defaultTarget, target);
    const hasExplicitTarget = target.pid !== undefined || target.pkg !== undefined;
    // If explicitly binding pkg while parent had pid (or vice versa), avoid stale cross-binding.
    if (target.pkg !== undefined && target.pid === undefined) {
      delete merged.pid;
    } else if (target.pid !== undefined && target.pkg === undefined) {
      delete merged.pkg;
    }

    return new ArtseekClient(undefined, {
      transport: this.transport,
      batcher: this.batcher,
      defaultTarget: merged,
      resolveTarget: hasExplicitTarget ? undefined : this.resolveTarget,
      batching: this.batching,
      extensions: { ...this.extensions },
    });
  }

  /**
   * Registers custom `$`-prefixed HeapProxy extension methods on this client
   * and returns `this` for fluent chaining.
   */
  public extend(extensions: HeapExtensionsMap): this {
    const validated = validateAndCopyExtensions(extensions);
    this.extensions = { ...this.extensions, ...validated };
    return this;
  }

  // -------------------------------------------------------------------------
  // Lazy HeapProxy Entrypoints
  // -------------------------------------------------------------------------

  /** Current resumed/focused Activity (`root: { kind: 'activity', which: 'current' }`). */
  public get currentActivity(): HeapProxy<any> {
    return createHeapProxy(
      { root: { kind: 'activity', which: 'current' }, steps: [], collection: false },
      this.proxyExecutor,
      () => this.extensions,
    );
  }

  /** All live activities in the target process (`root: { kind: 'activity', which: 'all' }`). */
  public get activities(): HeapProxy<ActivityInfo[]> {
    return createHeapProxy<ActivityInfo[]>(
      { root: { kind: 'activity', which: 'all' }, steps: [], collection: true },
      this.proxyExecutor,
      () => this.extensions,
    );
  }

  /** Scans heap instances matching `type` (exact name, JNI descriptor, or `*` glob). */
  public scan(opts: ScanOptions): HeapProxy<any[]> {
    const root: QuerySpec['root'] = { kind: 'scan', type: opts.type };
    if (opts.limit !== undefined) root.limit = opts.limit;
    if (opts.includeSubclasses !== undefined) root.includeSubclasses = opts.includeSubclasses;

    return createHeapProxy<any[]>(
      { root, steps: [], collection: true },
      this.proxyExecutor,
      () => this.extensions,
    );
  }

  /** Locates a single `android.view.View` object by `a11yViewId`, bounds, resourceId, text, desc, or class. */
  public view(selector: ViewSelector): HeapProxy<any> {
    return createHeapProxy(
      { root: { kind: 'view', ...selector }, steps: [], collection: false },
      this.proxyExecutor,
      () => this.extensions,
    );
  }

  /** Direct heap object reference by hex address (optionally verified against `expectedClass`). */
  public object(addr: string, expectedClass?: string): HeapProxy<any> {
    const root: QuerySpec['root'] = { kind: 'addr', addr };
    if (expectedClass !== undefined) root.expectedClass = expectedClass;

    return createHeapProxy(
      { root, steps: [], collection: false },
      this.proxyExecutor,
      () => this.extensions,
    );
  }

  // -------------------------------------------------------------------------
  // Direct Protocol Operations
  // -------------------------------------------------------------------------

  public async ping(): Promise<PingResult> {
    return this.execGlobal<PingResult>('ping', {});
  }

  public async selfcheck(
    params: SelfcheckParams = {},
    target?: TargetSpec,
  ): Promise<SelfcheckResult> {
    return this.execDirect<SelfcheckResult>(
      'selfcheck',
      params as Record<string, unknown>,
      target,
      false,
    );
  }

  public async calibrate(): Promise<CalibrateResult> {
    return this.execGlobal<CalibrateResult>('calibrate', {});
  }

  public async roots(target?: TargetSpec): Promise<RootsResult> {
    return this.execDirect<RootsResult>('roots', {}, target, true);
  }

  public async tree(params: TreeParams = {}, target?: TargetSpec): Promise<TreeResult> {
    return this.execDirect<TreeResult>('tree', params as Record<string, unknown>, target, true);
  }

  public async strsearch(
    params: StrSearchParams,
    target?: TargetSpec,
  ): Promise<StrSearchResult> {
    return this.execDirect<StrSearchResult>(
      'strsearch',
      params as unknown as Record<string, unknown>,
      target,
      true,
    );
  }

  public async dumpobj(params: DumpObjParams, target?: TargetSpec): Promise<any> {
    return this.execDirect<any>(
      'dumpobj',
      params as unknown as Record<string, unknown>,
      target,
      true,
    );
  }

  public async whorefs(params: WhoRefsParams, target?: TargetSpec): Promise<WhoRefsResult> {
    return this.execDirect<WhoRefsResult>(
      'whorefs',
      params as unknown as Record<string, unknown>,
      target,
      true,
    );
  }

  public async instances(
    params: InstancesParams,
    target?: TargetSpec,
  ): Promise<InstancesResult> {
    return this.execDirect<InstancesResult>(
      'instances',
      params as unknown as Record<string, unknown>,
      target,
      true,
    );
  }

  /** Executes a compiled `QuerySpec` directly and returns the full protocol envelope including `meta`. */
  public async rawQuery<T = any>(
    spec: QuerySpec,
    target?: TargetSpec,
  ): Promise<ArtseekResponse<T>> {
    const resolved = await this.resolveEffectiveTarget(target, true);
    return this.transport.request<T>({
      op: 'query',
      ...this.targetEnvelope(resolved),
      params: spec as unknown as Record<string, unknown>,
    });
  }

  /** Executes a batch of sub-queries against the same target within a single freeze window. */
  public async batch(queries: BatchSubQuery[], target?: TargetSpec): Promise<BatchResult> {
    const resolved = await this.resolveEffectiveTarget(target, true);
    const res = await this.transport.request<BatchResult>({
      op: 'batch',
      ...this.targetEnvelope(resolved),
      params: { queries },
    });
    if (!res.ok) {
      throw new ArtseekError(
        res.error?.code ?? 'INTERNAL_ERROR',
        res.error?.message ?? 'artseek batch request failed',
        { meta: res.meta, requestId: res.id },
      );
    }
    return res.data;
  }

  /** Low-level escape hatch sending an arbitrary protocol request through the underlying transport. */
  public async request<T = unknown>(req: ArtseekRequest): Promise<ArtseekResponse<T>> {
    const needsTarget =
      req.op !== 'ping' && req.op !== 'calibrate' && req.op !== 'selfcheck';
    const override: TargetSpec = {
      pid: req.pid,
      pkg: req.pkg,
      freeze: req.freeze,
      madvise: req.madvise,
      timeoutMs: req.timeoutMs,
    };
    const resolved =
      req.pid !== undefined || req.pkg !== undefined
        ? mergeTargets(this.defaultTarget, override)
        : await this.resolveEffectiveTarget(override, needsTarget);

    return this.transport.request<T>({
      ...this.targetEnvelope(resolved),
      ...req,
    });
  }

  public close(): Promise<void> {
    return this.transport.close();
  }

  // -------------------------------------------------------------------------
  // Internal Execution & Target Resolution
  // -------------------------------------------------------------------------

  private enqueueForProxy<T>(
    op: Exclude<ArtseekOp, 'batch'>,
    params: Record<string, unknown>,
  ): Promise<T> {
    let targetOrPromise: TargetSpec | Promise<TargetSpec>;
    try {
      targetOrPromise = this.resolveEffectiveTarget(undefined, true);
    } catch (err) {
      return Promise.reject(err);
    }

    if (isPromiseLike<TargetSpec>(targetOrPromise)) {
      return Promise.resolve(targetOrPromise).then((resolved) =>
        this.batcher.enqueue<T>(resolved, op, params, this.batching),
      );
    }
    return this.batcher.enqueue<T>(targetOrPromise, op, params, this.batching);
  }

  private async execGlobal<T>(
    op: 'ping' | 'calibrate',
    params: Record<string, unknown>,
  ): Promise<T> {
    const req: ArtseekRequest = { op, params };
    if (this.defaultTarget.timeoutMs !== undefined) {
      req.timeoutMs = this.defaultTarget.timeoutMs;
    }
    const res = await this.transport.request<T>(req);
    if (!res.ok) {
      throw new ArtseekError(
        res.error?.code ?? 'INTERNAL_ERROR',
        res.error?.message ?? `artseek ${op} request failed`,
        { meta: res.meta, requestId: res.id },
      );
    }
    return res.data;
  }

  private async execDirect<T>(
    op: Exclude<ArtseekOp, 'batch'>,
    params: Record<string, unknown>,
    target: TargetSpec | undefined,
    requireTarget: boolean,
  ): Promise<T> {
    const resolved = await this.resolveEffectiveTarget(target, requireTarget);
    const res = await this.transport.request<T>({
      op,
      ...this.targetEnvelope(resolved),
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

  private resolveEffectiveTarget(
    override: TargetSpec | undefined,
    requireTarget: boolean,
  ): TargetSpec | Promise<TargetSpec> {
    const base = mergeTargets(this.defaultTarget, override);
    const hasExplicit = base.pid !== undefined || base.pkg !== undefined;

    if (hasExplicit || !this.resolveTarget) {
      if (requireTarget && !hasExplicit) {
        throw new ArtseekError(
          'INVALID_QUERY',
          'Target process required: specify pkg or pid via client options, resolveTarget, or forTarget()',
        );
      }
      return base;
    }

    if (!this.inFlightResolveTarget) {
      const p = Promise.resolve(this.resolveTarget()).finally(() => {
        if (this.inFlightResolveTarget === p) {
          this.inFlightResolveTarget = null;
        }
      });
      this.inFlightResolveTarget = p;
    }

    return this.inFlightResolveTarget.then((resolved) => {
      const merged = mergeTargets(mergeTargets(this.defaultTarget, resolved), override);
      if (requireTarget && merged.pid === undefined && merged.pkg === undefined) {
        throw new ArtseekError(
          'INVALID_QUERY',
          'resolveTarget() did not return a pkg or pid',
        );
      }
      return merged;
    });
  }

  private targetEnvelope(target: TargetSpec): Partial<ArtseekRequest> {
    const env: Partial<ArtseekRequest> = {};
    if (target.pid !== undefined) env.pid = target.pid;
    if (target.pkg !== undefined) env.pkg = target.pkg;
    if (target.freeze !== undefined) env.freeze = target.freeze;
    if (target.madvise !== undefined) env.madvise = target.madvise;
    if (target.timeoutMs !== undefined) env.timeoutMs = target.timeoutMs;
    return env;
  }
}

export function createArtseekClient(options: ArtseekClientOptions = {}): ArtseekClient {
  return new ArtseekClient(options);
}
