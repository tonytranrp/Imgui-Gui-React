export type ScalarValue = string | number | boolean | null;
export type DocumentProps = Record<string, unknown>;
export type ChildInput =
  | ElementNode
  | string
  | number
  | boolean
  | null
  | undefined
  | ChildInput[];

export interface ElementNode<TProps extends DocumentProps = DocumentProps> {
  type: string;
  key?: string;
  props: TProps;
  children: ElementNode[];
}

export interface WindowProps extends DocumentProps {
  title: string;
  x?: number;
  y?: number;
  width?: number;
  height?: number;
}

export interface StackProps extends DocumentProps {
  axis?: "horizontal" | "vertical";
}

export interface ClipRectProps extends DocumentProps {
  width?: number;
  height?: number;
}

export interface TextProps extends DocumentProps {
  value: string;
  font?: string;
}

export interface ButtonProps extends DocumentProps {
  label: string;
  enabled?: boolean;
}

export interface CheckboxProps extends DocumentProps {
  label: string;
  checked?: boolean;
}

export interface ImageProps extends DocumentProps {
  texture: string;
  resource?: string;
  width?: number;
  height?: number;
  label?: string;
}

export interface ProgressBarProps extends DocumentProps {
  label: string;
  value: number;
}

export interface SeparatorProps extends DocumentProps {}

export interface FillRectProps extends DocumentProps {
  x?: number;
  y?: number;
  width: number;
  height: number;
  color?: string;
}

export interface StrokeRectProps extends FillRectProps {
  thickness?: number;
}

export interface LineProps extends DocumentProps {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
  thickness?: number;
  color?: string;
}

export type ShaderLanguage = "hlsl" | "glsl";
export type ShaderBlendMode = "alpha" | "opaque" | "additive";
export type TransportResourceMode = "replace" | "retain";

export interface TransportShaderStage {
  language: ShaderLanguage;
  entryPoint?: string;
  source: string;
}

export interface ShaderNodeProps extends DocumentProps {
  shader: string;
  texture?: string;
  resource?: string;
  tint?: string;
  param0?: string;
  param1?: string;
  param2?: string;
  param3?: string;
}

export interface ShaderRectProps extends ShaderNodeProps {
  x?: number;
  y?: number;
  width: number;
  height: number;
}

export interface ShaderImageProps extends ShaderNodeProps {
  texture: string;
  width?: number;
  height?: number;
  label?: string;
}

export interface FontResourceDesc {
  family?: string;
  size?: number;
  weight?: "regular" | "medium" | "semibold" | "bold";
  style?: "normal" | "italic";
  locale?: string;
}

export interface ImageResourceDesc {
  texture: string;
  width?: number;
  height?: number;
  u?: number;
  v?: number;
  uvWidth?: number;
  uvHeight?: number;
  tint?: string;
}

export interface TransportShaderResource {
  key: string;
  vertex?: TransportShaderStage;
  pixel: TransportShaderStage;
  samplesTexture?: boolean;
  blendMode?: ShaderBlendMode;
}

export interface TransportHostOptions {
  hostMode?: "owned_window" | "external_swap_chain" | "injected_overlay";
  presentationMode?: "backend_managed" | "host_managed";
  resizeMode?: "backend_managed" | "host_managed";
  inputMode?: "none" | "external_forwarded" | "subclassed_window_proc";
  clearTarget?: boolean;
  restoreHostState?: boolean;
}

export interface TransportSession {
  name?: string;
  targetBackend?: "any" | "dx11" | "dx12";
  host?: TransportHostOptions;
}

export interface TransportFontResource extends FontResourceDesc {
  key: string;
}

export interface TransportImageResource extends ImageResourceDesc {
  key: string;
}

export interface TransportEnvelope {
  kind: "igr.document.v1";
  sequence: number;
  resourceMode?: TransportResourceMode;
  session?: TransportSession;
  fonts?: TransportFontResource[];
  images?: TransportImageResource[];
  shaders?: TransportShaderResource[];
  root: ElementNode;
}

export interface TransportEnvelopeOptions {
  session?: TransportSession;
  resourceMode?: TransportResourceMode;
  fonts?: TransportFontResource[];
  images?: TransportImageResource[];
  shaders?: TransportShaderResource[];
}

function normalizeChildren(input: ChildInput[]): ElementNode[] {
  const output: ElementNode[] = [];

  for (const value of input) {
    if (Array.isArray(value)) {
      output.push(...normalizeChildren(value));
      continue;
    }

    if (value === null || value === undefined || value === false) {
      continue;
    }

    if (typeof value === "string" || typeof value === "number") {
      output.push({
        type: "text",
        props: { value: String(value) },
        children: []
      });
      continue;
    }

    if (typeof value === "boolean") {
      output.push({
        type: "text",
        props: { value: value ? "true" : "false" },
        children: []
      });
      continue;
    }

    output.push(value);
  }

  return output;
}

export function createNode<TProps extends DocumentProps>(
  type: string,
  props: TProps,
  ...children: ChildInput[]
): ElementNode<TProps> {
  const { key, ...rest } = props as TProps & { key?: string };
  const node: ElementNode<TProps> = {
    type,
    props: rest as TProps,
    children: normalizeChildren(children)
  };

  if (typeof key === "string") {
    node.key = key;
  }

  return node;
}

export function fragment(...children: ChildInput[]): ElementNode {
  return {
    type: "fragment",
    props: {},
    children: normalizeChildren(children)
  };
}

export function defineElement<TProps extends DocumentProps>(type: string) {
  return (props: TProps, ...children: ChildInput[]): ElementNode<TProps> =>
    createNode(type, props, ...children);
}

export function countNodes(node: ElementNode): number {
  return 1 + node.children.reduce((total, child) => total + countNodes(child), 0);
}

export function serializeDocument(node: ElementNode | ElementNode[]): string {
  return JSON.stringify(node, null, 2);
}

export function createTransportEnvelope(
  root: ElementNode,
  sequence = 0,
  options: TransportEnvelopeOptions = {}
): TransportEnvelope {
  return {
    kind: "igr.document.v1",
    sequence,
    ...(options.resourceMode !== undefined ? { resourceMode: options.resourceMode } : {}),
    ...(options.session !== undefined ? { session: options.session } : {}),
    ...(options.fonts !== undefined ? { fonts: options.fonts } : {}),
    ...(options.images !== undefined ? { images: options.images } : {}),
    ...(options.shaders !== undefined ? { shaders: options.shaders } : {}),
    root
  };
}

export function serializeTransportEnvelope(envelope: TransportEnvelope): string {
  return JSON.stringify(envelope);
}

export function parseTransportEnvelope(payload: string): TransportEnvelope {
  const envelope = JSON.parse(payload) as TransportEnvelope;
  if (envelope.kind !== "igr.document.v1") {
    throw new Error(`Unsupported transport kind: ${String((envelope as { kind?: unknown }).kind)}`);
  }
  return envelope;
}
