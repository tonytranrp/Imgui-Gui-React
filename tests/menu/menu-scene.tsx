import type {
  ElementNode,
  TransportEnvelope,
  TransportFontResource,
  TransportImageResource,
  TransportSession
} from "@igr/core";
import { createTransportEnvelope, serializeTransportEnvelope } from "@igr/core";

import { menuShaders } from "./menu-shaders.js";

type MenuTab = "overview" | "render" | "react";

export type MenuRuntimeState = {
  readonly selectedTab: MenuTab;
  readonly showStats: boolean;
  readonly glowEnabled: boolean;
  readonly accentIndex: number;
  readonly applyCount: number;
  readonly lastAction: string;
};

export type MenuFrameRequest = {
  sequence?: number;
  frameIndex?: number;
  frame?: {
    frameIndex?: number;
    viewport?: {
      width?: number;
      height?: number;
    };
    deltaSeconds?: number;
  };
  viewport?: {
    width?: number;
    height?: number;
  };
  deltaSeconds?: number;
  state?: Partial<MenuRuntimeState>;
  stateJson?: string;
};

type ResolvedMenuFrame = {
  readonly frameIndex: number;
  readonly viewportWidth: number;
  readonly viewportHeight: number;
  readonly deltaSeconds: number;
  readonly state: MenuRuntimeState;
};

const fonts: readonly TransportFontResource[] = [
  { key: "body-md", family: "Segoe UI", size: 15, weight: "medium", style: "normal", locale: "en-us" },
  { key: "mono-sm", family: "Consolas", size: 13, weight: "regular", style: "normal", locale: "en-us" }
];

const images: readonly TransportImageResource[] = [
  {
    key: "menu-gradient-card",
    texture: "menu-gradient",
    width: 152,
    height: 92,
    u: 0,
    v: 0,
    uvWidth: 1,
    uvHeight: 1,
    tint: "#FFFFFFFF"
  }
];

const session: TransportSession = {
  name: "react-test-menu",
  targetBackend: "any",
  host: {
    hostMode: "owned_window",
    presentationMode: "backend_managed",
    resizeMode: "backend_managed",
    inputMode: "external_forwarded",
    clearTarget: true,
    restoreHostState: true
  }
};

const defaultState: MenuRuntimeState = {
  selectedTab: "overview",
  showStats: true,
  glowEnabled: true,
  accentIndex: 0,
  applyCount: 0,
  lastAction: "Ready"
};

const accentPalettes = [
  { accent: "#61C4FFF2", accentSoft: "#61C4FF52", accentText: "#DDF6FFFF", fill: "#10212BF2" },
  { accent: "#FFB362F2", accentSoft: "#FFB36252", accentText: "#FFF4E6FF", fill: "#2B1C10F2" },
  { accent: "#6EE3B9F2", accentSoft: "#6EE3B952", accentText: "#E8FFF7FF", fill: "#10251FF2" },
  { accent: "#F36D74F2", accentSoft: "#F36D7452", accentText: "#FFF0F2FF", fill: "#2B1418F2" }
] as const;

function normalizeState(input?: Partial<MenuRuntimeState> | null): MenuRuntimeState {
  const state = input ?? {};
  const selectedTab =
    state.selectedTab === "render" || state.selectedTab === "react" || state.selectedTab === "overview"
      ? state.selectedTab
      : defaultState.selectedTab;
  return {
    selectedTab,
    showStats: state.showStats ?? defaultState.showStats,
    glowEnabled: state.glowEnabled ?? defaultState.glowEnabled,
    accentIndex: Math.max(0, Math.trunc(state.accentIndex ?? defaultState.accentIndex)),
    applyCount: Math.max(0, Math.trunc(state.applyCount ?? defaultState.applyCount)),
    lastAction: state.lastAction ?? defaultState.lastAction
  };
}

function parseStateFromRequest(request: MenuFrameRequest): MenuRuntimeState {
  if (request.state) {
    return normalizeState(request.state);
  }

  if (request.stateJson) {
    try {
      return normalizeState(JSON.parse(request.stateJson) as Partial<MenuRuntimeState>);
    } catch {
      return defaultState;
    }
  }

  return defaultState;
}

function resolveFrame(request: number | MenuFrameRequest = 1): ResolvedMenuFrame {
  if (typeof request === "number") {
    return {
      frameIndex: request,
      viewportWidth: 1280,
      viewportHeight: 720,
      deltaSeconds: 1 / 60,
      state: defaultState
    };
  }

  return {
    frameIndex: request.frame?.frameIndex ?? request.frameIndex ?? request.sequence ?? 1,
    viewportWidth: request.frame?.viewport?.width ?? request.viewport?.width ?? 1280,
    viewportHeight: request.frame?.viewport?.height ?? request.viewport?.height ?? 720,
    deltaSeconds: request.frame?.deltaSeconds ?? request.deltaSeconds ?? 1 / 60,
    state: parseStateFromRequest(request)
  };
}

function tabLabel(tab: MenuTab, current: MenuTab): string {
  return tab === current ? `${tab[0]?.toUpperCase()}${tab.slice(1)} *` : `${tab[0]?.toUpperCase()}${tab.slice(1)}`;
}

function OverviewPanel(frame: ResolvedMenuFrame): ElementNode {
  const palette = accentPalettes[frame.state.accentIndex % accentPalettes.length]!;
  return (
    <stack key="panel-overview" axis="vertical">
      <text key="overview-title" value="Overview" font="body-md" />
      <text
        key="overview-copy"
        value={`Render a native menu from TSX and drive it with runtime state. Accent index ${frame.state.accentIndex}.`}
        font="mono-sm"
      />
      <fill_rect key="overview-fill" x={8} y={10} width={188} height={10} color={palette.accentSoft} />
      <line key="overview-line" x1={8} y1={34} x2={212} y2={34} thickness={2} color={palette.accent} />
      <progress_bar key="overview-progress" label="Apply readiness" value={Math.min(1, frame.state.applyCount / 8)} />
    </stack>
  );
}

function RenderPanel(frame: ResolvedMenuFrame): ElementNode {
  return (
    <stack key="panel-render" axis="vertical">
      <text key="render-title" value="Render Controls" font="body-md" />
      <shader_rect
        key="render-banner"
        shader="menu-banner-hlsl"
        x={0}
        y={0}
        width={280}
        height={48}
        tint={accentPalettes[frame.state.accentIndex % accentPalettes.length]!.accentText}
        param0={`${frame.state.accentIndex}, ${frame.deltaSeconds.toFixed(6)}, ${frame.viewportWidth}, ${frame.viewportHeight}`}
      />
      <shader_image
        key="render-preview"
        shader="menu-preview-glsl"
        texture="menu-gradient"
        resource="menu-gradient-card"
        width={152}
        height={92}
        label="GLSL preview"
        tint={frame.state.glowEnabled ? "#FFFFFFFF" : "#D6DEE4FF"}
        param0={`${frame.state.glowEnabled ? 1 : 0}, ${frame.state.accentIndex}, ${frame.frameIndex}, ${frame.state.applyCount}`}
      />
      <checkbox key="toggle-glow" label="Preview glow" checked={frame.state.glowEnabled} />
    </stack>
  );
}

function ReactPanel(frame: ResolvedMenuFrame): ElementNode {
  return (
    <stack key="panel-react" axis="vertical">
      <text key="react-title" value="React Runtime" font="body-md" />
      <text key="react-copy" value="Buttons, checkboxes, shaders, and windows in this menu all originate from TSX." font="mono-sm" />
      <text key="react-frame" value={`Frame ${frame.frameIndex}  dt=${(frame.deltaSeconds * 1000).toFixed(2)}ms`} font="mono-sm" />
      <checkbox key="toggle-stats" label="Diagnostics window" checked={frame.state.showStats} />
      <progress_bar key="react-progress" label="Bridge sync" value={0.92} />
    </stack>
  );
}

function ActivePanel(frame: ResolvedMenuFrame): ElementNode {
  switch (frame.state.selectedTab) {
    case "render":
      return <RenderPanel {...frame} />;
    case "react":
      return <ReactPanel {...frame} />;
    case "overview":
    default:
      return <OverviewPanel {...frame} />;
  }
}

export function ReactMenuScene(request: number | MenuFrameRequest = 1): ElementNode {
  const frame = resolveFrame(request);
  const palette = accentPalettes[frame.state.accentIndex % accentPalettes.length]!;
  const statsWindowX = Math.max(500, Math.min(frame.viewportWidth - 332, 540));

  return (
    <>
      <window key="menu-window" title="React Control Menu" x={28} y={24} width={468} height={388}>
        <stack key="menu-layout" axis="vertical">
          <text key="menu-headline" value={`Selected tab: ${frame.state.selectedTab}`} font="body-md" />
          <text key="menu-subhead" value={`Last action: ${frame.state.lastAction}`} font="mono-sm" />
          <stack key="menu-tabs" axis="horizontal">
            <button key="tab-overview" label={tabLabel("overview", frame.state.selectedTab)} enabled />
            <button key="tab-render" label={tabLabel("render", frame.state.selectedTab)} enabled />
            <button key="tab-react" label={tabLabel("react", frame.state.selectedTab)} enabled />
          </stack>
          <clip_rect key="menu-panel-clip" width={304} height={182}>
            <fill_rect key="menu-panel-bg" x={0} y={0} width={304} height={182} color={palette.fill} />
            <ActivePanel {...frame} />
          </clip_rect>
          <stack key="menu-actions" axis="horizontal">
            <button key="accent-prev" label="Accent -" enabled />
            <button key="accent-next" label="Accent +" enabled />
            <button key="action-apply" label={`Apply (${frame.state.applyCount})`} enabled />
            <button key="action-reset" label="Reset" enabled />
            <button key="action-close" label="Close" enabled />
          </stack>
        </stack>
      </window>

      {frame.state.showStats ? (
        <window key="menu-stats-window" title="Menu Diagnostics" x={statsWindowX} y={24} width={308} height={268}>
          <stack key="stats-layout" axis="vertical">
            <text key="stats-headline" value="Runtime state" font="body-md" />
            <text key="stats-tab" value={`Tab=${frame.state.selectedTab}`} font="mono-sm" />
            <text key="stats-accent" value={`Accent=${frame.state.accentIndex}`} font="mono-sm" />
            <text key="stats-apply" value={`ApplyCount=${frame.state.applyCount}`} font="mono-sm" />
            <text key="stats-action" value={frame.state.lastAction} font="mono-sm" />
            <separator key="stats-separator" />
            <image
              key="stats-image"
              texture="menu-gradient"
              resource="menu-gradient-card"
              width={152}
              height={92}
              label="Native backend texture"
            />
          </stack>
        </window>
      ) : null}
    </>
  );
}

export interface MenuTransportOptions {
  includeResources?: boolean;
  resourceMode?: "replace" | "retain";
}

export function createMenuTransportEnvelope(
  request: number | MenuFrameRequest = 1,
  options: MenuTransportOptions = {}
): TransportEnvelope {
  const frame = resolveFrame(request);
  const includeResources = options.includeResources ?? true;
  return createTransportEnvelope(ReactMenuScene(request), frame.frameIndex, {
    session,
    ...(options.resourceMode !== undefined ? { resourceMode: options.resourceMode } : {}),
    ...(includeResources ? { fonts: [...fonts], images: [...images], shaders: [...menuShaders] } : {})
  });
}

export function createMenuTransportPayload(
  request: number | MenuFrameRequest = 1,
  options: MenuTransportOptions = {}
): string {
  return serializeTransportEnvelope(createMenuTransportEnvelope(request, options));
}
