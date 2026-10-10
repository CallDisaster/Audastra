import assert from 'node:assert/strict';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import ts from 'typescript';
const output = new URL('../output/check-workbench/', import.meta.url);
await mkdir(output, { recursive: true });
for (const [source, target] of [['studio/model.ts', 'model.mjs'], ['workbench/sample.ts', 'sample.mjs'], ['workbench/triggerCode.ts', 'triggerCode.mjs'], ['workbench/valueSources.ts', 'valueSources.mjs'], ['workbench/references.ts', 'references.mjs'], ['workbench/parameterFields.ts', 'parameterFields.mjs'], ['workbench/objectCatalog.ts', 'objectCatalog.mjs'], ['workbench/objectModel.ts', 'objectModel.mjs'], ['workbench/ecaModel.ts', 'ecaModel.mjs'], ['workbench/triggerCatalog.ts', 'triggerCatalog.mjs'], ['workbench/referenceIndex.ts', 'referenceIndex.mjs'], ['workbench/triggerFolders.ts', 'triggerFolders.mjs'], ['workbench/dockLayout.ts', 'dockLayout.mjs'], ['workbench/worldModel.ts', 'worldModel.mjs'], ['workbench/projectDiff.ts', 'projectDiff.mjs'], ['workbench/commandRank.ts', 'commandRank.mjs'], ['workbench/assistantIntents.ts', 'assistantIntents.mjs']]) {
  const code = await readFile(new URL(`../src/${source}`, import.meta.url), 'utf8');
  const result = ts.transpileModule(code, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022 } }).outputText.replaceAll('../studio/model', './model.mjs').replace(/from '\.\/([A-Za-z]+)'/g, "from './$1.mjs'");
  await writeFile(new URL(target, output), result);
}
const { createSample, createEmpty, readWorkbench } = await import(new URL('sample.mjs', output));
const { generateTrigger } = await import(new URL('triggerCode.mjs', output));
const { validateProject, makeNode, connectPorts } = await import(new URL('model.mjs', output));
const { triggerCatalog, withCatalog, withoutCatalog } = await import(new URL('triggerCatalog.mjs', output));
const { resolveFieldType } = await import(new URL('parameterFields.mjs', output));
const objectModel = await import(new URL('objectModel.mjs', output));
const objectCatalog = await import(new URL('objectCatalog.mjs', output));
const { referencesTo } = await import(new URL('references.mjs', output));
const referenceIndex = await import(new URL('referenceIndex.mjs', output));
const folders = await import(new URL('triggerFolders.mjs', output));
const dock = await import(new URL('dockLayout.mjs', output));
const worldModel = await import(new URL('worldModel.mjs', output));
const projectDiff = await import(new URL('projectDiff.mjs', output));
const commandRank = await import(new URL('commandRank.mjs', output));
const assistant = await import(new URL('assistantIntents.mjs', output));
const { valueFunctionDefinitions, compatibleValueType, returnTypeOf, presetValues } = await import(new URL('valueSources.mjs', output));
const { buildEca, setChain, moveItem, removeItems, insertNode, findBlock, locateItem, makeCondition, sentenceOf } = await import(new URL('ecaModel.mjs', output));
const results = [];
function test(name, run) { run(); results.push(name); }
test('R02完整工程往返不丢失九模块数据与音频内容', () => { const p = createSample(); assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(p))), p); assert.equal(validateProject(p).length, 0); });
test('空工程所有模块数据可读取且不残留无效实例', () => { const p = createEmpty(); assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(p))), p); assert.equal(p.world.instances.length, 0); assert.equal(p.campaign.maps.length, 0); });
test('未知扩展数据和物编等级默认基线保留', () => { const p = createSample(); p.futureData = { opaque: [1, 2, 3] }; p.objects[0].levelDefaults = { damage: [60, 100, 140] }; p.graphs[0].nodes[0].futurePayload = 'keep'; assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(p))), p); });
test('特殊编号函数按定义正常创建与保存', () => { const p = createSample(); for (const id of ['9857', '9527']) { const d = { ...p.definitions[5], id: `custom.${id}`, title: `扩展 ${id}`, namespace: 'custom' }; p.definitions.push(d); p.graphs[0].nodes.push(makeNode(d, 20, 20)); } assert.equal(validateProject(p).length, 0); assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(p))), p); });
test('缺失必要字段、重复变量ID及非法数值拒绝导入', () => { const a = createSample(); delete a.objects[0].fields; assert.throws(() => readWorkbench(a)); const b = createSample(); b.variables.push(b.variables[0]); assert.throws(() => readWorkbench(b)); const c = createSample(); c.ai.build[0].count = -1; assert.throws(() => readWorkbench(c)); });
test('变量初值与连接的数据节点生成同一变量引用', () => { const p = createSample(); p.variables.find(v => v.name === 'spawnCount').initial = '7'; const result = generateTrigger(p.graphs[0], p, 'JASS'); assert.match(result.content, /udg_spawnCount/); assert.match(result.content, /udg_spawnCount\s*=\s*7/); });
test('三种语言生成区域符号而非把俯视比例当地图坐标', () => { const p = createSample(); for (const language of ['JASS', 'vJass', 'Lua']) { const code = generateTrigger(p.graphs[0], p, language); assert.match(code.content, /gg_rct_region_spawn/); assert.ok(code.warnings.some(w => w.includes('区域'))); } });
test('变量与物体的节点绑定进入引用索引', () => { const p = createSample(); p.graphs[0].nodes[0].bindings = { interval: { mode: 'variable', value: p.variables[0].id } }; assert.ok(referencesTo(p, p.variables[0].id).some(r => r.module === 'triggers')); assert.ok(referencesTo(p, 'unit.soldier').some(r => r.module === 'world')); });

test('单位类型选取只匹配单位类型变量与返回函数', () => { const p = createSample(); assert.ok(!compatibleValueType('unit', 'unitcode')); assert.ok(!compatibleValueType('integer', 'unitcode')); const functions = valueFunctionDefinitions(p).filter(f => compatibleValueType(returnTypeOf(f), 'unitcode')); assert.ok(functions.some(f => f.id === 'native.GetUnitTypeId')); assert.ok(!functions.some(f => f.id === 'native.GetTriggerUnit')); });

test('嵌套函数及数组变量在三种语言中保留语义', () => { const p = createSample(); const n = p.graphs[0].nodes.find(n => n.definition === 'core.spawn'); n.bindings = { template: { mode: 'function', value: 'native.GetUnitTypeId', args: { unit: { value: 'null', binding: { mode: 'function', value: 'native.GetTriggerUnit', args: {} } } } } }; assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(p))), p); for (const lang of ['JASS', 'vJass', 'Lua']) assert.match(generateTrigger(p.graphs[0], p, lang).content, /GetUnitTypeId\(GetTriggerUnit\(\)\)/); n.bindings.template.args.unit.binding = { mode: 'variable', value: 'var.hero', index: '2' }; assert.match(generateTrigger(p.graphs[0], p, 'JASS').content, /GetUnitTypeId\(udg_playerHero\[2\]\)/); assert.ok(referencesTo(p, 'var.hero').some(r => r.module === 'triggers')); });

test('动态区域表达式实际生成且不退回旧常量', () => { const p = createSample(); const n = p.graphs[0].nodes.find(n => n.definition === 'core.spawn'); n.bindings = { position: { mode: 'function', value: 'native.GetWorldBounds', args: {} } }; const code = generateTrigger(p.graphs[0], p, 'JASS').content; assert.match(code, /position_\w+ = GetWorldBounds\(\)/); assert.match(code, /GetRectCenterX\(position_/); });

test('导入的自定义函数定义参与选取与代码生成', () => { const p = createSample(); p.definitions.push({ id:'custom.9857',title:'自定义类型',namespace:'扩展',version:'1',kind:'value',returnType:'unitcode',fields:[],ports:[{id:'result',label:'类型',direction:'out',type:'unitcode'}],codegen:{jass:'GetCustomType',lua:'GetCustomType'} }); p.graphs[0].nodes.find(n => n.definition === 'core.spawn').bindings = { template:{mode:'function',value:'custom.9857',args:{}} }; assert.ok(valueFunctionDefinitions(p).some(f=>f.id==='custom.9857')); assert.match(generateTrigger(p.graphs[0],p,'JASS').content,/GetCustomType\(\)/); });

test('缺失函数定义明确诊断且生成预览不崩溃', () => { const p = createSample(); p.graphs[0].nodes.find(n => n.definition === 'core.spawn').bindings = { template: {mode:'function',value:'missing.function',args:{}} }; assert.ok(generateTrigger(p.graphs[0],p,'JASS').warnings.some(w=>w.includes('missing.function'))); });
// ---- 传统 ECA 视图：与蓝图共用执行连线，重排/移动/删除只改写执行连线 ----
const eca = p => buildEca(p.graphs[0], p.definitions);
const titles = block => block.items.map(item => item.node.definition);
const execOrder = p => generateTrigger(p.graphs[0], p, 'JASS').content.split('\n').filter(l => /CreateUnit|GroupPointOrder|DisplayText|^\s*(if|else|endif)\b/.test(l)).map(l => l.trim().split(/[ (]/)[0] === 'call' ? l.trim().split(/[ (]/)[1] : l.trim().split(/[ (=]/)[0]);
test('传统视图按执行连线推导事件/动作块，事件下不重复动作', () => { const p = createSample(); const d = eca(p); assert.equal(d.events.length, 1); assert.deepEqual(titles(d.actions[0]), ['core.branch']); const [yes, no] = d.actions[0].items[0].blocks; assert.equal(yes.label, '成立 - 动作'); assert.deepEqual(titles(yes), ['core.spawn', 'core.order']); assert.deepEqual(titles(no), ['core.log']); assert.equal(d.detached, null); assert.ok(!yes.locked && !no.locked); });
test('同块重排改写执行顺序且蓝图结构仍有效', () => { const p = createSample(); const yes = eca(p).actions[0].items[0].blocks[0]; p.graphs[0] = setChain(p.graphs[0], p.definitions, yes, [yes.items[1].node.id, yes.items[0].node.id]); assert.deepEqual(titles(eca(p).actions[0].items[0].blocks[0]), ['core.order', 'core.spawn']); assert.equal(validateProject(p).length, 0); const order = execOrder(p); assert.ok(order.indexOf('GroupPointOrder') < order.indexOf('set')); });
test('跨分支移动保留数据连线且空分支不生成 else', () => { const p = createSample(); const dataEdges = p.graphs[0].edges.filter(e => e.input === 'count' || e.input === 'objects').length; const d = eca(p); const log = d.actions[0].items[0].blocks[1].items[0].node.id; const yes = d.actions[0].items[0].blocks[0]; p.graphs[0] = moveItem(p.graphs[0], p.definitions, log, yes, 1); const [a, b] = eca(p).actions[0].items[0].blocks; assert.deepEqual(titles(a), ['core.spawn', 'core.log', 'core.order']); assert.deepEqual(titles(b), []); assert.equal(p.graphs[0].edges.filter(e => e.input === 'count' || e.input === 'objects').length, dataEdges); assert.equal(validateProject(p).length, 0); assert.ok(!execOrder(p).includes('else')); });
test('不能把流程节点移入自身子块', () => { const p = createSample(); const d = eca(p); const branch = d.actions[0].items[0]; assert.throws(() => moveItem(p.graphs[0], p.definitions, branch.node.id, branch.blocks[0], 0), /自身/); });
test('删除动作桥接前后执行连线，删除流程节点连同子块', () => { const p = createSample(); const spawn = eca(p).actions[0].items[0].blocks[0].items[0].node.id; const q = structuredClone(p); q.graphs[0] = removeItems(q.graphs[0], q.definitions, [spawn]); assert.deepEqual(titles(eca(q).actions[0].items[0].blocks[0]), ['core.order']); assert.equal(validateProject(q).length, 0); const branch = eca(p).actions[0].items[0].node.id; p.graphs[0] = removeItems(p.graphs[0], p.definitions, [branch]); assert.equal(eca(p).actions[0].items.length, 0); assert.ok(!p.graphs[0].nodes.some(n => ['core.spawn', 'core.order', 'core.log', 'core.branch'].includes(n.definition))); });
test('插入到块内指定位置；无后续端口的流程节点只能在块末尾', () => { const p = createSample(); const d = eca(p); const yes = d.actions[0].items[0].blocks[0]; const log = makeNode(p.definitions.find(x => x.id === 'core.log'), 0, 0); p.graphs[0] = insertNode(p.graphs[0], p.definitions, yes, 1, log); assert.deepEqual(titles(eca(p).actions[0].items[0].blocks[0]), ['core.spawn', 'core.log', 'core.order']); const branch = makeNode(p.definitions.find(x => x.id === 'core.branch'), 0, 0); const block = eca(p).actions[0].items[0].blocks[0]; assert.throws(() => insertNode(p.graphs[0], p.definitions, block, 0, branch), /末尾/); assert.equal(findBlock(eca(p), block).items.length, 3); });
test('无事件触发器的动作链按地图初始化执行，不标为未连接', () => { const p = createSample(); const g = p.graphs[1]; const d = buildEca(g, p.definitions); assert.equal(d.events.length, 0); assert.equal(d.detached, null); assert.equal(d.actions[0].items.length, g.nodes.filter(n => n.definition !== 'core.number').length); assert.match(generateTrigger(g, p, 'JASS').content, /TriggerExecute\(t\)/); });
test('有事件时无前驱的动作列为未连接且不生成', () => { const p = createSample(); const orphan = makeNode(p.definitions.find(x => x.id === 'core.log'), 0, 0); orphan.values.message = 'orphan-text'; p.graphs[0].nodes.push(orphan); const d = eca(p); assert.equal(d.detached.items[0].node.id, orphan.id); assert.ok(d.detached.locked); assert.ok(!generateTrigger(p.graphs[0], p, 'JASS').content.includes('orphan-text')); assert.ok(locateItem(d, orphan.id)); });
test('触发条件生成条件函数，禁用项与空条件组不改变语义', () => { const p = createSample(); const c = makeCondition('integer'); c.left = { value: 0, binding: { mode: 'variable', value: 'var.wave' } }; c.op = '!='; c.right = { value: 3 }; const off = makeCondition('boolean'); off.enabled = false; const group = makeCondition('or'); p.graphs[0].conditions = [c, off, group]; const jass = generateTrigger(p.graphs[0], p, 'JASS').content; assert.match(jass, /function Conditions_\w+ takes nothing returns boolean\n    return \(udg_wave != 3\) and \(true\)/); assert.match(jass, /TriggerAddCondition\(t, Condition\(function Conditions_\w+\)\)/); assert.ok(jass.indexOf('function Conditions_') < jass.indexOf('function InitTrig_')); assert.match(generateTrigger(p.graphs[0], p, 'Lua').content, /return \(wave ~= 3\)/); assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(p))), p); });
test('无事件触发器带条件时用条件执行', () => { const p = createSample(); p.graphs[1].conditions = [makeCondition('boolean')]; assert.match(generateTrigger(p.graphs[1], p, 'JASS').content, /ConditionalTriggerExecute\(t\)/); });
test('无效条件结构拒绝导入', () => { const p = createSample(); p.graphs[0].conditions = [{ id: 'x', kind: 'compare', valueType: 'integer', op: '=~', left: { value: 1 }, right: { value: 2 } }]; assert.throws(() => readWorkbench(JSON.parse(JSON.stringify(p))), /条件无效/); });
test('语句模板按字段/端口占位，未知定义回退为标题加参数', () => { const p = createSample(); const spawn = sentenceOf(p.definitions.find(d => d.id === 'core.spawn')); assert.equal(spawn.category, '单位'); assert.deepEqual(spawn.parts.filter(x => x.field).map(x => x.field.id), ['count', 'template', 'position']); const order = sentenceOf(p.definitions.find(d => d.id === 'core.order')); assert.ok(order.parts.some(x => x.port === 'objects')); const custom = sentenceOf({ ...p.definitions[5], id: 'x.custom', title: '扩展动作', namespace: 'ext' }); assert.equal(custom.category, 'ext'); assert.equal(custom.parts[0].text, '扩展动作'); });
test('玩家预设生成原生 Player 调用', () => { const presets = presetValues('player'); assert.equal(presets[0].value, 'Player(0)'); assert.equal(presets.length, 14); assert.deepEqual(presetValues('integer'), []); });
// ---- R05 内置触发目录与模板代码生成 ----
const catalogGraph = (p, spec) => { const project = withCatalog(p); const defs = project.definitions; const nodes = spec.nodes.map(([key, id, values = {}, extra = {}]) => { const n = makeNode(defs.find(d => d.id === id), 0, 0); n.id = key; Object.assign(n.values, values); return Object.assign(n, extra); }); let g = { id: 'g1', name: '测试', folder: '测试', nodes, edges: [] }; for (const [from, output, to, input = 'in'] of spec.edges) g = connectPorts(g, defs, from, output, to, input); project.graphs = [g, ...project.graphs]; return project; };
const jassOf = (project, language = 'JASS') => generateTrigger(project.graphs[0], project, language);
test('内置目录：ID 唯一，语句与模板占位都指向真实字段或端口', () => { const ids = triggerCatalog.map(d => d.id); assert.equal(new Set(ids).size, ids.length); for (const d of triggerCatalog) { assert.ok(typeof d.category === 'string' && d.description, d.id); const fields = new Set(d.fields.map(f => f.id)); const ports = new Set(d.ports.map(port => port.id)); for (const [, name] of String(d.sentence).matchAll(/\{([^}]+)\}/g)) assert.ok(fields.has(name) || ports.has(name), `${d.id} 语句 ${name}`); const t = d.codegen.template ?? d.codegen.register; if (!t) { assert.ok(d.codegen.init, d.id); continue; } for (const text of [t.jass, t.lua]) for (const [, name] of text.matchAll(/\{([^}]+)\}/g)) assert.ok(fields.has(name) || name === 'conditions' || /^(body|callback):/.test(name) && ports.has(name.split(':')[1]), `${d.id} 模板 ${name}`); } });
test('内置目录只在运行时并入，写回工程时去除且工程同 ID 定义优先', () => { const p = createSample(); const merged = withCatalog(p); assert.ok(merged.definitions.length > p.definitions.length); assert.deepEqual(withoutCatalog(merged).definitions, p.definitions); const own = { ...triggerCatalog[0], title: '工程覆盖' }; p.definitions.push(own); const again = withCatalog(p); assert.equal(again.definitions.filter(d => d.id === own.id).length, 1); assert.equal(again.definitions.find(d => d.id === own.id).title, '工程覆盖'); assert.ok(withoutCatalog(again).definitions.includes(own)); });
test('If 多项条件：无 Else 动作不生成 else，后续动作在 endif 之后，地图初始化按条件执行', () => { const c = { id: 'c1', kind: 'compare', valueType: 'integer', op: '<', left: { value: 0, binding: { mode: 'variable', value: 'var.wave' } }, right: { value: 3 } }; const project = catalogGraph(createSample(), { nodes: [['e', 'we.event.mapInit'], ['if', 'we.flow.if', {}, { conditions: [c] }], ['msg', 'we.action.displayText', { text: '开始' }], ['wait', 'we.action.wait', { seconds: 1 }]], edges: [['e', 'then', 'if'], ['if', 'yes', 'msg'], ['if', 'then', 'wait']] }); const code = jassOf(project).content; assert.match(code, /if \(udg_wave < 3\) then\n\s+call DisplayTextToForce\(GetPlayersAll\(\), "开始"\)\n\s+endif\n\s+call TriggerSleepAction\(1\)/); assert.ok(!/\belse\b/.test(code)); assert.match(code, /ConditionalTriggerExecute\(t\)/); const d = buildEca(project.graphs[0], project.definitions); assert.deepEqual(d.actions[0].items.map(i => i.node.id), ['if', 'wait']); assert.deepEqual(d.actions[0].items[0].blocks.map(b => b.label), ['Then - 动作', 'Else - 动作']); assert.equal(validateProject(project).length, 0); assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(withoutCatalog(project)))), withoutCatalog(project)); });
test('选取单位组做动作：JASS 提升为回调，Lua 内联闭包，回调内等待给出诊断', () => { const project = catalogGraph(createSample(), { nodes: [['e', 'we.event.periodic'], ['each', 'we.flow.forRect', { rect: 'region.spawn' }], ['kill', 'we.action.killUnit', {}, { bindings: { unit: { mode: 'function', value: 'native.GetEnumUnit', args: {} } } }], ['wait', 'we.action.wait']], edges: [['e', 'then', 'each'], ['each', 'loop', 'kill'], ['kill', 'then', 'wait']] }); const result = jassOf(project); assert.match(result.content, /function Callback_g1_1 takes nothing returns nothing[\s\S]*call KillUnit\(GetEnumUnit\(\)\)[\s\S]*endfunction/); assert.match(result.content, /call ForGroupBJ\(GetUnitsInRectAll\(gg_rct_region_spawn\), function Callback_g1_1\)/); assert.ok(result.content.indexOf('function Callback_g1_1') < result.content.indexOf('function Actions_g1_0')); assert.ok(result.warnings.some(w => w.includes('不能使用等待'))); assert.match(jassOf(project, 'Lua').content, /ForGroupBJ\(GetUnitsInRectAll\(gg_rct_region_spawn\), function\(\)\n\s+KillUnit\(GetEnumUnit\(\)\)/); });
test('设置变量：值类型跟随所选变量，数组下标写入赋值目标', () => { const project = catalogGraph(createSample(), { nodes: [['e', 'we.event.unitDies'], ['set', 'we.action.setVariable', {}, { bindings: { variable: { mode: 'variable', value: 'var.hero', index: '2' }, value: { mode: 'function', value: 'native.GetKillingUnitBJ', args: {} } } }]], edges: [['e', 'then', 'set']] }); const node = project.graphs[0].nodes.find(n => n.id === 'set'); const valueField = project.definitions.find(d => d.id === 'we.action.setVariable').fields[1]; assert.equal(resolveFieldType(valueField, node, project.variables), 'unit'); assert.match(jassOf(project).content, /set udg_playerHero\[2\] = GetKillingUnitBJ\(\)/); assert.match(jassOf(project).content, /TriggerRegisterAnyUnitEventBJ\(t, EVENT_PLAYER_UNIT_DEATH\)/); delete node.bindings.variable; assert.ok(jassOf(project).warnings.some(w => w.includes('未选择变量'))); });
test('事件注册、点/声音/触发器常量与算术模板生成原生调用', () => { const p = createSample(); const project = catalogGraph(p, { nodes: [['e', 'we.event.chat', { text: '-go' }], ['make', 'we.action.createUnits', { count: 2, unitType: 'unit.knight', location: 'region.spawn' }], ['move', 'we.action.moveUnit', { location: '128, -64' }, { bindings: { unit: { mode: 'function', value: 'native.GetLastCreatedUnit', args: {} } } }], ['gold', 'we.action.addPlayerState', {}, { bindings: { value: { mode: 'function', value: 'native.arith.integer', args: { a: { value: 0, binding: { mode: 'variable', value: 'var.wave' } }, op: { value: '*' }, b: { value: 50 } } } } }], ['snd', 'we.action.playSound', { sound: 'sound.notice' }], ['run', 'we.action.runTrigger', { trigger: 'startup' }]], edges: [['e', 'then', 'make'], ['make', 'then', 'move'], ['move', 'then', 'gold'], ['gold', 'then', 'snd'], ['snd', 'then', 'run']] }); const code = jassOf(project).content; assert.match(code, /TriggerRegisterPlayerChatEvent\(t, Player\(0\), "-go", true\)/); assert.match(code, /CreateNUnitsAtLoc\(2, 'h002', Player\(0\), GetRectCenter\(gg_rct_region_spawn\), 270\)/); assert.match(code, /SetUnitPositionLoc\(GetLastCreatedUnit\(\), Location\(128, -64\)\)/); assert.match(code, /AdjustPlayerStateBJ\(\(udg_wave \* 50\), Player\(0\), PLAYER_STATE_RESOURCE_GOLD\)/); assert.match(code, /PlaySoundBJ\(gg_snd_LevelNotice\)/); assert.match(code, /ConditionalTriggerExecute\(gg_trg_/); assert.equal(validateProject(project).length, 0); });
test('For 循环整数 A 与整数变量循环生成可嵌套的循环体', () => { const project = catalogGraph(createSample(), { nodes: [['e', 'we.event.mapInit'], ['outer', 'we.flow.forA', { from: 1, to: 3 }], ['inner', 'we.flow.forVar', { from: 0, to: 2 }, { bindings: { variable: { mode: 'variable', value: 'var.spawnCount' } } }], ['note', 'we.action.comment', { text: '内层' }]], edges: [['e', 'then', 'outer'], ['outer', 'loop', 'inner'], ['inner', 'loop', 'note']] }); const code = jassOf(project).content; assert.match(code, /set bj_forLoopAIndex = 1\n\s+set bj_forLoopAIndexEnd = 3\n\s+loop\n\s+exitwhen bj_forLoopAIndex > bj_forLoopAIndexEnd\n\s+set udg_spawnCount = 0\n\s+loop\n\s+exitwhen udg_spawnCount > 2\n\s+\/\/ 内层/); assert.match(jassOf(project, 'Lua').content, /while udg_spawnCount <= 2 do|while spawnCount <= 2 do/); });
test('节点条件结构无效时拒绝导入', () => { const p = createSample(); p.graphs[0].nodes[0].conditions = [{ id: 'x', kind: 'nand', children: [] }]; assert.throws(() => readWorkbench(JSON.parse(JSON.stringify(p))), /节点的条件无效/); });
// ---- R06 物编：原版只读 + 地图覆盖（合成目录，不依赖本机游戏数据） ----
const field = (id, name, category, type, extra = {}) => ({ id, field: extra.field ?? id, slk: 'X', index: -1, repeat: extra.repeat ?? 0, data: 0, category, name, sort: id, type, canBeEmpty: false, ...extra });
const miniIndex = () => ({ generatedAt: '', source: '', categories: { text: '文本', stats: '状态' }, enums: { attackType: [{ value: 'normal', label: '普通' }, { value: 'pierce', label: '穿刺' }] }, kinds: {
  unit: { fields: [field('unam', '名字', 'text', 'string', { field: 'Name' }), field('uhpm', '生命', 'stats', 'int'), field('ua1t', '攻击类型', 'stats', 'attackType'), field('uabi', '技能', 'abil', 'abilityList'), field('ustr', '力量', 'stats', 'int')], objects: [{ id: 'hfoo', name: '步兵', suffix: '', group: ['人族', '对战', '单位'], race: 'human', hidden: false, levels: 1, code: 'hfoo', icon: 'btnfootman.png' }, { id: 'Hpal', name: '圣骑士', suffix: '', group: ['人族', '对战', '英雄'], race: 'human', hidden: false, levels: 1, code: 'Hpal', icon: '' }, { id: 'hhid', name: '隐藏', suffix: '', group: ['人族'], race: 'human', hidden: true, levels: 1, code: 'hhid', icon: '' }] },
  ability: { fields: [field('anam', '名字', 'text', 'string', { field: 'Name' }), field('alev', '等级', 'stats', 'int'), field('acdn', '冷却', 'stats', 'unreal', { repeat: 4 })], objects: [{ id: 'AHbz', name: '暴风雪', suffix: '', group: ['人族', '英雄'], race: 'human', hidden: false, levels: 3, code: 'AHbz', icon: '' }] },
  item: { fields: [], objects: [] }, buff: { fields: [], objects: [] }, upgrade: { fields: [], objects: [] }, destructable: { fields: [], objects: [] }, doodad: { fields: [], objects: [] } } });
const unitValues = { hfoo: { unam: '步兵', uhpm: 420, ua1t: 'normal', uabi: 'Adef' }, Hpal: { unam: '圣骑士', uhpm: 100, ua1t: 'hero', uabi: '', ustr: 22 }, hhid: { unam: '隐藏', uhpm: 1 } };
const blank = () => { const p = createEmpty(); p.objects = []; return p; };
test('物编：修改原版物体只存差异，改回原值不留空记录', () => {
  const index = miniIndex(); let p = blank();
  const row = () => objectModel.listObjects(p, index, 'unit').find(r => r.rawcode === 'hfoo');
  p = objectModel.setFieldValue(p, index, unitValues, row(), 'uhpm', 500);
  assert.equal(p.objects.length, 1); assert.deepEqual(p.objects[0].values, { uhpm: 500 }); assert.equal(p.objects[0].base, 'hfoo'); assert.equal(row().modified, 1);
  assert.equal(objectModel.effectiveValues(index, 'unit', unitValues, row()).uhpm, 500);
  p = objectModel.setFieldValue(p, index, unitValues, row(), 'uhpm', 420);
  assert.equal(p.objects.length, 0); assert.equal(row().modified, 0);
  assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(objectModel.setFieldValue(p, index, unitValues, row(), 'unam', '精英步兵')))).objects[0].values, { unam: '精英步兵' });
});
test('物编：复制为自定义生成原版风格 ID，字段以基础物体为准，只存差异，删除只限自定义', () => {
  const index = miniIndex(); let p = blank();
  const rows = () => objectModel.listObjects(p, index, 'unit');
  const first = objectModel.createCustom(p, index, 'unit', rows().find(r => r.rawcode === 'hfoo')); p = first.project;
  assert.equal(first.rawcode, 'h000');
  assert.equal(objectModel.createCustom(p, index, 'unit', rows().find(r => r.rawcode === 'hfoo')).rawcode, 'h001');
  assert.equal(objectModel.createCustom(p, index, 'unit', rows().find(r => r.rawcode === 'Hpal')).rawcode, 'H000');
  assert.equal(objectModel.nextRawcode(p, index, 'ability', 'AHbz'), 'A000');
  const custom = rows().find(r => r.rawcode === 'h000');
  assert.ok(custom.custom); assert.deepEqual(custom.group, ['自定义', '人族', '对战', '单位']);
  assert.deepEqual(objectModel.applicableFields(index, 'unit', unitValues, custom).map(f => f.id), ['unam', 'uhpm', 'ua1t', 'uabi']);
  p = objectModel.setFieldValue(p, index, unitValues, custom, 'uhpm', 420);
  assert.equal(rows().find(r => r.rawcode === 'h000').modified, 0); assert.ok(p.objects.some(o => o.rawcode === 'h000'));
  assert.throws(() => objectModel.deleteCustom(p, rows().find(r => r.rawcode === 'hfoo')));
  p = objectModel.deleteCustom(p, rows().find(r => r.rawcode === 'h000'));
  assert.ok(!rows().some(r => r.rawcode === 'h000'));
});
test('物编：等级数随等级字段覆盖变化，显示值翻译枚举/布尔/引用并保留未知 ID', () => {
  const index = miniIndex(); const values = { AHbz: { anam: '暴风雪', alev: 3, acdn: [6, 6, 6] } }; let p = blank();
  const row = () => objectModel.listObjects(p, index, 'ability')[0];
  assert.equal(objectModel.levelCount('ability', objectModel.effectiveValues(index, 'ability', values, row()), row()), 3);
  p = objectModel.setFieldValue(p, index, values, row(), 'alev', 5);
  assert.equal(objectModel.levelCount('ability', objectModel.effectiveValues(index, 'ability', values, row()), row()), 5);
  assert.equal(objectModel.displayValue(p, index, index.kinds.unit.fields[2], 'pierce'), '穿刺');
  assert.equal(objectModel.displayValue(p, index, field('ubld', '建筑', 'stats', 'bool'), 1), '是');
  assert.equal(objectModel.displayValue(p, index, field('uabi', '技能', 'abil', 'abilityList'), 'AHbz,Xzzz'), '暴风雪，Xzzz');
  assert.equal(objectModel.objectLabel(p, index, ['ability'], 'Xzzz').known, false);
});
test('物编：旧工程未关联原版的自定义物体仍列出并保留名称，示例物体关联原版基础', () => {
  const index = miniIndex(); const p = createSample(); const soldier = p.objects.find(o => o.id === 'unit.soldier'); delete soldier.base;
  const row = objectModel.listObjects(p, index, 'unit').find(r => r.rawcode === soldier.rawcode);
  assert.ok(row.custom); assert.equal(row.name, '守卫步兵'); assert.deepEqual(row.group, ['自定义', '未关联原版']);
  assert.deepEqual(objectModel.applicableFields(index, 'unit', unitValues, row), []);
  const guard = objectModel.listObjects(createSample(), index, 'unit').find(r => r.rawcode === 'h001');
  assert.equal(guard.base, 'hfoo'); assert.equal(objectModel.effectiveValues(index, 'unit', unitValues, guard).unam, '守卫步兵');
});
test('选择器物体目录：地图物体优先，原版隐藏物体不列出，按 rawcode 查找并提供基础图标', () => {
  const index = miniIndex(); const p = createSample();
  objectCatalog.registerStandardCatalog({ objects: kind => index.kinds[kind].objects, iconUrl: icon => `icons/${icon}` });
  try {
    const units = objectCatalog.pickableObjects(p, 'unit');
    assert.ok(units.findIndex(o => o.rawcode === 'h001') < units.findIndex(o => o.rawcode === 'hfoo'));
    assert.ok(!units.some(o => o.rawcode === 'hhid'));
    assert.equal(objectCatalog.findObject(p, 'hfoo', 'unit').name, '步兵');
    assert.equal(objectCatalog.findObject(p, 'hfoo', 'ability'), undefined);
    assert.equal(objectCatalog.catalogIcon(p.objects.find(o => o.rawcode === 'h001')), 'icons/btnfootman.png');
    const node = p.graphs[0].nodes.find(n => n.definition === 'core.spawn'); node.values.template = 'Hpal';
    assert.match(generateTrigger(p.graphs[0], p, 'JASS').content, /'Hpal'/);
  } finally { objectCatalog.registerStandardCatalog(undefined); }
  assert.equal(objectCatalog.findObject(createSample(), 'hfoo'), undefined);
});
test('原版颜色代码拆分为着色片段与换行', () => {
  assert.deepEqual(objectModel.wc3Segments('攻击|n|cffffcc00A|r 键'), [{ text: '攻击', color: undefined }, { text: '', newline: true }, { text: 'A', color: '#ffcc00' }, { text: ' 键', color: undefined }]);
});
test('静态引用包含触发条件、If 条件、物编列表字段与原版 rawcode', () => {
  const p = createSample();
  const c = makeCondition('integer'); c.left = { value: 0, binding: { mode: 'variable', value: 'var.maxWaves' } };
  p.graphs[1].conditions = [c];
  assert.ok(referencesTo(p, 'var.maxWaves').some(r => r.detail === '触发条件' && r.target === p.graphs[1].id));
  const inner = makeCondition('integer'); inner.right = { value: 0, binding: { mode: 'variable', value: 'var.spawnCount' } };
  p.graphs[0].nodes[0].conditions = [inner];
  assert.ok(referencesTo(p, 'var.spawnCount').some(r => r.target === p.graphs[0].nodes[0].id));
  p.objects[0].values = { uabi: 'Adef,AHbz' };
  assert.ok(referencesTo(p, 'AHbz', ['AHbz']).some(r => r.module === 'objects' && r.target === p.objects[0].id));
  assert.ok(!referencesTo(p, 'AHb', ['AHb']).some(r => r.module === 'objects'));
  p.graphs[0].nodes.find(n => n.definition === 'core.spawn').values.template = 'hkni';
  assert.ok(referencesTo(p, 'hkni', ['hkni']).some(r => r.module === 'triggers'));
});
// ---- R07 引用索引、物体管理器与触发器目录 ----
test('引用索引：函数调用位置、条件函数、表达式标识符与变量初值', () => {
  const p = withCatalog(createSample());
  const c = makeCondition('integer'); c.left = { value: 0, binding: { mode: 'variable', value: 'var.wave' } };
  p.graphs[0].conditions = [c];
  const index = referenceIndex.buildReferenceIndex(p);
  assert.ok(index.functions.get('core.spawn').some(u => u.ownerId === p.graphs[0].id && u.text.includes('创建')));
  assert.ok(index.functions.get('condition:integer').some(u => u.target === p.graphs[0].id && u.place === '条件'));
  const wave = p.variables.find(v => v.name === 'wave');
  assert.ok(referenceIndex.usagesOf(index, [wave.id, wave.name]).length >= 1, '表达式 wave < maxWaves 应计入 wave');
  p.variables.find(v => v.name === 'spawnUnitType').initial = 'hfoo';
  assert.ok(referenceIndex.usagesOf(referenceIndex.buildReferenceIndex(p), ['hfoo']).some(u => u.module === 'variables' && u.place === '初始值'));
  const catalog = referenceIndex.functionCatalog(p);
  assert.ok(catalog.some(f => f.id === 'we.action.setVariable' && f.kind === '动作'));
  assert.ok(catalog.some(f => f.id === 'condition:integer' && f.kind === '条件'));
});
test('引用索引：禁用触发器的引用标记为禁用，物体自身字段不算引用', () => {
  const p = createSample();
  p.graphs[0].enabled = false;
  const obj = p.objects[0]; obj.values = { uabi: `Adef,${obj.rawcode}` };
  const index = referenceIndex.buildReferenceIndex(withCatalog(p));
  assert.ok(index.functions.get('core.spawn').every(u => u.disabled));
  assert.ok(!referenceIndex.usagesOf(index, [obj.id, obj.rawcode], obj.id).some(u => u.ownerId === obj.id));
  const groups = referenceIndex.groupUsages(referenceIndex.usagesOf(index, ['Adef']));
  assert.equal(groups.length, 1); assert.equal(groups[0].ownerId, obj.id);
});
test('触发器目录：新建触发器与空文件夹按原版命名并在往返后保留', () => {
  let p = createSample();
  const folder = folders.createFolder(p, p.graphs[0].folder); p = folder.project;
  assert.equal(folder.name, '未命名类别 001');
  assert.deepEqual(folders.folderList(p).slice(0, 2), [p.graphs[0].folder, '未命名类别 001']);
  const a = folders.createTrigger(p, folder.name); p = a.project;
  const b = folders.createTrigger(p, folder.name); p = b.project;
  assert.deepEqual(p.graphs.filter(g => g.folder === folder.name).map(g => g.name), ['未命名触发器 001', '未命名触发器 002']);
  const empty = folders.createFolder(p).project;
  const back = readWorkbench(JSON.parse(JSON.stringify(empty)));
  assert.ok(folders.folderList(back).includes('未命名类别 002'), '空文件夹不能丢失');
  assert.throws(() => readWorkbench({ ...JSON.parse(JSON.stringify(empty)), triggerFolders: [1] }));
});
test('触发器目录：重命名校验重名，移动与排序只改顺序，删除文件夹连同触发器', () => {
  let p = createSample();
  const [first, second] = p.graphs;
  assert.throws(() => folders.renameTrigger(p, second.id, first.name), /已有/);
  assert.throws(() => folders.renameFolder(p, first.folder, second.folder), /已有/);
  p = folders.renameFolder(p, first.folder, '战斗');
  assert.equal(p.graphs[0].folder, '战斗'); assert.equal(folders.folderList(p)[0], '战斗');
  p = folders.moveTrigger(p, second.id, '战斗', first.id);
  assert.deepEqual(p.graphs.map(g => g.id), [second.id, first.id]); assert.equal(p.graphs[0].folder, '战斗');
  p = folders.moveFolder(folders.createFolder(p).project, '未命名类别 001', '战斗');
  assert.equal(folders.folderList(p)[0], '未命名类别 001');
  const removed = folders.deleteFolder(p, '战斗');
  assert.equal(removed.graphs.length, 0); assert.ok(!folders.folderList(removed).includes('战斗'));
});
test('触发器副本：节点与连线换新 ID 且连线仍指向副本节点，代码可生成', () => {
  const p = withCatalog(createSample());
  const source = p.graphs[0];
  const { project, id } = folders.cloneTrigger(p, source);
  const copy = project.graphs.find(g => g.id === id);
  assert.equal(copy.name, `${source.name} 副本`); assert.equal(project.graphs.indexOf(copy), 1);
  const ids = new Set(copy.nodes.map(n => n.id));
  assert.ok(copy.nodes.every(n => !source.nodes.some(o => o.id === n.id)));
  assert.ok(copy.edges.every(e => ids.has(e.from) && ids.has(e.to)));
  assert.equal(folders.cloneTrigger(project, source).project.graphs.filter(g => g.name.startsWith(`${source.name} 副本`)).length, 2);
  assert.ok(generateTrigger(copy, project, 'JASS').content.length > 0);
});
test('允许与初始打开：禁用不生成代码，初始关闭生成 DisableTrigger', () => {
  const p = createSample();
  p.graphs[0].initiallyOn = false;
  assert.match(generateTrigger(p.graphs[0], p, 'JASS').content, /DisableTrigger\(t\)/);
  p.graphs[0].enabled = false;
  assert.doesNotMatch(generateTrigger(p.graphs[0], p, 'JASS').content, /CreateTrigger/);
  const bad = JSON.parse(JSON.stringify(createSample())); bad.graphs[0].enabled = 'yes';
  assert.throws(() => readWorkbench(bad), /允许/);
});
const allModules = ['triggers', 'objects', 'variables', 'sounds', 'assets', 'manager', 'campaign', 'ai', 'world'];
test('停靠布局：模块常驻主区，每个模块只在一处，右侧与浮动移出后主区显示回退', () => {
  let l = dock.defaultLayout();
  assert.equal(dock.mainShown(l, allModules), 'triggers');
  l = dock.activate(l, 'objects');
  assert.equal(dock.mainShown(l, allModules), 'objects');
  l = dock.place(l, 'objects', { area: 'float', x: 10, y: 20, w: 700, h: 500 });
  assert.equal(dock.whereIs(l, 'objects'), 'float');
  assert.equal(dock.mainShown(l, allModules), 'triggers', '主区显示的模块浮动后回退到主区第一个模块');
  l = dock.place(l, 'manager', { area: 'side' });
  l = dock.place(l, 'variables', { area: 'side', index: 0 });
  assert.deepEqual(l.side, ['variables', 'manager']); assert.equal(l.sideActive, 'variables');
  assert.deepEqual(dock.visibleModules(l, allModules).sort(), ['objects', 'triggers', 'variables']);
  l = dock.place(l, 'sounds', { area: 'float', x: 30, y: 40 });
  l = dock.activate(l, 'objects');
  assert.equal(l.floats.at(-1).id, 'objects', '点击窗口提到最前');
  l = dock.place(l, 'variables', { area: 'main' });
  assert.deepEqual(l.side, ['manager']); assert.equal(l.sideActive, 'manager'); assert.equal(dock.mainShown(l, allModules), 'variables');
  l = dock.place(l, 'objects', { area: 'main' });
  l = dock.place(l, 'objects', { area: 'float', x: 5, y: 5 });
  assert.equal(l.floats.find(f => f.id === 'objects').w, 700, '放回后再拖出沿用上次尺寸');
  const placed = [l.side, l.floats.map(f => f.id)].flat();
  assert.equal(new Set(placed).size, placed.length);
  assert.equal(dock.mainModules(l, allModules).length + placed.length, allModules.length);
});
test('停靠布局：收起窗口仅在显式打开时展开，预设与读取校验', () => {
  let l = dock.place(dock.defaultLayout(), 'ai', { area: 'float', x: 0, y: 0, w: 500, h: 400 });
  l = dock.updateFloat(l, 'ai', { collapsed: true });
  assert.ok(dock.activate(l, 'ai').floats[0].collapsed);
  assert.ok(!dock.activate(l, 'ai', true).floats[0].collapsed);
  const preset = dock.layoutPresets.find(p => p.id === 'triggers-manager').make({ ...l, sideWidth: 600 });
  assert.equal(preset.mainActive, 'triggers'); assert.deepEqual(preset.side, ['manager']); assert.equal(preset.floats.length, 0); assert.equal(preset.sideWidth, 600);
  const known = ['triggers', 'objects', 'ai'];
  const read = dock.readLayout({ main: ['triggers'], mainActive: 'bogus', side: ['objects', 'objects', 'bogus'], floats: [{ id: 'ai', x: 1, y: 2, w: 3, h: NaN }, { id: 'objects', x: 0, y: 0, w: 400, h: 300 }], sideWidth: 10, sizes: { ai: { w: 500, h: 300 }, bogus: { w: 1, h: 1 } } }, known);
  assert.equal(read.mainActive, 'triggers'); assert.deepEqual(read.side, ['objects']); assert.equal(read.floats.length, 0);
  assert.equal(read.sideWidth, 320); assert.deepEqual(Object.keys(read.sizes), ['ai']);
  assert.deepEqual(dock.readLayout('garbage', known), dock.defaultLayout());
  const clamped = dock.clampFloat({ id: 'ai', x: 5000, y: -50, w: 100, h: 9999 }, { width: 1200, height: 800 });
  assert.deepEqual([clamped.x, clamped.y, clamped.w, clamped.h], [1120, 0, 360, 800]);
});

// ---- R08：世界坐标、修改审阅、命令排序、助手提议 ----
test('R08 旧版俯视比例读取时换算为世界坐标，新工程往返不变', () => {
  const legacy = JSON.parse(JSON.stringify(createSample()));
  legacy.world = { instances: [{ id: 'instance.a', objectId: 'unit.hero', x: 50, y: 50, owner: 1, rotation: 0 }], regions: [{ id: 'region.a', name: 'A', x: 0, y: 0, width: 50, height: 50 }] };
  const read = readWorkbench(legacy);
  assert.equal(read.world.space, 'world');
  assert.deepEqual([read.world.instances[0].x, read.world.instances[0].y], [0, 0]);
  assert.deepEqual([read.world.regions[0].x, read.world.regions[0].y, read.world.regions[0].width, read.world.regions[0].height], [-4096, 0, 4096, 4096], '区域左上角换算为左下角，y 向北');
  const sample = createSample();
  assert.deepEqual(readWorkbench(JSON.parse(JSON.stringify(sample))), sample);
  assert.ok(sample.world.cameras.length >= 2);
});
test('R08 世界批量编辑：平移受地图范围限制，复制生成新 ID 并整体平移，删除只删选择集', () => {
  const p = createSample();
  const moved = worldModel.moveEntities(p, ['instance.1', 'region.spawn'], 100000, 0);
  assert.equal(moved.world.instances.find(i => i.id === 'instance.1').x, 4096);
  const spawn = moved.world.regions.find(r => r.id === 'region.spawn');
  assert.equal(spawn.x + spawn.width, 4096, '区域右边界贴地图边');
  const dup = worldModel.duplicateEntities(p, ['instance.1', 'camera.base'], 64, -64);
  assert.equal(dup.ids.length, 2);
  assert.ok(dup.ids.every(id => !p.world.instances.some(i => i.id === id)));
  const copy = dup.project.world.instances.find(i => i.id === dup.ids[0]);
  assert.deepEqual([copy.x, copy.y], [p.world.instances[0].x + 64, p.world.instances[0].y - 64]);
  assert.notEqual(dup.project.world.cameras.find(c => c.id === dup.ids[1]).name, '基地全景', '副本名称不重名');
  const removed = worldModel.removeEntities(p, ['instance.2']);
  assert.equal(removed.world.instances.length, p.world.instances.length - 1);
  assert.equal(worldModel.angleTo({ x: 0, y: 0 }, { x: 0, y: 10 }), 90, '朝北为 90°');
  assert.equal(worldModel.angleTo({ x: 0, y: 0 }, { x: -10, y: -9 }, 45), 225);
  const cells = worldModel.gridInRegion({ id: 'r', name: 'r', x: 0, y: 0, width: 512, height: 512 }, 5);
  assert.equal(new Set(cells.map(c => `${c.x},${c.y}`)).size, 5);
  assert.ok(cells.every(c => c.x >= 0 && c.x <= 512 && c.y >= 0 && c.y <= 512));
});
test('R08 本次修改：按模块列出新增/删除/字段变化，逐项撤销回到基线', () => {
  const base = createSample();
  let now = worldModel.patchEntities(base, ['instance.2'], { instance: { owner: 3 } });
  now = { ...now, objects: now.objects.map(o => o.id === 'unit.soldier' ? { ...o, health: 500, fields: { ...o.fields, armor: 6 } } : o) };
  now = worldModel.removeEntities(now, ['region.mine']);
  now = worldModel.placeInstance(now, 'hfoo', 0, 0, 1).project;
  now = { ...now, settings: { ...now.settings, fog: false } };
  const groups = projectDiff.diffProjects(base, now);
  const find = key => groups.flatMap(g => g.items).find(i => i.key === key);
  const owner = find('instances:instance.2');
  assert.equal(owner.kind, 'modified'); assert.deepEqual(owner.fields.map(f => [f.label, f.before, f.after]), [['所属玩家', '1', '3']]);
  assert.deepEqual(find('objects:unit.soldier').fields.map(f => f.label).sort(), ['护甲', '生命值'].sort());
  assert.equal(find('regions:region.mine').kind, 'removed');
  assert.equal(groups.find(g => g.id === 'instances').items.filter(i => i.kind === 'added').length, 1);
  assert.equal(find('section:settings').fields[0].label, '战争迷雾');
  let reverted = now;
  for (const item of groups.flatMap(g => g.items)) reverted = projectDiff.revertChange(reverted, base, item);
  assert.deepEqual(reverted, base, '逐项撤销后与基线完全一致（含删除项放回原位）');
  assert.equal(projectDiff.changeCount(projectDiff.diffProjects(base, base)), 0);
});
test('R08 命令面板排序：精确与开头优先，子序列可命中，多个词须全部命中', () => {
  const items = ['打开触发器', '测试地图', '触发器', '审阅本次修改', '选择同类对象'];
  assert.deepEqual(commandRank.rank(items, '触发器', x => x).slice(0, 2), ['触发器', '打开触发器']);
  assert.deepEqual(commandRank.rank(items, '审修', x => x), ['审阅本次修改']);
  assert.deepEqual(commandRank.rank(items, '打开 触发', x => x), ['打开触发器']);
  assert.equal(commandRank.fuzzyScore('xyz', '测试地图'), null);
  assert.equal(commandRank.rank(items, '', x => x).length, items.length);
});
test('R08 助手：查询不改数据，编辑只产生提议并绑定修订，无法识别时不猜测', () => {
  const p = createSample();
  const ctx = { project: p, selection: ['instance.2', 'instance.3'], revision: 'r7', owner: 1 };
  const unused = assistant.interpret('查找未使用的变量', ctx);
  assert.ok(!unused.proposal && Array.isArray(unused.results));
  const owner = assistant.interpret('把选中的单位改为玩家 2', ctx);
  assert.ok(owner.proposal && owner.calls[0].includes('baseRevision="r7"'));
  assert.deepEqual(owner.proposal.project.world.instances.filter(i => i.owner === 2).map(i => i.id), ['instance.2', 'instance.3']);
  assert.equal(p.world.instances.find(i => i.id === 'instance.2').owner, 1, '原工程不被修改');
  const health = assistant.interpret('把 守卫步兵 的生命值增加 50', ctx);
  assert.equal(health.proposal.project.objects.find(o => o.id === 'unit.soldier').health, 470);
  const place = assistant.interpret('在 防守区域 放置 4 个 守卫步兵', ctx);
  assert.equal(place.proposal.project.world.instances.length, p.world.instances.length + 4);
  const region = p.world.regions.find(r => r.name === '防守区域');
  assert.ok(place.proposal.project.world.instances.slice(-4).every(i => i.x >= region.x && i.x <= region.x + region.width && i.y >= region.y && i.y <= region.y + region.height));
  const pick = assistant.interpret('选中所有 巡林射手', ctx);
  assert.deepEqual(pick.select, p.world.instances.filter(i => i.objectId === 'unit.archer').map(i => i.id));
  assert.ok(!assistant.interpret('把玩家 99 的东西都删掉', ctx).proposal);
  assert.ok(/玩家编号/.test(assistant.interpret('把选中的单位改为玩家 99', ctx).reply));
});
await writeFile(new URL('results.json', output), JSON.stringify({ passed: results.length, tests: results }, null, 2));
console.log(JSON.stringify({ passed: results.length, tests: results }, null, 2));
