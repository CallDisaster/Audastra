import { useId, useRef, useState } from 'react';
import { ArrowLeft, ArrowUpRight, Braces, Check, ChevronRight, Code2, GitBranch, Hash, MapPin, Plus, Search, Volume2, X } from 'lucide-react';
import { enumChoices } from './parameterFields';
import { uid } from '../studio/model';
import type { Definition } from '../studio/model';
import type { EditorObject, ModuleProps, ObjectKind, VariableRecord, WorkbenchProject } from './types';
import type { ParameterBinding } from './triggerCode';
import { bindingLabel, compatibleValueType, expectedFieldType, objectKindForType, presetValues, returnTypeOf, valueFunctionDefinitions } from './valueSources';
import { ObjectPortrait } from './ObjectPortrait';
import { findObject, pickableObjects } from './objectCatalog';
import './sharedValuePicker.css';

export type ValueSource = 'variable' | 'function' | 'constant';
export interface SharedValuePickerProps {
  project: WorkbenchProject;
  expectedType: string;
  objectKind?: ObjectKind;
  multiple?: boolean;
  allowedSources?: ValueSource[];
  value: unknown;
  binding?: ParameterBinding;
  /** created：本次选择中新建的全局变量，由调用方与绑定在同一次修改中写入工程。 */
  onSelect: (value: unknown, binding?: ParameterBinding, created?: VariableRecord[]) => void;
  onNavigate: ModuleProps['onNavigate'];
  onClose?: () => void;
  /** 调用方能原子写入 created 时才显示“新建变量”。 */
  canCreateVariable?: boolean;
}
type Field = Definition['fields'][number];
type FunctionArgs = NonNullable<ParameterBinding['args']>;
const sourceTitles: Record<ValueSource, string> = { variable: '全局变量', function: '函数', constant: '固定值' };
const numeric = (type: string) => ['integer', 'real', 'number'].includes(type);
const scalar = (type: string) => numeric(type) || ['string', 'text', 'boolean'].includes(type);
const rawText = (value: unknown) => typeof value === 'string' ? value : String(value ?? '');
const description = (object: EditorObject) => String(object.fields.tooltip ?? object.fields.description ?? '').replace(/\|c[0-9a-f]{8}/gi, '').replace(/\|r/g, '').replace(/\|n/g, '\n');

function ObjectDetails({ object, project, id }: { object: EditorObject; project: WorkbenchProject; id: string }) {
  const fields = object.kind === 'unit' ? [['health', '生命'], ['mana', '魔法'], ['attack', '攻击'], ['armor', '护甲'], ['speed', '移速'], ['cost', '费用']] : object.kind === 'ability' ? [['damage', '伤害'], ['mana', '魔法消耗'], ['cooldown', '冷却'], ['duration', '持续'], ['radius', '范围']] : [['cost', '费用'], ['charges', '次数'], ['health', '生命'], ['duration', '持续']];
  const fieldValue = (key: string) => object.fields[key] ?? object.levels[key]?.[0] ?? object.defaults[key] ?? object[key];
  return <div id={id} className="sv-object-details" role="tooltip" aria-label={`${object.name}详细信息`}><div className="sv-detail-title"><ObjectPortrait object={object} project={project} size={34}/><span><strong>{object.name}</strong><small>{object.rawcode} · {object.category}</small></span></div>{description(object) && <p>{description(object)}</p>}<dl>{fields.filter(([key]) => fieldValue(key) !== undefined).map(([key, label]) => <div key={key}><dt>{label}</dt><dd>{String(fieldValue(key))}</dd></div>)}</dl></div>;
}

export function SharedValuePicker({ project: sourceProject, expectedType, objectKind: requestedKind, multiple = false, allowedSources = ['variable', 'function', 'constant'], value, binding, onSelect: select, onNavigate, onClose, canCreateVariable = false }: SharedValuePickerProps) {
  // 函数参数中新建的变量先暂存，与最终的函数绑定一同提交；暂存变量在本面板内可见。
  const createdRef = useRef<VariableRecord[]>([]);
  const [draftVariables, setDraftVariables] = useState<VariableRecord[]>([]);
  const project = draftVariables.length ? { ...sourceProject, variables: [...sourceProject.variables, ...draftVariables] } : sourceProject;
  const onSelect = (next: unknown, nextBinding?: ParameterBinding, created: VariableRecord[] = []) => { const all = [...createdRef.current, ...created]; select(next, nextBinding, all.length ? all : undefined); };
  const [newVariable, setNewVariable] = useState('');
  const newVariableError = !newVariable.trim() ? '' : !/^[A-Za-z_][A-Za-z0-9_]*$/.test(newVariable.trim()) ? '名称只能包含字母、数字和下划线' : project.variables.some(item => item.name === newVariable.trim()) ? '变量名已存在' : '';
  // “globalvar[:类型]” 表示参数本身就是变量（赋值目标），只列变量，选择结果即变量本身。
  const isVariableTarget = expectedType.startsWith('globalvar');
  const targetType = isVariableTarget ? expectedType.split(':')[1] : undefined;
  const [newVariableType, setNewVariableType] = useState(targetType ?? 'integer');
  const variableType = isVariableTarget ? newVariableType : expectedType === 'text' ? 'string' : expectedType === 'number' ? 'real' : expectedType;
  const chooseVariable = (nextBinding: ParameterBinding, created?: VariableRecord[]) => onSelect(isVariableTarget ? nextBinding.value : value, nextBinding, created);
  const createVariable = () => {
    const name = newVariable.trim();
    if (!name || newVariableError) return;
    const record: VariableRecord = { id: `var.${uid()}`, name, type: variableType, array: false, size: 1, initial: '', folder: '触发器' };
    setNewVariable('');
    chooseVariable({ mode: 'variable', value: record.id }, [record]);
  };
  const sources = (['variable', 'function', 'constant'] as const).filter(source => allowedSources.includes(source) && (!isVariableTarget || source === 'variable'));
  const currentSource = binding?.mode === 'variable' ? 'variable' : binding?.mode === 'function' || binding?.mode === 'expression' ? 'function' : 'constant';
  const [source, setSource] = useState<ValueSource>(sources.includes(currentSource) ? currentSource : sources[0] ?? 'constant');
  const [search, setSearch] = useState('');
  const [rawValue, setRawValue] = useState(rawText(value));
  const [rawError, setRawError] = useState('');
  const [selectedVariable, setSelectedVariable] = useState(binding?.mode === 'variable' ? binding.value : '');
  const [arrayIndex, setArrayIndex] = useState(binding?.index ?? '0');
  const [selectedFunction, setSelectedFunction] = useState(binding?.mode === 'function' ? binding.value : '');
  const [functionArgs, setFunctionArgs] = useState<FunctionArgs>(binding?.mode === 'function' ? structuredClone(binding.args ?? {}) : {});
  const [expressionMode, setExpressionMode] = useState(binding?.mode === 'expression');
  const [expression, setExpression] = useState(binding?.mode === 'expression' ? binding.value : '');
  const [editingArgument, setEditingArgument] = useState<Field | null>(null);
  const [hoveredObject, setHoveredObject] = useState<string>();
  const [raceFilter, setRaceFilter] = useState('');
  const [categoryFilter, setCategoryFilter] = useState('');
  const functionDrafts = useRef<Record<string, FunctionArgs>>({});
  const detailsId = useId();
  const tabsId = useId();
  const objectKind = requestedKind ?? objectKindForType(expectedType);
  const normalizedSearch = search.trim().toLowerCase();
  const matching = (text: string) => text.toLowerCase().includes(normalizedSearch);
  const variables = project.variables.filter(variable => compatibleValueType(variable.type, expectedType) && matching(`${variable.name} ${variable.type}`));
  const variable = project.variables.find(item => item.id === selectedVariable || item.name === selectedVariable);
  const functions = valueFunctionDefinitions(project).filter(definition => { const type = returnTypeOf(definition); return !!type && compatibleValueType(type, expectedType); });
  const matchingFunctions = functions.filter(definition => matching(`${definition.title} ${definition.id} ${definition.namespace}`));
  const selectedDefinition = functions.find(definition => definition.id === selectedFunction);
  const category = (object: EditorObject) => typeof object.fields.abilityCategory === 'string' ? object.fields.abilityCategory : object.custom ? '自定义' : '标准';
  const availableObjects = objectKind ? pickableObjects(project, objectKind) : [];
  const races = [...new Set(availableObjects.map(object => object.race).filter(Boolean))];
  const categories = [...new Set(availableObjects.map(category))];
  const objects = availableObjects.filter(object => (!raceFilter || object.race === raceFilter) && (!categoryFilter || category(object) === categoryFilter) && matching(`${object.name} ${object.rawcode} ${object.race}`));
  const detailObject = availableObjects.find(object => object.id === hoveredObject);
  const selectedValues = multiple && Array.isArray(value) ? value : [value];
  const objectIsSelected = (object: EditorObject) => selectedValues.includes(object.id) || selectedValues.includes(object.rawcode);
  const chooseObject = (object: EditorObject) => {
    if (!multiple) { onSelect(object.id); return; }
    const next = Array.isArray(value) ? [...value] : value === undefined || value === '' ? [] : [value];
    onSelect(objectIsSelected(object) ? next.filter(item => item !== object.id && item !== object.rawcode) : [...next, object.id]);
  };
  const chooseRawcode = () => {
    const raw = rawValue.trim();
    const existing = findObject(project, raw, objectKind);
    if (existing) { if (existing.kind !== objectKind) { setRawError('物体类型不匹配'); return; } chooseObject(existing); return; }
    if (raw.length !== 4) { setRawError('rawcode 必须为 4 个字符'); return; }
    setRawError('');
    const selected = Array.isArray(value) ? value : [];
    onSelect(multiple ? selected.includes(raw) ? selected : [...selected, raw] : raw);
  };
  const setArgument = (field: Field, nextValue: unknown, nextBinding?: ParameterBinding) => { const next = { ...functionArgs, [field.id]: { value: nextValue, ...(nextBinding ? { binding: nextBinding } : {}) } }; setFunctionArgs(next); if (selectedFunction) functionDrafts.current[selectedFunction] = next; };
  const argumentValue = (field: Field) => functionArgs[field.id] ?? { value: field.default };
  const applyFunction = () => { if (selectedDefinition) onSelect(value, { mode: 'function', value: selectedDefinition.id, args: Object.fromEntries(selectedDefinition.fields.map(field => [field.id, argumentValue(field)])) }); };
  const invalidArguments = selectedDefinition?.fields.some(field => {
    const argument = argumentValue(field), type = expectedFieldType(field);
    if (argument.binding) return false;
    return numeric(type) && (rawText(argument.value).trim() === '' || !Number.isFinite(Number(argument.value)) || (type === 'integer' && !Number.isInteger(Number(argument.value))));
  });
  const chooseFunction = (definition: Definition) => {
    setSelectedFunction(definition.id); setExpressionMode(false); setSearch('');
    const args = Object.fromEntries(definition.fields.map(field => [field.id, { value: field.default }]));
    setFunctionArgs(functionDrafts.current[definition.id] ?? (definition.id === binding?.value && binding?.args ? structuredClone(binding.args) : args));
    if (!definition.fields.length) onSelect(value, { mode: 'function', value: definition.id, args: {} });
  };
  const applyConstant = () => {
    if (numeric(expectedType)) { if (rawValue.trim() && Number.isFinite(Number(rawValue)) && (expectedType !== 'integer' || Number.isInteger(Number(rawValue)))) onSelect(Number(rawValue)); }
    else onSelect(rawValue);
  };
  const functionArgumentLabel = (field: Field) => {
    const argument = argumentValue(field);
    if (argument.binding) return bindingLabel(argument.binding, project);
    return findObject(project, argument.value)?.name ?? project.world.regions.find(region => region.id === argument.value)?.name ?? rawText(argument.value);
  };

  if (editingArgument && selectedDefinition) {
    const argument = argumentValue(editingArgument);
    return <div className="sv-picker sv-nested"><div className="sv-breadcrumb"><button aria-label="返回函数参数" onClick={() => setEditingArgument(null)}><ArrowLeft size={13}/></button><span>{selectedDefinition.title}<ChevronRight size={11}/>{editingArgument.label}</span></div><SharedValuePicker project={project} expectedType={expectedFieldType(editingArgument)} objectKind={typeof editingArgument.objectKind === 'string' ? editingArgument.objectKind as ObjectKind : undefined} value={argument.value} binding={argument.binding} onNavigate={onNavigate} canCreateVariable={canCreateVariable} onSelect={(next, nextBinding, created) => { if (created?.length) { createdRef.current = [...createdRef.current, ...created]; setDraftVariables(list => [...list, ...created]); } setArgument(editingArgument, next, nextBinding); setEditingArgument(null); }}/></div>;
  }

  const searchBox = <label className="sv-search"><Search size={13}/><input autoFocus aria-label={source === 'variable' ? '搜索全局变量' : source === 'function' ? '搜索函数' : objectKind ? '搜索物体名称或 rawcode' : '搜索固定值'} placeholder={source === 'variable' ? '搜索变量名称' : source === 'function' ? '搜索函数名称' : objectKind ? '搜索名称或 rawcode' : '搜索名称'} value={search} onChange={event => setSearch(event.target.value)} onKeyDown={event => {
    if (event.key !== 'Enter') return;
    if (source === 'constant' && objectKind && objects.length === 1) chooseObject(objects[0]);
    if (source === 'variable' && variables.length === 1 && !variables[0].array) chooseVariable({ mode: 'variable', value: variables[0].id });
    if (source === 'function' && matchingFunctions.length === 1) chooseFunction(matchingFunctions[0]);
  }}/>{search && <button aria-label="清除搜索" onClick={() => setSearch('')}><X size={11}/></button>}</label>;

  return <div className="sv-picker" data-expected-type={expectedType} onMouseLeave={() => setHoveredObject(undefined)}>
    <div className="sv-source-tabs" role="tablist" aria-label="取值来源" onKeyDown={event => { if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key)) return; event.preventDefault(); const index = sources.indexOf(source); const next = event.key === 'Home' ? 0 : event.key === 'End' ? sources.length - 1 : (index + (event.key === 'ArrowRight' ? 1 : -1) + sources.length) % sources.length; setSource(sources[next]); setSearch(''); (event.currentTarget.querySelectorAll<HTMLButtonElement>('[role=tab]')[next])?.focus(); }}>{sources.map(item => <button key={item} role="tab" id={`${tabsId}-${item}`} aria-controls={`${tabsId}-panel`} tabIndex={source === item ? 0 : -1} aria-selected={source === item} className={source === item ? 'is-active' : ''} onClick={() => { setSource(item); setSearch(''); setHoveredObject(undefined); }}>{sourceTitles[item]}</button>)}</div>
    <div className="sv-source-panel" role="tabpanel" id={`${tabsId}-panel`} aria-labelledby={`${tabsId}-${source}`}>{source === 'variable' && <>{searchBox}<div className="sv-result-list">{variables.map(item => <button key={item.id} className={selectedVariable === item.id ? 'is-selected' : ''} aria-label={`选择变量 ${item.name}`} onClick={() => { setSelectedVariable(item.id); if (!item.array) chooseVariable({ mode: 'variable', value: item.id }); }}><Hash size={15}/><span>{item.name}<small>{item.type}{item.array ? '[]' : ''}</small></span>{selectedVariable === item.id && <Check size={12}/>}</button>)}{!variables.length && <div className="sv-empty">{search ? '没有匹配的全局变量' : '没有该类型的变量'}</div>}</div>{canCreateVariable && <div className="sv-create-variable"><input aria-label="新变量名称" placeholder={`新建 ${variableType} 变量`} value={newVariable} aria-invalid={!!newVariableError} title={newVariableError || undefined} onChange={event => setNewVariable(event.target.value)} onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); createVariable(); } }}/>{isVariableTarget && !targetType && <select aria-label="新变量类型" value={newVariableType} onChange={event => setNewVariableType(event.target.value)}>{['integer', 'real', 'boolean', 'string', 'unit', 'unitcode', 'player', 'group', 'location', 'rect'].map(type => <option key={type}>{type}</option>)}</select>}<button className="sv-apply" disabled={!newVariable.trim() || !!newVariableError} onClick={createVariable}><Plus size={12}/>新建并使用</button>{newVariableError && <small role="alert">{newVariableError}</small>}</div>}{variable?.array && <div className="sv-array-index"><label>下标<input aria-label="数组下标" value={arrayIndex} onChange={event => setArrayIndex(event.target.value)} onKeyDown={event => { if (event.key === 'Enter' && arrayIndex.trim()) chooseVariable({ mode: 'variable', value: variable.id, index: arrayIndex.trim() }); }}/></label><button className="sv-apply" disabled={!arrayIndex.trim()} onClick={() => chooseVariable({ mode: 'variable', value: variable.id, index: arrayIndex.trim() })}>应用<Check size={12}/></button></div>}<div className="sv-footer"><button onClick={() => { onClose?.(); onNavigate('variables'); }}><Hash size={12}/>管理变量<ArrowUpRight size={12}/></button><code>{expectedType}</code></div></>}
    {source === 'function' && <>{selectedDefinition && !expressionMode ? <div className="sv-function-editor"><div className="sv-function-title"><button aria-label="返回函数列表" onClick={() => setSelectedFunction('')}><ArrowLeft size={13}/></button><span><strong>{selectedDefinition.title}</strong><code>{selectedDefinition.id}</code></span><small>{returnTypeOf(selectedDefinition)}</small></div><div className="sv-function-arguments">{selectedDefinition.fields.map(field => { const argument = argumentValue(field), type = expectedFieldType(field); return <div key={field.id} className="sv-function-argument"><label>{field.label}<small>{type}</small></label>{!argument.binding && enumChoices(field).length > 0 ? <select aria-label={`函数参数 ${field.label}`} value={rawText(argument.value)} onChange={event => setArgument(field, event.target.value)}>{enumChoices(field).map(choice => <option key={choice.value} value={choice.value}>{choice.label}</option>)}</select> : !argument.binding && numeric(type) ? <input aria-label={`函数参数 ${field.label}`} inputMode="decimal" value={rawText(argument.value)} onChange={event => setArgument(field, event.target.value.trim() && Number.isFinite(Number(event.target.value)) ? Number(event.target.value) : event.target.value)} onKeyDown={event => { if (event.key === 'Enter' && !invalidArguments) applyFunction(); }}/> : !argument.binding && ['text', 'string'].includes(type) ? <input aria-label={`函数参数 ${field.label}`} value={rawText(argument.value)} onChange={event => setArgument(field, event.target.value)}/> : !argument.binding && type === 'boolean' ? <button role="switch" aria-label={`函数参数 ${field.label}`} aria-checked={!!argument.value} onClick={() => setArgument(field, !argument.value)}>{argument.value ? '真' : '假'}</button> : <button className="sv-argument-value" aria-label={`选择函数参数 ${field.label}`} onClick={() => setEditingArgument(field)}>{functionArgumentLabel(field)}<ChevronRight size={12}/></button>}<button className="sv-argument-source" aria-label={`${field.label}函数参数来源`} title="选择参数来源" onClick={() => setEditingArgument(field)}><Braces size={13}/></button></div>; })}</div><footer><code>{expectedType}</code><button className="sv-apply" disabled={invalidArguments} onClick={applyFunction}>应用函数<Check size={12}/></button></footer></div> : expressionMode ? <div className="sv-expression"><div className="sv-expression-header"><button aria-label="返回函数列表" onClick={() => { setExpressionMode(false); setSelectedFunction(''); }}><ArrowLeft size={13}/></button><strong>自定义表达式</strong></div><textarea autoFocus aria-label="自定义表达式" spellCheck={false} placeholder={numeric(expectedType) ? 'spawnCount * 2' : '表达式'} value={expression} onChange={event => setExpression(event.target.value)} onKeyDown={event => { if (event.key === 'Enter' && !event.shiftKey) { event.preventDefault(); if (expression.trim()) onSelect(value, { mode: 'expression', value: expression.trim() }); } }}/><footer><span>{expectedType} · Enter 应用</span><button className="sv-apply" disabled={!expression.trim()} onClick={() => onSelect(value, { mode: 'expression', value: expression.trim() })}>应用<Check size={12}/></button></footer></div> : <>{searchBox}<div className="sv-result-list sv-function-list">{matchingFunctions.map(definition => <button key={definition.id} aria-label={`选择函数 ${definition.title}`} onClick={() => chooseFunction(definition)}><Braces size={15}/><span>{definition.title}<small>{definition.id}</small></span><code>{returnTypeOf(definition)}</code><ChevronRight size={12}/></button>)}{!matchingFunctions.length && <div className="sv-empty">没有匹配的返回值函数</div>}</div><div className="sv-footer"><button onClick={() => { setExpressionMode(true); setSelectedFunction(''); }}><Code2 size={13}/>自定义表达式</button><code>{expectedType}</code></div></>}</>}
    {source === 'constant' && <>{objectKind ? <>{searchBox}<div className="sv-object-filters"><select aria-label="种族筛选" value={raceFilter} onChange={event => setRaceFilter(event.target.value)}><option value="">全部种族</option>{races.map(race => <option key={race}>{race}</option>)}</select><select aria-label="分类筛选" value={categoryFilter} onChange={event => setCategoryFilter(event.target.value)}><option value="">全部分类</option>{categories.map(item => <option key={item}>{item}</option>)}</select></div><div className="sv-object-grid">{objects.slice(0, 240).map(object => <button key={object.id} className={objectIsSelected(object) ? 'is-selected' : ''} aria-label={`选择${object.name} ${object.rawcode}`} aria-pressed={objectIsSelected(object)} onClick={() => chooseObject(object)} onMouseEnter={() => setHoveredObject(object.id)} aria-describedby={hoveredObject === object.id ? detailsId : undefined} onFocus={() => setHoveredObject(object.id)}><ObjectPortrait object={object} project={project} size={48}/><span>{object.name}</span><small>{object.rawcode}</small>{objectIsSelected(object) && <Check className="sv-grid-check" size={12}/>}</button>)}{objects.length > 240 && <div className="sv-empty">还有 {objects.length - 240} 个，请输入名称或 rawcode</div>}{!objects.length && <div className="sv-empty">没有匹配的物体</div>}</div>{detailObject && <ObjectDetails object={detailObject} project={project} id={detailsId}/>}<div className="sv-footer"><label className="sv-rawcode">{rawError || 'rawcode'}<input aria-label="自定义 rawcode" value={project.objects.find(object => object.id === rawValue)?.rawcode ?? rawValue} onChange={event => { setRawValue(event.target.value); setRawError(''); }} aria-invalid={!!rawError} title={rawError || undefined} onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); chooseRawcode(); } }}/></label><code>{expectedType}</code></div></> : expectedType === 'rect' || expectedType === 'location' ? <>{searchBox}<div className="sv-result-list">{project.world.regions.filter(region => matching(`${region.name} ${region.id}`)).map(region => <button key={region.id} className={value === region.id ? 'is-selected' : ''} aria-label={`选择区域 ${region.name}`} onClick={() => expectedType === 'location' ? onSelect(value, { mode: 'function', value: 'native.GetRectCenter', args: { rect: { value: region.id } } }) : onSelect(region.id)}><MapPin size={15}/><span>{region.name}<small>{region.id}</small></span>{value === region.id && <Check size={12}/>}</button>)}</div><div className="sv-footer sv-coordinate"><input aria-label="坐标或区域 ID" value={rawValue} onChange={event => setRawValue(event.target.value)} onKeyDown={event => { if (event.key === 'Enter') applyConstant(); }}/><button className="sv-apply" onClick={applyConstant}><Check size={13}/></button></div></> : ['unit', 'item', 'destructable'].includes(expectedType) ? <>{searchBox}<div className="sv-result-list"><button onClick={() => onSelect('null')} className={value === 'null' ? 'is-selected' : ''}><span>无<small>null</small></span></button>{project.world.instances.filter(instance => { const object = project.objects.find(item => item.id === instance.objectId); return object?.kind === expectedType && matching(`${object.name} ${instance.id}`); }).map(instance => { const object = project.objects.find(item => item.id === instance.objectId)!; return <button key={instance.id} className={value === instance.id ? 'is-selected' : ''} aria-label={`选择实例 ${object.name} ${instance.id}`} onClick={() => onSelect(instance.id)}><ObjectPortrait object={object} project={project} size={30}/><span>{object.name}<small>{instance.id} · 玩家 {instance.owner}</small></span></button>; })}</div></> : expectedType === 'trigger' ? <>{searchBox}<div className="sv-result-list">{project.graphs.filter(graph => matching(`${graph.name} ${graph.folder}`)).map(graph => <button key={graph.id} className={value === graph.id ? 'is-selected' : ''} aria-label={`选择触发器 ${graph.name}`} onClick={() => onSelect(graph.id)}><GitBranch size={15}/><span>{graph.name}<small>{graph.folder}</small></span>{value === graph.id && <Check size={12}/>}</button>)}</div></> : expectedType === 'sound' ? <>{searchBox}<div className="sv-result-list">{project.sounds.filter(sound => matching(`${sound.name} ${sound.id}`)).map(sound => <button key={sound.id} className={value === sound.id ? 'is-selected' : ''} onClick={() => onSelect(sound.id)}><Volume2 size={15}/><span>{sound.name}<small>{sound.spatial ? '3D 声音' : '2D 声音'}</small></span></button>)}</div></> : expectedType === 'boolean' ? <div className="sv-result-list sv-boolean-values">{[true, false].map(next => <button key={String(next)} className={value === next ? 'is-selected' : ''} onClick={() => onSelect(next)}><span>{next ? '真 / True' : '假 / False'}</span>{value === next && <Check size={12}/>}</button>)}</div> : <>{presetValues(expectedType).length > 0 && <div className="sv-result-list sv-presets" aria-label="预设">{presetValues(expectedType).map(preset => <button key={String(preset.value)} className={String(value) === String(preset.value) ? 'is-selected' : ''} aria-label={`选择预设 ${preset.label}`} onClick={() => onSelect(preset.value)}><span>{preset.label}<small>{String(preset.value)}</small></span>{String(value) === String(preset.value) && <Check size={12}/>}</button>)}</div>}<div className="sv-constant-editor"><label>{scalar(expectedType) ? '值' : '原始值'}<input autoFocus aria-label="固定值" inputMode={numeric(expectedType) ? 'decimal' : undefined} value={rawValue} onChange={event => setRawValue(event.target.value)} onKeyDown={event => { if (event.key === 'Enter') { event.preventDefault(); applyConstant(); } }}/></label><footer><code>{expectedType}</code><button className="sv-apply" disabled={numeric(expectedType) && (!rawValue.trim() || !Number.isFinite(Number(rawValue)) || (expectedType === 'integer' && !Number.isInteger(Number(rawValue))))} onClick={applyConstant}>应用<Check size={12}/></button></footer></div></>}</>}
    </div>
  </div>;
}
