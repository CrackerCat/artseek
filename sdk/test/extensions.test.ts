import { describe, expect, it, vi } from 'vitest';
import {
  ArtseekError,
  createArtseekClient,
  type ArtseekRequest,
  type ArtseekTransport,
  type HeapProxy,
  type ViewMeta,
} from '../src/index.js';

// Downstream module augmentation pattern (mirrors `declare module 'artseek'`)
declare module '../src/index.js' {
  interface HeapNodeExtensions {
    $customExt(prefix: string): Promise<string>;
    $describeAst(): string;
  }
}

function createEchoTransport(): ArtseekTransport {
  return {
    request: vi.fn(async (req: ArtseekRequest) => ({
      ok: true,
      data: req.params as any,
      error: null,
    })),
    close: vi.fn(async () => {}),
  };
}

describe('HeapProxy dynamic extensions & TypeScript augmentation', () => {
  it('supports extensions registered via createArtseekClient({ extensions }) and client.extend()', async () => {
    const transport = createEchoTransport();

    const client = createArtseekClient({
      transport,
      pkg: 'com.example.app',
      extensions: {
        $customExt: async (node: HeapProxy, prefix: string) => {
          const ast = node.$query();
          const lastStep = ast.steps[ast.steps.length - 1];
          const fieldName = lastStep && lastStep.op === 'field' ? lastStep.name : 'root';
          return `${prefix}:${fieldName}`;
        },
      },
    });

    client.extend({
      $describeAst: (node: HeapProxy) => `${node.$query().root.kind}:${node.$query().steps.length}`,
    });

    const out = await client.currentActivity.mHeader.mTitle.$customExt('hello');
    expect(out).toBe('hello:mTitle');

    const desc = client.currentActivity.a.b.$describeAst();
    expect(desc).toBe('activity:2');

    // Type-level verification for built-ins + augmented extensions
    const viewMetaProxy: HeapProxy<ViewMeta | null> = client.view({ text: 'OK' }).$viewMeta();
    const selectProxy: HeapProxy<{ title: any; price: any }> = client.currentActivity.$select({
      title: 'mTitle',
      price: 'mPrice',
    });
    expect(viewMetaProxy.$query().steps).toEqual([{ op: 'view_meta' }]);
    expect(selectProxy.$query().steps).toEqual([
      { op: 'select', shape: { title: 'mTitle', price: 'mPrice' } },
    ]);
  });

  it('inherits extensions in forTarget() while keeping scoped extend() isolated', () => {
    const transport = createEchoTransport();
    const base = createArtseekClient({
      transport,
      extensions: {
        $describeAst: (node: HeapProxy) => `base:${node.$query().steps.length}`,
      },
    });

    const scoped = base.forTarget({ pkg: 'com.scoped.app' }).extend({
      $customExt: async (_node: HeapProxy, prefix: string) => `scoped:${prefix}`,
    });

    expect(scoped.currentActivity.x.$describeAst()).toBe('base:1');
    expect(typeof scoped.currentActivity.$customExt).toBe('function');
    // Parent does NOT receive scoped-only extension
    expect((base.currentActivity as any).$customExt).toBeUndefined();
  });

  it('rejects extension names that do not start with "$"', () => {
    const transport = createEchoTransport();
    expect(() =>
      createArtseekClient({
        transport,
        extensions: {
          badName: () => {},
        } as any,
      }),
    ).toThrow(ArtseekError);
  });
});
