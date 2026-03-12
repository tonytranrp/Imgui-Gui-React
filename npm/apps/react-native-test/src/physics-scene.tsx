import type {
  ElementNode,
  TransportEnvelope,
  TransportFontResource,
  TransportImageResource,
  TransportSession,
  TransportShaderResource
} from "@igr/core";
import { createTransportEnvelope, serializeTransportEnvelope } from "@igr/core";

export type PhysicsFrameRequest = {
  sequence?: number;
  frameIndex?: number;
  viewport?: {
    width?: number;
    height?: number;
  };
  deltaSeconds?: number;
};

type BodyState = {
  readonly id: string;
  readonly x: number;
  readonly y: number;
  readonly size: number;
  readonly color: string;
  readonly trailColor: string;
  readonly velocity: number;
  readonly energy: number;
};

const bodies: readonly BodyState[] = [
  { id: "alpha", x: 40, y: 28, size: 16, color: "#62C7FFEE", trailColor: "#62C7FF66", velocity: 4.8, energy: 0.72 },
  { id: "beta", x: 126, y: 102, size: 22, color: "#7EF0C3EE", trailColor: "#7EF0C366", velocity: 3.9, energy: 0.58 },
  { id: "gamma", x: 214, y: 44, size: 18, color: "#FFC972EE", trailColor: "#FFC97266", velocity: 5.2, energy: 0.81 },
  { id: "delta", x: 316, y: 132, size: 14, color: "#FF8A9EEE", trailColor: "#FF8A9E66", velocity: 2.7, energy: 0.41 }
];

const fonts: readonly TransportFontResource[] = [
  { key: "body-md", family: "Segoe UI", size: 15, weight: "medium", style: "normal", locale: "en-us" },
  { key: "mono-sm", family: "Consolas", size: 13, weight: "regular", style: "normal", locale: "en-us" }
];

const images: readonly TransportImageResource[] = [
  {
    key: "physics-gradient-card",
    texture: "physics-gradient",
    width: 112,
    height: 72,
    u: 0,
    v: 0,
    uvWidth: 1,
    uvHeight: 1,
    tint: "#FFFFFFFF"
  }
];

const shaders: readonly TransportShaderResource[] = [
  {
    key: "physics-energy-field",
    pixel: {
      language: "hlsl",
      entryPoint: "main",
      source: `
cbuffer IgrShaderConstants : register(b0) {
  float4 igrTint;
  float4 igrParam0;
  float4 igrParam1;
  float4 igrParam2;
  float4 igrParam3;
  float4 igrRect;
  float4 igrViewportAndTime;
  float4 igrFrameData;
};

struct PSInput {
  float4 position : SV_POSITION;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
  const float2 center = float2(0.5, 0.5);
  const float ring = smoothstep(0.72, 0.08, abs(length(input.uv - center) - 0.28));
  const float pulse = 0.55 + 0.45 * sin(igrParam0.x * 0.08 + igrViewportAndTime.z * 2.0);
  const float glow = (0.28 + ring * 0.72) * pulse;
  const float4 field = float4(0.20 + input.uv.x * 0.18, 0.50 + input.uv.y * 0.32, 0.92, glow * 0.85);
  return field * igrTint * input.color;
}
`
    },
    samplesTexture: false,
    blendMode: "alpha"
  }
];

const session: TransportSession = {
  name: "react-native-test-physics",
  targetBackend: "any",
  host: {
    hostMode: "injected_overlay",
    presentationMode: "host_managed",
    resizeMode: "host_managed",
    inputMode: "external_forwarded",
    clearTarget: false,
    restoreHostState: true
  }
};

type PhysicsFrameState = {
  readonly frameIndex: number;
  readonly viewportWidth: number;
  readonly viewportHeight: number;
  readonly deltaSeconds: number;
};

function resolveFrameState(request: number | PhysicsFrameRequest = 1): PhysicsFrameState {
  if (typeof request === "number") {
    return {
      frameIndex: request,
      viewportWidth: 1280,
      viewportHeight: 720,
      deltaSeconds: 1 / 60
    };
  }

  return {
    frameIndex: request.frameIndex ?? request.sequence ?? 1,
    viewportWidth: request.viewport?.width ?? 1280,
    viewportHeight: request.viewport?.height ?? 720,
    deltaSeconds: request.deltaSeconds ?? 1 / 60
  };
}

function Grid(): ElementNode {
  return (
    <>
      <fill_rect key="sim-backdrop" x={0} y={0} width={432} height={196} color="#0F1720F0" />
      {Array.from({ length: 7 }, (_, index) => (
        <line
          key={`grid-x-${index}`}
          x1={index * 72}
          y1={0}
          x2={index * 72}
          y2={196}
          thickness={1}
          color="#22405599"
        />
      ))}
      {Array.from({ length: 5 }, (_, index) => (
        <line
          key={`grid-y-${index}`}
          x1={0}
          y1={index * 49}
          x2={432}
          y2={index * 49}
          thickness={1}
          color="#22405599"
        />
      ))}
    </>
  );
}

function Bodies(): ElementNode {
  return (
    <>
      {bodies.map((body) => (
        <line
          key={`${body.id}-trail`}
          x1={body.x - 24}
          y1={body.y + body.size * 0.5}
          x2={body.x + body.size * 0.5}
          y2={body.y + body.size * 0.5}
          thickness={3}
          color={body.trailColor}
        />
      ))}
      {bodies.map((body) => (
        <fill_rect key={body.id} x={body.x} y={body.y} width={body.size} height={body.size} color={body.color} />
      ))}
      {bodies.map((body) => (
        <stroke_rect
          key={`${body.id}-outline`}
          x={body.x - 2}
          y={body.y - 2}
          width={body.size + 4}
          height={body.size + 4}
          thickness={1.5}
          color="#F7FBFFFF"
        />
      ))}
    </>
  );
}

function MetricsPanel(frame: PhysicsFrameState): ElementNode {
  return (
    <stack key="metrics" axis="vertical">
      <text key="metrics-title" value="Constraint Solver" font="body-md" />
      <text key="metrics-iterations" value="Iterations: 16" font="mono-sm" />
      <text key="metrics-contacts" value="Contacts: 42" font="mono-sm" />
      <text
        key="metrics-frame"
        value={`Frame ${frame.frameIndex}  dt=${(frame.deltaSeconds * 1000).toFixed(2)}ms`}
        font="mono-sm"
      />
      <progress_bar key="metrics-progress" label="Frame Stability" value={0.91} />
      <checkbox key="metrics-substep" label="Sub-step Integration" checked />
      <checkbox key="metrics-sleep" label="Sleep Islands" checked={false} />
    </stack>
  );
}

export function PhysicsOverlayScene(request: number | PhysicsFrameRequest = 1): ElementNode {
  const frame = resolveFrameState(request);
  const telemetryWidth = Math.max(292, Math.min(frame.viewportWidth - 64, 360));

  return (
    <>
      <window key="physics-overlay" title="Physics Sandbox" x={28} y={20} width={520} height={356}>
        <stack key="overlay-layout" axis="vertical">
          <text key="headline" value="External-host React scene" font="body-md" />
          <text
            key="subhead"
            value={`Frame ${frame.frameIndex} on ${frame.viewportWidth}x${frame.viewportHeight} host viewport`}
            font="mono-sm"
          />
          <clip_rect key="sim-world" width={432} height={196}>
            <Grid />
            <shader_rect
              key="energy-field"
              shader="physics-energy-field"
              x={12}
              y={14}
              width={404}
              height={164}
              tint="#7ED8FFFF"
              param0={`${frame.frameIndex}, ${frame.deltaSeconds.toFixed(6)}, ${frame.viewportWidth}, ${frame.viewportHeight}`}
              param1="0.18, 0.82, 0.35, 1.0"
            />
            <Bodies />
            <line key="impact-axis" x1={22} y1={164} x2={392} y2={42} thickness={2} color="#62C7FFAA" />
          </clip_rect>
          <stack key="bottom-row" axis="horizontal">
            <MetricsPanel {...frame} />
            <stack key="preview-stack" axis="vertical">
              <image
                key="preview-card"
                texture="physics-gradient"
                resource="physics-gradient-card"
                width={112}
                height={72}
                label="Energy heatmap"
              />
              <text key="preview-caption" value="Atlas-backed preview" font="mono-sm" />
            </stack>
          </stack>
        </stack>
      </window>
      <window key="physics-telemetry" title="Runtime Telemetry" x={568} y={20} width={telemetryWidth} height={324}>
        <stack key="telemetry-layout" axis="vertical">
          <text key="telemetry-title" value="Scene Bodies" font="body-md" />
          {bodies.map((body) => (
            <text
              key={`telemetry-${body.id}`}
              value={`${body.id.toUpperCase()}  vel=${body.velocity.toFixed(1)}  energy=${body.energy.toFixed(2)}`}
              font="mono-sm"
            />
          ))}
          <separator key="telemetry-separator" />
          <text
            key="telemetry-frame"
            value={`Viewport=${frame.viewportWidth}x${frame.viewportHeight} frame=${frame.frameIndex}`}
            font="mono-sm"
          />
          <text key="telemetry-note" value="All nodes in this overlay are emitted from TSX." font="mono-sm" />
        </stack>
      </window>
    </>
  );
}

export function createPhysicsTransportEnvelope(request: number | PhysicsFrameRequest = 1): TransportEnvelope {
  const frame = resolveFrameState(request);
  return createTransportEnvelope(PhysicsOverlayScene(request), frame.frameIndex, {
    session,
    fonts: [...fonts],
    images: [...images],
    shaders: [...shaders]
  });
}

export function createPhysicsTransportPayload(request: number | PhysicsFrameRequest = 1): string {
  return serializeTransportEnvelope(createPhysicsTransportEnvelope(request));
}
