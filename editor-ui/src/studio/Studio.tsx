import { useEffect, useRef, useState } from "react";
import type { ReactNode } from "react";
import {
  GitBranch,
  FolderOpen,
  Folder,
  ChevronRight,
  ChevronDown,
  Search,
  Plus,
  X,
  Box,
  Code2,
  Image,
  Puzzle,
  Settings2,
  Save,
  Undo2,
  Redo2,
  Check,
  Circle,
  PanelsTopLeft,
  PanelRightClose,
  PanelLeftClose,
  Download,
  Upload,
  Copy,
  Trash2,
  FileCode2,
  Command,
  CheckCircle2,
  TriangleAlert,
  ArrowUpRight,
  SlidersHorizontal,
  Hexagon,
  ListTree,
  FileText,
  Pencil,
  Grip,
  ExternalLink,
} from "lucide-react";
import { Blueprint } from "./Blueprint";
import { DesignSheet } from "./DesignSheet";
import {
  blankProject,
  downloadJSON,
  makeNode,
  parseDefinitions,
  parseProject,
  removeNodes,
  sampleProject,
  uid,
  validateProject,
} from "./model";
import type { Definition, Graph, Issue, Project } from "./model";
import "./studio.css";

type Mode = "blueprint" | "script" | "data" | "assets" | "extensions";
type ModalKind =
  | "nodes"
  | "project"
  | "settings"
  | "commands"
  | "definitions"
  | null;
const modes = [
  { id: "blueprint", name: "蓝图", icon: GitBranch },
  { id: "script", name: "脚本", icon: Code2 },
  { id: "data", name: "数据", icon: Box },
  { id: "assets", name: "资源", icon: Image },
  { id: "extensions", name: "扩展", icon: Puzzle },
] as const;
const storageKey = "forge-studio-project-v1";
function initialProject(): { project: Project; error: string } {
  try {
    const saved = localStorage.getItem(storageKey);
    return {
      project: saved ? parseProject(JSON.parse(saved)) : sampleProject(),
      error: "",
    };
  } catch {
    return {
      project: sampleProject(),
      error: "本地工程无法读取，已打开示例；原存储未覆盖",
    };
  }
}
function Modal({
  title,
  children,
  close,
  wide = false,
}: {
  title: string;
  children: ReactNode;
  close: () => void;
  wide?: boolean;
}) {
  const ref = useRef<HTMLDialogElement>(null);
  useEffect(() => {
    ref.current?.showModal();
  }, []);
  return (
    <dialog
      ref={ref}
      className={`studio-dialog ${wide ? "wide" : ""}`}
      onCancel={(e) => {
        e.preventDefault();
        close();
      }}
      onClick={(e) => {
        if (e.target === e.currentTarget) close();
      }}
    >
      <div className="modal-title">
        <h2>{title}</h2>
        <button aria-label="关闭对话框" onClick={close}>
          <X size={18} />
        </button>
      </div>
      {children}
    </dialog>
  );
}
function Editable({
  value,
  onChange,
  label,
  type = "text",
}: {
  value: string | number;
  onChange: (v: string) => void;
  label: string;
  type?: string;
}) {
  const [draft, setDraft] = useState(String(value));
  const [invalid, setInvalid] = useState(false);
  function commit() {
    if (
      type === "number" &&
      (draft.trim() === "" || !Number.isFinite(Number(draft)))
    ) {
      setInvalid(true);
      return;
    }
    setInvalid(false);
    if (draft !== String(value)) onChange(draft);
  }
  return (
    <input
      aria-label={label}
      aria-invalid={invalid}
      title={invalid ? "请输入有效数值" : label}
      type={type}
      step={type === "number" ? "any" : undefined}
      value={draft}
      onChange={(e) => setDraft(e.target.value)}
      onBlur={commit}
      onKeyDown={(e) => {
        if (e.key === "Enter") e.currentTarget.blur();
        if (e.key === "Escape") {
          setDraft(String(value));
          setInvalid(false);
        }
      }}
    />
  );
}
function parseText(text: string): unknown {
  if (text.length > 12 * 1024 * 1024) throw new Error("工程文件超过 12 MB");
  return JSON.parse(text);
}

export default function Studio() {
  const [initial] = useState(initialProject);
  const [history, setHistory] = useState<{
    past: Project[];
    current: Project;
    future: Project[];
  }>({ past: [], current: initial.project, future: [] });
  const project = history.current;
  const [saved, setSaved] = useState(JSON.stringify(initial.project));
  const [mode, setMode] = useState<Mode>("blueprint");
  const [graphId, setGraphId] = useState(project.graphs[0]?.id ?? "");
  const graph =
    project.graphs.find((g) => g.id === graphId) ?? project.graphs[0];
  const [selected, setSelected] = useState<string | null>(
    project.graphs[0]?.nodes[2]?.id ?? null,
  );
  const [objectId, setObjectId] = useState(project.objects[0]?.id ?? "");
  const object =
    project.objects.find((o) => o.id === objectId) ?? project.objects[0];
  const [scriptId, setScriptId] = useState(project.scripts[0]?.id ?? "");
  const script =
    project.scripts.find((s) => s.id === scriptId) ?? project.scripts[0];
  const [modal, setModal] = useState<ModalKind>(null);
  const [search, setSearch] = useState("");
  const [paletteSearch, setPaletteSearch] = useState("");
  const [treeOpen, setTreeOpen] = useState(true);
  const [inspectorOpen, setInspectorOpen] = useState(true);
  const [folderOpen, setFolderOpen] = useState(true);
  const [showDesign, setShowDesign] = useState(location.hash === "#design");
  const [mobileInspector, setMobileInspector] = useState(false);
  const [mobileTree, setMobileTree] = useState(false);
  const [theme, setTheme] = useState("graphite");
  const [compact, setCompact] = useState(false);
  const [view, setView] = useState<"graph" | "document">("graph");
  const [documentDraft, setDocumentDraft] = useState("");
  const [fitVersion, setFitVersion] = useState(0);
  const [toast, setToast] = useState(initial.error);
  const toastTimer = useRef<ReturnType<typeof setTimeout> | undefined>(
    undefined,
  );
  const [issues, setIssues] = useState<Issue[] | null>(null);
  const [checkedSnapshot, setCheckedSnapshot] = useState("");
  const [diagnosticsOpen, setDiagnosticsOpen] = useState(false);
  const [log, setLog] = useState("就绪");
  const [newName, setNewName] = useState("未命名工程");
  const [importMode, setImportMode] = useState<"project" | "definitions">(
    "project",
  );
  const fileRef = useRef<HTMLInputElement>(null);
  const assetRef = useRef<HTMLInputElement>(null);
  const [definitionDraft, setDefinitionDraft] = useState(
    JSON.stringify(
      [
        {
          id: "custom.function",
          namespace: "custom",
          version: "1.0",
          title: "自定义函数",
          kind: "action",
          ports: [
            { id: "in", label: "执行", type: "exec", direction: "in" },
            { id: "out", label: "完成", type: "exec", direction: "out" },
          ],
          fields: [{ id: "value", label: "参数", type: "text", default: "" }],
        },
      ],
      null,
      2,
    ),
  );
  const [dialogError, setDialogError] = useState("");
  const dirty = JSON.stringify(project) !== saved;
  const currentNode = graph?.nodes.find((n) => n.id === selected);
  const currentDefinition = project.definitions.find(
    (d) => d.id === currentNode?.definition,
  );
  function notify(message: string) {
    setToast(message);
    clearTimeout(toastTimer.current);
    toastTimer.current = setTimeout(() => setToast(""), 4000);
  }
  useEffect(() => () => clearTimeout(toastTimer.current), []);
  function edit(next: Project, label: string) {
    if (JSON.stringify(next) === JSON.stringify(project)) return;
    setHistory((h) => ({
      past: [...h.past.slice(-99), h.current],
      current: next,
      future: [],
    }));
    setLog(label);
  }
  function updateGraph(next: Graph, label: string) {
    edit(
      {
        ...project,
        graphs: project.graphs.map((g) => (g.id === next.id ? next : g)),
      },
      label,
    );
  }
  function undo() {
    setHistory((h) =>
      h.past.length
        ? {
            past: h.past.slice(0, -1),
            current: h.past[h.past.length - 1],
            future: [h.current, ...h.future],
          }
        : h,
    );
    setLog("撤销");
  }
  function redo() {
    setHistory((h) =>
      h.future.length
        ? {
            past: [...h.past, h.current],
            current: h.future[0],
            future: h.future.slice(1),
          }
        : h,
    );
    setLog("重做");
  }
  function save() {
    try {
      localStorage.setItem(storageKey, JSON.stringify(project));
      setSaved(JSON.stringify(project));
      notify("工程已保存到本机");
    } catch {
      notify("本地保存失败，请导出工程文件");
    }
  }
  function check() {
    const result = validateProject(project);
    setIssues(result);
    setCheckedSnapshot(JSON.stringify(project));
    setDiagnosticsOpen(true);
    setLog(`检查完成 · ${result.length} 项问题`);
  }
  function removeSelected() {
    if (graph && selected) {
      updateGraph(removeNodes(graph, [selected]), "删除节点");
      setSelected(null);
    }
  }
  function duplicateSelected() {
    if (!graph || !currentNode) return;
    const copy = {
      ...structuredClone(currentNode),
      id: uid(),
      x: currentNode.x + 40,
      y: currentNode.y + 40,
    };
    updateGraph({ ...graph, nodes: [...graph.nodes, copy] }, "复制节点");
    setSelected(copy.id);
  }
  function openModal(kind: ModalKind) {
    setDialogError("");
    setPaletteSearch("");
    setModal(kind);
  }

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      const editing = (e.target as HTMLElement).matches(
        "input,textarea,select,[contenteditable]",
      );
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "s") {
        e.preventDefault();
        save();
      }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        openModal("commands");
      }
      if (modal || showDesign || editing) return;
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "z") {
        e.preventDefault();
        if (e.shiftKey) redo();
        else undo();
      }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "y") {
        e.preventDefault();
        redo();
      }
      if (mode === "blueprint" && view === "graph") {
        if (e.key === "Delete" || e.key === "Backspace") {
          e.preventDefault();
          removeSelected();
        }
        if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "d") {
          e.preventDefault();
          duplicateSelected();
        }
      }
    };
    window.addEventListener("keydown", handler);
    return () => window.removeEventListener("keydown", handler);
  });
  useEffect(() => {
    const onUnload = (e: BeforeUnloadEvent) => {
      if (dirty) e.preventDefault();
    };
    window.addEventListener("beforeunload", onUnload);
    return () => window.removeEventListener("beforeunload", onUnload);
  }, [dirty]);
  function selectGraph(id: string) {
    setGraphId(id);
    setSelected(null);
    setMode("blueprint");
    setView("graph");
  }
  function addGraph() {
    const g = {
      id: uid(),
      name: `新建蓝图 ${project.graphs.length + 1}`,
      folder: "逻辑",
      nodes: [],
      edges: [],
    };
    edit({ ...project, graphs: [...project.graphs, g] }, "新建蓝图");
    selectGraph(g.id);
  }
  function addNode(def: Definition) {
    if (!graph) return;
    const node = makeNode(
      def,
      150 + (graph.nodes.length % 4) * 200,
      240 + Math.floor(graph.nodes.length / 4) * 110,
    );
    updateGraph({ ...graph, nodes: [...graph.nodes, node] }, "添加节点");
    setSelected(node.id);
    setModal(null);
    setView("graph");
    setFitVersion((v) => v + 1);
  }
  function replaceProject(p: Project) {
    setHistory((h) => ({
      past: [...h.past.slice(-99), h.current],
      current: p,
      future: [],
    }));
    setGraphId(p.graphs[0]?.id ?? "");
    setSelected(null);
    setIssues(null);
    setMode("blueprint");
    setView("graph");
    setModal(null);
    notify("已打开工程");
  }
  function importDefs(defs: Definition[]) {
    const merged = [...project.definitions];
    defs.forEach((def) => {
      const index = merged.findIndex((d) => d.id === def.id);
      if (index < 0) merged.push(def);
      else merged[index] = def;
    });
    edit({ ...project, definitions: merged }, "导入定义");
    setModal(null);
    notify(`已导入 ${defs.length} 个定义`);
  }
  async function importFile(file: File | undefined) {
    if (!file) return;
    try {
      const value = parseText(await file.text());
      if (importMode === "definitions") importDefs(parseDefinitions(value));
      else replaceProject(parseProject(value));
    } catch (err) {
      setDialogError(err instanceof Error ? err.message : "文件无效");
      notify("导入失败，当前工程未改变");
    }
  }
  function exportProject() {
    downloadJSON(
      project,
      `${project.name.replace(/[<>:"/\\|?*]/g, "_")}.forge.json`,
    );
    notify("已导出工程 JSON");
  }
  function newScript() {
    const s = {
      id: uid(),
      name: `script-${project.scripts.length + 1}.lua`,
      language: "Lua",
      content: "",
    };
    edit({ ...project, scripts: [...project.scripts, s] }, "新建脚本");
    setScriptId(s.id);
    setMode("script");
  }
  function newObject() {
    const o = {
      id: `object.${Date.now()}`,
      name: "新建对象",
      category: "自定义",
      health: 100,
      speed: 0,
      enabled: true,
    };
    edit({ ...project, objects: [...project.objects, o] }, "新建对象");
    setObjectId(o.id);
  }
  function showSheet() {
    setShowDesign(true);
    window.history.replaceState(null, "", "#design");
    setModal(null);
  }
  const results = project.definitions.filter((d) =>
    `${d.title} ${d.id} ${d.namespace}`
      .toLowerCase()
      .includes(paletteSearch.toLowerCase()),
  );
  const commands = [
    { name: "新建蓝图", detail: "蓝图", action: "newGraph" },
    { name: "新建脚本", detail: "脚本", action: "newScript" },
    { name: "打开工程", detail: "JSON", action: "open" },
    { name: "保存工程", detail: "Ctrl S", action: "save" },
    { name: "导出工程", detail: "JSON", action: "export" },
    { name: "检查蓝图", detail: "文档结构", action: "check" },
    { name: "界面图纸", detail: "设计", action: "design" },
    ...project.graphs.map((g) => ({
      name: g.name,
      detail: "蓝图",
      action: g.id,
    })),
  ];
  function runCommand(action: string) {
    switch (action) {
      case "newGraph":
        addGraph();
        break;
      case "newScript":
        newScript();
        break;
      case "open":
        openModal("project");
        break;
      case "save":
        save();
        break;
      case "export":
        exportProject();
        break;
      case "check":
        check();
        break;
      case "design":
        showSheet();
        break;
      default:
        selectGraph(action);
    }
  }
  if (showDesign)
    return (
      <DesignSheet
        close={() => {
          setShowDesign(false);
          window.history.replaceState(
            null,
            "",
            location.pathname + location.search,
          );
        }}
      />
    );
  return (
    <div
      className={`studio theme-${theme} ${mobileInspector ? "mobile-inspector" : ""} ${mobileTree ? "mobile-tree" : ""} ${compact ? "compact" : ""} ${!treeOpen ? "tree-hidden" : ""} ${!inspectorOpen ? "inspector-hidden" : ""}`}
    >
      <header className="app-header">
        <div className="brand">
          <Hexagon size={22} strokeWidth={1.5} />
          <strong>
            FORGE<span>STUDIO</span>
          </strong>
        </div>
        <span className="header-divider" />
        <button className="project-switch" onClick={() => openModal("project")}>
          <FolderOpen size={15} />
          <span>{project.name}</span>
          <ChevronDown size={12} />
        </button>
        <span className="local-badge">本地</span>
        <div className="header-spacer" />
        <button
          className="command-search"
          onClick={() => openModal("commands")}
        >
          <Search size={14} />
          <span>搜索命令、文档…</span>
          <kbd>Ctrl K</kbd>
        </button>
        <button
          className="header-icon"
          title="界面图纸"
          aria-label="界面图纸"
          onClick={showSheet}
        >
          <PanelsTopLeft size={17} />
        </button>
        <button className="save-btn" onClick={save}>
          <Save size={15} />
          保存{dirty && <i className="dirty-dot" />}
        </button>
        <button className="primary export-btn" onClick={exportProject}>
          <Download size={14} />
          <span>导出工程</span>
        </button>
      </header>
      <div className="workbench">
        <nav className="module-rail" aria-label="编辑模块">
          <div className="rail-top">
            {modes.map((m) => (
              <button
                key={m.id}
                title={m.name}
                aria-label={m.name}
                aria-pressed={mode === m.id}
                className={mode === m.id ? "active" : ""}
                onClick={() => {
                  setMode(m.id);
                  setSearch("");
                }}
              >
                <m.icon size={20} />
                <span>{m.name}</span>
              </button>
            ))}
          </div>
          <div className="rail-bottom">
            <button
              title="工程树"
              aria-label="切换工程树"
              onClick={() => {
                if (window.innerWidth < 1024) setMobileTree(!mobileTree);
                else setTreeOpen(!treeOpen);
              }}
            >
              <PanelLeftClose size={19} />
            </button>
            <button
              title="设置"
              aria-label="设置"
              onClick={() => openModal("settings")}
            >
              <Settings2 size={20} />
            </button>
          </div>
        </nav>
        <aside className="explorer">
          <div className="panel-heading">
            <strong>工程</strong>
            <button title="新建蓝图" aria-label="新建蓝图" onClick={addGraph}>
              <Plus size={16} />
            </button>
          </div>
          <label className="search-field">
            <Search size={14} />
            <input
              aria-label="搜索工程"
              placeholder="搜索工程…"
              value={search}
              onChange={(e) => setSearch(e.target.value)}
            />
            <kbd>⌕</kbd>
          </label>
          <div className="project-tree">
            <button
              className="tree-folder"
              onClick={() => setFolderOpen(!folderOpen)}
            >
              {folderOpen ? (
                <ChevronDown size={13} />
              ) : (
                <ChevronRight size={13} />
              )}
              <FolderOpen size={15} />
              <b>蓝图</b>
              <small>{project.graphs.length}</small>
            </button>
            {folderOpen &&
              project.graphs
                .filter((g) => g.name.includes(search))
                .map((g) => (
                  <button
                    key={g.id}
                    className={`tree-item ${mode === "blueprint" && graph?.id === g.id ? "active" : ""}`}
                    onClick={() => selectGraph(g.id)}
                  >
                    <GitBranch size={14} />
                    <span>{g.name}</span>
                    {mode === "blueprint" && graph?.id === g.id && <i />}
                  </button>
                ))}
            <button className="tree-folder" onClick={() => setMode("script")}>
              <ChevronDown size={13} />
              <Folder size={15} />
              <b>脚本</b>
              <small>{project.scripts.length}</small>
            </button>
            {project.scripts
              .filter((s) => s.name.includes(search))
              .map((s) => (
                <button
                  key={s.id}
                  className={`tree-item ${mode === "script" && script?.id === s.id ? "active" : ""}`}
                  onClick={() => {
                    setScriptId(s.id);
                    setMode("script");
                  }}
                >
                  <FileCode2 size={14} />
                  <span>{s.name}</span>
                </button>
              ))}
            <button
              className={`tree-folder ${mode === "data" ? "active" : ""}`}
              onClick={() => setMode("data")}
            >
              <ChevronRight size={13} />
              <Box size={15} />
              <b>对象数据</b>
              <small>{project.objects.length}</small>
            </button>
            <button
              className={`tree-folder ${mode === "assets" ? "active" : ""}`}
              onClick={() => setMode("assets")}
            >
              <ChevronRight size={13} />
              <Image size={15} />
              <b>资源</b>
              <small>{project.assets.length}</small>
            </button>
            <button
              className={`tree-folder ${mode === "extensions" ? "active" : ""}`}
              onClick={() => setMode("extensions")}
            >
              <ChevronRight size={13} />
              <Puzzle size={15} />
              <b>扩展定义</b>
              <small>{project.definitions.length}</small>
            </button>
          </div>
          <div className="explorer-bottom">
            <div className="panel-subheading">
              {mode === "blueprint" ? "节点大纲" : "工程信息"}
              <ListTree size={13} />
            </div>
            {mode === "blueprint" && graph ? (
              graph.nodes.map((n) => (
                <button
                  key={n.id}
                  className={`outline-item ${selected === n.id ? "active" : ""}`}
                  onClick={() => setSelected(n.id)}
                >
                  <Circle size={7} />
                  <span>
                    {project.definitions.find((d) => d.id === n.definition)
                      ?.title ?? n.definition}
                  </span>
                </button>
              ))
            ) : (
              <div className="project-meta">
                <span>{project.name}</span>
                <code>forge-project / v1</code>
              </div>
            )}
          </div>
          <button
            className="explorer-footer"
            onClick={() => openModal("project")}
          >
            <FolderOpen size={14} />
            打开工程
            <ArrowUpRight size={13} />
          </button>
        </aside>
        <div className="editor-column">
          <div className="document-tabs">
            <button
              className={`document-tab ${mode === "blueprint" ? "active" : ""}`}
              onClick={() => {
                if (graph) selectGraph(graph.id);
              }}
            >
              <GitBranch size={14} />
              <span>{graph?.name ?? "蓝图"}</span>
              {dirty && <i />}
            </button>
            {project.scripts.slice(0, 1).map((s) => (
              <button
                key={s.id}
                className={`document-tab ${mode === "script" ? "active" : ""}`}
                onClick={() => {
                  setScriptId(s.id);
                  setMode("script");
                }}
              >
                <Code2 size={14} />
                <span>{s.name}</span>
              </button>
            ))}
            <div className="header-spacer" />
            <button
              title="切换属性面板"
              aria-label="切换属性面板"
              onClick={() => {
                if (window.innerWidth < 720)
                  setMobileInspector(!mobileInspector);
                else setInspectorOpen(!inspectorOpen);
              }}
            >
              <PanelRightClose size={16} />
            </button>
          </div>
          <div className="editor-toolbar">
            <div className="breadcrumbs">
              <span>{modes.find((m) => m.id === mode)?.name}</span>
              <ChevronRight size={12} />
              <strong>
                {mode === "blueprint"
                  ? (graph?.name ?? "未选择蓝图")
                  : mode === "script"
                    ? (script?.name ?? "脚本")
                    : project.name}
              </strong>
            </div>
            <div className="header-spacer" />
            <button
              title="撤销 Ctrl+Z"
              aria-label="撤销"
              disabled={!history.past.length}
              onClick={undo}
            >
              <Undo2 size={15} />
            </button>
            <button
              title="重做 Ctrl+Y"
              aria-label="重做"
              disabled={!history.future.length}
              onClick={redo}
            >
              <Redo2 size={15} />
            </button>
            <i className="toolbar-divider" />
            {mode === "blueprint" && (
              <div className="segmented">
                <button
                  className={view === "graph" ? "active" : ""}
                  onClick={() => setView("graph")}
                >
                  <GitBranch size={13} />
                  蓝图
                </button>
                <button
                  className={view === "document" ? "active" : ""}
                  onClick={() => {
                    setDocumentDraft(JSON.stringify(graph, null, 2));
                    setView("document");
                  }}
                >
                  <Code2 size={13} />
                  文档
                </button>
              </div>
            )}
            <button className="check-button" onClick={check}>
              <CheckCircle2 size={14} />
              检查
            </button>
          </div>
          <main className="editor-content">
            {mode === "blueprint" &&
              (graph ? (
                view === "graph" ? (
                  <Blueprint
                    graph={graph}
                    definitions={project.definitions}
                    selected={selected}
                    onSelect={setSelected}
                    onChange={updateGraph}
                    onAdd={() => openModal("nodes")}
                    notify={notify}
                    fitVersion={fitVersion}
                  />
                ) : (
                  <div className="document-editor">
                    <div className="page-title compact-title">
                      <div>
                        <span className="eyebrow">GRAPH DOCUMENT</span>
                        <h1>{graph.name}.json</h1>
                      </div>
                      <button
                        className="primary"
                        onClick={() => {
                          try {
                            const value = parseText(documentDraft);
                            const p = parseProject({
                              ...project,
                              graphs: project.graphs.map((g) =>
                                g.id === graph.id ? value : g,
                              ),
                            });
                            edit(p, "应用蓝图文档");
                            notify("蓝图文档已应用");
                          } catch {
                            notify("蓝图文档格式无效，原内容未改变");
                          }
                        }}
                      >
                        <Check size={15} />
                        应用修改
                      </button>
                    </div>
                    <textarea
                      className="code-editor"
                      aria-label="蓝图 JSON 文档"
                      value={documentDraft}
                      spellCheck={false}
                      onChange={(e) => setDocumentDraft(e.target.value)}
                    />
                  </div>
                )
              ) : (
                <div className="empty-state">
                  <GitBranch size={36} />
                  <h2>没有蓝图</h2>
                  <button className="primary" onClick={addGraph}>
                    <Plus size={15} />
                    新建蓝图
                  </button>
                </div>
              ))}
            {mode === "data" && (
              <div className="data-page">
                <div className="page-title">
                  <div>
                    <span className="eyebrow">OBJECT DATABASE</span>
                    <h1>
                      对象数据 <small>{project.objects.length}</small>
                    </h1>
                  </div>
                  <button className="primary" onClick={newObject}>
                    <Plus size={15} />
                    新建对象
                  </button>
                </div>
                <div className="page-filter">
                  <label className="search-field">
                    <Search size={14} />
                    <input
                      placeholder="搜索对象名称、ID…"
                      aria-label="搜索对象"
                      value={search}
                      onChange={(e) => setSearch(e.target.value)}
                    />
                  </label>
                  <span>全部对象</span>
                </div>
                <div className="table-scroll">
                  <table className="data-table">
                    <thead>
                      <tr>
                        <th>对象名称</th>
                        <th>标识符</th>
                        <th>类别</th>
                        <th>生命值</th>
                        <th>移动速度</th>
                        <th>启用</th>
                      </tr>
                    </thead>
                    <tbody>
                      {project.objects
                        .filter((o) => `${o.name} ${o.id}`.includes(search))
                        .map((o) => (
                          <tr
                            key={o.id}
                            onClick={() => setObjectId(o.id)}
                            className={object?.id === o.id ? "selected" : ""}
                          >
                            <td>
                              <span className="object-icon">
                                <Box size={14} />
                              </span>
                              <b>{o.name}</b>
                            </td>
                            <td>
                              <code>{o.id}</code>
                            </td>
                            <td>{o.category}</td>
                            <td>{o.health}</td>
                            <td>{o.speed}</td>
                            <td>
                              <span
                                className={`status-pill ${o.enabled ? "enabled" : ""}`}
                              >
                                {o.enabled ? "启用" : "停用"}
                              </span>
                            </td>
                          </tr>
                        ))}
                    </tbody>
                  </table>
                  {!project.objects.length && (
                    <div className="table-empty">没有对象</div>
                  )}
                </div>
                <div className="table-foot">
                  {project.objects.length} 个对象<span>在属性面板中编辑</span>
                </div>
              </div>
            )}
            {mode === "script" && (
              <div className="document-editor">
                <div className="page-title compact-title">
                  <div>
                    <span className="eyebrow">SCRIPT EDITOR</span>
                    <h1>{script?.name ?? "脚本"}</h1>
                  </div>
                  <button onClick={newScript}>
                    <Plus size={15} />
                    新建脚本
                  </button>
                </div>
                {script ? (
                  <>
                    <div className="script-subbar">
                      <FileCode2 size={14} />
                      {script.language}
                      <span>UTF-8</span>
                      <small>{script.content.split("\n").length} 行</small>
                    </div>
                    <div className="script-body">
                      <div className="line-numbers">
                        {script.content.split("\n").map((_, i) => (
                          <span key={i}>{i + 1}</span>
                        ))}
                      </div>
                      <textarea
                        key={script.id}
                        spellCheck={false}
                        aria-label="脚本内容"
                        className="code-editor"
                        value={script.content}
                        onChange={(e) =>
                          edit(
                            {
                              ...project,
                              scripts: project.scripts.map((s) =>
                                s.id === script.id
                                  ? { ...s, content: e.target.value }
                                  : s,
                              ),
                            },
                            "编辑脚本",
                          )
                        }
                      />
                    </div>
                  </>
                ) : (
                  <div className="empty-state">
                    <Code2 size={34} />
                    <h2>没有脚本</h2>
                    <button className="primary" onClick={newScript}>
                      新建脚本
                    </button>
                  </div>
                )}
              </div>
            )}
            {mode === "assets" && (
              <div className="data-page">
                <div className="page-title">
                  <div>
                    <span className="eyebrow">ASSET LIBRARY</span>
                    <h1>
                      资源索引 <small>{project.assets.length}</small>
                    </h1>
                  </div>
                  <button
                    className="primary"
                    onClick={() => assetRef.current?.click()}
                  >
                    <Plus size={15} />
                    添加索引
                  </button>
                </div>
                <div className="asset-notice">
                  <FileText size={14} />
                  仅保存文件信息与路径
                </div>
                {project.assets.length ? (
                  <table className="data-table">
                    <thead>
                      <tr>
                        <th>文件</th>
                        <th>类型</th>
                        <th>大小</th>
                        <th>路径</th>
                        <th />
                      </tr>
                    </thead>
                    <tbody>
                      {project.assets.map((a) => (
                        <tr key={a.id}>
                          <td>
                            <FileText size={14} />
                            {a.name}
                          </td>
                          <td>{a.type || "文件"}</td>
                          <td>{(a.size / 1024).toFixed(1)} KB</td>
                          <td>
                            <code>{a.path}</code>
                          </td>
                          <td>
                            <button
                              title="移除索引"
                              aria-label={`移除 ${a.name}`}
                              onClick={() =>
                                edit(
                                  {
                                    ...project,
                                    assets: project.assets.filter(
                                      (x) => x.id !== a.id,
                                    ),
                                  },
                                  "移除资源索引",
                                )
                              }
                            >
                              <X size={14} />
                            </button>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                ) : (
                  <div className="empty-state">
                    <Image size={38} />
                    <h2>没有资源</h2>
                    <button onClick={() => assetRef.current?.click()}>
                      <Plus size={15} />
                      添加资源索引
                    </button>
                  </div>
                )}
              </div>
            )}
            {mode === "extensions" && (
              <div className="data-page">
                <div className="page-title">
                  <div>
                    <span className="eyebrow">DEFINITION REGISTRY</span>
                    <h1>
                      扩展定义 <small>{project.definitions.length}</small>
                    </h1>
                  </div>
                  <button
                    className="primary"
                    onClick={() => openModal("definitions")}
                  >
                    <Plus size={15} />
                    导入定义
                  </button>
                </div>
                <div className="page-filter">
                  <label className="search-field">
                    <Search size={14} />
                    <input
                      aria-label="搜索定义"
                      placeholder="搜索函数、命名空间…"
                      value={paletteSearch}
                      onChange={(e) => setPaletteSearch(e.target.value)}
                    />
                  </label>
                  <span>
                    {new Set(project.definitions.map((d) => d.namespace)).size}{" "}
                    个命名空间
                  </span>
                </div>
                <div className="table-scroll">
                  <table className="data-table">
                    <thead>
                      <tr>
                        <th>函数</th>
                        <th>定义 ID</th>
                        <th>来源</th>
                        <th>版本</th>
                        <th>操作</th>
                      </tr>
                    </thead>
                    <tbody>
                      {results.map((d) => (
                        <tr key={d.id}>
                          <td>
                            <Puzzle size={14} />
                            <b>{d.title}</b>
                          </td>
                          <td>
                            <code>{d.id}</code>
                          </td>
                          <td>{d.namespace}</td>
                          <td>{d.version}</td>
                          <td>
                            <button
                              aria-label={`添加 ${d.title}`}
                              disabled={!graph}
                              onClick={() => {
                                setMode("blueprint");
                                addNode(d);
                              }}
                            >
                              <Plus size={14} />
                              添加
                            </button>
                            <button
                              title="编辑定义"
                              aria-label={`编辑定义 ${d.title}`}
                              onClick={() => {
                                setDefinitionDraft(
                                  JSON.stringify([d], null, 2),
                                );
                                openModal("definitions");
                              }}
                            >
                              <Pencil size={13} />
                            </button>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              </div>
            )}
          </main>
          <div className={`diagnostics ${diagnosticsOpen ? "open" : ""}`}>
            <button
              className="diagnostic-toggle"
              onClick={() => setDiagnosticsOpen(!diagnosticsOpen)}
            >
              <span>
                {diagnosticsOpen ? (
                  <ChevronDown size={12} />
                ) : (
                  <ChevronRight size={12} />
                )}
                问题{issues !== null && <b>{issues.length}</b>}
              </span>
              <span>
                {issues === null
                  ? "未检查"
                  : checkedSnapshot !== JSON.stringify(project)
                    ? "内容已更改 · 重新检查"
                    : issues.length
                      ? `${issues.length} 项待处理`
                      : "文档结构检查通过"}
                <i />
                <FileText size={12} />
                {log}
              </span>
            </button>
            {diagnosticsOpen && (
              <div className="diagnostic-list">
                {issues === null ? (
                  <button onClick={check}>检查文档</button>
                ) : issues.length ? (
                  issues.map((i, index) => (
                    <button
                      key={index}
                      onClick={() => {
                        if (i.graph) selectGraph(i.graph);
                        setSelected(i.node ?? null);
                      }}
                    >
                      <TriangleAlert size={14} />
                      <span>{i.message}</span>
                      <ArrowUpRight size={13} />
                    </button>
                  ))
                ) : (
                  <div className="diagnostic-pass">
                    <CheckCircle2 size={15} />
                    节点定义、参数与连线结构通过
                  </div>
                )}
              </div>
            )}
          </div>
        </div>
        <aside className="inspector">
          <div className="panel-heading">
            <strong>属性</strong>
            <SlidersHorizontal size={15} />
          </div>
          <div className="inspector-scroll">
            {mode === "blueprint" && currentNode ? (
              <>
                <div className="inspector-identity">
                  <div className="entity-icon">
                    <GitBranch size={22} />
                  </div>
                  <span className="eyebrow">
                    {currentDefinition?.kind.toUpperCase() ?? "NODE"}
                  </span>
                  <h2>{currentDefinition?.title ?? currentNode.definition}</h2>
                  <code>{currentNode.definition}</code>
                </div>
                <section className="property-section">
                  <h3>
                    <ChevronDown size={12} />
                    参数
                  </h3>
                  {currentDefinition ? (
                    currentDefinition.fields.map((f) => (
                      <label className="property" key={f.id}>
                        <span>{f.label}</span>
                        {f.type === "boolean" ? (
                          <button
                            className={`switch ${currentNode.values[f.id] ? "on" : ""}`}
                            role="switch"
                            aria-checked={Boolean(currentNode.values[f.id])}
                            aria-label={f.label}
                            onClick={() =>
                              updateGraph(
                                {
                                  ...graph!,
                                  nodes: graph!.nodes.map((n) =>
                                    n.id === currentNode.id
                                      ? {
                                          ...n,
                                          values: {
                                            ...n.values,
                                            [f.id]: !n.values[f.id],
                                          },
                                        }
                                      : n,
                                  ),
                                },
                                "修改参数",
                              )
                            }
                          >
                            <i />
                          </button>
                        ) : (
                          <Editable
                            key={`${currentNode.id}-${f.id}-${String(currentNode.values[f.id])}`}
                            label={f.label}
                            value={String(
                              currentNode.values[f.id] ?? f.default,
                            )}
                            type={f.type === "number" ? "number" : "text"}
                            onChange={(v) =>
                              updateGraph(
                                {
                                  ...graph!,
                                  nodes: graph!.nodes.map((n) =>
                                    n.id === currentNode.id
                                      ? {
                                          ...n,
                                          values: {
                                            ...n.values,
                                            [f.id]:
                                              f.type === "number"
                                                ? Number(v)
                                                : v,
                                          },
                                        }
                                      : n,
                                  ),
                                },
                                "修改参数",
                              )
                            }
                          />
                        )}
                      </label>
                    ))
                  ) : (
                    <>
                      <p className="missing-definition">缺少节点定义</p>
                      <button onClick={() => openModal("definitions")}>
                        <Upload size={14} />
                        导入定义
                      </button>
                      <pre className="raw-content">
                        {JSON.stringify(currentNode.values, null, 2)}
                      </pre>
                    </>
                  )}
                </section>
                <section className="property-section">
                  <h3>
                    <ChevronDown size={12} />
                    端口
                  </h3>
                  {currentDefinition?.ports.map((p) => (
                    <div className="port-property" key={p.id}>
                      <Circle size={7} />
                      <span>{p.label}</span>
                      <code>{p.type}</code>
                      <small>{p.direction === "in" ? "输入" : "输出"}</small>
                    </div>
                  ))}
                </section>
                <section className="property-section">
                  <h3>
                    <ChevronDown size={12} />
                    定义
                  </h3>
                  <div className="read-property">
                    <span>来源</span>
                    <code>{currentDefinition?.namespace ?? "未知"}</code>
                  </div>
                  <div className="read-property">
                    <span>版本</span>
                    <code>{currentDefinition?.version ?? "—"}</code>
                  </div>
                  <div className="read-property">
                    <span>位置</span>
                    <code>
                      {currentNode.x}, {currentNode.y}
                    </code>
                  </div>
                </section>
                <div className="node-actions">
                  <button onClick={duplicateSelected}>
                    <Copy size={14} />
                    复制
                  </button>
                  <button className="danger-text" onClick={removeSelected}>
                    <Trash2 size={14} />
                    删除
                  </button>
                </div>
              </>
            ) : mode === "data" && object ? (
              <>
                <div className="inspector-identity">
                  <div className="entity-icon">
                    <Box size={22} />
                  </div>
                  <span className="eyebrow">OBJECT</span>
                  <h2>{object.name}</h2>
                  <code>{object.id}</code>
                </div>
                <section className="property-section">
                  <h3>
                    <ChevronDown size={12} />
                    对象属性
                  </h3>
                  {(["name", "category", "health", "speed"] as const).map(
                    (field, i) => (
                      <label key={field} className="property">
                        <span>{["名称", "类别", "生命值", "移动速度"][i]}</span>
                        <Editable
                          key={`${object.id}-${field}-${object[field]}`}
                          label={
                            [
                              "对象名称",
                              "对象类别",
                              "对象生命值",
                              "对象移动速度",
                            ][i]
                          }
                          value={object[field]}
                          type={
                            typeof object[field] === "number"
                              ? "number"
                              : "text"
                          }
                          onChange={(v) =>
                            edit(
                              {
                                ...project,
                                objects: project.objects.map((o) =>
                                  o.id === object.id
                                    ? {
                                        ...o,
                                        [field]:
                                          typeof object[field] === "number"
                                            ? Number(v)
                                            : v,
                                      }
                                    : o,
                                ),
                              },
                              "修改对象",
                            )
                          }
                        />
                      </label>
                    ),
                  )}
                  <label className="property">
                    <span>启用</span>
                    <input
                      type="checkbox"
                      aria-label="对象启用"
                      checked={object.enabled}
                      onChange={(e) =>
                        edit(
                          {
                            ...project,
                            objects: project.objects.map((o) =>
                              o.id === object.id
                                ? { ...o, enabled: e.target.checked }
                                : o,
                            ),
                          },
                          "修改对象",
                        )
                      }
                    />
                  </label>
                </section>
              </>
            ) : (
              <>
                <div className="inspector-identity">
                  <div className="entity-icon">
                    <FolderOpen size={23} />
                  </div>
                  <span className="eyebrow">PROJECT</span>
                  <h2>
                    {mode === "blueprint"
                      ? (graph?.name ?? project.name)
                      : project.name}
                  </h2>
                </div>
                <section className="property-section">
                  <h3>
                    <ChevronDown size={12} />
                    文档属性
                  </h3>
                  <label className="property">
                    <span>工程名称</span>
                    <Editable
                      key={project.name}
                      value={project.name}
                      label="工程名称"
                      onChange={(v) => {
                        if (v.trim())
                          edit({ ...project, name: v.trim() }, "重命名工程");
                      }}
                    />
                  </label>
                  {mode === "blueprint" && graph && (
                    <label className="property">
                      <span>蓝图名称</span>
                      <Editable
                        key={`${graph.id}-${graph.name}`}
                        value={graph.name}
                        label="蓝图名称"
                        onChange={(v) => {
                          if (v.trim())
                            updateGraph(
                              { ...graph, name: v.trim() },
                              "重命名蓝图",
                            );
                        }}
                      />
                    </label>
                  )}
                  <div className="read-property">
                    <span>蓝图</span>
                    <code>{project.graphs.length}</code>
                  </div>
                  <div className="read-property">
                    <span>扩展定义</span>
                    <code>{project.definitions.length}</code>
                  </div>
                  <div className="read-property">
                    <span>格式</span>
                    <code>Forge JSON v1</code>
                  </div>
                </section>
                <section className="property-section">
                  <h3>
                    <ChevronDown size={12} />
                    连接
                  </h3>
                  <div className="read-property">
                    <span>C++ Core</span>
                    <span className="muted">未连接</span>
                  </div>
                  <div className="read-property">
                    <span>工程存储</span>
                    <span>本机</span>
                  </div>
                </section>
              </>
            )}
          </div>
          <button
            className="inspector-footer"
            onClick={() => openModal("definitions")}
          >
            <Puzzle size={14} />
            定义管理
            <ExternalLink size={12} />
          </button>
        </aside>
      </div>
      <footer className="status-bar">
        <span>
          <span className="status-light" />
          {dirty ? "未保存" : "就绪"}
        </span>
        <span>{project.graphs.length} 张蓝图</span>
        <div className="header-spacer" />
        <span>本地工程</span>
        <span>Core 未连接</span>
        <span>UTF-8</span>
        <button onClick={() => openModal("settings")}>
          <Grip size={12} />
          默认布局
        </button>
      </footer>
      {toast && (
        <div className="toast" role="status">
          <CheckCircle2 size={15} />
          {toast}
        </div>
      )}
      <input
        hidden
        ref={fileRef}
        type="file"
        accept=".json"
        onChange={(e) => {
          void importFile(e.target.files?.[0]);
          e.target.value = "";
        }}
      />
      <input
        hidden
        ref={assetRef}
        type="file"
        multiple
        onChange={(e) => {
          const assets = Array.from(e.target.files ?? []).map((f) => ({
            id: uid(),
            name: f.name,
            type: f.type,
            size: f.size,
            path: f.name,
          }));
          edit(
            { ...project, assets: [...project.assets, ...assets] },
            "添加资源索引",
          );
          e.target.value = "";
        }}
      />
      {modal && (
        <Modal
          title={
            {
              nodes: "添加节点",
              project: "工程",
              settings: "设置",
              commands: "命令",
              definitions: "导入定义",
            }[modal]
          }
          close={() => setModal(null)}
          wide={modal === "definitions"}
        >
          {(modal === "nodes" || modal === "commands") && (
            <>
              <label className="palette-search">
                <Search size={19} />
                <input
                  autoFocus
                  aria-label="搜索命令或节点"
                  placeholder={
                    modal === "nodes"
                      ? "搜索函数、类型、来源…"
                      : "搜索命令、文档…"
                  }
                  value={paletteSearch}
                  onChange={(e) => setPaletteSearch(e.target.value)}
                />
              </label>
              <div className="palette-results">
                {modal === "nodes" ? (
                  results.length ? (
                    results.map((d) => (
                      <button key={d.id} onClick={() => addNode(d)}>
                        <div className={`palette-icon kind-${d.kind}`}>
                          <GitBranch size={18} />
                        </div>
                        <div>
                          <b>{d.title}</b>
                          <code>{d.id}</code>
                        </div>
                        <small>{d.namespace}</small>
                        <Plus size={15} />
                      </button>
                    ))
                  ) : (
                    <div className="table-empty">没有匹配的节点</div>
                  )
                ) : (
                  commands
                    .filter((c) => c.name.includes(paletteSearch))
                    .map((c) => (
                      <button
                        key={c.name}
                        onClick={() => {
                          setModal(null);
                          runCommand(c.action);
                        }}
                      >
                        <Command size={17} />
                        <b>{c.name}</b>
                        <small>{c.detail}</small>
                        <ChevronRight size={14} />
                      </button>
                    ))
                )}
              </div>
              <div className="modal-foot">
                <kbd>Esc</kbd>关闭
                <span>
                  {modal === "nodes" ? `${results.length} 个定义` : "命令面板"}
                </span>
              </div>
            </>
          )}
          {modal === "project" && (
            <div className="modal-body">
              <label className="dialog-label">
                工程名称
                <input
                  value={newName}
                  aria-label="新工程名称"
                  onChange={(e) => setNewName(e.target.value)}
                />
              </label>
              <div className="dialog-actions">
                <button
                  className="primary"
                  onClick={() =>
                    replaceProject(blankProject(newName.trim() || "未命名工程"))
                  }
                >
                  <Plus size={15} />
                  新建工程
                </button>
                <button
                  onClick={() => {
                    setImportMode("project");
                    fileRef.current?.click();
                  }}
                >
                  <FolderOpen size={15} />
                  打开 JSON 工程
                </button>
              </div>
              <div className="dialog-separator" />
              <button
                className="dialog-row"
                onClick={() => replaceProject(sampleProject())}
              >
                <GitBranch size={18} />
                <span>通用工程示例</span>
                <ChevronRight size={15} />
              </button>
              {dirty && <p className="muted">当前未保存 · 切换后可撤销恢复</p>}
              {dialogError && <pre className="dialog-error">{dialogError}</pre>}
            </div>
          )}
          {modal === "settings" && (
            <div className="modal-body">
              <label className="dialog-label">
                界面主题
                <select
                  aria-label="界面主题"
                  value={theme}
                  onChange={(e) => setTheme(e.target.value)}
                >
                  <option value="graphite">石墨</option>
                  <option value="midnight">深海</option>
                  <option value="paper">浅色</option>
                </select>
              </label>
              <label className="check-row">
                <input
                  type="checkbox"
                  checked={compact}
                  onChange={(e) => setCompact(e.target.checked)}
                />
                紧凑密度
              </label>
              <label className="check-row">
                <input
                  type="checkbox"
                  checked={treeOpen}
                  onChange={(e) => setTreeOpen(e.target.checked)}
                />
                工程树
              </label>
              <label className="check-row">
                <input
                  type="checkbox"
                  checked={inspectorOpen}
                  onChange={(e) => setInspectorOpen(e.target.checked)}
                />
                属性面板
              </label>
              <button className="dialog-row" onClick={showSheet}>
                <PanelsTopLeft size={17} />
                界面图纸
                <ArrowUpRight size={14} />
              </button>
            </div>
          )}
          {modal === "definitions" && (
            <div className="modal-body">
              <div className="definition-actions">
                <span>节点定义 JSON</span>
                <button
                  onClick={() => {
                    setImportMode("definitions");
                    fileRef.current?.click();
                  }}
                >
                  <Upload size={14} />
                  选择文件
                </button>
              </div>
              <textarea
                aria-label="节点定义 JSON"
                className="definition-text"
                value={definitionDraft}
                spellCheck={false}
                onChange={(e) => setDefinitionDraft(e.target.value)}
              />
              {dialogError && <pre className="dialog-error">{dialogError}</pre>}
              <div className="dialog-actions">
                <button
                  className="primary"
                  onClick={() => {
                    try {
                      importDefs(parseDefinitions(parseText(definitionDraft)));
                    } catch (err) {
                      setDialogError((err as Error).message);
                    }
                  }}
                >
                  <Check size={14} />
                  应用定义
                </button>
                <button
                  onClick={() =>
                    downloadJSON(project.definitions, "definitions.json")
                  }
                >
                  <Download size={14} />
                  导出全部
                </button>
              </div>
            </div>
          )}
        </Modal>
      )}
    </div>
  );
}
