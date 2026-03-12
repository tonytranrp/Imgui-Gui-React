import {
  type ButtonProps,
  type CheckboxProps,
  type ChildInput,
  type ClipRectProps,
  createNode,
  type DocumentProps,
  type ElementNode,
  type FillRectProps,
  fragment,
  type ImageProps,
  type LineProps,
  type ProgressBarProps,
  type SeparatorProps,
  type ShaderImageProps,
  type ShaderRectProps,
  type StackProps,
  type StrokeRectProps,
  type TextProps,
  type WindowProps
} from "@igr/core";

type Component<P = Record<string, unknown>> = (props: P) => ElementNode;
type ElementType = string | Component<Record<string, unknown>>;

function extractChildren(children: unknown): ChildInput[] {
  if (Array.isArray(children)) {
    return children as ChildInput[];
  }

  if (children === undefined) {
    return [];
  }

  return [children as ChildInput];
}

export function jsx(type: ElementType, props: Record<string, unknown>, key?: string): ElementNode {
  const nextProps = props ?? {};
  const children = extractChildren(nextProps.children);
  const { children: _children, ...restProps } = nextProps;

  if (typeof type === "function") {
    return type({ ...nextProps, key, children });
  }

  return createNode(type, { ...(restProps as DocumentProps), key } as DocumentProps, ...children);
}

export const jsxs = jsx;

export function Fragment(props: { children?: ChildInput | ChildInput[] }): ElementNode {
  return fragment(...extractChildren(props.children));
}

export type {
  ButtonProps,
  CheckboxProps,
  ClipRectProps,
  ElementNode,
  FillRectProps,
  ImageProps,
  LineProps,
  ProgressBarProps,
  SeparatorProps,
  ShaderImageProps,
  ShaderRectProps,
  StackProps,
  StrokeRectProps,
  TextProps,
  WindowProps
};

declare global {
  namespace JSX {
    type Element = ElementNode;

    interface ElementChildrenAttribute {
      children: {};
    }

    interface IntrinsicElements {
      window: WindowProps & { key?: string; children?: ChildInput | ChildInput[] };
      stack: StackProps & { key?: string; children?: ChildInput | ChildInput[] };
      clip_rect: ClipRectProps & { key?: string; children?: ChildInput | ChildInput[] };
      text: TextProps & { key?: string };
      button: ButtonProps & { key?: string };
      checkbox: CheckboxProps & { key?: string };
      image: ImageProps & { key?: string };
      progress_bar: ProgressBarProps & { key?: string };
      separator: SeparatorProps & { key?: string };
      fill_rect: FillRectProps & { key?: string };
      stroke_rect: StrokeRectProps & { key?: string };
      line: LineProps & { key?: string };
      shader_rect: ShaderRectProps & { key?: string };
      shader_image: ShaderImageProps & { key?: string };
      fragment: { key?: string; children?: ChildInput | ChildInput[] };
    }
  }
}
