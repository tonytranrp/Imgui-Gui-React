import { createNode, fragment } from "@igr/core";
function extractChildren(children) {
    if (Array.isArray(children)) {
        return children;
    }
    if (children === undefined) {
        return [];
    }
    return [children];
}
export function jsx(type, props, key) {
    const nextProps = props ?? {};
    const children = extractChildren(nextProps.children);
    if (typeof type === "function") {
        return type({ ...nextProps, key, children });
    }
    return createNode(type, { ...nextProps, key }, ...children);
}
export const jsxs = jsx;
export function Fragment(props) {
    return fragment(...extractChildren(props.children));
}
//# sourceMappingURL=jsx-runtime.js.map