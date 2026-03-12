export {
  type ButtonProps,
  type CheckboxProps,
  type ChildInput,
  type ClipRectProps,
  countNodes,
  createTransportEnvelope,
  createNode,
  defineElement,
  type DocumentProps,
  type ElementNode,
  type FillRectProps,
  type FontResourceDesc,
  fragment,
  type ImageProps,
  type ImageResourceDesc,
  type LineProps,
  parseTransportEnvelope,
  type ProgressBarProps,
  type SeparatorProps,
  type ShaderBlendMode,
  type ShaderImageProps,
  type ShaderLanguage,
  type ShaderNodeProps,
  type ShaderRectProps,
  type StackProps,
  type StrokeRectProps,
  type TextProps,
  type TransportEnvelope,
  type TransportEnvelopeOptions,
  type TransportFontResource,
  type TransportHostOptions,
  type TransportImageResource,
  type TransportResourceMode,
  type TransportSession,
  type TransportShaderResource,
  type TransportShaderStage,
  type WindowProps,
  serializeDocument,
  serializeTransportEnvelope
} from "@igr/core";

import type { ChildInput, ElementNode, TransportEnvelopeOptions } from "@igr/core";
import { createTransportEnvelope, fragment, serializeTransportEnvelope } from "@igr/core";

function normalizeTopLevel(input: ChildInput | ChildInput[]): ChildInput[] {
  return Array.isArray(input) ? input : [input];
}

export function createDocument(...children: ChildInput[]): ElementNode {
  return fragment(...children);
}

export function renderDocument(input: ChildInput | ChildInput[]): string {
  return JSON.stringify(createDocument(...normalizeTopLevel(input)), null, 2);
}

export function renderTransportDocument(
  input: ChildInput | ChildInput[],
  sequence = 0,
  options: TransportEnvelopeOptions = {}
): string {
  return serializeTransportEnvelope(createTransportEnvelope(createDocument(...normalizeTopLevel(input)), sequence, options));
}
