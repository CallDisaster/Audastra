import type { Definition, Graph, GraphNode } from '../studio/model';
import type { WorkbenchProject } from './types';
import { expectedFieldType, objectKindForType, returnTypeOf, valueFunctionDefinitions } from './valueSources';
import type { TriggerCondition } from './ecaModel';
import { resolveFieldType } from './parameterFields';

type Template = { jass: string; lua: string };
/** 以 {名称} 占位的模板替换；未知占位原样保留，便于诊断。 */
export const fillTemplate = (text: string, resolve: (name: string) => string | undefined) => text.replace(/\{([A-Za-z0-9_:]+)\}/g, (whole, name: string) => resolve(name) ?? whole);

export type ScriptLanguage = 'JASS' | 'vJass' | 'Lua';
export type ParameterBinding = { mode: 'constant' | 'variable' | 'expression' | 'function'; value: string; index?: string; args?: Record<string, { value: unknown; binding?: ParameterBinding }> };
export type TriggerText = { language: ScriptLanguage; content: string; generated: string; source: Graph; warnings: string[] };
export type CodeResult = { content: string; warnings: string[]; mapped: number };
export const identifier = (value: string) => value.replace(/[^a-zA-Z0-9_]/g, '_').replace(/^[0-9]/, 'v_$&') || 'trigger';
export function bindingOf(node: GraphNode, field: string): ParameterBinding | undefined {
  return (node.bindings as Record<string, ParameterBinding> | undefined)?.[field];
}
export function generateTrigger(graph: Graph, project: WorkbenchProject, language: ScriptLanguage): CodeResult {
  const lua = language === 'Lua';
  const prefix = identifier(graph.id);
  const warnings: string[] = [];
  const comment = lua ? '--' : '//';
  const definitions = new Map(project.definitions.map(d => [d.id, d]));
  const functions = new Map(valueFunctionDefinitions(project).map(d => [d.id, d]));
  const nodes = new Map(graph.nodes.map(n => [n.id, n]));
  const active = graph.nodes.filter(n => n.enabled !== false);
  const mappedNodes = new Set<string>();
  const quote = (s: unknown) => JSON.stringify(String(s ?? ''));
  const variableName = (id: string) => {
    const variable = project.variables.find(v => v.id === id || v.name === id);
    return variable ? (lua ? identifier(variable.name) : `udg_${identifier(variable.name)}`) : identifier(id);
  };
  const expression = (value: string) => {
    let output = value;
    for (const variable of project.variables) {
      const name = identifier(variable.name);
      output = output.replace(new RegExp(`\\b${name}\\b`, 'g'), variableName(variable.id));
    }
    return output.replace(/&&/g, ' and ').replace(/\|\|/g, ' or ').replace(/!==|!=/g, lua ? '~=' : '!=');
  };
  const constant = (raw: unknown, type: string): string => {
    if (objectKindForType(type)) {
      const rawcode = project.objects.find(object => object.id === raw || object.rawcode === raw)?.rawcode ?? String(raw ?? '');
      if (!/^[\x20-\x7e]{4}$/.test(rawcode)) { warnings.push(`物体类型未解析：${rawcode || '空'}`); return '0'; }
      return lua ? `FourCC(${quote(rawcode)})` : `'${rawcode}'`;
    }
    if (type === 'rect') {
      const region = project.world.regions.find(item => item.id === raw || item.name === raw);
      if (region) { const symbol = `gg_rct_${identifier(region.id)}`; warnings.push(`区域 ${region.name}：地图后端需提供 ${symbol}`); return symbol; }
    }
    if (type === 'string' || type === 'text') return quote(raw);
    if (type === 'unit') {
      const instance = project.world.instances.find(item => item.id === raw);
      if (instance) { const symbol = `gg_unit_${identifier(instance.id)}`; warnings.push(`放置单位 ${instance.id}：地图后端需提供 ${symbol}`); return symbol; }
    }
    if (type === 'location') {
      const region = project.world.regions.find(item => item.id === raw || item.name === raw);
      if (region) { const symbol = `gg_rct_${identifier(region.id)}`; warnings.push(`区域 ${region.name}：地图后端需提供 ${symbol}`); return `GetRectCenter(${symbol})`; }
      const coords = String(raw ?? '').match(/^\s*(-?\d+(?:\.\d+)?)\s*[,，]\s*(-?\d+(?:\.\d+)?)\s*$/);
      if (coords) return `Location(${coords[1]}, ${coords[2]})`;
    }
    if (type === 'sound') {
      const sound = project.sounds.find(item => item.id === raw || item.name === raw);
      if (sound) { const symbol = `gg_snd_${identifier(sound.name)}`; warnings.push(`声音 ${sound.name}：地图后端需提供 ${symbol}`); return symbol; }
    }
    if (type === 'trigger') {
      const target = project.graphs.find(item => item.id === raw);
      if (target) { const symbol = `gg_trg_${identifier(target.name)}`; warnings.push(`触发器 ${target.name}：地图后端需提供 ${symbol}`); return symbol; }
      warnings.push('触发器参数未选择'); return lua ? 'nil' : 'null';
    }
    if (type === 'order') return quote(raw);
    if (['unit', 'item', 'player', 'group', 'rect', 'location', 'timer', 'sound'].includes(type)) return raw === 'null' || raw === '' || raw == null ? lua ? 'nil' : 'null' : String(raw);
    return String(raw);
  };
  const boundValue = (binding: ParameterBinding | undefined, raw: unknown, type: string, depth = 0): string => {
    if (depth > 16) { warnings.push('函数参数嵌套超过预览深度'); return lua ? 'nil' : '0'; }
    if (binding?.mode === 'variable') return variableName(binding.value) + (project.variables.find(v => v.id === binding.value)?.array ? `[${expression(binding.index ?? '0')}]` : '');
    if (binding?.mode === 'expression') return expression(binding.value);
    if (binding?.mode === 'function') {
      const definition = functions.get(binding.value);
      const codegen = definition?.codegen as Record<string, unknown> | undefined;
      const template = (codegen?.template as Template | undefined)?.[lua ? 'lua' : 'jass'];
      const argument = (field: Definition['fields'][number]) => { const arg = binding.args?.[field.id]; return boundValue(arg?.binding, arg?.value ?? field.default, expectedFieldType(field), depth + 1); };
      if (definition && template) return fillTemplate(template, name => { const field = definition.fields.find(item => item.id === name); return field ? argument(field) : undefined; });
      const symbol = codegen?.[lua ? 'lua' : 'jass'] as string | undefined;
      if (!definition || !symbol || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(symbol)) { warnings.push(`函数缺少 ${language} 映射：${binding.value}`); return lua ? 'nil' : '0'; }
      return `${symbol}(${definition.fields.map(argument).join(', ')})`;
    }
    return constant(raw, type);
  };
  const value = (node: GraphNode, field: string, fallback: unknown = '', text = false): string => {
    const binding = bindingOf(node, field);
    const raw = node.values[field] ?? fallback;
    const definition = definitions.get(node.definition)?.fields.find(item => item.id === field);
    return boundValue(binding, raw, text ? 'string' : definition ? resolveFieldType(definition, node, project.variables) : 'any');
  };
  const position = (raw: unknown): [string, string] => {
    const region = project.world.regions.find(r => r.id === raw || r.name === raw);
    if (region) { const symbol = `gg_rct_${identifier(region.id)}`; warnings.push(`区域 ${region.name}：地图后端需提供 ${symbol}`); return [`GetRectCenterX(${symbol})`, `GetRectCenterY(${symbol})`]; }
    const coords = String(raw ?? '').match(/^\s*(-?\d+(?:\.\d+)?)\s*[,，]\s*(-?\d+(?:\.\d+)?)\s*$/);
    if (coords) return [coords[1], coords[2]];
    warnings.push(`未解析位置：${String(raw ?? '空')}`);
    return ['0', '0'];
  };
  const positionRefs = active.flatMap(node => {
    const field = node.definition === 'core.spawn' ? 'position' : node.definition === 'core.order' ? 'target' : undefined;
    const binding = field && bindingOf(node, field);
    if (!field || !binding || binding.mode === 'constant') return [];
    const definition = functions.get(binding.value);
    const type = binding.mode === 'variable' ? project.variables.find(variable => variable.id === binding.value || variable.name === binding.value)?.type : binding.mode === 'function' && definition ? returnTypeOf(definition) : 'rect';
    return [{ node: node.id, field, type: type === 'location' ? 'location' : 'rect', name: `position_${identifier(node.id)}` }];
  });
  const groupName = (node: GraphNode) => `spawn_${identifier(node.id)}`;
  const calls = (text: string) => lua ? text : `call ${text}`;
  const body: string[] = [];
  let out = body;
  const callbacks: string[] = [];
  const localGroups = active.filter(n => n.definition === 'core.spawn');
  const emit = (depth: number, text: string) => out.push(`${'    '.repeat(depth)}${text}`);
  const coordinates = (node: GraphNode, field: string, depth: number): [string, string] => {
    const reference = positionRefs.find(item => item.node === node.id && item.field === field);
    if (!reference) return position(node.values[field]);
    emit(depth, `${lua ? '' : 'set '}${reference.name} = ${value(node, field)}`);
    return reference.type === 'location' ? [`GetLocationX(${reference.name})`, `GetLocationY(${reference.name})`] : [`GetRectCenterX(${reference.name})`, `GetRectCenterY(${reference.name})`];
  };
  const follow = (node: GraphNode, output: string, depth: number, path: Set<string>) => {
    graph.edges.filter(e => e.from === node.id && e.output === output).forEach(e => {
      const next = nodes.get(e.to);
      if (next) compile(next, depth, path);
    });
  };
  const compile = (node: GraphNode, depth: number, path: Set<string>) => {
    if (path.has(node.id)) { warnings.push('执行连线存在循环；使用循环节点或回调'); emit(depth, `${comment} 循环连线未生成`); return; }
    const nextPath = new Set(path).add(node.id);
    const definition = definitions.get(node.definition);
    if (node.enabled === false) { if (definition?.kind === 'action') definition.ports.filter(p => p.direction === 'out' && p.type === 'exec').forEach(p => follow(node, p.id, depth, nextPath)); return; }
    mappedNodes.add(node.id);
    if (node.definition === 'core.branch') {
      const input = graph.edges.find(e => e.to === node.id && e.input === 'condition');
      const inputNode = input && nodes.get(input.from);
      const condition = inputNode?.definition === 'core.boolean' ? value(inputNode, 'value', true) : expression(value(node, 'expression', 'true'));
      emit(depth, `if ${condition} then`);
      follow(node, 'yes', depth + 1, nextPath);
      if (graph.edges.some(e => e.from === node.id && e.output === 'no')) { emit(depth, 'else'); follow(node, 'no', depth + 1, nextPath); }
      emit(depth, lua ? 'end' : 'endif');
      return;
    }
    if (node.definition === 'core.spawn') {
      const object = project.objects.find(o => o.id === node.values.template || o.rawcode === node.values.template);
      const rawcode = object?.rawcode || String(node.values.template ?? 'hfoo');
      if (!/^[\x20-\x7e]{4}$/.test(rawcode)) warnings.push(`单位类型未解析：${rawcode}`);
      const objectBinding = bindingOf(node, 'template');
      const typeExpr = objectBinding && objectBinding.mode !== 'constant' ? value(node, 'template') : lua ? `FourCC(${quote(rawcode)})` : `'${rawcode}'`;
      const [x, y] = coordinates(node, 'position', depth);
      const countEdge = graph.edges.find(e => e.to === node.id && e.input === 'count');
      const countNode = countEdge && nodes.get(countEdge.from);
      const variable = countNode?.definition === 'core.number' && project.variables.find(v => v.name === countNode.values.name || v.id === countNode.values.name);
      const count = countNode?.definition === 'core.number' ? !bindingOf(countNode, 'value') && variable ? variableName(variable.id) : value(countNode, 'value', 1) : value(node, 'count', 1);
      if (countNode) mappedNodes.add(countNode.id);
      emit(depth, `${lua ? '' : 'set '}i = 0`);
      emit(depth, lua ? `while i < ${count} do` : 'loop');
      if (!lua) emit(depth + 1, `exitwhen i >= ${count}`);
      emit(depth + 1, `${lua ? '' : 'set '}u = CreateUnit(Player(${value(node, 'owner', 0)}), ${typeExpr}, ${x}, ${y}, 270.0)`);
      emit(depth + 1, calls(`GroupAddUnit(${groupName(node)}, u)`));
      emit(depth + 1, `${lua ? '' : 'set '}i = i + 1`);
      emit(depth, lua ? 'end' : 'endloop');
    } else if (node.definition === 'core.order') {
      const edge = graph.edges.find(e => e.to === node.id && e.input === 'objects');
      const source = edge && nodes.get(edge.from);
      const [x, y] = coordinates(node, 'target', depth);
      if (source?.definition === 'core.spawn') {
        emit(depth, calls(`GroupPointOrder(${groupName(source)}, ${value(node, 'command', 'attack', true)}, ${x}, ${y})`));
      } else { warnings.push(`${definition?.title ?? '发送指令'}缺少单位组来源`); emit(depth, `${comment} 缺少单位组来源`); }
    } else if (node.definition === 'core.log') emit(depth, calls(`DisplayTextToPlayer(GetLocalPlayer(), 0, 0, ${value(node, 'message', '', true)})`));
    else if (definition?.kind === 'event') { /* 事件在 InitTrig 中注册 */ }
    else if ((definition?.codegen as Record<string, unknown> | undefined)?.template) { emitTemplate(node, definition!, depth, nextPath); return; }
    else if (node.definition !== 'core.interval' && node.definition !== 'core.number') {
      const codegen = definition?.codegen as Record<string, string> | undefined;
      const functionName = codegen?.[lua ? 'lua' : 'jass'];
      if (functionName && /^[A-Za-z_][A-Za-z0-9_]*$/.test(functionName)) emit(depth, calls(`${functionName}(${definition!.fields.map(f => value(node, f.id, f.default, f.type === 'text')).join(', ')})`));
      else { mappedNodes.delete(node.id); warnings.push(`缺少${language}映射：${definition?.title ?? node.definition}`); emit(depth, `${comment} 未映射：${definition?.title ?? node.definition}`); }
    }
    const out = definition?.ports.filter(p => p.direction === 'out' && p.type === 'exec') ?? [];
    out.forEach(port => follow(node, port.id, depth, nextPath));
  };
  // 模板语句：{body:端口} 就地展开子块；{callback:端口} 在 JASS 中提升为独立回调函数；“?端口:” 行仅在端口有动作时输出。
  const hasBody = (node: GraphNode, port: string) => graph.edges.some(edge => edge.from === node.id && edge.output === port);
  const emitTemplate = (node: GraphNode, definition: Definition, depth: number, path: Set<string>) => {
    const template = ((definition.codegen as Record<string, unknown>).template as Template)[lua ? 'lua' : 'jass'];
    const resolve = (name: string): string | undefined => {
      if (name === 'conditions') { const parts = (Array.isArray(node.conditions) ? node.conditions as TriggerCondition[] : []).map(conditionExpression).filter((item): item is string => !!item); return parts.length ? parts.map(part => `(${part})`).join(' and ') : 'true'; }
      const callback = name.match(/^callback:(.+)$/);
      if (callback) {
        const functionName = `Callback_${prefix}_${callbacks.length + 1}`;
        const saved = out; out = [];
        follow(node, callback[1], 1, path);
        const lines = out; out = saved;
        if (lines.some(line => line.includes('TriggerSleepAction'))) warnings.push(`${definition.title}：回调中不能使用等待`);
        if (lines.some(line => /\bspawn_\w+|\bposition_\w+/.test(line))) warnings.push(`${definition.title}：回调中不能使用动作的局部单位组或位置`);
        callbacks.push(`function ${functionName} takes nothing returns nothing\n    local integer i = 0\n    local unit u = null\n${lines.join('\n')}\n    set u = null\nendfunction`);
        return functionName;
      }
      const field = definition.fields.find(item => item.id === name);
      if (!field) return undefined;
      if (resolveFieldType(field, node, project.variables).startsWith('globalvar') && bindingOf(node, name)?.mode !== 'variable') { warnings.push(`${definition.title}：未选择变量`); return lua ? '_' : 'udg_unset'; }
      return value(node, name, field.default);
    };
    for (const raw of template.split('\n')) {
      let line = raw;
      const optional = line.match(/^(\s*)\?([A-Za-z0-9_]+):(.*)$/);
      if (optional) { if (!hasBody(node, optional[2])) continue; line = optional[1] + optional[3]; }
      const indent = Math.floor((line.match(/^\s*/)?.[0].length ?? 0) / 4);
      const text = line.trim();
      const block = text.match(/^\{body:([A-Za-z0-9_]+)\}$/);
      if (block) {
        // 子块中的“?端口:else”已跳过时，对应空子块也不输出。
        follow(node, block[1], depth + indent, path);
        continue;
      }
      emit(depth + indent, fillTemplate(text, resolve));
    }
    const next = definition.ports.find(port => port.direction === 'out' && port.type === 'exec' && port.id === 'then');
    if (next) follow(node, next.id, depth, path);
  };
  // 触发条件：全部成立才执行动作（原版条件列表语义）；禁用项不参与，空 And/Or 视为成立。
  const conditionExpression = (condition: TriggerCondition): string | undefined => {
    if (condition.enabled === false) return undefined;
    if (condition.kind === 'compare') {
      const op = lua && condition.op === '!=' ? '~=' : condition.op;
      return `${boundValue(condition.left.binding, condition.left.value, condition.valueType)} ${op} ${boundValue(condition.right.binding, condition.right.value, condition.valueType)}`;
    }
    const parts = condition.children.map(conditionExpression).filter((item): item is string => !!item);
    return parts.length ? `(${parts.map(part => `(${part})`).join(condition.kind === 'and' ? ' and ' : ' or ')})` : 'true';
  };
  const conditionParts = (Array.isArray(graph.conditions) ? graph.conditions as TriggerCondition[] : []).map(conditionExpression).filter((item): item is string => !!item);
  const conditionName = `Conditions_${prefix}`;
  const conditionFunction = conditionParts.length ? lua
    ? `local function ${conditionName}()
    return ${conditionParts.map(part => `(${part})`).join(' and ')}
end`
    : `function ${conditionName} takes nothing returns boolean
    return ${conditionParts.map(part => `(${part})`).join(' and ')}
endfunction` : '';
  const roots = active.filter(n => definitions.get(n.definition)?.kind === 'event');
  const declarations = localGroups.map(n => `    local ${lua ? '' : 'group '}${groupName(n)} = CreateGroup()`);
  declarations.push(...positionRefs.map(reference => `    local ${lua ? '' : `${reference.type} `}${reference.name} = ${lua ? 'nil' : 'null'}`));
  declarations.push(lua ? '    local i, u = 0, nil' : '    local integer i = 0\n    local unit u = null');
  const cleanup = localGroups.map(n => `    ${calls(`DestroyGroup(${groupName(n)})`)}${lua ? '' : `\n    set ${groupName(n)} = null`}`);
  if (!lua) cleanup.push(...positionRefs.map(reference => `    set ${reference.name} = null`), '    set u = null');
  const handlers: { name: string; body: string; root?: GraphNode }[] = [];
  const writeHandler = (name: string, root?: GraphNode) => {
    handlers.push({ name, root, body: `${lua ? `local function ${name}()` : `function ${name} takes nothing returns nothing`}\n${[...declarations, ...body, ...cleanup].join('\n')}\n${lua ? 'end' : 'endfunction'}` });
    body.length = 0;
  };
  if (roots.length) roots.forEach((root, index) => { compile(root, 1, new Set()); writeHandler(`Actions_${prefix}_${index}`, root); });
  else {
    active.filter(n => definitions.get(n.definition)?.kind !== 'value' && !graph.edges.some(e => e.to === n.id && definitions.get(nodes.get(e.from)?.definition ?? '')?.ports.some(p => p.id === e.output && p.type === 'exec'))).forEach(root => compile(root, 1, new Set()));
    writeHandler(`Actions_${prefix}`);
  }
  const supportedTypes = ['integer', 'real', 'boolean', 'string', 'unit', 'item', 'group', 'force', 'timer', 'player', 'rect', 'location', 'unitcode', 'itemcode', 'abilcode', 'buffcode', 'destructablecode', 'doodadcode', 'upgradecode', 'number'];
  project.variables.filter(v => !supportedTypes.includes(v.type)).forEach(v => warnings.push(`全局类型未映射：${v.name} (${v.type})`));
  const arrays: { name: string; initial: string; size: number }[] = [];
  const globalLines = project.variables.filter(v => supportedTypes.includes(v.type)).map(v => {
    const type = objectKindForType(v.type) ? 'integer' : v.type === 'number' ? 'real' : v.type;
    let initial = v.initial || (type === 'string' ? '""' : type === 'boolean' ? 'false' : ['integer', 'real'].includes(type) ? '0' : lua ? 'nil' : 'null');
    if (type === 'string' && !initial.startsWith('"')) initial = quote(initial);
    if (objectKindForType(v.type) && initial !== '0') initial = constant(initial.replace(/^'|'$/g, ''), v.type);
    if (v.array) arrays.push({ name: variableName(v.id), initial: lua ? initial.replace(/\bnull\b/g, 'nil') : initial, size: Math.max(0, Math.floor(v.size)) });
    return lua ? `${variableName(v.id)} = ${v.array ? '{}' : initial.replace(/\bnull\b/g, 'nil')}` : `    ${type}${v.array ? ' array' : ''} ${variableName(v.id)}${v.array ? '' : ` = ${initial}`}`;
  });
  const initializeArrays = arrays.flatMap(a => lua ? [`    for index = 0, ${a.size - 1} do`, `        ${a.name}[index] = ${a.initial}`, '    end'] : ['    set index = 0', '    loop', `        exitwhen index >= ${a.size}`, `        set ${a.name}[index] = ${a.initial}`, '        set index = index + 1', '    endloop']);
  const initLines = [lua ? '    local t' : '    local trigger t = null'];
  if (arrays.length && !lua) initLines.push('    local integer index = 0');
  initLines.push(...initializeArrays);
  handlers.forEach(handler => {
    initLines.push(`    ${lua ? '' : 'set '}t = CreateTrigger()`);
    const eventCodegen = handler.root && definitions.get(handler.root.definition)?.codegen as Record<string, unknown> | undefined;
    const register = (eventCodegen?.register as Template | undefined)?.[lua ? 'lua' : 'jass'];
    if (handler.root?.definition === 'core.interval') initLines.push(`    ${calls(`TriggerRegisterTimerEvent(t, ${value(handler.root, 'interval', 30)}, ${value(handler.root, 'repeat', true)})`)}`);
    else if (handler.root && register) { const root = handler.root; initLines.push(`    ${fillTemplate(register, name => definitions.get(root.definition)?.fields.some(item => item.id === name) ? value(root, name) : undefined)}`); }
    else if (handler.root && !eventCodegen?.init) warnings.push(`事件注册未映射：${handler.root.definition}`);
    if (conditionFunction) initLines.push(`    ${calls(`TriggerAddCondition(t, Condition(${lua ? '' : 'function '}${conditionName}))`)}`);
    initLines.push(`    ${calls(`TriggerAddAction(t, ${lua ? '' : 'function '}${handler.name})`)}`);
    if (graph.initiallyOn === false) initLines.push(`    ${calls('DisableTrigger(t)')}`);
    if (!handler.root && graph.initiallyOn !== false) initLines.push(`    ${calls(conditionFunction ? 'ConditionalTriggerExecute(t)' : 'TriggerExecute(t)')}`);
    // 地图初始化事件：与原版 RunInitializationTriggers 一致，初始化时按条件执行一次。
    if (eventCodegen?.init) initLines.push(`    ${calls('ConditionalTriggerExecute(t)')}`);
  });
  if (!lua) initLines.push('    set t = null');
  const init = `${lua ? `function InitTrig_${prefix}()` : `function InitTrig_${prefix} takes nothing returns nothing`}\n${initLines.join('\n')}\n${lua ? 'end' : 'endfunction'}`;
  const globals = globalLines.length ? lua ? globalLines.join('\n') : `globals\n${globalLines.join('\n')}\nendglobals` : '';
  const pieces = [`${comment} ${graph.name}`, globals, conditionFunction, ...callbacks, ...handlers.map(h => h.body), init].filter(Boolean);
  if (language === 'vJass') pieces.splice(1, 0, `scope Trigger_${prefix} initializer InitTrig_${prefix}`);
  if (language === 'vJass') pieces.push('endscope');
  return { content: graph.enabled === false ? `${comment} ${graph.name} · 已禁用\n` : pieces.join('\n\n') + '\n', warnings: Array.from(new Set(warnings)), mapped: mappedNodes.size };
}

export function definitionSignature(definition: Definition): string {
  return `${definition.id}(${definition.fields.map(f => f.id).join(', ')})`;
}
