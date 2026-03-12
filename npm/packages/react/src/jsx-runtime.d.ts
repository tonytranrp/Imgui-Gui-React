import { type CheckboxProps, type ClipRectProps, type FillRectProps, type ButtonProps, type ChildInput, type ElementNode, type ImageProps, type LineProps, type ProgressBarProps, type SeparatorProps, type StackProps, type StrokeRectProps, type TextProps, type WindowProps } from "@igr/core";
type Component<P = Record<string, unknown>> = (props: P) => ElementNode;
type ElementType = string | Component<Record<string, unknown>>;
export declare function jsx(type: ElementType, props: Record<string, unknown>, key?: string): ElementNode;
export declare const jsxs: typeof jsx;
export declare function Fragment(props: {
    children?: ChildInput | ChildInput[];
}): ElementNode;
export type { ButtonProps, CheckboxProps, ClipRectProps, ElementNode, FillRectProps, ImageProps, LineProps, ProgressBarProps, SeparatorProps, StackProps, StrokeRectProps, TextProps, WindowProps };
declare global {
    namespace JSX {
        type Element = ElementNode;
        interface ElementChildrenAttribute {
            children: {};
        }
        interface IntrinsicElements {
            window: WindowProps & {
                key?: string;
                children?: ChildInput | ChildInput[];
            };
            stack: StackProps & {
                key?: string;
                children?: ChildInput | ChildInput[];
            };
            clip_rect: ClipRectProps & {
                key?: string;
                children?: ChildInput | ChildInput[];
            };
            text: TextProps & {
                key?: string;
            };
            button: ButtonProps & {
                key?: string;
            };
            checkbox: CheckboxProps & {
                key?: string;
            };
            image: ImageProps & {
                key?: string;
            };
            progress_bar: ProgressBarProps & {
                key?: string;
            };
            separator: SeparatorProps & {
                key?: string;
            };
            fill_rect: FillRectProps & {
                key?: string;
            };
            stroke_rect: StrokeRectProps & {
                key?: string;
            };
            line: LineProps & {
                key?: string;
            };
            fragment: {
                key?: string;
                children?: ChildInput | ChildInput[];
            };
        }
    }
}
//# sourceMappingURL=jsx-runtime.d.ts.map