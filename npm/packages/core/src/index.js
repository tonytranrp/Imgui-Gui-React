function normalizeChildren(input) {
    const output = [];
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
export function createNode(type, props, ...children) {
    const { key, ...rest } = props;
    const node = {
        type,
        props: rest,
        children: normalizeChildren(children)
    };
    if (typeof key === "string") {
        node.key = key;
    }
    return node;
}
export function fragment(...children) {
    return {
        type: "fragment",
        props: {},
        children: normalizeChildren(children)
    };
}
export function defineElement(type) {
    return (props, ...children) => createNode(type, props, ...children);
}
export function countNodes(node) {
    return 1 + node.children.reduce((total, child) => total + countNodes(child), 0);
}
export function serializeDocument(node) {
    return JSON.stringify(node, null, 2);
}
export function createTransportEnvelope(root, sequence = 0, options = {}) {
    return {
        kind: "igr.document.v1",
        sequence,
        ...(options.session !== undefined ? { session: options.session } : {}),
        ...(options.fonts !== undefined ? { fonts: options.fonts } : {}),
        ...(options.images !== undefined ? { images: options.images } : {}),
        root
    };
}
export function serializeTransportEnvelope(envelope) {
    return JSON.stringify(envelope);
}
export function parseTransportEnvelope(payload) {
    const envelope = JSON.parse(payload);
    if (envelope.kind !== "igr.document.v1") {
        throw new Error(`Unsupported transport kind: ${String(envelope.kind)}`);
    }
    return envelope;
}
//# sourceMappingURL=index.js.map