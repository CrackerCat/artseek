import type {
  ArtseekOp,
  CrawlOptions,
  DumpOptions,
  FindOwnerOptions,
  HeapExtensionFn,
  HeapObjectRef,
  HeapProxy,
  OwnerViewOptions,
  QuerySpec,
  RefOwner,
  Step,
  WhereFilter,
  WhoRefsResult,
} from './types.js';

const INSPECT_CUSTOM = Symbol.for('nodejs.util.inspect.custom');
const INTEGER_KEY_RE = /^-?(?:0|[1-9]\d*)$/;

const IGNORED_INTROSPECTION_KEYS = new Set<string>([
  '__proto__',
  'prototype',
  'inspect',
  'asymmetricMatch',
  'nodeType',
  '@@__IMMUTABLE_ITERABLE__@@',
  '@@__IMMUTABLE_RECORD__@@',
]);

function HeapProxyClass() {}

export interface HeapProxyExecutor {
  executeQuery<T = unknown>(spec: QuerySpec): Promise<T>;
  executeOp<T = unknown>(
    op: Exclude<ArtseekOp, 'batch'>,
    params: Record<string, unknown>,
  ): Promise<T>;
}

interface HeapNodeState {
  readonly spec: QuerySpec;
  readonly executor: HeapProxyExecutor;
  readonly getExtensions: () => Readonly<Record<string, HeapExtensionFn>>;
  readonly mode: 'query' | 'whorefs';
  readonly whoRefsLimit?: number;
}

function cloneQuerySpec(spec: QuerySpec): QuerySpec {
  return structuredClone(spec);
}

export function formatHeapProxy(
  spec: QuerySpec,
  mode: 'query' | 'whorefs' = 'query',
  whoRefsLimit?: number,
): string {
  let rootText: string;
  switch (spec.root.kind) {
    case 'activity':
      rootText =
        spec.root.which === 'current'
          ? 'currentActivity'
          : spec.root.pattern
            ? `activities(${JSON.stringify(spec.root.pattern)})`
            : 'activities';
      break;
    case 'scan':
      rootText = `scan(${JSON.stringify(spec.root.type)})`;
      break;
    case 'view': {
      const { kind: _k, ...selector } = spec.root;
      rootText = `view(${JSON.stringify(selector)})`;
      break;
    }
    case 'addr':
      rootText = `object(${JSON.stringify(spec.root.addr)})`;
      break;
  }

  let chain = rootText;
  for (const step of spec.steps) {
    switch (step.op) {
      case 'field':
        chain += /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(step.name)
          ? `.${step.name}`
          : `.$field(${JSON.stringify(step.name)})`;
        break;
      case 'index':
        chain += `[${step.index}]`;
        break;
      case 'first':
        chain += '.$first()';
        break;
      case 'where':
        chain += `.$where(${JSON.stringify(step.filter)})`;
        break;
      case 'parent':
        chain += step.hops !== undefined ? `.$parent(${step.hops})` : '.$parent()';
        break;
      case 'find_owner':
        chain += `.$findOwner(${JSON.stringify({
          ...(step.instanceOf !== undefined ? { instanceOf: step.instanceOf } : {}),
          ...(step.maxHops !== undefined ? { maxHops: step.maxHops } : {}),
        })})`;
        break;
      case 'crawl':
        chain += `.$crawl(${step.match !== undefined ? JSON.stringify(step.match) : ''})`;
        break;
      case 'select':
        chain += `.$select(${JSON.stringify(step.shape)})`;
        break;
      case 'dump':
        chain += '.$dump()';
        break;
      case 'ref':
        chain += '.$ref()';
        break;
      case 'view_meta':
        chain += '.$viewMeta()';
        break;
    }
  }

  if (mode === 'whorefs') {
    chain += whoRefsLimit !== undefined ? `.$whoRefs(${whoRefsLimit})` : '.$whoRefs()';
  }

  return `HeapProxy(${chain})`;
}

function executeNode<T>(state: HeapNodeState): Promise<T> {
  if (state.mode === 'whorefs') {
    return executeWhoRefsNode<T>(state);
  }
  return state.executor.executeQuery<T>(cloneQuerySpec(state.spec));
}

async function executeWhoRefsNode<T>(state: HeapNodeState): Promise<T> {
  const limitParam = state.whoRefsLimit !== undefined ? { limit: state.whoRefsLimit } : {};

  // Fast path: direct address root with no intermediate steps -> single `whorefs` op.
  if (state.spec.root.kind === 'addr' && state.spec.steps.length === 0) {
    const res = await state.executor.executeOp<WhoRefsResult>('whorefs', {
      addr: state.spec.root.addr,
      ...limitParam,
    });
    return (res?.owners ?? []) as T;
  }

  // General path: resolve object ref(s) first, then query `whorefs`.
  const refSpec: QuerySpec = {
    root: state.spec.root,
    steps: [...state.spec.steps, { op: 'ref' }],
    collection: state.spec.collection,
  };
  const resolved = await state.executor.executeQuery<HeapObjectRef | HeapObjectRef[] | null>(
    refSpec,
  );
  if (!resolved) {
    return [] as T;
  }
  if (Array.isArray(resolved)) {
    const addrs = resolved.map((r) => r?.addr).filter((a): a is string => Boolean(a));
    if (addrs.length === 0) {
      return [] as T;
    }
    const results = await Promise.all(
      addrs.map((addr) =>
        state.executor.executeOp<WhoRefsResult>('whorefs', { addr, ...limitParam }),
      ),
    );
    const owners: RefOwner[] = [];
    for (const item of results) {
      if (item?.owners) {
        owners.push(...item.owners);
      }
    }
    return owners as T;
  }
  if (!resolved.addr) {
    return [] as T;
  }
  const res = await state.executor.executeOp<WhoRefsResult>('whorefs', {
    addr: resolved.addr,
    ...limitParam,
  });
  return (res?.owners ?? []) as T;
}

function buildHeapProxy<T = any>(state: HeapNodeState): HeapProxy<T> {
  const appendStep = <Next = any>(
    step: Step,
    collectionOverride?: boolean,
  ): HeapProxy<Next> => {
    const nextSpec: QuerySpec = {
      root: state.spec.root,
      steps: [...state.spec.steps, step],
      collection: collectionOverride ?? state.spec.collection,
    };
    return buildHeapProxy<Next>({
      spec: nextSpec,
      executor: state.executor,
      getExtensions: state.getExtensions,
      mode: 'query',
    });
  };

  const target: Record<PropertyKey, unknown> = {};
  Object.defineProperty(target, INSPECT_CUSTOM, {
    value: () => formatHeapProxy(state.spec, state.mode, state.whoRefsLimit),
    enumerable: false,
    configurable: true,
  });
  Object.defineProperty(target, Symbol.toStringTag, {
    value: 'HeapProxy',
    enumerable: false,
    configurable: true,
  });

  const proxy: HeapProxy<T> = new Proxy(target as unknown as HeapProxy<T>, {
    get(_target, prop) {
      // 1. Symbol traps (Node util.inspect, toStringTag, toPrimitive, etc.)
      if (typeof prop === 'symbol') {
        if (prop === INSPECT_CUSTOM) {
          return () => formatHeapProxy(state.spec, state.mode, state.whoRefsLimit);
        }
        if (prop === Symbol.toStringTag) {
          return 'HeapProxy';
        }
        if (prop === Symbol.toPrimitive) {
          return (hint: string) =>
            hint === 'number'
              ? Number.NaN
              : formatHeapProxy(state.spec, state.mode, state.whoRefsLimit);
        }
        return undefined;
      }

      // 2. Non-triggering JS/Node/testing introspection properties
      if (IGNORED_INTROSPECTION_KEYS.has(prop)) {
        return undefined;
      }
      if (prop === 'toJSON') {
        return () => cloneQuerySpec(state.spec);
      }
      if (prop === 'valueOf') {
        return () => proxy;
      }
      if (prop === 'toString') {
        return () => formatHeapProxy(state.spec, state.mode, state.whoRefsLimit);
      }
      if (prop === 'constructor') {
        return HeapProxyClass;
      }

      // 3. Promise / Thenable traps (trigger live execution!)
      if (prop === 'then') {
        return (
          onFulfilled?: ((value: T) => any) | null,
          onRejected?: ((reason: any) => any) | null,
        ) => executeNode<T>(state).then(onFulfilled, onRejected);
      }
      if (prop === 'catch') {
        return (onRejected?: ((reason: any) => any) | null) =>
          executeNode<T>(state).catch(onRejected);
      }
      if (prop === 'finally') {
        return (onFinally?: (() => void) | null) => executeNode<T>(state).finally(onFinally);
      }

      // 4. Built-in `$` DSL methods & dynamic extensions
      if (prop.startsWith('$')) {
        switch (prop) {
          case '$query':
            return () => cloneQuerySpec(state.spec);
          case '$field':
            return <V = any>(name: string) => appendStep<V>({ op: 'field', name: String(name) });
          case '$first':
            return () => appendStep({ op: 'first' }, false);
          case '$where':
            return (filter: WhereFilter) =>
              appendStep<any[]>({ op: 'where', filter: structuredClone(filter) }, true);
          case '$parent':
            return (hops?: number) =>
              appendStep(hops !== undefined ? { op: 'parent', hops } : { op: 'parent' });
          case '$findOwner':
            return (opts?: FindOwnerOptions) => {
              const step: Step = { op: 'find_owner' };
              if (opts?.instanceOf !== undefined) step.instanceOf = opts.instanceOf;
              if (opts?.maxHops !== undefined) step.maxHops = opts.maxHops;
              return appendStep(step);
            };
          case '$ownerView':
            return (opts?: OwnerViewOptions) => {
              const step: Step = { op: 'find_owner', instanceOf: 'android.view.View' };
              if (opts?.maxHops !== undefined) step.maxHops = opts.maxHops;
              return appendStep(step);
            };
          case '$viewMeta':
            return () => appendStep({ op: 'view_meta' });
          case '$crawl':
            return (match?: string, opts?: CrawlOptions) => {
              const step: Step = { op: 'crawl' };
              if (match !== undefined) step.match = match;
              if (opts?.depth !== undefined) step.depth = opts.depth;
              if (opts?.maxObjects !== undefined) step.maxObjects = opts.maxObjects;
              return appendStep(step);
            };
          case '$select':
            return (shape: Record<string, string>) =>
              appendStep({ op: 'select', shape: { ...shape } });
          case '$dump':
            return (opts?: DumpOptions) => {
              const step: Step = { op: 'dump' };
              if (opts?.depth !== undefined) step.depth = opts.depth;
              if (opts?.schemaOnly !== undefined) step.schemaOnly = opts.schemaOnly;
              if (opts?.expandCollections !== undefined) {
                step.expandCollections = opts.expandCollections;
              }
              if (opts?.maxArrayItems !== undefined) step.maxArrayItems = opts.maxArrayItems;
              return appendStep(step);
            };
          case '$whoRefs':
            return (limit?: number) =>
              buildHeapProxy<RefOwner[]>({
                spec: state.spec,
                executor: state.executor,
                getExtensions: state.getExtensions,
                mode: 'whorefs',
                whoRefsLimit: limit,
              });
          case '$ref':
            return () => appendStep({ op: 'ref' });
          default: {
            const extensions = state.getExtensions();
            if (Object.prototype.hasOwnProperty.call(extensions, prop)) {
              const fn = extensions[prop];
              return (...args: unknown[]) => fn(proxy, ...args);
            }
            return undefined;
          }
        }
      }

      // 5. Numeric index access (`[0]`, `[-1]`)
      if (INTEGER_KEY_RE.test(prop)) {
        const index = Number(prop);
        if (Number.isSafeInteger(index)) {
          return appendStep({ op: 'index', index }, false);
        }
      }

      // 6. Arbitrary Java field / Map key access
      return appendStep({ op: 'field', name: prop });
    },

    has(_target, prop) {
      if (typeof prop === 'symbol') {
        return prop === INSPECT_CUSTOM || prop === Symbol.toStringTag || prop === Symbol.toPrimitive;
      }
      if (prop === 'then' || prop === 'catch' || prop === 'finally') {
        return true;
      }
      if (prop.startsWith('$')) {
        const builtins = new Set([
          '$query',
          '$field',
          '$first',
          '$where',
          '$parent',
          '$findOwner',
          '$ownerView',
          '$viewMeta',
          '$crawl',
          '$select',
          '$dump',
          '$whoRefs',
          '$ref',
        ]);
        if (builtins.has(prop)) return true;
        return Object.prototype.hasOwnProperty.call(state.getExtensions(), prop);
      }
      return false;
    },

    ownKeys() {
      return [];
    },

    getPrototypeOf() {
      return Object.prototype;
    },

    set() {
      return false;
    },

    defineProperty() {
      return false;
    },

    deleteProperty() {
      return false;
    },
  });

  return proxy;
}

export function createHeapProxy<T = any>(
  spec: QuerySpec,
  executor: HeapProxyExecutor,
  getExtensions: () => Readonly<Record<string, HeapExtensionFn>>,
): HeapProxy<T> {
  return buildHeapProxy<T>({
    spec: cloneQuerySpec(spec),
    executor,
    getExtensions,
    mode: 'query',
  });
}
