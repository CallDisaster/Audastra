import { uid } from '../studio/model';
import type { Graph } from '../studio/model';
import type { WorkbenchProject } from './types';

// 触发器目录（原版“类别”）：graph.folder 仍是文件夹名；project.triggerFolders 保存文件夹顺序并保留空文件夹。
// 触发器在文件夹内的顺序即 project.graphs 的顺序。全部为纯函数，由 check-workbench 覆盖。

export const folderList = (project: WorkbenchProject): string[] => {
  const stored = Array.isArray(project.triggerFolders) ? (project.triggerFolders as unknown[]).filter((name): name is string => typeof name === 'string') : [];
  return [...new Set([...stored, ...project.graphs.map(graph => graph.folder)])];
};
const withFolders = (project: WorkbenchProject, folders: string[], graphs: Graph[] = project.graphs): WorkbenchProject => ({ ...project, graphs, triggerFolders: folders });

/** 原版命名方式：“未命名触发器 001”，递增到不重名为止。 */
export function uniqueName(existing: Iterable<string>, base: string): string {
  const names = new Set(existing);
  for (let i = 1; ; i++) { const name = `${base} ${String(i).padStart(3, '0')}`; if (!names.has(name)) return name; }
}
export function validateTriggerName(project: WorkbenchProject, name: string, self?: string): string | undefined {
  const text = name.trim();
  if (!text) return '名称不能为空';
  if (project.graphs.some(graph => graph.id !== self && graph.name === text)) return `已有名为“${text}”的触发器`;
  return undefined;
}
export function validateFolderName(project: WorkbenchProject, name: string, self?: string): string | undefined {
  const text = name.trim();
  if (!text) return '名称不能为空';
  if (folderList(project).some(folder => folder !== self && folder === text)) return `已有名为“${text}”的文件夹`;
  return undefined;
}

/** 新建触发器：放在 after 之后，否则放在文件夹末尾。 */
export function createTrigger(project: WorkbenchProject, folder: string, after?: string): { project: WorkbenchProject; id: string } {
  const graph: Graph = { id: uid(), name: uniqueName(project.graphs.map(item => item.name), '未命名触发器'), folder, nodes: [], edges: [], enabled: true, initiallyOn: true };
  const graphs = [...project.graphs];
  const anchor = after ? graphs.findIndex(item => item.id === after && item.folder === folder) : -1;
  const lastInFolder = graphs.map(item => item.folder).lastIndexOf(folder);
  graphs.splice(anchor >= 0 ? anchor + 1 : lastInFolder >= 0 ? lastInFolder + 1 : graphs.length, 0, graph);
  const folders = folderList(project);
  return { project: withFolders(project, folders.includes(folder) ? folders : [...folders, folder], graphs), id: graph.id };
}
export function createFolder(project: WorkbenchProject, after?: string): { project: WorkbenchProject; name: string } {
  const folders = folderList(project);
  const name = uniqueName(folders, '未命名类别');
  const at = after ? folders.indexOf(after) : -1;
  folders.splice(at >= 0 ? at + 1 : folders.length, 0, name);
  return { project: withFolders(project, folders), name };
}
export function renameFolder(project: WorkbenchProject, from: string, to: string): WorkbenchProject {
  const name = to.trim();
  const error = validateFolderName(project, name, from);
  if (error) throw new Error(error);
  return withFolders(project, folderList(project).map(folder => folder === from ? name : folder), project.graphs.map(graph => graph.folder === from ? { ...graph, folder: name } : graph));
}
export function renameTrigger(project: WorkbenchProject, id: string, to: string): WorkbenchProject {
  const error = validateTriggerName(project, to, id);
  if (error) throw new Error(error);
  return { ...project, graphs: project.graphs.map(graph => graph.id === id ? { ...graph, name: to.trim() } : graph) };
}
/** 删除文件夹连同其中的触发器（与原版一致；界面上先确认，可撤销）。 */
export function deleteFolder(project: WorkbenchProject, name: string): WorkbenchProject {
  return withFolders(project, folderList(project).filter(folder => folder !== name), project.graphs.filter(graph => graph.folder !== name));
}
export function deleteTrigger(project: WorkbenchProject, id: string): WorkbenchProject {
  return withFolders(project, folderList(project), project.graphs.filter(graph => graph.id !== id));
}
/** 移动触发器到文件夹；before 为目标位置之后的触发器，缺省放在文件夹末尾。 */
export function moveTrigger(project: WorkbenchProject, id: string, folder: string, before?: string): WorkbenchProject {
  const moving = project.graphs.find(graph => graph.id === id);
  if (!moving || before === id) return project;
  const graphs = project.graphs.filter(graph => graph.id !== id);
  const at = before ? graphs.findIndex(graph => graph.id === before) : -1;
  const lastInFolder = graphs.map(graph => graph.folder).lastIndexOf(folder);
  graphs.splice(at >= 0 ? at : lastInFolder >= 0 ? lastInFolder + 1 : graphs.length, 0, { ...moving, folder });
  const folders = folderList(project);
  return withFolders(project, folders.includes(folder) ? folders : [...folders, folder], graphs);
}
export function moveFolder(project: WorkbenchProject, name: string, before?: string): WorkbenchProject {
  if (name === before) return project;
  const folders = folderList(project).filter(folder => folder !== name);
  const at = before ? folders.indexOf(before) : -1;
  folders.splice(at >= 0 ? at : folders.length, 0, name);
  return withFolders(project, folders);
}
/** 复制触发器：节点与连线换新 ID，条件与绑定原样保留；名称加“副本”并避开重名。 */
export function cloneTrigger(project: WorkbenchProject, source: Graph, folder = source.folder, after?: string): { project: WorkbenchProject; id: string } {
  const ids = new Map(source.nodes.map(node => [node.id, uid()]));
  const copy = structuredClone(source);
  const names = project.graphs.map(graph => graph.name);
  let name = `${source.name} 副本`;
  if (names.includes(name)) name = uniqueName(names, `${source.name} 副本`);
  const graph: Graph = { ...copy, id: uid(), name, folder, nodes: copy.nodes.map(node => ({ ...node, id: ids.get(node.id)! })), edges: copy.edges.map(edge => ({ ...edge, id: uid(), from: ids.get(edge.from) ?? edge.from, to: ids.get(edge.to) ?? edge.to })) };
  const graphs = [...project.graphs];
  const anchor = graphs.findIndex(item => item.id === (after ?? source.id));
  const lastInFolder = graphs.map(item => item.folder).lastIndexOf(folder);
  graphs.splice(anchor >= 0 && graphs[anchor].folder === folder ? anchor + 1 : lastInFolder >= 0 ? lastInFolder + 1 : graphs.length, 0, graph);
  const folders = folderList(project);
  return { project: withFolders(project, folders.includes(folder) ? folders : [...folders, folder], graphs), id: graph.id };
}
