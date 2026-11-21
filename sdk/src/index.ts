/**
 * Empty extension interface intended for TypeScript module augmentation by
 * downstream frameworks:
 *
 * ```ts
 * declare module 'artseek' {
 *   interface HeapNodeExtensions {
 *     $click(): Promise<void>;
 *   }
 * }
 * ```
 */
export interface HeapNodeExtensions {}

export {
  ArtseekClient,
  createArtseekClient,
  type ArtseekClientOptions,
  type TargetResolver,
} from './client.js';

export { MicrotaskBatcher } from './batcher.js';

export {
  createHeapProxy,
  formatHeapProxy,
  type HeapProxyExecutor,
} from './proxy.js';

export {
  AdbStdioTransport,
  StdioTransport,
  resolveDefaultBinaryPath,
  type AdbStdioTransportOptions,
  type StdioTransportOptions,
} from './transport.js';

export {
  ArtseekError,
  type ActivityInfo,
  type ArtseekErrorCode,
  type ArtseekErrorObject,
  type ArtseekMeta,
  type ArtseekOp,
  type ArtseekRequest,
  type ArtseekResponse,
  type ArtseekTransport,
  type BatchParams,
  type BatchResult,
  type BatchSubQuery,
  type BatchSubResult,
  type Bounds,
  type CalibrateResult,
  type CrawlHit,
  type CrawlOptions,
  type DumpObjParams,
  type DumpOptions,
  type FindOwnerOptions,
  type HeapExtensionFn,
  type HeapExtensionsMap,
  type HeapObjectRef,
  type HeapProxy,
  type HeapProxyBuiltins,
  type HeapProxyFields,
  type InstanceItem,
  type InstancesParams,
  type InstancesResult,
  type OffscreenDirection,
  type OwnerViewOptions,
  type ParentChainEntry,
  type PingResult,
  type QuerySpec,
  type RefOwner,
  type RootSource,
  type RootsResult,
  type ScanOptions,
  type ScreenSize,
  type ScrollableAncestor,
  type SelfcheckCheck,
  type SelfcheckParams,
  type SelfcheckResult,
  type Step,
  type StrSearchHit,
  type StrSearchParams,
  type StrSearchResult,
  type TargetSpec,
  type TreeParams,
  type TreeResult,
  type TreeRootEntry,
  type ViewMeta,
  type ViewRootInfo,
  type ViewSelector,
  type ViewTreeNode,
  type Visibility,
  type WhereFilter,
  type WhereFilterValue,
  type WhereOperator,
  type WhoRefsParams,
  type WhoRefsResult,
} from './types.js';
