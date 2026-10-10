import type { Definition, Port } from '../studio/model';
import type { WorkbenchProject } from './types';

// 原版常用事件/动作/流程的内置目录。与原生函数目录一样在运行时并入，不写进工程文件；工程中同 ID 的定义优先。
// codegen.template 以 {字段} 占位；流程体用独占一行的 {body:端口}，JASS 回调用 {callback:端口}；以 “?端口:” 开头的行仅在该端口有动作时输出。
type Field = Definition['fields'][number];
type Template = { jass: string; lua: string };
const port = (id: string, label: string, type: string, direction: 'in' | 'out'): Port => ({ id, label, type, direction });
const field = (id: string, label: string, valueType: string, initial: string | number | boolean, extra: Partial<Field> = {}): Field => ({ id, label, valueType, type: typeof initial === 'number' ? 'number' : typeof initial === 'boolean' ? 'boolean' : 'text', default: initial, ...extra });
const call = (jass: string): Template => ({ jass: `call ${jass}`, lua: jass });
const base = (id: string, kind: Definition['kind'], category: string, title: string, sentence: string, description: string, fields: Field[], ports: Port[], codegen: Record<string, unknown>): Definition => ({ id: `we.${id}`, title, namespace: 'Warcraft III', version: '1', kind, category, sentence, description, fields, ports, codegen });
const execIn = port('in', '执行', 'exec', 'in'), execThen = port('then', '完成', 'exec', 'out');
const event = (id: string, category: string, title: string, sentence: string, description: string, fields: Field[], register: Template | 'init') =>
  base(`event.${id}`, 'event', category, title, sentence, description, fields, [port('then', '触发', 'exec', 'out')], register === 'init' ? { init: true } : { register });
const action = (id: string, category: string, title: string, sentence: string, description: string, fields: Field[], template: Template, extra: Partial<Definition> = {}) =>
  ({ ...base(`action.${id}`, 'action', category, title, sentence, description, fields, [execIn, execThen], { template }), ...extra });
const flow = (id: string, category: string, title: string, sentence: string, description: string, fields: Field[], blocks: Port[], template: Template, extra: Partial<Definition> = {}) =>
  ({ ...base(`flow.${id}`, 'flow', category, title, sentence, description, fields, [execIn, execThen, ...blocks], { template }), ...extra });

const unitOrders = [{ label: '攻击', value: 'attack' }, { label: '移动', value: 'move' }, { label: '巡逻', value: 'patrol' }, { label: '智能', value: 'smart' }, { label: '攻击移动', value: 'attackground' }];
const playerStates = [{ label: '金币', value: 'PLAYER_STATE_RESOURCE_GOLD' }, { label: '木材', value: 'PLAYER_STATE_RESOURCE_LUMBER' }, { label: '人口上限', value: 'PLAYER_STATE_RESOURCE_FOOD_CAP' }];
const loc = (id = 'location', label = '点') => field(id, label, 'location', '0, 0');
const player = (id = 'player', label = '玩家') => field(id, label, 'player', 'Player(0)');
const unit = (id = 'unit', label = '单位') => field(id, label, 'unit', 'null');

export const triggerCatalog: Definition[] = [
  event('mapInit', '地图', '地图初始化', '地图初始化', '地图载入完成后运行一次。', [], 'init'),
  event('elapsed', '时间', '游戏逝去时间', '游戏逝去 {seconds} 秒', '游戏开始后经过指定时间运行一次。', [field('seconds', '秒', 'real', 5)], { jass: 'call TriggerRegisterTimerEventSingle(t, {seconds})', lua: 'TriggerRegisterTimerEventSingle(t, {seconds})' }),
  event('periodic', '时间', '周期事件', '每隔 {seconds} 秒触发（循环）', '按固定间隔重复运行。', [field('seconds', '秒', 'real', 2)], { jass: 'call TriggerRegisterTimerEventPeriodic(t, {seconds})', lua: 'TriggerRegisterTimerEventPeriodic(t, {seconds})' }),
  event('unitDies', '单位', '任意单位死亡', '任意单位死亡', '任何玩家的单位死亡时运行；用“死亡单位”“凶手单位”取值。', [], { jass: 'call TriggerRegisterAnyUnitEventBJ(t, EVENT_PLAYER_UNIT_DEATH)', lua: 'TriggerRegisterAnyUnitEventBJ(t, EVENT_PLAYER_UNIT_DEATH)' }),
  event('enterRect', '单位', '单位进入区域', '单位进入 {rect}', '单位进入指定矩形区域时运行；用“进入区域的单位”取值。', [field('rect', '区域', 'rect', '')], { jass: 'call TriggerRegisterEnterRectSimple(t, {rect})', lua: 'TriggerRegisterEnterRectSimple(t, {rect})' }),
  event('chat', '玩家', '聊天消息', '{player} 输入 {text} 作为聊天信息（完全匹配：{exact}）', '玩家发送匹配的聊天信息时运行。', [player(), field('text', '文本', 'string', '-test'), field('exact', '完全匹配', 'boolean', true)], { jass: 'call TriggerRegisterPlayerChatEvent(t, {player}, {text}, {exact})', lua: 'TriggerRegisterPlayerChatEvent(t, {player}, {text}, {exact})' }),

  flow('if', '通用', 'If / Then / Else（多项动作）', 'If（所有条件成立）Then 执行 Then 动作，Else 执行 Else 动作', '条件全部成立时执行 Then 动作，否则执行 Else 动作。', [], [port('yes', 'Then', 'exec', 'out'), port('no', 'Else', 'exec', 'out')],
    { jass: 'if {conditions} then\n    {body:yes}\n?no:else\n    {body:no}\nendif', lua: 'if {conditions} then\n    {body:yes}\n?no:else\n    {body:no}\nend' }, { conditions: true }),
  flow('forA', '通用', 'For 循环整数 A', 'For 循环整数 A 从 {from} 到 {to}，运行 Loop 动作', '整数 A 从起始值递增到结束值，每次执行 Loop 动作；在动作中用“整数 A”取值。', [field('from', '起始', 'integer', 1), field('to', '结束', 'integer', 10)], [port('loop', 'Loop', 'exec', 'out')],
    { jass: 'set bj_forLoopAIndex = {from}\nset bj_forLoopAIndexEnd = {to}\nloop\n    exitwhen bj_forLoopAIndex > bj_forLoopAIndexEnd\n    {body:loop}\n    set bj_forLoopAIndex = bj_forLoopAIndex + 1\nendloop', lua: 'bj_forLoopAIndex = {from}\nbj_forLoopAIndexEnd = {to}\nwhile bj_forLoopAIndex <= bj_forLoopAIndexEnd do\n    {body:loop}\n    bj_forLoopAIndex = bj_forLoopAIndex + 1\nend' }),
  flow('forVar', '通用', 'For 循环整数变量', 'For 循环 {variable} 从 {from} 到 {to}，运行 Loop 动作', '指定的整数变量从起始值递增到结束值，每次执行 Loop 动作。', [field('variable', '整数变量', 'globalvar:integer', ''), field('from', '起始', 'integer', 1), field('to', '结束', 'integer', 10)], [port('loop', 'Loop', 'exec', 'out')],
    { jass: 'set {variable} = {from}\nloop\n    exitwhen {variable} > {to}\n    {body:loop}\n    set {variable} = {variable} + 1\nendloop', lua: '{variable} = {from}\nwhile {variable} <= {to} do\n    {body:loop}\n    {variable} = {variable} + 1\nend' }),
  flow('forGroup', '单位组', '选取单位组内单位做动作', '选取 {group} 内所有单位做 Loop 动作', '对单位组中的每个单位执行 Loop 动作；在动作中用“选取单位”取值。JASS 生成独立回调，不能使用等待。', [field('group', '单位组', 'group', '')], [port('loop', 'Loop', 'exec', 'out')],
    { jass: 'call ForGroupBJ({group}, function {callback:loop})', lua: 'ForGroupBJ({group}, function()\n    {body:loop}\nend)' }),
  flow('forRect', '单位组', '选取区域内单位做动作', '选取 {rect} 内所有单位做 Loop 动作', '对区域内的所有单位执行 Loop 动作；在动作中用“选取单位”取值。', [field('rect', '区域', 'rect', '')], [port('loop', 'Loop', 'exec', 'out')],
    { jass: 'call ForGroupBJ(GetUnitsInRectAll({rect}), function {callback:loop})', lua: 'ForGroupBJ(GetUnitsInRectAll({rect}), function()\n    {body:loop}\nend)' }),

  action('setVariable', '通用', '设置变量', '设置 {variable} = {value}', '把值赋给全局变量；值的类型跟随所选变量。', [field('variable', '变量', 'globalvar', ''), field('value', '值', '$type:variable', '')], { jass: 'set {variable} = {value}', lua: '{variable} = {value}' }),
  action('wait', '通用', '等待', '等待 {seconds} 秒', '暂停当前动作序列；不能在“选取单位做动作”的回调中使用。', [field('seconds', '秒', 'real', 2)], call('TriggerSleepAction({seconds})')),
  action('comment', '通用', '注释', '{text}', '只作说明，不生成代码。', [field('text', '注释', 'code', '说明')], { jass: '// {text}', lua: '-- {text}' }, { comment: true }),
  action('customScript', '通用', '自定义代码', '自定义代码：{code}', '原样写入一行目标语言代码。', [field('code', '代码', 'code', '')], { jass: '{code}', lua: '{code}' }),
  action('createUnits', '单位', '创建单位（面向角度）', '创建 {count} 个 {unitType} 给 {player}，在 {location}，面向 {facing} 度', '在指定点为玩家创建单位；用“最后创建的单位”取值。', [field('count', '数量', 'integer', 1), field('unitType', '单位类型', 'unitcode', ''), player(), loc(), field('facing', '面向角度', 'real', 270)], call('CreateNUnitsAtLoc({count}, {unitType}, {player}, {location}, {facing})')),
  action('killUnit', '单位', '杀死', '杀死 {unit}', '立即杀死单位，触发死亡事件。', [unit()], call('KillUnit({unit})')),
  action('removeUnit', '单位', '删除', '删除 {unit}', '从游戏中移除单位，不触发死亡事件。', [unit()], call('RemoveUnit({unit})')),
  action('setLifePercent', '单位', '设置生命值（百分比）', '设置 {unit} 的生命值为 {percent}%', '按最大生命值的百分比设置当前生命。', [unit(), field('percent', '百分比', 'real', 100)], call('SetUnitLifePercentBJ({unit}, {percent})')),
  action('issuePoint', '单位', '发布命令（指定点）', '命令 {unit} {order} 到 {location}', '让单位对目标点执行命令。', [unit(), field('order', '命令', 'order', 'attack', { options: unitOrders }), loc()], call('IssuePointOrderLoc({unit}, {order}, {location})')),
  action('moveUnit', '单位', '移动单位（立即）', '立即移动 {unit} 到 {location}', '立即改变单位位置。', [unit(), loc()], call('SetUnitPositionLoc({unit}, {location})')),
  action('heroXp', '英雄', '增加经验值', '为 {hero} 增加 {xp} 点经验值（显示升级效果：{show}）', '增加英雄经验，可能导致升级。', [unit('hero', '英雄'), field('xp', '经验值', 'integer', 100), field('show', '显示效果', 'boolean', true)], call('AddHeroXP({hero}, {xp}, {show})')),
  action('setPlayerState', '玩家', '设置玩家属性', '设置 {player} 的 {state} 为 {value}', '直接设置玩家的资源或属性。', [player(), field('state', '属性', 'playerstate', 'PLAYER_STATE_RESOURCE_GOLD', { options: playerStates }), field('value', '数值', 'integer', 500)], call('SetPlayerStateBJ({player}, {state}, {value})')),
  action('addPlayerState', '玩家', '增加玩家属性', '为 {player} 的 {state} 增加 {value}', '在当前值上增加资源或属性。', [player(), field('state', '属性', 'playerstate', 'PLAYER_STATE_RESOURCE_GOLD', { options: playerStates }), field('value', '数值', 'integer', 100)], call('AdjustPlayerStateBJ({value}, {player}, {state})')),
  action('displayText', '游戏', '显示文本（所有玩家）', '对所有玩家显示文本：{text}', '在屏幕左下方显示文本。', [field('text', '文本', 'string', '')], call('DisplayTextToForce(GetPlayersAll(), {text})')),
  action('victory', '游戏', '胜利', '{player} 获得胜利', '结束该玩家的游戏并显示胜利画面。', [player()], call('CustomVictoryBJ({player}, true, true)')),
  action('defeat', '游戏', '失败', '{player} 失败，显示 {text}', '结束该玩家的游戏并显示失败画面。', [player(), field('text', '提示', 'string', '你失败了')], call('CustomDefeatBJ({player}, {text})')),
  action('runTrigger', '触发器', '运行触发器（检查条件）', '运行 {trigger}（检查条件）', '条件成立时执行目标触发器的动作。', [field('trigger', '触发器', 'trigger', '')], call('ConditionalTriggerExecute({trigger})')),
  action('enableTrigger', '触发器', '开启触发器', '开启 {trigger}', '使目标触发器响应事件。', [field('trigger', '触发器', 'trigger', '')], call('EnableTrigger({trigger})')),
  action('disableTrigger', '触发器', '关闭触发器', '关闭 {trigger}', '使目标触发器不再响应事件。', [field('trigger', '触发器', 'trigger', '')], call('DisableTrigger({trigger})')),
  action('effectAtPoint', '特效', '创建特效（指定点）', '在 {location} 创建特效 {model}', '在点上播放模型特效；用“最后创建的特效”取值。', [loc(), field('model', '模型', 'string', 'Abilities\\Spells\\Human\\ThunderClap\\ThunderClapCaster.mdl')], call('AddSpecialEffectLocBJ({location}, {model})')),
  action('playSound', '声音', '播放声音', '播放 {sound}', '播放声音编辑器中定义的声音。', [field('sound', '声音', 'sound', '')], call('PlaySoundBJ({sound})')),
  action('panCamera', '镜头', '平移镜头（限时）', '为 {player} 平移镜头到 {location}，用时 {duration} 秒', '在指定时间内把玩家镜头平移到点。', [player(), loc(), field('duration', '时间', 'real', 0)], call('PanCameraToTimedLocForPlayer({player}, {location}, {duration})')),
];

/** 触发器模块使用的完整定义表：内置目录 + 工程定义（同 ID 以工程为准）。 */
export function withCatalog(project: WorkbenchProject): WorkbenchProject {
  const own = new Set(project.definitions.map(definition => definition.id));
  return { ...project, definitions: [...triggerCatalog.filter(definition => !own.has(definition.id)), ...project.definitions] };
}
/** 写回工程前去掉并入的内置目录项（按对象身份识别，工程自有的同 ID 定义保留）。 */
export function withoutCatalog(project: WorkbenchProject): WorkbenchProject {
  return { ...project, definitions: project.definitions.filter(definition => !triggerCatalog.includes(definition)) };
}
