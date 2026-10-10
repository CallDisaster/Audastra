import type { Project } from '../studio/model';

export type ModuleId = 'triggers' | 'objects' | 'variables' | 'sounds' | 'assets' | 'manager' | 'campaign' | 'ai' | 'world';
export type ObjectKind = 'unit' | 'item' | 'destructable' | 'doodad' | 'ability' | 'buff' | 'upgrade';
export type FieldValue = string | number | boolean | string[];
export type EditorObject = Project['objects'][number] & {
  kind: ObjectKind;
  rawcode: string;
  parent: string;
  race: string;
  custom: boolean;
  fields: Record<string, FieldValue>;
  defaults: Record<string, FieldValue>;
  levels: Record<string, number[]>;
  /** 原版基础物体 rawcode（真实物编数据）；values 为对基础物体的字段覆盖，等级字段为数组。 */
  base?: string;
  values?: Record<string, string | number | (string | number)[]>;
}
export interface VariableRecord {
  id: string;
  name: string;
  type: string;
  array: boolean;
  size: number;
  initial: string;
  folder: string;
}
export type EditorAsset = Project['assets'][number] & {
  scope: '地图' | '战役' | '游戏';
  dataUrl?: string;
  source?: string;
}
export interface SoundRecord {
  id: string;
  name: string;
  assetId: string;
  music: boolean;
  loop: boolean;
  spatial: boolean;
  volume: number;
  pitch: number;
  minDistance: number;
  maxDistance: number;
  fadeIn: number;
  fadeOut: number;
  channel: string;
}
export interface CampaignChapter {
  id: string;
  title: string;
  subtitle: string;
  map: string;
  visible: boolean;
}
export interface AiBuildRow { id: string; objectId: string; count: number; town: string; condition: string }
export interface AiWave { id: string; name: string; unitId: string; count: number; delay: number; target: string }
/** 放置实例。objectId 为地图物体 ID 或原版 rawcode；可选字段缺省时按原版默认值（缩放 1、生命/魔法 100%、等级 1）。 */
export interface WorldInstance { id: string; objectId: string; x: number; y: number; owner: number; rotation: number; z?: number; scale?: number; life?: number; mana?: number; level?: number }
export interface WorldRegion { id: string; name: string; x: number; y: number; width: number; height: number; color?: string }
export interface WorldCamera { id: string; name: string; x: number; y: number; distance: number; rotation: number; aoa: number; fov: number; z?: number }
export type WorkbenchProject = Pick<Project, 'format' | 'version' | 'name' | 'definitions' | 'graphs' | 'scripts'> & {
  [key: string]: unknown;
  objects: EditorObject[];
  assets: EditorAsset[];
  revision: number;
  variables: VariableRecord[];
  sounds: SoundRecord[];
  campaign: { name: string; author: string; description: string; difficulty: string; chapters: CampaignChapter[]; maps: { id: string; name: string; path: string }[] };
  ai: { name: string; race: string; repair: boolean; retreat: boolean; workers: number; build: AiBuildRow[]; waves: AiWave[]; hero: string; skills: string[] };
  /** space = 'world' 表示使用世界坐标；缺省为 R07 及以前的 0–100 俯视比例，读取时换算。 */
  world: { space?: 'world'; bounds?: { minX: number; minY: number; maxX: number; maxY: number }; instances: WorldInstance[]; regions: WorldRegion[]; cameras?: WorldCamera[] };
  settings: { target: string; language: string; players: number; fog: boolean; name: string; author: string };
};
export interface ModuleProps {
  project: WorkbenchProject;
  onChange: (project: WorkbenchProject, label: string) => void;
  notify: (message: string) => void;
  onNavigate: (module: ModuleId, targetId?: string) => void;
  focusId?: string;
}
export const objectKinds: { id: ObjectKind; name: string; short: string }[] = [
  { id: 'unit', name: '单位', short: 'U' }, { id: 'item', name: '物品', short: 'I' },
  { id: 'destructable', name: '可破坏物', short: 'D' }, { id: 'doodad', name: '装饰物', short: 'P' },
  { id: 'ability', name: '技能', short: 'A' }, { id: 'buff', name: '魔法效果', short: 'B' },
  { id: 'upgrade', name: '科技', short: 'R' },
];
