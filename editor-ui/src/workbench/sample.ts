import { z } from 'zod';
import { parseProject, sampleProject, uid } from '../studio/model';
import type { EditorObject, FieldValue, ObjectKind, WorkbenchProject, WorldInstance } from './types';
import { defaultBounds, migrateLegacyWorld } from './worldModel';

function object(kind: ObjectKind, id: string, rawcode: string, name: string, health: number, speed: number, fields: Record<string, FieldValue> = {}): EditorObject {
  const byKind: Record<ObjectKind, Record<string, FieldValue>> = {
    unit: { armor: 2, attack: 18, range: 128, cost: 135, mana: 0, scale: 1, abilities: [] },
    item: { cost: 100, charges: 1, abilities: [], scale: 1 },
    ability: { damage: 60, mana: 50, cooldown: 8, duration: 3, radius: 250, targets: ['地面', '敌人'] },
    buff: { duration: 3 }, upgrade: { cost: 150 },
    destructable: { armor: 0, scale: 1 }, doodad: { scale: 1 },
  };
  const values: Record<string, FieldValue> = { tooltip: '', model: '', icon: '', ...byKind[kind], ...fields };
  const category = { unit: '单位', item: '物品', ability: '技能', buff: '魔法效果', upgrade: '科技', destructable: '可破坏物', doodad: '装饰物' }[kind];
  const defaults: Record<string, FieldValue> = { name, health, speed, enabled: true, race: '人族', category, ...values };
  return { id, rawcode, name, kind, category, parent: rawcode, race: '人族', custom: true, health, speed, enabled: true, fields: { ...values }, defaults, levels: kind === 'ability' ? { damage: [60, 100, 140], cooldown: [10, 9, 8], mana: [50, 65, 80] } : kind === 'upgrade' ? { cost: [150, 225, 300], ...(fields.armor ? { armor: [2, 4, 6] } : {}), ...(fields.attack ? { attack: [3, 6, 9] } : {}) } : {} };
}
function tone(): string {
  const rate = 12000, length = 12000;
  const bytes = new Uint8Array(44 + length * 2), data = new DataView(bytes.buffer);
  const text = (offset: number, value: string) => [...value].forEach((c, i) => data.setUint8(offset + i, c.charCodeAt(0)));
  text(0, 'RIFF'); data.setUint32(4, 36 + length * 2, true); text(8, 'WAVE'); text(12, 'fmt ');
  data.setUint32(16, 16, true); data.setUint16(20, 1, true); data.setUint16(22, 1, true); data.setUint32(24, rate, true); data.setUint32(28, rate * 2, true); data.setUint16(32, 2, true); data.setUint16(34, 16, true); text(36, 'data'); data.setUint32(40, length * 2, true);
  for (let i = 0; i < length; i++) { const t = i / rate; data.setInt16(44 + i * 2, Math.sin(t * Math.PI * 2 * 523.25) * Math.exp(-t * 5) * Math.min(t * 100, 1) * 9000, true); }
  let binary = ''; for (const b of bytes) binary += String.fromCharCode(b);
  return `data:audio/wav;base64,${btoa(binary)}`;
}
// 示例世界：64×64 地图（±4096），基地居中，西北为敌方出生点。原版 rawcode（如 htow、ngno）在有本机原版数据时显示真实名称与图标。
function sampleWorld(): WorkbenchProject['world'] {
  let n = 0;
  const put = (objectId: string, x: number, y: number, owner = 1, rotation = 270, extra: Partial<WorldInstance> = {}): WorldInstance => ({ id: `instance.${++n}`, objectId, x, y, owner, rotation, ...extra });
  const instances: WorldInstance[] = [
    put('unit.hero', -256, 128, 1, 270, { level: 3 }), put('unit.soldier', -64, 256, 1, 90), put('unit.soldier', 64, 256, 1, 90), put('unit.archer', -128, 384, 1, 90), put('unit.archer', 128, 384, 1, 90),
    put('unit.knight', 320, 64, 1, 45), put('unit.mage', -384, -64, 1, 0), put('unit.worker', 448, -448), put('unit.worker', 512, -384),
    put('htow', 0, -384, 1, 270), put('hbar', 512, 0, 1, 270), put('hhou', -512, -512, 1, 270),
    put('ngnw', -2688, 2560, 13, 315), put('ngnb', -2560, 2688, 13, 300), put('ngno', -2304, 2432, 13, 330),
    put('item.potion', -192, -128, 16, 0), put('item.crystal', 2304, -2432, 16, 0),
    put('dood.torch', -192, 640, 16, 0), put('dood.torch', 192, 640, 16, 0), put('dood.rock', 1536, 1280, 16, 45, { scale: 1.2 }), put('dood.rock', 1664, 1152, 16, 120, { scale: 0.9 }),
    put('dest.gate', 0, 768, 16, 270),
  ];
  for (let i = 0; i < 10; i++) instances.push(put('dest.tree', -3584 + i * 192, -3200 + (i % 3) * 96, 16, i * 36));
  for (let i = 0; i < 8; i++) instances.push(put('dest.tree', 3200 + (i % 2) * 128, 1024 + i * 176, 16, i * 45));
  return {
    space: 'world', bounds: { ...defaultBounds }, instances,
    regions: [
      { id: 'region.spawn', name: '进攻出生点', x: -3072, y: 2048, width: 1152, height: 1024 },
      { id: 'route.destination', name: '防守区域', x: -640, y: -768, width: 1280, height: 1664 },
      { id: 'region.mine', name: '金矿', x: 2048, y: -2816, width: 640, height: 640, color: '#cfb67c' },
    ],
    cameras: [
      { id: 'camera.intro', name: '开场镜头', x: -2560, y: 2432, distance: 1650, rotation: 90, aoa: 304, fov: 70 },
      { id: 'camera.base', name: '基地全景', x: 0, y: 0, distance: 2400, rotation: 90, aoa: 320, fov: 70 },
    ],
  };
}
export function createSample(): WorkbenchProject {
  const base = sampleProject();
  base.name = '边境守望';
  base.graphs[0].name = '进攻波次';
  base.graphs[0].folder = '关卡逻辑';
  [[50, 90], [360, 90], [670, 90], [670, 460], [360, 460], [50, 440]].forEach(([x, y], i) => { base.graphs[0].nodes[i].x = x; base.graphs[0].nodes[i].y = y; });
  base.graphs[0].nodes.find(n => n.definition === 'core.spawn')!.values.position = 'region.spawn';
  base.definitions = base.definitions.map(definition => ({ ...definition, fields: definition.fields.map(field => field.id === 'template' ? { ...field, valueType: 'unitcode', objectKind: 'unit' } : ['position', 'target'].includes(field.id) ? { ...field, valueType: 'rect', referenceType: 'region' } : field) }));
  base.graphs[1].name = '地图初始化';
  base.graphs[1].folder = '初始化';
  const objects = [
    object('unit', 'unit.soldier', 'h001', '守卫步兵', 420, 270, { abilities: ['ability.defend'], tooltip: '前线守卫。能够举盾抵挡远程攻击。', armor: 3 }),
    object('unit', 'unit.archer', 'e001', '巡林射手', 300, 300, { range: 600, attack: 24, armor: 1, cost: 170 }),
    object('unit', 'unit.knight', 'h002', '黎明骑士', 800, 320, { attack: 36, armor: 5, cost: 245 }),
    object('unit', 'unit.mage', 'h003', '奥术法师', 350, 280, { mana: 400, range: 550, abilities: ['ability.bolt'] }),
    object('unit', 'unit.worker', 'h004', '筑城工匠', 230, 260, { attack: 5, cost: 75 }),
    object('unit', 'unit.hero', 'H001', '白银领主', 950, 300, { mana: 350, abilities: ['ability.heal', 'ability.defend'] }),
    object('item', 'item.potion', 'I001', '治疗药水', 0, 0, { cost: 150, tooltip: '使用后恢复生命值。', charges: 1 }),
    object('item', 'item.crystal', 'I002', '奥术水晶', 0, 0, { cost: 320, charges: 3 }),
    object('destructable', 'dest.tree', 'B001', '白桦树', 50, 0, { model: 'Environment/Tree.mdx', scale: 1.2 }),
    object('destructable', 'dest.gate', 'B002', '要塞城门', 1200, 0, { armor: 8 }),
    object('doodad', 'dood.torch', 'D001', '石制火炬', 0, 0, { scale: 1.4 }),
    object('doodad', 'dood.rock', 'D002', '山地岩石', 0, 0, { scale: 0.8 }),
    object('ability', 'ability.bolt', 'A001', '奥术冲击', 0, 0, { mana: 65, cooldown: 9, radius: 180, tooltip: '向目标释放奥术能量，造成伤害并短暂眩晕。' }),
    object('ability', 'ability.heal', 'A002', '圣光治疗', 0, 0, { mana: 80, cooldown: 6, targets: ['友军', '存活'] }),
    object('ability', 'ability.defend', 'A003', '坚守阵地', 0, 0, { mana: 0, cooldown: 0, duration: 8 }),
    object('buff', 'buff.stun', 'B003', '奥术眩晕', 0, 0, { duration: 3, tooltip: '无法移动或攻击。' }),
    object('buff', 'buff.defend', 'B004', '坚守', 0, 0, { duration: 8 }),
    object('upgrade', 'upgrade.armor', 'R001', '精钢护甲', 0, 0, { cost: 150, armor: 2 }),
    object('upgrade', 'upgrade.attack', 'R002', '锻造武器', 0, 0, { cost: 180, attack: 3 }),
  ];
  // 示例自定义物体关联真实原版基础物体（有本机原版数据时显示真实字段与图标）。
  const bases: Record<string, string> = { 'unit.soldier': 'hfoo', 'unit.archer': 'earc', 'unit.knight': 'hkni', 'unit.mage': 'hsor', 'unit.worker': 'hpea', 'unit.hero': 'Hpal', 'item.potion': 'phea', 'item.crystal': 'pman', 'dest.tree': 'LTlt', 'dest.gate': 'LTg1', 'dood.torch': 'LOth', 'dood.rock': 'ARrk', 'ability.bolt': 'AHtb', 'ability.heal': 'AHhb', 'ability.defend': 'Adef', 'buff.stun': 'BPSE', 'buff.defend': 'Bdef', 'upgrade.armor': 'Rhar', 'upgrade.attack': 'Rhme' };
  for (const item of objects) if (bases[item.id]) item.base = bases[item.id];
  objects[0].fields.armor = 4;
  objects[0].fields.cost = 150;
  return {
    ...base, revision: 2, objects,
    variables: [
      { id: 'var.wave', name: 'wave', type: 'integer', array: false, size: 1, initial: '0', folder: '关卡' },
      { id: 'var.maxWaves', name: 'maxWaves', type: 'integer', array: false, size: 1, initial: '10', folder: '关卡' },
      { id: 'var.spawnCount', name: 'spawnCount', type: 'integer', array: false, size: 1, initial: '4', folder: '关卡' },
      { id: 'var.spawnUnitType', name: 'spawnUnitType', type: 'unitcode', array: false, size: 1, initial: 'h001', folder: '关卡' },
      { id: 'var.hero', name: 'playerHero', type: 'unit', array: true, size: 12, initial: 'null', folder: '玩家' },
      { id: 'var.started', name: 'gameStarted', type: 'boolean', array: false, size: 1, initial: 'false', folder: '初始化' },
      { id: 'var.enemies', name: 'enemyGroup', type: 'group', array: false, size: 1, initial: 'CreateGroup()', folder: '关卡' },
    ],
    assets: [
      { id: 'asset.bell', name: '关卡提示.wav', type: 'audio/wav', size: 24044, path: 'Audio/LevelNotice.wav', scope: '地图', source: '生成的示例音频', dataUrl: tone() },
      { id: 'asset.readme', name: '关卡配置.txt', type: 'text/plain', size: 42, path: 'Data/LevelConfig.txt', scope: '地图', source: '示例文本', dataUrl: 'data:text/plain;charset=utf-8,' + encodeURIComponent('waves=10\nspawnCount=4\ninterval=30\n') },
    ],
    sounds: [
      { id: 'sound.notice', name: 'LevelNotice', assetId: 'asset.bell', music: false, loop: false, spatial: false, volume: 80, pitch: 1, minDistance: 600, maxDistance: 10000, fadeIn: 0, fadeOut: 0, channel: '界面' },
      { id: 'sound.ambient', name: 'WatchtowerBell', assetId: 'asset.bell', music: false, loop: true, spatial: true, volume: 65, pitch: 0.8, minDistance: 350, maxDistance: 3000, fadeIn: 100, fadeOut: 200, channel: '环境' },
    ],
    campaign: { name: '边境守望', author: 'Map Studio', description: '边境守望示例战役', difficulty: '普通', maps: [{ id: 'map.1', name: '北境哨站', path: 'Maps/NorthWatch.w3x' }, { id: 'map.2', name: '穿越迷雾', path: 'Maps/MistValley.w3x' }, { id: 'map.3', name: '黎明要塞', path: 'Maps/DawnKeep.w3x' }], chapters: [
      { id: 'chapter.1', title: '第一章', subtitle: '北境哨站', map: 'Maps/NorthWatch.w3x', visible: true },
      { id: 'chapter.2', title: '第二章', subtitle: '穿越迷雾', map: 'Maps/MistValley.w3x', visible: false },
      { id: 'chapter.3', title: '第三章', subtitle: '黎明要塞', map: 'Maps/DawnKeep.w3x', visible: false },
    ] },
    ai: { name: '边境守军', race: '人族', repair: true, retreat: true, workers: 5, hero: 'unit.hero', skills: ['ability.heal', 'ability.defend'], build: [
      { id: 'build.1', objectId: 'unit.worker', count: 5, town: '主基地', condition: '总是' },
      { id: 'build.2', objectId: 'unit.soldier', count: 6, town: '主基地', condition: '人口 < 40' },
      { id: 'build.3', objectId: 'unit.archer', count: 4, town: '主基地', condition: '总是' },
      { id: 'build.4', objectId: 'upgrade.armor', count: 1, town: '主基地', condition: '金币 ≥ 150' },
    ], waves: [
      { id: 'wave.1', name: '前哨侦察', unitId: 'unit.soldier', count: 4, delay: 60, target: '敌方基地' },
      { id: 'wave.2', name: '弓步混编', unitId: 'unit.archer', count: 6, delay: 120, target: '敌方分矿' },
      { id: 'wave.3', name: '骑兵突袭', unitId: 'unit.knight', count: 3, delay: 180, target: '敌方基地' },
    ] },
    world: sampleWorld(),
    settings: { target: 'Warcraft III 1.27', language: 'JASS', players: 12, fog: true, name: '边境守望', author: 'Map Studio' },
  };
}

const valueSchema = z.union([z.string(), z.number().finite(), z.boolean(), z.array(z.string())]);
const extraSchema = z.object({
  revision: z.number(),
  objects: z.array(z.object({ id: z.string(), name: z.string(), kind: z.enum(['unit', 'item', 'destructable', 'doodad', 'ability', 'buff', 'upgrade']), rawcode: z.string(), parent: z.string(), race: z.string(), custom: z.boolean(), fields: z.record(z.string(), valueSchema), defaults: z.record(z.string(), valueSchema), levels: z.record(z.string(), z.array(z.number().finite())), base: z.string().optional(), values: z.record(z.string(), z.union([z.string(), z.number().finite(), z.array(z.union([z.string(), z.number().finite()]))])).optional() }).passthrough()),
  assets: z.array(z.object({ id: z.string(), name: z.string(), type: z.string(), size: z.number().nonnegative(), path: z.string(), scope: z.enum(['地图', '战役', '游戏']), dataUrl: z.string().optional(), source: z.string().optional() }).passthrough()),
  variables: z.array(z.object({ id: z.string(), name: z.string(), type: z.string(), array: z.boolean(), size: z.number().int().positive().max(1000000), initial: z.string(), folder: z.string() })),
  sounds: z.array(z.object({ id: z.string(), name: z.string(), assetId: z.string(), music: z.boolean(), loop: z.boolean(), spatial: z.boolean(), volume: z.number().min(0).max(127), pitch: z.number().positive(), minDistance: z.number().nonnegative(), maxDistance: z.number().nonnegative(), fadeIn: z.number().nonnegative(), fadeOut: z.number().nonnegative(), channel: z.string() })),
  campaign: z.object({ name: z.string(), author: z.string(), description: z.string(), difficulty: z.string(), maps: z.array(z.object({ id: z.string(), name: z.string(), path: z.string() })), chapters: z.array(z.object({ id: z.string(), title: z.string(), subtitle: z.string(), map: z.string(), visible: z.boolean() })) }),
  ai: z.object({ name: z.string(), race: z.string(), repair: z.boolean(), retreat: z.boolean(), workers: z.number().nonnegative(), hero: z.string(), skills: z.array(z.string()), build: z.array(z.object({ id: z.string(), objectId: z.string(), count: z.number().nonnegative(), town: z.string(), condition: z.string() })), waves: z.array(z.object({ id: z.string(), name: z.string(), unitId: z.string(), count: z.number().nonnegative(), delay: z.number().nonnegative(), target: z.string() })) }),
  world: z.object({
    space: z.literal('world').optional(),
    bounds: z.object({ minX: z.number().finite(), minY: z.number().finite(), maxX: z.number().finite(), maxY: z.number().finite() }).optional(),
    instances: z.array(z.object({ id: z.string(), objectId: z.string(), x: z.number().finite(), y: z.number().finite(), owner: z.number(), rotation: z.number(), z: z.number().finite().optional(), scale: z.number().positive().optional(), life: z.number().min(0).max(100).optional(), mana: z.number().min(0).max(100).optional(), level: z.number().int().positive().optional() })),
    regions: z.array(z.object({ id: z.string(), name: z.string(), x: z.number(), y: z.number(), width: z.number().positive(), height: z.number().positive(), color: z.string().optional() })),
    cameras: z.array(z.object({ id: z.string(), name: z.string(), x: z.number().finite(), y: z.number().finite(), distance: z.number().positive(), rotation: z.number(), aoa: z.number(), fov: z.number().positive(), z: z.number().optional() })).optional(),
  }),
  settings: z.object({ target: z.string(), language: z.string(), players: z.number().int().positive().max(24), fog: z.boolean(), name: z.string(), author: z.string() }),
});

// 触发条件树（传统视图）；绑定沿用参数绑定结构，交给代码生成统一解析。
type ConditionShape = { id: string; kind: 'compare' | 'and' | 'or'; enabled?: boolean; children?: ConditionShape[] };
const operandSchema = z.object({ value: z.unknown(), binding: z.object({ mode: z.enum(['constant', 'variable', 'expression', 'function']), value: z.string() }).passthrough().optional() });
const conditionSchema: z.ZodType<ConditionShape> = z.lazy(() => z.union([
  z.object({ id: z.string(), kind: z.literal('compare'), valueType: z.string(), op: z.enum(['==', '!=', '<', '<=', '>', '>=']), left: operandSchema, right: operandSchema, enabled: z.boolean().optional() }),
  z.object({ id: z.string(), kind: z.enum(['and', 'or']), children: z.array(conditionSchema), enabled: z.boolean().optional() }),
]));

export function readWorkbench(value: unknown): WorkbenchProject {
  const core = parseProject(value);
  if (core.revision !== 2) throw new Error('请选择 R02 工程 JSON；旧版工程可在 R01 中打开');
  if (core.triggerFolders !== undefined && !z.array(z.string()).safeParse(core.triggerFolders).success) throw new Error('触发器文件夹列表无效');
  for (const graph of core.graphs) for (const flag of ['enabled', 'initiallyOn'] as const) if (graph[flag] !== undefined && typeof graph[flag] !== 'boolean') throw new Error(`触发器 ${graph.name} 的${flag === 'enabled' ? '允许' : '初始打开'}状态无效`);
  for (const graph of core.graphs) if (graph.conditions !== undefined) { const parsed = z.array(conditionSchema).safeParse(graph.conditions); if (!parsed.success) throw new Error(`触发器 ${graph.name} 的条件无效`); }
  for (const graph of core.graphs) for (const node of graph.nodes) if (node.conditions !== undefined && !z.array(conditionSchema).safeParse(node.conditions).success) throw new Error(`触发器 ${graph.name} 中节点的条件无效`);
  const extra = extraSchema.parse(value);
  for (const [label, list] of Object.entries({ variables: extra.variables, sounds: extra.sounds, chapters: extra.campaign.chapters, build: extra.ai.build, waves: extra.ai.waves, instances: extra.world.instances, regions: extra.world.regions, cameras: extra.world.cameras ?? [] })) {
    if (new Set(list.map(v => v.id)).size !== list.length) throw new Error(`${label} ID 重复`);
  }
  return { ...core, ...extra, world: migrateLegacyWorld(extra.world) } as WorkbenchProject;
}

export function createEmpty(): WorkbenchProject {
  const project = createSample();
  project.name = '未命名工程'; project.settings.name = project.name;
  project.graphs = [{ id: uid(), name: '未命名触发器', folder: '触发器', nodes: [], edges: [] }];
  project.objects = []; project.variables = []; project.assets = []; project.sounds = []; project.scripts = [];
  project.campaign.chapters = []; project.campaign.maps = []; project.ai.build = []; project.ai.waves = []; project.ai.hero = ''; project.ai.skills = [];
  project.world = { space: 'world', bounds: { ...defaultBounds }, instances: [], regions: [], cameras: [] };
  return project;
}
