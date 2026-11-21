import util from 'node:util';
import { describe, expect, it, vi } from 'vitest';
import { createArtseekClient } from '../src/index.js';
import type { ArtseekRequest, ArtseekResponse, ArtseekTransport } from '../src/types.js';

function createMockTransport(
  handler: (req: ArtseekRequest) => ArtseekResponse<any> | Promise<ArtseekResponse<any>>,
) {
  const requests: ArtseekRequest[] = [];
  const transport: ArtseekTransport = {
    request: vi.fn(async (req: ArtseekRequest) => {
      requests.push(req);
      return handler(req);
    }),
    close: vi.fn(async () => {}),
  };
  return { transport, requests };
}

describe('HeapProxy lazy path recording & introspection safety', () => {
  it('records nested fields, numeric indices, negative indices, and built-in $ steps without executing', () => {
    const { transport } = createMockTransport(() => ({ ok: true, data: null, error: null }));
    const heap = createArtseekClient({ transport, pkg: 'com.example.app' });

    const q1 = heap.currentActivity.a.b;
    expect(q1.$query()).toEqual({
      root: { kind: 'activity', which: 'current' },
      steps: [
        { op: 'field', name: 'a' },
        { op: 'field', name: 'b' },
      ],
      collection: false,
    });

    const q2 = heap
      .scan({ type: '*ItemModel', limit: 20 })
      .$where({ 'priceInfo.price': { $gt: 10 }, title: { $contains: 'pro' } })[0]
      .c;
    expect(q2.$query()).toEqual({
      root: { kind: 'scan', type: '*ItemModel', limit: 20 },
      steps: [
        {
          op: 'where',
          filter: { 'priceInfo.price': { $gt: 10 }, title: { $contains: 'pro' } },
        },
        { op: 'index', index: 0 },
        { op: 'field', name: 'c' },
      ],
      collection: false,
    });

    const q3 = heap.currentActivity.items[-1].$field('then').$field('catch').$field('$VALUES');
    expect(q3.$query()).toEqual({
      root: { kind: 'activity', which: 'current' },
      steps: [
        { op: 'field', name: 'items' },
        { op: 'index', index: -1 },
        { op: 'field', name: 'then' },
        { op: 'field', name: 'catch' },
        { op: 'field', name: '$VALUES' },
      ],
      collection: false,
    });

    const q4 = heap
      .view({ a11yViewId: 7847, onlyFocused: true })
      .$parent(2)
      .$ownerView({ maxHops: 4 })
      .$viewMeta();
    expect(q4.$query()).toEqual({
      root: { kind: 'view', a11yViewId: 7847, onlyFocused: true },
      steps: [
        { op: 'parent', hops: 2 },
        { op: 'find_owner', instanceOf: 'android.view.View', maxHops: 4 },
        { op: 'view_meta' },
      ],
      collection: false,
    });

    const q5 = heap
      .scan({ type: 'com.foo.Card' })
      .$first()
      .$findOwner({ instanceOf: 'android.widget.LinearLayout', maxHops: 5 })
      .$select({ title: 'mTitle.text', tags: 'mTags[*].name' });
    expect(q5.$query()).toEqual({
      root: { kind: 'scan', type: 'com.foo.Card' },
      steps: [
        { op: 'first' },
        { op: 'find_owner', instanceOf: 'android.widget.LinearLayout', maxHops: 5 },
        { op: 'select', shape: { title: 'mTitle.text', tags: 'mTags[*].name' } },
      ],
      collection: false,
    });

    const q6 = heap.object('0x12345678', 'com.foo.Model').$crawl('price', { depth: 4, maxObjects: 500 });
    expect(q6.$query()).toEqual({
      root: { kind: 'addr', addr: '0x12345678', expectedClass: 'com.foo.Model' },
      steps: [{ op: 'crawl', match: 'price', depth: 4, maxObjects: 500 }],
      collection: false,
    });

    const q7 = heap.activities.$dump({ depth: 2, schemaOnly: true, expandCollections: false, maxArrayItems: 10 });
    expect(q7.$query()).toEqual({
      root: { kind: 'activity', which: 'all' },
      steps: [{ op: 'dump', depth: 2, schemaOnly: true, expandCollections: false, maxArrayItems: 10 }],
      collection: true,
    });

    // Zero transport calls because nothing was awaited!
    expect(transport.request).not.toHaveBeenCalled();
  });

  it('keeps proxy nodes strictly immutable when branching or mutating $query() output', () => {
    const { transport } = createMockTransport(() => ({ ok: true, data: null, error: null }));
    const heap = createArtseekClient({ transport, pkg: 'com.example.app' });

    const base = heap.currentActivity.mRoot;
    const branchA = base.childA;
    const branchB = base.childB;

    const ast = base.$query();
    ast.steps.push({ op: 'field', name: 'mutated' });

    expect(base.$query().steps).toEqual([{ op: 'field', name: 'mRoot' }]);
    expect(branchA.$query().steps).toEqual([
      { op: 'field', name: 'mRoot' },
      { op: 'field', name: 'childA' },
    ]);
    expect(branchB.$query().steps).toEqual([
      { op: 'field', name: 'mRoot' },
      { op: 'field', name: 'childB' },
    ]);
    expect(transport.request).not.toHaveBeenCalled();
  });

  it('never triggers execution or records bogus fields on console.log / util.inspect / JSON.stringify', () => {
    const { transport } = createMockTransport(() => ({ ok: true, data: 'should-not-run', error: null }));
    const heap = createArtseekClient({ transport, pkg: 'com.example.app' });

    const proxy = heap.currentActivity.mAdapter.mItems[0].title;

    const inspected = util.inspect(proxy);
    expect(inspected).toBe('HeapProxy(currentActivity.mAdapter.mItems[0].title)');

    const inspectedNoCustom = util.inspect(proxy, { customInspect: false });
    expect(typeof inspectedNoCustom).toBe('string');

    expect(String(proxy)).toBe('HeapProxy(currentActivity.mAdapter.mItems[0].title)');
    expect(Object.prototype.toString.call(proxy)).toBe('[object HeapProxy]');
    expect(JSON.parse(JSON.stringify(proxy))).toEqual(proxy.$query());
    expect(Object.keys(proxy)).toEqual([]);
    expect((proxy as any).$$typeof).toBeUndefined();
    expect((proxy as any).asymmetricMatch).toBeUndefined();
    expect((proxy as any).$nonExistent).toBeUndefined();

    // Simulate console.log cleanly without polluting test output
    const logSpy = vi.spyOn(console, 'log').mockImplementation(() => {});
    console.log(proxy);
    logSpy.mockRestore();

    expect(transport.request).not.toHaveBeenCalled();
  });

  it('supports $whoRefs() for both direct object addresses and arbitrary heap chains', async () => {
    const { transport, requests } = createMockTransport((req) => {
      if (req.op === 'whorefs') {
        return {
          ok: true,
          data: {
            targetAddr: req.params?.addr,
            owners: [
              {
                refAddr: '0x1111',
                ownerAddr: '0x2222',
                ownerClass: 'com.example.Holder',
                fieldName: 'mModel',
                fieldType: 'Lcom/example/Model;',
              },
            ],
          },
          error: null,
        };
      }
      if (req.op === 'query') {
        return {
          ok: true,
          data: { addr: '0x9999', className: 'com.example.Target', pid: 42 },
          error: null,
        };
      }
      throw new Error(`Unexpected op ${req.op}`);
    });

    const heap = createArtseekClient({ transport, pkg: 'com.example.app' });

    // Direct address -> 1 whorefs op directly
    const directOwners = await heap.object('0xabcd').$whoRefs(5);
    expect(directOwners).toHaveLength(1);
    expect(directOwners[0].ownerClass).toBe('com.example.Holder');
    expect(requests).toHaveLength(1);
    expect(requests[0].op).toBe('whorefs');
    expect(requests[0].params).toEqual({ addr: '0xabcd', limit: 5 });

    // Chained expression -> resolves $ref() first, then whorefs
    const chainedOwners = await heap.currentActivity.mModel.$whoRefs(10);
    expect(chainedOwners).toHaveLength(1);
    expect(requests).toHaveLength(3);
    expect(requests[1].op).toBe('query');
    expect(requests[1].params).toEqual({
      root: { kind: 'activity', which: 'current' },
      steps: [{ op: 'field', name: 'mModel' }, { op: 'ref' }],
      collection: false,
    });
    expect(requests[2].op).toBe('whorefs');
    expect(requests[2].params).toEqual({ addr: '0x9999', limit: 10 });
  });

  it('re-executes live query each time a saved lazy proxy is awaited', async () => {
    let counter = 0;
    const { transport } = createMockTransport(() => ({
      ok: true,
      data: `value-${++counter}`,
      error: null,
    }));
    const heap = createArtseekClient({ transport, pkg: 'com.example.app' });

    const titleHandle = heap.currentActivity.mTitle;
    expect(await titleHandle).toBe('value-1');
    expect(await titleHandle).toBe('value-2');
    expect(transport.request).toHaveBeenCalledTimes(2);
  });
});
