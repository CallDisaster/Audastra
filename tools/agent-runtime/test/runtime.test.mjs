import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { once } from 'node:events';
const { runHarness, runManualCompaction, compactHistory, redact, providerToolNames, inlineSchemaRefs } = await import(process.env.FORGE_RUNTIME_TEST_ENTRY || '../worker.mjs');
const { pruneLargeToolResults, compactWithWarmCache, estimateTextTokens } = await import('../compaction.mjs');

function chunk(response, choices, usage) {
  response.write(`data: ${JSON.stringify({ id: 'forge-fixture', object: 'chat.completion.chunk',
    created: 1, model: 'fixture', choices, ...(usage ? { usage } : {}) })}\n\n`);
}

test('真实 PI 流式工具回合，串行调用、回读与密钥隔离', async () => {
  const bodies = [];
  const server = createServer(async (request, response) => {
    let source = '';
    for await (const part of request) source += part;
    const body = JSON.parse(source);
    bodies.push(body);
    assert.equal(request.headers.authorization, 'Bearer fixture-secret-123');
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    if (bodies.length === 1) {
      chunk(response, [{ index: 0, delta: { role: 'assistant', tool_calls: [{ index: 0,
        id: 'fixture-call-1', type: 'function', function: { name: 'editor_state', arguments: '{}' } }] }, finish_reason: null }]);
      chunk(response, [{ index: 0, delta: {}, finish_reason: 'tool_calls' }]);
    } else {
      assert(body.messages.some(message => message.role === 'tool' && message.content.includes('revision-7')));
      chunk(response, [{ index: 0, delta: { role: 'assistant', content: '已回读当前地图。' }, finish_reason: null }]);
      chunk(response, [{ index: 0, delta: {}, finish_reason: 'stop' }]);
    }
    chunk(response, [], { prompt_tokens: 42, completion_tokens: 7, total_tokens: 49 });
    response.end('data: [DONE]\n\n');
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const events = [];
  let invoked = 0;
  try {
    await runHarness({ provider: { id: 'fixture-provider', name: '本机测试', protocol: 'openai-chat',
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: 'fixture-secret-123' },
      model: { id: 'fixture', contextWindow: 262144 }, prompt: '读取当前地图身份。',
      systemPrompt: '使用工具取得当前地图，不修改内容。',
      tools: [{ name: 'editor_state', description: '当前地图身份', inputSchema: {
        type: 'object', properties: {}, additionalProperties: false } }] },
    event => events.push(event), async call => {
      ++invoked;
      assert.equal(call.name, 'editor_state');
      return { revision: 'revision-7', map: '独立测试地图' };
    });
    assert.equal(invoked, 1);
    assert.equal(bodies.length, 2);
    assert(events.some(event => event.type === 'assistant_delta' && event.text.includes('已回读')));
    assert(events.some(event => event.type === 'tool_start'));
    assert(events.some(event => event.type === 'tool_end' && event.success));
    assert(events.some(event => event.type === 'usage' && event.available === true
      && event.input_tokens === 42 && event.output_tokens === 7 && event.cost_available === false
      && !Object.hasOwn(event.details, 'cost')));
    assert(events.some(event => event.type === 'run_end' && event.success));
    assert(events.some(event => event.type === 'usage' && event.cost_available === false));
    assert(events.filter(event => event.type === 'usage').every(event => !Object.hasOwn(event.details, 'cost')));
    assert(!JSON.stringify(events).includes('fixture-secret-123'));
  } finally {
    server.closeAllConnections();
    server.close();
  }
});

test('含点号的编辑器工具名按站点规则改名，调用与事件仍用原名', async () => {
  const pattern = /^[a-zA-Z0-9_-]{1,64}$/;
  const names = providerToolNames(['code.check', 'code_check', 'map.query', '工具.中文']);
  assert.deepEqual([...names.values()], ['code_check', 'code_check_2', 'map_query', '_____']);
  assert([...names.values()].every(name => pattern.test(name)));
  const bodies = [];
  const server = createServer(async (request, response) => {
    let source = '';
    for await (const part of request) source += part;
    const body = JSON.parse(source);
    bodies.push(body);
    // 与 DeepSeek 网关一致：任一工具名不合规即 400。
    if (!body.tools.every(tool => pattern.test(tool.function.name))
      || body.messages.some(m => (m.tool_calls ?? []).some(call => !pattern.test(call.function.name)))) {
      response.writeHead(400, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ error: { message: "Invalid 'tools[0].function.name'" } }));
      return;
    }
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    if (bodies.length === 1) {
      chunk(response, [{ index: 0, delta: { role: 'assistant', tool_calls: [{ index: 0,
        id: 'fixture-call-1', type: 'function', function: { name: 'code_check', arguments: '{}' } }] }, finish_reason: null }]);
      chunk(response, [{ index: 0, delta: {}, finish_reason: 'tool_calls' }]);
    } else {
      chunk(response, [{ index: 0, delta: { role: 'assistant', content: '检查完成。' }, finish_reason: null }]);
      chunk(response, [{ index: 0, delta: {}, finish_reason: 'stop' }]);
    }
    response.end('data: [DONE]\n\n');
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const events = [], calls = [];
  try {
    await runHarness({ provider: { id: 'strict-provider', name: '严格工具名站点', protocol: 'openai-chat',
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: 'strict-key' },
      model: { id: 'fixture', contextWindow: 262144 }, prompt: '检查代码。', systemPrompt: '使用工具。',
      // 历史里旧的点号工具名同样要换成合规名。
      messages: [{ role: 'user', content: [{ type: 'text', text: '旧回合' }], timestamp: 1 },
        { role: 'assistant', content: [{ type: 'toolCall', id: 'old-1', name: 'code.check', arguments: {} }],
          api: 'openai-completions', provider: 'strict-provider', model: 'fixture', stopReason: 'toolUse', timestamp: 2,
          usage: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
            cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } } },
        { role: 'toolResult', toolCallId: 'old-1', toolName: 'code.check', content: [{ type: 'text', text: '{}' }],
          isError: false, timestamp: 3 }],
      tools: [{ name: 'code.check', description: '检查代码', inputSchema: { type: 'object', properties: {} } }] },
    event => events.push(event), async call => { calls.push(call.name); return { ok: true }; });
    assert.deepEqual(calls, ['code.check']);
    assert.equal(bodies.length, 2);
    assert.equal(bodies[0].tools[0].function.name, 'code_check');
    assert(events.some(event => event.type === 'tool_start' && event.tool_name === 'code.check'));
    assert(events.some(event => event.type === 'tool_end' && event.tool_name === 'code.check' && event.success));
    assert(events.some(event => event.type === 'run_end' && event.success));
  } finally {
    server.closeAllConnections();
    server.close();
  }
});

test('压缩只裁完整回合，保留最近工具请求及结果', () => {
  const history = [{ role: 'user', content: '旧任务' },
    { role: 'assistant', content: 'x'.repeat(12000) },
    { role: 'user', content: '当前任务' },
    { role: 'assistant', content: [{ type: 'toolCall', id: 'call-current', name: 'editor_state', arguments: {} }] },
    { role: 'toolResult', toolCallId: 'call-current', toolName: 'editor_state', content: [{ type: 'text', text: 'current-id' }] }];
  const compacted = compactHistory(history, 4096);
  assert(compacted.compacted);
  assert.equal(compacted.removedMessages, 2);
  assert(compacted.messages.some(message => message.role === 'toolResult' && message.toolCallId === 'call-current'));
  assert.equal(history[1].content.length, 12000);
  assert.throws(() => compactHistory([{ role: 'user', content: 'x'.repeat(15000) }], 4096), /单个回合/);
});

test('站点没有usage时不把PI全零占位当实际用量', async () => {
  const server = createServer((_request, response) => {
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    chunk(response, [{ index: 0, delta: { role: 'assistant', content: '没有用量元数据。' }, finish_reason: null }]);
    chunk(response, [{ index: 0, delta: {}, finish_reason: 'stop' }]);
    response.end('data: [DONE]\n\n');
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const events = [];
  try {
    await runHarness({ provider: { id: 'fixture', protocol: 'openai-chat',
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: 'test-only' },
      model: { id: 'fixture' }, prompt: '只读测试', systemPrompt: '读取', tools: [] },
    event => events.push(event), () => {});
    const usage = events.filter(event => event.type === 'usage');
    assert.equal(usage.length, 1);
    assert.equal(usage[0].available, false);
    assert.equal(usage[0].input_tokens, null);
    assert.equal(usage[0].output_tokens, null);
    assert.equal(usage[0].cost_available, false);
    assert(events.some(event => event.type === 'run_end' && event.success));
  } finally {
    server.closeAllConnections();
    server.close();
  }
});

test('敏感错误被隐藏，未知协议不会进行网络请求', async () => {
  assert.equal(redact('auth abc123 failed', ['abc123']), 'auth [已隐藏密钥] failed');
  await assert.rejects(runHarness({ provider: { protocol: 'unsupported' } }, () => {}, () => {}), /未支持/);
});

test('停止流式请求保留已有事件并正常收口', async () => {
  const server = createServer((_request, response) => {
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    chunk(response, [{ index: 0, delta: { role: 'assistant', content: '进行中' }, finish_reason: null }]);
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const aborter = new AbortController();
  const events = [];
  const timeout = setTimeout(() => aborter.abort(), 1000);
  try {
    await runHarness({ provider: { id: 'fixture', protocol: 'openai-chat',
      baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: 'test-only' },
      model: { id: 'fixture' }, prompt: '停止测试', systemPrompt: '读取', tools: [] },
    event => { events.push(event); if (event.type === 'assistant_delta') aborter.abort(); }, () => {}, aborter.signal);
    assert(events.some(event => event.type === 'run_end' && !event.success));
  } finally {
    clearTimeout(timeout);
    server.closeAllConnections();
    server.close();
  }
});

// 与编辑器触发器图工具同形：定义在根 $defs，eca 与 argument 互相递归。
const recursiveSchema = {
  type: 'object', additionalProperties: false, required: ['plan'],
  properties: {
    plan: { type: 'object', properties: { root: { $ref: '#/$defs/eca' }, value: { $ref: '#/$defs/argument' } } },
    $defs: { type: 'string', description: '名为 $defs 的普通字段' },
  },
  $defs: {
    eca: { type: 'object', properties: { function: { type: 'string' },
      arguments: { type: 'array', items: { $ref: '#/$defs/argument' } }, children: { type: 'array', items: { $ref: '#/$defs/eca' } } } },
    argument: { type: 'object', properties: { scalar: { type: 'string' }, call: { $ref: '#/$defs/eca' }, index: { $ref: '#/$defs/argument' } } },
  },
};

test('Gemini 参数 schema 展开引用并打断递归', () => {
  const inlined = inlineSchemaRefs(recursiveSchema);
  const text = JSON.stringify(inlined);
  assert(!text.includes('"$ref"'));
  assert(!Object.hasOwn(inlined, '$defs'));
  assert.equal(inlined.properties.$defs.type, 'string');
  const root = inlined.properties.plan.properties.root;
  assert.equal(root.properties.function.type, 'string');
  const argument = root.properties.arguments.items;
  assert.equal(argument.properties.scalar.type, 'string');
  assert.equal(argument.properties.call.type, 'object');
  assert(!Object.hasOwn(argument.properties.call, 'properties'));
  assert.equal(root.properties.children.items.type, 'object');
  assert(!Object.hasOwn(root.properties.children.items, 'properties'));
  assert.equal(inlined.properties.plan.properties.value.properties.call.properties.function.type, 'string');
  assert.deepEqual(inlineSchemaRefs({ type: 'object', properties: { x: { $ref: '#/$defs/missing' } } }).properties.x.type, 'object');
  assert(JSON.stringify(recursiveSchema).includes('"$ref"'));
});

test('发给 Gemini 的工具声明不含 $ref 与 $defs', async () => {
  const bodies = [];
  const server = createServer(async (request, response) => {
    let source = '';
    for await (const part of request) source += part;
    bodies.push({ url: request.url, body: JSON.parse(source) });
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    response.end(`data: ${JSON.stringify({ candidates: [{ content: { role: 'model', parts: [{ text: '已收到。' }] },
      finishReason: 'STOP', index: 0 }], usageMetadata: { promptTokenCount: 5, candidatesTokenCount: 2, totalTokenCount: 7 } })}

`);
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const events = [];
  try {
    await runHarness({ provider: { id: 'gemini-fixture', name: '本机测试', protocol: 'gemini',
      baseUrl: `http://127.0.0.1:${server.address().port}`, apiKey: 'fixture-gemini-key' },
      model: { id: 'fixture-gemini', contextWindow: 262144 }, prompt: '看看当前地图。', systemPrompt: '只读。',
      tools: [{ name: 'editor_query_trigger_graph', description: '查询触发器', inputSchema: recursiveSchema }] },
    event => events.push(event), async () => ({}));
    assert.equal(bodies.length, 1);
    const declarations = bodies[0].body.tools.flatMap(tool => tool.functionDeclarations ?? []);
    assert.equal(declarations.length, 1);
    const parameters = declarations[0].parametersJsonSchema ?? declarations[0].parameters;
    assert(parameters);
    assert(!JSON.stringify(parameters).includes('"$ref"'));
    assert(!Object.hasOwn(parameters, '$defs'));
    assert(events.some(event => event.type === 'run_end' && event.success));
    assert(!JSON.stringify(events).includes('fixture-gemini-key'));
  } finally {
    server.closeAllConnections();
    server.close();
  }
});


test('DeepSeek 缓存命中、未缓存输入与思考链流式输出及截断续跑', async () => {
  const bodies = [];
  const server = createServer(async (request, response) => {
    let source = '';
    for await (const part of request) source += part;
    const body = JSON.parse(source);
    bodies.push(body);
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    if (bodies.length === 1) {
      // 第一轮：在思考链中触碰 length 上限，尚未发出工具调用（复现“规划完触发器直接结束”场景）。
      chunk(response, [{ index: 0, delta: { role: 'assistant', reasoning_content: '正在规划触发器事件与动作结构…' }, finish_reason: null }]);
      chunk(response, [{ index: 0, delta: {}, finish_reason: 'length' }],
        { prompt_tokens: 1200, prompt_tokens_details: { cached_tokens: 900 }, completion_tokens: 300, total_tokens: 1500 });
    } else if (bodies.length === 2) {
      // 自动续跑第二轮：发出工具调用。同时核验上一轮无文本消息 content 已归一化为 "" 而非 null。
      assert(body.messages.filter(m => m.role === 'assistant').every(m => m.content !== null));
      chunk(response, [{ index: 0, delta: { role: 'assistant', reasoning_content: '分批创建触发器', tool_calls: [{
        index: 0, id: 'trig-call-1', type: 'function',
        function: { name: 'editor_edit_trigger_graph', arguments: '{"op":"create"}' } }] }, finish_reason: null }]);
      chunk(response, [{ index: 0, delta: {}, finish_reason: 'tool_calls' }],
        { prompt_tokens: 1550, prompt_tokens_details: { cached_tokens: 1200 }, completion_tokens: 80, total_tokens: 1630 });
    } else {
      // 第三轮：核验带 tool_calls 的历史消息回传了 reasoning_content 且 content 不为 null。
      const priorToolAssistant = body.messages.find(m => m.role === 'assistant' && Array.isArray(m.tool_calls));
      assert(priorToolAssistant);
      assert.equal(priorToolAssistant.reasoning_content, '分批创建触发器');
      assert.equal(priorToolAssistant.content, '');
      chunk(response, [{ index: 0, delta: { role: 'assistant', content: '触发器已分批创建完成。' }, finish_reason: null }]);
      chunk(response, [{ index: 0, delta: {}, finish_reason: 'stop' }],
        { prompt_tokens: 1700, prompt_tokens_details: { cached_tokens: 1500 }, completion_tokens: 25, total_tokens: 1725 });
    }
    response.end('data: [DONE]\n\n');
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const events = [];
  let editCalls = 0;
  try {
    await runHarness({
      sessionId: 'session-deepseek-cache-test',
      provider: { id: 'deepseek', name: 'DeepSeek', protocol: 'openai-chat',
        baseUrl: `http://127.0.0.1:${server.address().port}/v1`, apiKey: 'ds-key-test' },
      model: { id: 'deepseek-reasoner', contextWindow: 128000 },
      prompt: '查询习惯后创建装备合成触发器。',
      systemPrompt: '默认使用自然语言触发器 T。',
      tools: [{ name: 'editor_edit_trigger_graph', description: '编辑触发器', inputSchema: { type: 'object', properties: { op: { type: 'string' } } } }],
    }, event => events.push(event), async call => {
      assert.equal(call.name, 'editor_edit_trigger_graph');
      ++editCalls;
      return { ok: true, triggerId: 'trg-equip-1' };
    });
    assert.equal(editCalls, 1);
    assert.equal(bodies.length, 3);
    assert(events.some(e => e.type === 'thinking_delta' && e.text.includes('正在规划触发器')));
    assert(events.some(e => e.type === 'turn_checkpoint' && Array.isArray(e.messages)));
    const usages = events.filter(e => e.type === 'usage' && e.available);
    assert.equal(usages.length, 3);
    // 最后一轮：总输入 1700 = 缓存命中 1500 + 未缓存 200；输出 25；总上下文 1725。
    const lastUsage = usages[usages.length - 1];
    assert.equal(lastUsage.cache_read_tokens, 1500);
    assert.equal(lastUsage.uncached_input_tokens, 200);
    assert.equal(lastUsage.input_tokens, 1700);
    assert.equal(lastUsage.output_tokens, 25);
    assert.equal(lastUsage.context_tokens, 1725);
    assert(events.some(e => e.type === 'run_end' && e.success));
  } finally {
    server.closeAllConnections();
    server.close();
  }
});

test('DSH 三级压缩：第1级修剪超大工具结果，第2级热前缀摘要完整保留用户长串要求并还原工具名', async () => {
  assert(estimateTextTokens('这是一段中文触发器需求描述') > 5);
  const bigToolMessages = [
    { role: 'user', content: [{ type: 'text', text: '长串原始需求：所有英雄升级增加全属性5点，击杀精英掉落玄铁。' }], timestamp: 1 },
    { role: 'assistant', stopReason: 'toolUse', timestamp: 2,
      content: [{ type: 'toolCall', id: 'tc-1', name: 'code_check', arguments: {} }] },
    { role: 'toolResult', toolCallId: 'tc-1', toolName: 'code_check', timestamp: 3,
      content: [{ type: 'text', text: 'A'.repeat(14000) }] },
    { role: 'user', content: [{ type: 'text', text: '继续执行下一步' }], timestamp: 4 },
  ];
  // 第 1 级：修剪超大工具返回后若低于阈值，无需发起摘要请求。
  let llmCalled = 0;
  const tier1 = await compactWithWarmCache({
    messages: bigToolMessages,
    contextWindow: 4500,
    thresholdRatio: 0.80,
    summarizeFn: async () => { ++llmCalled; return '<compacted-summary>不应调用</compacted-summary>'; },
  });
  assert.equal(tier1.pruned, true);
  assert.equal(tier1.compacted, false);
  assert.equal(llmCalled, 0);
  assert.equal(bigToolMessages[2].content[0].text.length, 14000);

  // 第 2 级 + 手动压缩：调用热前缀摘要并保留原始需求，且工具名还原为含点号原名。
  const summaryServer = createServer(async (_req, res) => {
    res.writeHead(200, { 'Content-Type': 'text/event-stream' });
    chunk(res, [{ index: 0, delta: { role: 'assistant',
      content: '<compacted-summary>\n## 原始需求与核心约束\n- 所有英雄升级增加全属性5点，击杀精英掉落玄铁。\n## 待办清单与下一步行动\n- 创建升级与掉落触发器。\n</compacted-summary>' }, finish_reason: null }]);
    chunk(res, [{ index: 0, delta: {}, finish_reason: 'stop' }]);
    res.end('data: [DONE]\n\n');
  });
  summaryServer.listen(0, '127.0.0.1');
  await once(summaryServer, 'listening');
  const compactEvents = [];
  try {
    const compactedMessages = await runManualCompaction({
      provider: { id: 'fixture', name: '测试', protocol: 'openai-chat',
        baseUrl: `http://127.0.0.1:${summaryServer.address().port}/v1`, apiKey: 'secret-xyz' },
      model: { id: 'fixture', contextWindow: 8192 },
      systemPrompt: '系统提示',
      tools: [{ name: 'code.check', description: '检查', inputSchema: { type: 'object', properties: {} } }],
      messages: [
        { role: 'user', content: [{ type: 'text', text: '长串原始需求：所有英雄升级增加全属性5点，击杀精英掉落玄铁。' }], timestamp: 1 },
        { role: 'assistant', stopReason: 'toolUse', timestamp: 2,
          content: [{ type: 'toolCall', id: 'tc-1', name: 'code.check', arguments: {} }] },
        { role: 'toolResult', toolCallId: 'tc-1', toolName: 'code.check', timestamp: 3,
          content: [{ type: 'text', text: '{"ok":true}' }] },
        { role: 'user', content: [{ type: 'text', text: '整理会话后继续做完' }], timestamp: 4 },
        { role: 'assistant', stopReason: 'toolUse', timestamp: 5,
          content: [{ type: 'toolCall', id: 'tc-2', name: 'code.check', arguments: {} }] },
        { role: 'toolResult', toolCallId: 'tc-2', toolName: 'code.check', timestamp: 6,
          content: [{ type: 'text', text: '{"ok":true}' }] },
      ],
    }, ev => compactEvents.push(ev));
    assert(compactEvents.some(e => e.type === 'context_compacted' && e.summary.includes('全属性5点')));
    assert(compactEvents.some(e => e.type === 'compact_end' && e.success && e.compacted));
    // 保留尾部中的工具名已还原为原编辑器名 code.check（而非合规替换名 code_check）。
    assert(compactedMessages.some(m => m.role === 'toolResult' && m.toolName === 'code.check'));
  } finally {
    summaryServer.closeAllConnections();
    summaryServer.close();
  }
});
