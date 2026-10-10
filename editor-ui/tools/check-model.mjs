import assert from 'node:assert/strict';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import ts from 'typescript';
const out = new URL('../output/check-model/', import.meta.url);
await mkdir(out, { recursive: true });
const source = await readFile(new URL('../src/studio/model.ts', import.meta.url), 'utf8');
await writeFile(new URL('model.mjs', out), ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022 } }).outputText);
const m = await import(new URL('model.mjs', out));
const receipts = [];
function test(name, action) { action(); receipts.push({ name, passed: true }); }
test('示例工程结构有效', () => assert.equal(m.validateProject(m.sampleProject()).length, 0));
test('9857 / 9527 定义可创建编辑并完整往返', () => {
    const p = m.blankProject('第二个独立工程');
    const defs = m.parseDefinitions(['9857', '9527'].map(id => ({ ...m.builtins[5], id: `extension.${id}`, namespace: id, futureField: { opaque: 123 } })));
    p.definitions.push(...defs);
    p.graphs[0].nodes = defs.map(d => m.makeNode(d, 0, 0));
    p.graphs[0].nodes[0].values.message = '已编辑';
    p.futureProject = { hello: '原始扩展数据' };
    const back = m.parseProject(JSON.parse(JSON.stringify(p)));
    assert.deepEqual(back, p); assert.equal(m.validateProject(back).length, 0);
});
test('未知节点与未知字段保留', () => {
    const p = m.blankProject(); p.graphs[0].nodes.push({ id: 'unknown', definition: 'external.missing', x: 1, y: 2, values: { payload: [1, '字节', { x: true }] }, opaque: 'original' });
    assert.deepEqual(m.parseProject(JSON.parse(JSON.stringify(p))), p);
    assert.equal(m.validateProject(p)[0].level, 'warning');
});
test('错误类型 / 自连 / 错误方向拒绝且不修改图', () => {
    const p = m.sampleProject(), g = p.graphs[0], before = JSON.stringify(g);
    assert.throws(() => m.connectPorts(g, p.definitions, 'node-4', 'value', 'node-1', 'in'));
    assert.throws(() => m.connectPorts(g, p.definitions, 'node-1', 'yes', 'node-1', 'in'));
    assert.throws(() => m.connectPorts(g, p.definitions, 'node-0', 'tick', 'node-1', 'yes'));
    assert.equal(JSON.stringify(g), before);
});
test('同一输入替换连线，重复连线不增加', () => {
    const p = m.sampleProject(), g = p.graphs[0];
    const changed = m.connectPorts(g, p.definitions, 'node-0', 'tick', 'node-5', 'in');
    assert.equal(changed.edges.length, g.edges.length);
    assert.equal(changed.edges.find(e => e.to === 'node-5').from, 'node-0');
    assert.deepEqual(m.connectPorts(changed, p.definitions, 'node-0', 'tick', 'node-5', 'in'), changed);
});
test('删除节点清理关联边，原快照保持不变', () => {
    const g = m.sampleProject().graphs[0], next = m.removeNodes(g, ['node-2']);
    assert.equal(next.nodes.length, 5); assert.ok(next.edges.every(e => e.to !== 'node-2' && e.from !== 'node-2')); assert.equal(g.nodes.length, 6);
});
test('重复定义、节点 ID 和非有限坐标拒绝', () => {
    assert.throws(() => m.parseDefinitions([m.builtins[0], m.builtins[0]]));
    const p = m.sampleProject(); p.graphs[0].nodes.push(p.graphs[0].nodes[0]); assert.throws(() => m.parseProject(p));
    const q = m.sampleProject(); q.graphs[0].nodes[0].x = Infinity; assert.throws(() => m.parseProject(q));
});
await writeFile(new URL('results.json', out), JSON.stringify(receipts, null, 2));
console.log(JSON.stringify({ passed: receipts.length, total: receipts.length, tests: receipts }, null, 2));
