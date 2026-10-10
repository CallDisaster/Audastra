import { z } from "zod";

// 格式扩展只提供数据；前端不按来源名称或编号决定编辑权限。
const portSchema = z
  .object({
    id: z.string().min(1),
    label: z.string(),
    type: z.string(),
    direction: z.enum(["in", "out"]),
  })
  .passthrough();
const fieldSchema = z
  .object({
    id: z.string().min(1),
    label: z.string(),
    type: z.enum(["text", "number", "boolean"]),
    default: z.union([z.string(), z.number(), z.boolean()]),
  })
  .passthrough();
const definitionSchema = z
  .object({
    id: z.string().min(1),
    title: z.string(),
    namespace: z.string(),
    version: z.string(),
    kind: z.enum(["event", "flow", "action", "value"]),
    ports: z.array(portSchema),
    fields: z.array(fieldSchema),
  })
  .passthrough();
const nodeSchema = z
  .object({
    id: z.string(),
    definition: z.string(),
    x: z.number().finite(),
    y: z.number().finite(),
    values: z.record(z.string(), z.unknown()),
  })
  .passthrough();
const edgeSchema = z
  .object({
    id: z.string(),
    from: z.string(),
    output: z.string(),
    to: z.string(),
    input: z.string(),
  })
  .passthrough();
const graphSchema = z
  .object({
    id: z.string(),
    name: z.string(),
    folder: z.string(),
    nodes: z.array(nodeSchema),
    edges: z.array(edgeSchema),
  })
  .passthrough();
const objectSchema = z
  .object({
    id: z.string(),
    name: z.string(),
    category: z.string(),
    health: z.number().finite(),
    speed: z.number().finite(),
    enabled: z.boolean(),
  })
  .passthrough();
const projectSchema = z
  .object({
    format: z.literal("forge-project"),
    version: z.literal(1),
    name: z.string().min(1),
    definitions: z.array(definitionSchema),
    graphs: z.array(graphSchema),
    objects: z.array(objectSchema),
    scripts: z.array(
      z
        .object({
          id: z.string(),
          name: z.string(),
          language: z.string(),
          content: z.string(),
        })
        .passthrough(),
    ),
    assets: z.array(
      z
        .object({
          id: z.string(),
          name: z.string(),
          type: z.string(),
          size: z.number(),
          path: z.string(),
        })
        .passthrough(),
    ),
  })
  .passthrough();
export type Definition = z.infer<typeof definitionSchema>;
export type Port = z.infer<typeof portSchema>;
export type GraphNode = z.infer<typeof nodeSchema>;
export type Graph = z.infer<typeof graphSchema>;
export type Project = z.infer<typeof projectSchema>;
export type Issue = {
  message: string;
  node?: string;
  graph?: string;
  level: "error" | "warning";
};
export const uid = () => crypto.randomUUID();

function unique(ids: string[], label: string) {
  if (new Set(ids).size !== ids.length) throw new Error(`${label} ID 重复`);
}
function checkDefinitions(defs: Definition[]) {
  unique(
    defs.map((d) => d.id),
    "定义",
  );
  for (const d of defs) {
    unique(
      d.ports.map((p) => p.id),
      "端口",
    );
    unique(
      d.fields.map((f) => f.id),
      "参数",
    );
  }
}
export function parseDefinitions(value: unknown): Definition[] {
  const defs = z.array(definitionSchema).parse(value);
  checkDefinitions(defs);
  return defs;
}
export function parseProject(value: unknown): Project {
  const p = projectSchema.parse(value);
  checkDefinitions(p.definitions);
  unique(
    p.graphs.map((g) => g.id),
    "蓝图",
  );
  unique(
    p.objects.map((o) => o.id),
    "对象",
  );
  unique(
    p.scripts.map((s) => s.id),
    "脚本",
  );
  unique(
    p.assets.map((a) => a.id),
    "资源",
  );
  for (const g of p.graphs) {
    unique(
      g.nodes.map((n) => n.id),
      "节点",
    );
    unique(
      g.edges.map((e) => e.id),
      "连线",
    );
  }
  return p;
}
export function connectPorts(
  graph: Graph,
  definitions: Definition[],
  from: string,
  output: string,
  to: string,
  input: string,
): Graph {
  const source = graph.nodes.find((n) => n.id === from);
  const target = graph.nodes.find((n) => n.id === to);
  const a = definitions
    .find((d) => d.id === source?.definition)
    ?.ports.find((p) => p.id === output);
  const b = definitions
    .find((d) => d.id === target?.definition)
    ?.ports.find((p) => p.id === input);
  if (from === to) throw new Error("不能连接节点自身");
  if (!a || !b || a.direction !== "out" || b.direction !== "in")
    throw new Error("请选择一个输出和一个输入端口");
  if (a.type !== b.type && a.type !== "any" && b.type !== "any")
    throw new Error(`端口类型不匹配：${a.type} → ${b.type}`);
  if (
    graph.edges.some(
      (e) =>
        e.from === from &&
        e.output === output &&
        e.to === to &&
        e.input === input,
    )
  )
    return graph;
  return {
    ...graph,
    edges: [
      ...graph.edges.filter((e) => !(e.to === to && e.input === input)),
      { id: uid(), from, output, to, input },
    ],
  };
}
export function removeNodes(graph: Graph, ids: string[]): Graph {
  return {
    ...graph,
    nodes: graph.nodes.filter((n) => !ids.includes(n.id)),
    edges: graph.edges.filter(
      (e) => !ids.includes(e.from) && !ids.includes(e.to),
    ),
  };
}
export function validateProject(project: Project): Issue[] {
  const issues: Issue[] = [];
  for (const g of project.graphs) {
    for (const n of g.nodes) {
      const d = project.definitions.find((d) => d.id === n.definition);
      if (!d) {
        issues.push({
          level: "warning",
          graph: g.id,
          node: n.id,
          message: `缺少定义：${n.definition}`,
        });
        continue;
      }
      for (const f of d.fields) {
        const v = n.values[f.id];
        if (
          v !== undefined &&
          typeof v !== (f.type === "text" ? "string" : f.type)
        )
          issues.push({
            level: "error",
            graph: g.id,
            node: n.id,
            message: `${d.title} · ${f.label} 类型无效`,
          });
      }
    }
    const targets = new Set<string>();
    for (const e of g.edges) {
      try {
        connectPorts(
          { ...g, edges: [] },
          project.definitions,
          e.from,
          e.output,
          e.to,
          e.input,
        );
      } catch (err) {
        issues.push({
          level: "error",
          graph: g.id,
          node: e.to,
          message: err instanceof Error ? err.message : "无效连线",
        });
      }
      const key = `${e.to}:${e.input}`;
      if (targets.has(key))
        issues.push({
          level: "error",
          graph: g.id,
          node: e.to,
          message: "同一输入端口存在多条连线",
        });
      targets.add(key);
    }
  }
  return issues;
}
const port = (
  id: string,
  label: string,
  type: string,
  direction: "in" | "out",
): Port => ({ id, label, type, direction });
export const builtins: Definition[] = [
  {
    id: "core.interval",
    namespace: "core",
    version: "1.0",
    title: "定时事件",
    kind: "event",
    ports: [port("tick", "触发", "exec", "out")],
    fields: [
      { id: "interval", label: "间隔 / 秒", type: "number", default: 30 },
      { id: "repeat", label: "循环", type: "boolean", default: true },
    ],
  },
  {
    id: "core.branch",
    namespace: "core",
    version: "1.0",
    title: "条件分支",
    kind: "flow",
    ports: [
      port("in", "执行", "exec", "in"),
      port("condition", "条件", "boolean", "in"),
      port("yes", "成立", "exec", "out"),
      port("no", "不成立", "exec", "out"),
    ],
    fields: [
      {
        id: "expression",
        label: "条件表达式",
        type: "text",
        default: "wave < maxWaves",
      },
    ],
  },
  {
    id: "core.spawn",
    namespace: "core",
    version: "1.0",
    title: "创建对象",
    kind: "action",
    ports: [
      port("in", "执行", "exec", "in"),
      port("count", "数量", "number", "in"),
      port("then", "完成", "exec", "out"),
      port("objects", "对象", "object[]", "out"),
    ],
    fields: [
      {
        id: "template",
        label: "对象模板",
        type: "text",
        default: "unit.soldier",
      },
      { id: "count", label: "数量", type: "number", default: 4 },
      {
        id: "position",
        label: "生成位置",
        type: "text",
        default: "spawn.origin",
      },
    ],
  },
  {
    id: "core.order",
    namespace: "core",
    version: "1.0",
    title: "发送指令",
    kind: "action",
    ports: [
      port("in", "执行", "exec", "in"),
      port("objects", "目标", "object[]", "in"),
      port("then", "完成", "exec", "out"),
    ],
    fields: [
      { id: "command", label: "指令", type: "text", default: "move" },
      {
        id: "target",
        label: "目的地",
        type: "text",
        default: "route.destination",
      },
    ],
  },
  {
    id: "core.number",
    namespace: "core",
    version: "1.0",
    title: "数值变量",
    kind: "value",
    ports: [port("value", "数值", "number", "out")],
    fields: [
      { id: "name", label: "变量名", type: "text", default: "spawnCount" },
      { id: "value", label: "数值", type: "number", default: 4 },
    ],
  },
  {
    id: "core.log",
    namespace: "core",
    version: "1.0",
    title: "输出消息",
    kind: "action",
    ports: [
      port("in", "执行", "exec", "in"),
      port("then", "完成", "exec", "out"),
    ],
    fields: [
      { id: "message", label: "消息", type: "text", default: "全部波次已生成" },
    ],
  },
];
export function makeNode(d: Definition, x: number, y: number): GraphNode {
  return {
    id: uid(),
    definition: d.id,
    x,
    y,
    values: Object.fromEntries(d.fields.map((f) => [f.id, f.default])),
  };
}
export function blankProject(name = "未命名工程"): Project {
  return {
    format: "forge-project",
    version: 1,
    name,
    definitions: structuredClone(builtins),
    graphs: [
      { id: uid(), name: "主蓝图", folder: "逻辑", nodes: [], edges: [] },
    ],
    objects: [],
    scripts: [],
    assets: [],
  };
}
export function sampleProject(): Project {
  const p = blankProject("通用工程示例");
  const nodes = [
    makeNode(builtins[0], 70, 100),
    makeNode(builtins[1], 380, 100),
    makeNode(builtins[2], 700, 100),
    makeNode(builtins[3], 1020, 100),
    makeNode(builtins[4], 390, 425),
    makeNode(builtins[5], 700, 455),
  ];
  nodes.forEach((n, i) => (n.id = `node-${i}`));
  let graph: Graph = {
    id: "waves",
    name: "波次生成",
    folder: "逻辑",
    nodes,
    edges: [],
  };
  for (const [from, output, to, input] of [
    ["node-0", "tick", "node-1", "in"],
    ["node-1", "yes", "node-2", "in"],
    ["node-2", "then", "node-3", "in"],
    ["node-2", "objects", "node-3", "objects"],
    ["node-4", "value", "node-2", "count"],
    ["node-1", "no", "node-5", "in"],
  ])
    graph = connectPorts(graph, p.definitions, from, output, to, input);
  p.graphs = [
    graph,
    {
      id: "startup",
      name: "工程初始化",
      folder: "逻辑",
      nodes: [makeNode(builtins[5], 100, 100)],
      edges: [],
    },
  ];
  p.objects = [
    {
      id: "unit.soldier",
      name: "士兵",
      category: "单位",
      health: 420,
      speed: 270,
      enabled: true,
    },
    {
      id: "unit.archer",
      name: "弓箭手",
      category: "单位",
      health: 280,
      speed: 300,
      enabled: true,
    },
    {
      id: "unit.knight",
      name: "骑士",
      category: "单位",
      health: 800,
      speed: 320,
      enabled: true,
    },
    {
      id: "unit.mage",
      name: "法师",
      category: "单位",
      health: 350,
      speed: 280,
      enabled: true,
    },
  ];
  p.scripts = [
    {
      id: "config",
      name: "config.lua",
      language: "Lua",
      content:
        "-- 通用工程示例\nlocal config = {\n    maxWaves = 10,\n    spawnCount = 4,\n    interval = 30,\n}\n\nreturn config\n",
    },
  ];
  return p;
}
export function downloadJSON(value: unknown, name: string) {
  const url = URL.createObjectURL(
    new Blob([JSON.stringify(value, null, 2)], { type: "application/json" }),
  );
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
