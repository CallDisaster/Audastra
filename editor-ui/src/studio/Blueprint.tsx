import { useRef, useState, useLayoutEffect, useCallback } from "react";
import type { PointerEvent as ReactPointerEvent } from "react";
import {
  Plus,
  Minus,
  Maximize,
  MousePointer2,
  Hand,
  Zap,
  GitBranch,
  Box,
  Hash,
  MoreHorizontal,
  Unplug,
} from "lucide-react";
import { connectPorts } from "./model";
import type { Definition, Graph, GraphNode, Port } from "./model";

const kindIcons = { event: Zap, flow: GitBranch, action: Box, value: Hash };
const kindLabels = {
  event: "事件",
  flow: "流程",
  action: "动作",
  value: "数据",
};
export const nodeWidth = 240;
function portPosition(n: GraphNode, d: Definition, p: Port) {
  const index = d.ports
    .filter((item) => item.direction === p.direction)
    .findIndex((item) => item.id === p.id);
  return {
    x: n.x + (p.direction === "out" ? nodeWidth : 0),
    y: n.y + 89 + index * 30,
  };
}
const curve = (a: { x: number; y: number }, b: { x: number; y: number }) =>
  `M${a.x},${a.y} C${a.x + Math.max(70, Math.abs(b.x - a.x) / 2)},${a.y} ${b.x - Math.max(70, Math.abs(b.x - a.x) / 2)},${b.y} ${b.x},${b.y}`;

export function Blueprint({
  graph,
  definitions,
  selected,
  onSelect,
  onChange,
  onAdd,
  notify,
  fitVersion,
}: {
  graph: Graph;
  definitions: Definition[];
  selected: string | null;
  onSelect: (id: string | null) => void;
  onChange: (g: Graph, label: string) => void;
  onAdd: () => void;
  notify: (m: string) => void;
  fitVersion: number;
}) {
  const host = useRef<HTMLDivElement>(null);
  const [size, setSize] = useState({ width: 800, height: 600 });
  const [view, setView] = useState({ x: 40, y: 50, z: 0.75 });
  const [mode, setMode] = useState<"select" | "pan">("select");
  const [drag, setDrag] = useState<{ id: string; x: number; y: number } | null>(
    null,
  );
  const dragRef = useRef<typeof drag>(null);
  const pointer = useRef<{
    type: "node" | "pan";
    id?: string;
    x: number;
    y: number;
    ox: number;
    oy: number;
  } | null>(null);
  const [connection, setConnection] = useState<{
    node: string;
    port: Port;
    x: number;
    y: number;
  } | null>(null);
  const [edgeSelected, setEdgeSelected] = useState<string | null>(null);
  const fit = useCallback(() => {
    if (!host.current) return;
    const { width, height } = host.current.getBoundingClientRect();
    const minX = Math.min(0, ...graph.nodes.map((n) => n.x));
    const minY = Math.min(0, ...graph.nodes.map((n) => n.y));
    const maxX = Math.max(700, ...graph.nodes.map((n) => n.x + nodeWidth));
    const maxY = Math.max(440, ...graph.nodes.map((n) => n.y + 235));
    const z = Math.min(
      1,
      Math.max(
        0.25,
        Math.min((width - 90) / (maxX - minX), (height - 125) / (maxY - minY)),
      ),
    );
    setView({
      x: (width - (maxX - minX) * z) / 2 - minX * z,
      y: Math.max(25, (height - (maxY - minY) * z) / 2 - minY * z - 20),
      z,
    });
  }, [graph.nodes]);
  const fitRef = useRef(fit);
  useLayoutEffect(() => {
    fitRef.current = fit;
  });
  useLayoutEffect(() => {
    fitRef.current();
  }, [graph.id, fitVersion]);
  useLayoutEffect(() => {
    const observer = new ResizeObserver((entries) => {
      const rect = entries[0].contentRect;
      setSize({ width: rect.width, height: rect.height });
      fitRef.current();
    });
    if (host.current) observer.observe(host.current);
    return () => observer.disconnect();
  }, []);
  const displayedNodes = graph.nodes.map((n) =>
    drag?.id === n.id ? { ...n, x: drag.x, y: drag.y } : n,
  );
  function point(e: { clientX: number; clientY: number }) {
    const r = host.current!.getBoundingClientRect();
    return {
      x: (e.clientX - r.left - view.x) / view.z,
      y: (e.clientY - r.top - view.y) / view.z,
    };
  }
  function startNode(e: ReactPointerEvent, n: GraphNode) {
    if (e.button !== 0 || mode === "pan") return;
    e.stopPropagation();
    onSelect(n.id);
    setEdgeSelected(null);
    pointer.current = {
      type: "node",
      id: n.id,
      x: e.clientX,
      y: e.clientY,
      ox: n.x,
      oy: n.y,
    };
    e.currentTarget.setPointerCapture(e.pointerId);
  }
  function endPointer() {
    if (pointer.current?.type === "node" && dragRef.current) {
      const moved = dragRef.current;
      onChange(
        {
          ...graph,
          nodes: graph.nodes.map((n) =>
            n.id === moved.id ? { ...n, x: moved.x, y: moved.y } : n,
          ),
        },
        "移动节点",
      );
    }
    pointer.current = null;
    dragRef.current = null;
    setDrag(null);
  }
  function portClick(n: GraphNode, p: Port) {
    if (!connection) {
      const pos = portPosition(
        n,
        definitions.find((d) => d.id === n.definition)!,
        p,
      );
      setConnection({ node: n.id, port: p, ...pos });
      notify("选择另一端口 · Esc 取消");
      return;
    }
    try {
      const from =
        connection.port.direction === "out"
          ? { node: connection.node, port: connection.port.id }
          : { node: n.id, port: p.id };
      const to =
        connection.port.direction === "out"
          ? { node: n.id, port: p.id }
          : { node: connection.node, port: connection.port.id };
      onChange(
        connectPorts(
          graph,
          definitions,
          from.node,
          from.port,
          to.node,
          to.port,
        ),
        "连接端口",
      );
      notify("已连接端口");
    } catch (err) {
      notify((err as Error).message);
    }
    setConnection(null);
  }
  function zoom(delta: number) {
    setView((v) => {
      const z = Math.min(1.8, Math.max(0.25, v.z + delta));
      const w = (host.current?.clientWidth ?? 800) / 2,
        h = (host.current?.clientHeight ?? 600) / 2;
      return { x: w - ((w - v.x) * z) / v.z, y: h - ((h - v.y) * z) / v.z, z };
    });
  }
  const miniMaxX = Math.max(1380, ...graph.nodes.map((n) => n.x + 260));
  const miniMaxY = Math.max(800, ...graph.nodes.map((n) => n.y + 240));
  return (
    <div
      ref={host}
      className={`blueprint ${mode === "pan" ? "pan-mode" : ""}`}
      tabIndex={0}
      aria-label="蓝图画布"
      onKeyDown={(e) => {
        if (e.key === "Escape") {
          setConnection(null);
          setEdgeSelected(null);
        }
        if ((e.key === "Delete" || e.key === "Backspace") && edgeSelected) {
          e.stopPropagation();
          onChange(
            {
              ...graph,
              edges: graph.edges.filter((x) => x.id !== edgeSelected),
            },
            "删除连线",
          );
          setEdgeSelected(null);
        }
      }}
      onPointerDown={(e) => {
        if (
          e.button === 1 ||
          (e.button === 0 && (e.target === e.currentTarget || mode === "pan"))
        ) {
          e.preventDefault();
          e.currentTarget.focus();
          pointer.current = {
            type: "pan",
            x: e.clientX,
            y: e.clientY,
            ox: view.x,
            oy: view.y,
          };
          e.currentTarget.setPointerCapture(e.pointerId);
          if (mode !== "pan") {
            onSelect(null);
            setEdgeSelected(null);
          }
        }
      }}
      onPointerMove={(e) => {
        const p = pointer.current;
        if (p?.type === "pan")
          setView((v) => ({
            ...v,
            x: p.ox + e.clientX - p.x,
            y: p.oy + e.clientY - p.y,
          }));
        if (p?.type === "node") {
          const next = {
            id: p.id!,
            x: Math.round((p.ox + (e.clientX - p.x) / view.z) / 8) * 8,
            y: Math.round((p.oy + (e.clientY - p.y) / view.z) / 8) * 8,
          };
          dragRef.current = next;
          setDrag(next);
        }
        if (connection) setConnection((c) => (c ? { ...c, ...point(e) } : c));
      }}
      onPointerUp={endPointer}
      onPointerCancel={() => {
        pointer.current = null;
        dragRef.current = null;
        setDrag(null);
      }}
      onWheel={(e) => zoom(e.deltaY > 0 ? -0.05 : 0.05)}
      style={{
        backgroundPosition: `${view.x}px ${view.y}px`,
        backgroundSize: `${24 * view.z}px ${24 * view.z}px`,
      }}
    >
      <div className="canvas-caption">
        <span className="eyebrow">BLUEPRINT</span>
        <h1>{graph.name}</h1>
        <span>
          {graph.nodes.length} 个节点 <i> / </i> {graph.edges.length} 条连线
        </span>
      </div>
      <div
        className="graph-world"
        style={{
          transform: `translate(${view.x}px, ${view.y}px) scale(${view.z})`,
        }}
      >
        <svg className="wires" width="1" height="1" aria-label="蓝图连线">
          {graph.edges.map((edge) => {
            const a = displayedNodes.find((n) => n.id === edge.from),
              b = displayedNodes.find((n) => n.id === edge.to);
            const ad = definitions.find((d) => d.id === a?.definition),
              bd = definitions.find((d) => d.id === b?.definition);
            const ap = ad?.ports.find((p) => p.id === edge.output),
              bp = bd?.ports.find((p) => p.id === edge.input);
            if (!a || !b || !ad || !bd || !ap || !bp) return null;
            const path = curve(
              portPosition(a, ad, ap),
              portPosition(b, bd, bp),
            );
            return (
              <g
                key={edge.id}
                className={`wire type-${ap.type.replace("[]", "")} ${edgeSelected === edge.id ? "selected" : ""}`}
              >
                <path d={path} />
                <path
                  className="wire-hit"
                  d={path}
                  onPointerDown={(e) => e.stopPropagation()}
                  onClick={(e) => {
                    e.stopPropagation();
                    setEdgeSelected(edge.id);
                    onSelect(null);
                    host.current?.focus();
                    notify("已选择连线 · Delete 删除");
                  }}
                />
              </g>
            );
          })}
          {connection &&
            (() => {
              const n = displayedNodes.find((n) => n.id === connection.node)!;
              const d = definitions.find((d) => d.id === n.definition)!;
              return (
                <path
                  className="pending-wire"
                  d={curve(portPosition(n, d, connection.port), connection)}
                />
              );
            })()}
        </svg>
        {displayedNodes.map((n) => {
          const def = definitions.find((d) => d.id === n.definition);
          const Icon = def ? kindIcons[def.kind] : Unplug;
          const inputs = def?.ports.filter((p) => p.direction === "in") ?? [],
            outputs = def?.ports.filter((p) => p.direction === "out") ?? [];
          return (
            <div
              key={n.id}
              data-node={n.id}
              className={`graph-node kind-${def?.kind ?? "unknown"} ${selected === n.id ? "selected" : ""}`}
              style={{ left: n.x, top: n.y }}
              onPointerDown={(e) => startNode(e, n)}
              onClick={() => onSelect(n.id)}
            >
              <div className="node-title">
                <Icon size={17} />
                <strong>{def?.title ?? n.definition}</strong>
                <MoreHorizontal size={16} />
              </div>
              <div className="node-kind">
                {def
                  ? `${kindLabels[def.kind]} · ${def.namespace}`
                  : "缺少定义"}
              </div>
              <div
                className="port-rows"
                style={{ height: Math.max(inputs.length, outputs.length) * 30 }}
              >
                {(["in", "out"] as const).map((direction) => (
                  <div key={direction} className={`ports ports-${direction}`}>
                    {(direction === "in" ? inputs : outputs).map((p) => (
                      <button
                        key={p.id}
                        className={`port type-${p.type.replace("[]", "")} ${connection?.node === n.id && connection.port.id === p.id ? "connecting" : ""}`}
                        aria-label={`${def?.title} ${p.label} ${direction === "in" ? "输入" : "输出"}`}
                        onPointerDown={(e) => e.stopPropagation()}
                        onClick={(e) => {
                          e.stopPropagation();
                          portClick(n, p);
                        }}
                      >
                        <span
                          className={`port-dot ${p.type === "exec" ? "exec" : ""}`}
                        />
                        <span>{p.label}</span>
                      </button>
                    ))}
                  </div>
                ))}
              </div>
              <div className="node-values">
                {def ? (
                  def.fields.slice(0, 3).map((f) => (
                    <div key={f.id}>
                      <span>{f.label}</span>
                      <code>{String(n.values[f.id] ?? f.default)}</code>
                    </div>
                  ))
                ) : (
                  <span>原始内容已保留</span>
                )}
              </div>
            </div>
          );
        })}
      </div>
      {!graph.nodes.length && (
        <div className="canvas-empty">
          <GitBranch size={34} />
          <h2>空白蓝图</h2>
          <button className="primary" onClick={onAdd}>
            <Plus size={15} />
            添加节点
          </button>
        </div>
      )}
      <div className="canvas-tools" onPointerDown={(e) => e.stopPropagation()}>
        <button
          aria-label="选择工具"
          title="选择工具"
          className={mode === "select" ? "active" : ""}
          onClick={() => setMode("select")}
        >
          <MousePointer2 size={16} />
        </button>
        <button
          aria-label="平移工具"
          title="平移工具"
          className={mode === "pan" ? "active" : ""}
          onClick={() => setMode("pan")}
        >
          <Hand size={16} />
        </button>
        <i />
        <button onClick={() => zoom(-0.1)} aria-label="缩小">
          <Minus size={16} />
        </button>
        <span>{Math.round(view.z * 100)}%</span>
        <button onClick={() => zoom(0.1)} aria-label="放大">
          <Plus size={16} />
        </button>
        <button onClick={fit} aria-label="适应画布" title="适应画布">
          <Maximize size={16} />
        </button>
        <i />
        <button className="add-node" onClick={onAdd}>
          <Plus size={15} />
          添加节点
        </button>
      </div>
      <div
        className="minimap"
        title="点击定位"
        onPointerDown={(e) => e.stopPropagation()}
        onClick={(e) => {
          const r = e.currentTarget.getBoundingClientRect();
          const x = ((e.clientX - r.left) / r.width) * miniMaxX,
            y = ((e.clientY - r.top) / r.height) * miniMaxY;
          setView((v) => ({
            ...v,
            x: host.current!.clientWidth / 2 - x * v.z,
            y: host.current!.clientHeight / 2 - y * v.z,
          }));
        }}
      >
        <svg viewBox={`0 0 ${miniMaxX} ${miniMaxY}`} preserveAspectRatio="none">
          {graph.nodes.map((n) => (
            <rect
              key={n.id}
              x={n.x}
              y={n.y}
              width={240}
              height={150}
              rx={10}
              className={n.id === selected ? "selected" : ""}
            />
          ))}
          <rect
            className="mini-view"
            x={-view.x / view.z}
            y={-view.y / view.z}
            width={size.width / view.z}
            height={size.height / view.z}
          />
        </svg>
      </div>
    </div>
  );
}
