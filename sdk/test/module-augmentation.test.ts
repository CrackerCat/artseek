import { describe, expect, it, vi } from 'vitest';
import {
  createArtseekClient,
  type ArtseekRequest,
  type ArtseekTransport,
  type HeapProxy,
  type ViewMeta,
} from 'artseek';

declare module 'artseek' {
  interface HeapNodeExtensions {
    $pkgAugmented(tag: string): Promise<string>;
  }
}

describe("TypeScript `declare module 'artseek'` augmentation", () => {
  it('augments HeapNodeExtensions cleanly when importing from package name "artseek"', async () => {
    const transport: ArtseekTransport = {
      request: vi.fn(async (req: ArtseekRequest) => ({
        ok: true,
        data: req.params as any,
        error: null,
      })),
      close: vi.fn(async () => {}),
    };

    const client = createArtseekClient({
      transport,
      pkg: 'com.example.app',
      extensions: {
        $pkgAugmented: async (node: HeapProxy, tag: string) =>
          `${tag}:${node.$query().steps.length}`,
      },
    });

    // Must typecheck strictly via `declare module 'artseek'` without any casts!
    const result: string = await client.currentActivity.mHeader.$pkgAugmented('ok');
    expect(result).toBe('ok:1');

    // Negative type check: wrong argument type must fail compile if not suppressed
    // @ts-expect-error number is not assignable to parameter of type string
    const _badCall = () => client.currentActivity.mHeader.$pkgAugmented(123);
    void _badCall;

    // Exact type assertions (proving types are NOT collapsed to `any`)
    type IsExact<A, B> = (<T>() => T extends A ? 1 : 2) extends <T>() => T extends B ? 1 : 2
      ? true
      : false;
    const assertAugReturn: IsExact<typeof result, string> = true;
    const assertViewMeta: IsExact<Awaited<ReturnType<HeapProxy['$viewMeta']>>, ViewMeta | null> =
      true;
    expect(assertAugReturn).toBe(true);
    expect(assertViewMeta).toBe(true);
  });
});
