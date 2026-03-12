export type ScalarValue = string | number | boolean | null;
export type DocumentProps = Record<string, unknown>;
export type ChildInput = ElementNode | string | number | boolean | null | undefined | ChildInput[];
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
export interface SeparatorProps extends DocumentProps {
}
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
    session?: TransportSession;
    fonts?: TransportFontResource[];
    images?: TransportImageResource[];
    root: ElementNode;
}
export interface TransportEnvelopeOptions {
    session?: TransportSession;
    fonts?: TransportFontResource[];
    images?: TransportImageResource[];
}
export declare function createNode<TProps extends DocumentProps>(type: string, props: TProps, ...children: ChildInput[]): ElementNode<TProps>;
export declare function fragment(...children: ChildInput[]): ElementNode;
export declare function defineElement<TProps extends DocumentProps>(type: string): (props: TProps, ...children: ChildInput[]) => ElementNode<TProps>;
export declare function countNodes(node: ElementNode): number;
export declare function serializeDocument(node: ElementNode | ElementNode[]): string;
export declare function createTransportEnvelope(root: ElementNode, sequence?: number, options?: TransportEnvelopeOptions): TransportEnvelope;
export declare function serializeTransportEnvelope(envelope: TransportEnvelope): string;
export declare function parseTransportEnvelope(payload: string): TransportEnvelope;
//# sourceMappingURL=index.d.ts.map