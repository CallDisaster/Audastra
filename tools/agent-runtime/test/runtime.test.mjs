import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { once } from 'node:events';
const { runHarness, runManualCompaction, redact, providerToolNames, inlineSchemaRefs, anthropicThinkingProfile, parseMaxTokensRangeError }
  = await import(process.env.FORGE_RUNTIME_TEST_ENTRY || '../worker.mjs');
const { pruneLargeToolResults, compactWithWarmCache, estimateTextTokens, buildDeterministicSummaryBody, createCompactedSummaryMessage }
  = await import('../compaction.mjs');

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

test('压缩只裁完整回合，保留最近工具请求及结果', async () => {
  const history = [{ role: 'user', content: '旧任务' },
    { role: 'assistant', content: 'x'.repeat(12000) },
    { role: 'user', content: '当前任务' },
    { role: 'assistant', content: [{ type: 'toolCall', id: 'call-current', name: 'editor_state', arguments: {} }] },
    { role: 'toolResult', toolCallId: 'call-current', toolName: 'editor_state', content: [{ type: 'text', text: 'current-id' }] }];
  const compacted = await compactWithWarmCache({ messages: history, contextWindow: 4096 });
  assert(compacted.compacted);
  assert.equal(compacted.removedMessages, 2);
  assert(compacted.messages.some(message => message.role === 'toolResult' && message.toolCallId === 'call-current'));
  assert.equal(history[1].content.length, 12000);
  // 单个超长回合无法在完整回合边界内缩小：如实报告未压缩，原消息不变。
  const single = [{ role: 'user', content: 'x'.repeat(15000) }];
  const unchanged = await compactWithWarmCache({ messages: single, contextWindow: 4096 });
  assert.equal(unchanged.compacted, false);
  assert.deepEqual(unchanged.messages, single);
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

test('思考强度对齐 DSH：DeepSeek off/max 线协议与自定义模型档位及旧网关 400 自动回退', async () => {
  const bodies = [];
  let rejectEffortOnce = false;
  const server = createServer(async (req, res) => {
    let raw = '';
    for await (const part of req) raw += part;
    const body = JSON.parse(raw);
    bodies.push(body);
    if (rejectEffortOnce && body.reasoning_effort) {
      rejectEffortOnce = false;
      res.writeHead(400, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ error: { message: '400 invalid parameter: unknown field reasoning_effort' } }));
      return;
    }
    res.writeHead(200, { 'Content-Type': 'text/event-stream' });
    chunk(res, [{ index: 0, delta: { role: 'assistant', content: 'ok' }, finish_reason: null }]);
    chunk(res, [{ index: 0, delta: {}, finish_reason: 'stop' }]);
    res.end('data: [DONE]\n\n');
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const baseUrl = `http://127.0.0.1:${server.address().port}/v1`;
  try {
    // 1) DeepSeek V4.1 Flash Ultrafast 选 max：发送 thinking: enabled + reasoning_effort: max
    await runHarness({
      provider: { id: 'p1', name: 'DeepSeek', protocol: 'openai-chat', baseUrl, apiKey: 'k1' },
      model: { id: 'custom-ds-id', name: 'DeepSeek V4.1 Flash Ultrafast', contextWindow: 1000000,
        supportedThinkingLevels: ['off', 'low', 'high', 'max'], thinkingEffort: 'max' },
      systemPrompt: 'sys', prompt: 'hi', tools: [],
    }, () => {}, async () => ({}));
    assert.deepEqual(bodies.at(-1).thinking, { type: 'enabled' });
    assert.equal(bodies.at(-1).reasoning_effort, 'max');

    // 2) DeepSeek 选 off：发送 thinking: disabled 且不发 reasoning_effort
    await runHarness({
      provider: { id: 'p1', name: 'DeepSeek', protocol: 'openai-chat', baseUrl, apiKey: 'k1' },
      model: { id: 'custom-ds-id', name: 'DeepSeek V4.1 Flash Ultrafast', contextWindow: 1000000,
        supportedThinkingLevels: ['off', 'low', 'high', 'max'], thinkingEffort: 'off' },
      systemPrompt: 'sys', prompt: 'hi', tools: [],
    }, () => {}, async () => ({}));
    assert.deepEqual(bodies.at(-1).thinking, { type: 'disabled' });
    assert.equal(bodies.at(-1).reasoning_effort, undefined);

    // 3) 未知网关模型由用户手动指定支持档位 ['off', 'low', 'xhigh'] 并选 xhigh
    await runHarness({
      provider: { id: 'p2', name: 'Custom', protocol: 'openai-chat', baseUrl, apiKey: 'k1' },
      model: { id: 'my-private-model', name: 'My Private Model', contextWindow: 128000,
        thinkingCustomized: true, supportedThinkingLevels: ['off', 'low', 'xhigh'], thinkingEffort: 'xhigh' },
      systemPrompt: 'sys', prompt: 'hi', tools: [],
    }, () => {}, async () => ({}));
    assert.equal(bodies.at(-1).reasoning_effort, 'xhigh');

    // 4) 旧版 V3.2 网关不接受 reasoning_effort 返回 400 时，自动剥离 reasoning_effort 保留 thinking 重试成功
    rejectEffortOnce = true;
    const retryEvents = [];
    await runHarness({
      provider: { id: 'p1', name: 'DeepSeek', protocol: 'openai-chat', baseUrl, apiKey: 'k1' },
      model: { id: 'deepseek-reasoner', name: 'DeepSeek Reasoner', contextWindow: 128000,
        supportedThinkingLevels: ['off', 'low', 'high', 'max'], thinkingEffort: 'high' },
      systemPrompt: 'sys', prompt: 'hi', tools: [],
    }, ev => retryEvents.push(ev), async () => ({}));
    assert(retryEvents.some(e => e.type === 'run_end' && e.success));
    assert.deepEqual(bodies.at(-1).thinking, { type: 'enabled' });
    assert.equal(bodies.at(-1).reasoning_effort, undefined);
  } finally {
    server.closeAllConnections();
    server.close();
  }
});

// ---- 复审修复回归：以下全部使用本机回环假站点，不访问网络 ----

async function serveFixture(handler) {
  const bodies = [];
  const server = createServer(async (request, response) => {
    let source = '';
    for await (const part of request) source += part;
    let body;
    try { body = JSON.parse(source); } catch { body = source; }
    bodies.push({ url: request.url, body });
    await handler(request, response, body, bodies.length);
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  return {
    bodies,
    url: `http://127.0.0.1:${server.address().port}`,
    close() { server.closeAllConnections(); server.close(); },
  };
}

function sse(response, events, named) {
  for (const event of events) {
    if (named) response.write(`event: ${event.type}\n`);
    response.write(`data: ${JSON.stringify(event)}\n\n`);
  }
}

function chatText(response, text, reason = 'stop') {
  response.writeHead(200, { 'Content-Type': 'text/event-stream' });
  chunk(response, [{ index: 0, delta: { role: 'assistant', content: text }, finish_reason: null }]);
  chunk(response, [{ index: 0, delta: {}, finish_reason: reason }]);
  response.end('data: [DONE]\n\n');
}

function chatTool(response, id, name, args = '{}', reason = 'tool_calls') {
  response.writeHead(200, { 'Content-Type': 'text/event-stream' });
  chunk(response, [{ index: 0, delta: { role: 'assistant', tool_calls: [{ index: 0, id, type: 'function',
    function: { name, arguments: args } }] }, finish_reason: null }]);
  chunk(response, [{ index: 0, delta: {}, finish_reason: reason }]);
  response.end('data: [DONE]\n\n');
}

function anthropicText(response) {
  response.writeHead(200, { 'Content-Type': 'text/event-stream' });
  sse(response, [
    { type: 'message_start', message: { id: 'm1', type: 'message', role: 'assistant', model: 'm', content: [],
      stop_reason: null, usage: { input_tokens: 10, output_tokens: 1 } } },
    { type: 'content_block_start', index: 0, content_block: { type: 'text', text: '' } },
    { type: 'content_block_delta', index: 0, delta: { type: 'text_delta', text: 'ok' } },
    { type: 'content_block_stop', index: 0 },
    { type: 'message_delta', delta: { stop_reason: 'end_turn', stop_sequence: null }, usage: { output_tokens: 2 } },
    { type: 'message_stop' }], true);
  response.end();
}

function anthropicTool(response, id) {
  response.writeHead(200, { 'Content-Type': 'text/event-stream' });
  sse(response, [
    { type: 'message_start', message: { id: 'm1', type: 'message', role: 'assistant', model: 'm', content: [],
      stop_reason: null, usage: { input_tokens: 10, output_tokens: 1 } } },
    { type: 'content_block_start', index: 0, content_block: { type: 'tool_use', id, name: 'editor_state', input: {} } },
    { type: 'content_block_delta', index: 0, delta: { type: 'input_json_delta', partial_json: '{}' } },
    { type: 'content_block_stop', index: 0 },
    { type: 'message_delta', delta: { stop_reason: 'tool_use', stop_sequence: null }, usage: { output_tokens: 5 } },
    { type: 'message_stop' }], true);
  response.end();
}

function geminiText(response) {
  response.writeHead(200, { 'Content-Type': 'text/event-stream' });
  sse(response, [{ candidates: [{ content: { role: 'model', parts: [{ text: 'ok' }] }, finishReason: 'STOP', index: 0 }],
    usageMetadata: { promptTokenCount: 5, candidatesTokenCount: 2, totalTokenCount: 7 } }]);
  response.end();
}

function responsesText(response) {
  response.writeHead(200, { 'Content-Type': 'text/event-stream' });
  sse(response, [
    { type: 'response.created', response: { id: 'r1', status: 'in_progress' } },
    { type: 'response.output_item.added', output_index: 0, item: { type: 'message', id: 'm1', role: 'assistant', content: [], status: 'in_progress' } },
    { type: 'response.content_part.added', item_id: 'm1', output_index: 0, content_index: 0, part: { type: 'output_text', text: '', annotations: [] } },
    { type: 'response.output_text.delta', item_id: 'm1', output_index: 0, content_index: 0, delta: 'ok' },
    { type: 'response.output_item.done', output_index: 0, item: { type: 'message', id: 'm1', role: 'assistant', status: 'completed',
      content: [{ type: 'output_text', text: 'ok', annotations: [] }] } },
    { type: 'response.completed', response: { id: 'r1', status: 'completed', output: [],
      usage: { input_tokens: 5, output_tokens: 2, total_tokens: 7 } } }], true);
  response.end();
}

function badRequest(response, message) {
  response.writeHead(400, { 'Content-Type': 'application/json' });
  response.end(JSON.stringify({ type: 'error', error: { type: 'invalid_request_error', message } }));
}

const fixtureUsage = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
  cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
const U = text => ({ role: 'user', content: [{ type: 'text', text }], timestamp: 1 });
const A = text => ({ role: 'assistant', content: [{ type: 'text', text }], api: 'openai-completions', provider: 'p',
  model: 'm', stopReason: 'stop', timestamp: 2, usage: fixtureUsage });
const AT = (id, name = 'editor_state') => ({ role: 'assistant', content: [{ type: 'toolCall', id, name, arguments: {} }],
  api: 'openai-completions', provider: 'p', model: 'm', stopReason: 'toolUse', timestamp: 2, usage: fixtureUsage });
const T = (id, text, name = 'editor_state') => ({ role: 'toolResult', toolCallId: id, toolName: name,
  content: [{ type: 'text', text }], isError: false, timestamp: 3 });
const stateTool = { name: 'editor_state', description: '当前地图身份', inputSchema: { type: 'object', properties: {} } };
const isSummaryRequest = body => JSON.stringify(body?.messages?.at(-1) ?? '').includes('compacted-summary');
const maxOutputOf = body => body.max_tokens ?? body.max_completion_tokens ?? body.max_output_tokens
  ?? body.generationConfig?.maxOutputTokens;

test('多回合会话：当前回合内长工具链也会被压缩，且不重复发无效摘要请求', async () => {
  let summaries = 0;
  const site = await serveFixture((_req, res, body, index) => {
    if (isSummaryRequest(body)) {
      ++summaries;
      return chatText(res, '<compacted-summary>\n## 原始需求与核心约束\n- 当前长任务\n## 待办清单与下一步行动\n- 继续\n</compacted-summary>');
    }
    const round = index - summaries;
    if (round <= 10) return chatTool(res, `call-${round}`, 'editor_state');
    return chatText(res, '完成');
  });
  const events = [];
  try {
    const saved = await runHarness({ provider: { id: 'p', name: 'n', protocol: 'openai-chat', baseUrl: `${site.url}/v1`, apiKey: 'k-multi' },
      model: { id: 'fixture', contextWindow: 16000 }, prompt: '当前长任务', systemPrompt: 's', tools: [stateTool],
      messages: [U('旧任务'), A('好')] },
    event => events.push(event), async () => ({ d: 'x'.repeat(6000) }));
    const sizes = site.bodies.map(entry => ({ summary: isSummaryRequest(entry.body), size: JSON.stringify(entry.body.messages).length }));
    const summaryIndex = sizes.findIndex(entry => entry.summary);
    assert(summaryIndex > 0, '应在当前回合内触发一次摘要');
    assert.equal(summaries, 1);
    const before = sizes[summaryIndex - 1].size;
    const after = sizes[summaryIndex + 1].size;
    assert(after < before / 2, `压缩后请求应明显变小：${before} -> ${after}`);
    const mainSizes = sizes.filter(entry => !entry.summary).map(entry => entry.size);
    assert(Math.max(...mainSizes) < 16000 * 4, '主请求不应无限增长');
    assert(events.some(event => event.type === 'context_compacted' && event.removed_messages > 0));
    // 当前提问作为真实用户消息原样保留，摘要紧随其后。
    const promptIndex = saved.findIndex(m => m.role === 'user' && !m.forge_hidden && m.content?.[0]?.text === '当前长任务');
    assert(promptIndex >= 0);
    assert.equal(saved[promptIndex + 1].forge_injection, 'compaction_summary');
    assert(events.some(event => event.type === 'run_end' && event.success));
  } finally {
    site.close();
  }
});

test('手动压缩中途停止：不产出会话状态并报告已取消', async () => {
  const aborter = new AbortController();
  const site = await serveFixture((_req, res) => {
    res.writeHead(200, { 'Content-Type': 'text/event-stream' });
    chunk(res, [{ index: 0, delta: { role: 'assistant', content: '<compacted-summary>\n## 原始需求与核心约束\n- 半截' }, finish_reason: null }]);
    setTimeout(() => aborter.abort(), 30);
  });
  const history = [U('用户需求一：很重要的约束'), AT('c1'), T('c1', 'r'.repeat(6000)), U('用户需求二'), AT('c2'), T('c2', 'ok'), A('结束')];
  const events = [];
  try {
    const returned = await runManualCompaction({ provider: { id: 'p', name: 'n', protocol: 'openai-chat', baseUrl: `${site.url}/v1`, apiKey: 'k-abort' },
      model: { id: 'fixture', contextWindow: 8192 }, systemPrompt: 's', tools: [stateTool], messages: history },
    event => events.push(event), aborter.signal);
    assert(!events.some(event => event.type === 'session_state'));
    assert(!events.some(event => event.type === 'context_compacted'));
    const end = events.find(event => event.type === 'compact_end');
    assert.equal(end.success, false);
    assert.equal(end.cancelled, true);
    assert.equal(returned.length, history.length);
    assert(!returned.some(m => m.forge_injection === 'compaction_summary'));
  } finally {
    site.close();
  }
});

test('运行中压缩被停止：不采用半截摘要、不再发主请求，以已停止收口', async () => {
  const aborter = new AbortController();
  let mainAfterSummary = 0;
  let sawSummary = false;
  const site = await serveFixture((_req, res, body, index) => {
    if (isSummaryRequest(body)) {
      sawSummary = true;
      res.writeHead(200, { 'Content-Type': 'text/event-stream' });
      chunk(res, [{ index: 0, delta: { role: 'assistant', content: '<compacted-summary>半截摘要半截摘要半截摘要半截摘要' }, finish_reason: null }]);
      setTimeout(() => aborter.abort(), 30);
      return;
    }
    if (sawSummary) ++mainAfterSummary;
    chatTool(res, `call-${index}`, 'editor_state');
  });
  const events = [];
  try {
    await runHarness({ provider: { id: 'p', name: 'n', protocol: 'openai-chat', baseUrl: `${site.url}/v1`, apiKey: 'k-mid' },
      model: { id: 'fixture', contextWindow: 8192 }, prompt: '任务', systemPrompt: 's', tools: [stateTool] },
    event => events.push(event), async () => ({ d: 'x'.repeat(3000) }), aborter.signal);
    assert(sawSummary);
    assert.equal(mainAfterSummary, 0);
    assert(!events.some(event => event.type === 'context_compacted' && event.removed_messages > 0));
    const state = events.filter(event => event.type === 'session_state').at(-1);
    assert(state && !state.partial);
    assert(!state.messages.some(m => m.forge_injection === 'compaction_summary'));
    const end = events.find(event => event.type === 'run_end');
    assert.equal(end.success, false);
    assert.match(end.text, /已停止/);
  } finally {
    site.close();
  }
});

test('预压缩阶段停止：不发送主请求', async () => {
  const aborter = new AbortController();
  const site = await serveFixture((_req, res, body) => {
    if (isSummaryRequest(body)) {
      res.writeHead(200, { 'Content-Type': 'text/event-stream' });
      chunk(res, [{ index: 0, delta: { role: 'assistant', content: '<compacted-summary>半截' }, finish_reason: null }]);
      setTimeout(() => aborter.abort(), 30);
      return;
    }
    chatText(res, '主请求不应发出');
  });
  const history = [U('唯一任务'.repeat(20))];
  for (let i = 0; i < 12; ++i) history.push(AT(`c${i}`), T(`c${i}`, 'x'.repeat(4000)));
  const events = [];
  try {
    await runHarness({ provider: { id: 'p', name: 'n', protocol: 'openai-chat', baseUrl: `${site.url}/v1`, apiKey: 'k-pre' },
      model: { id: 'fixture', contextWindow: 8192 }, prompt: '继续', systemPrompt: 's', tools: [stateTool], messages: history },
    event => events.push(event), async () => ({}), aborter.signal);
    assert.equal(site.bodies.filter(entry => !isSummaryRequest(entry.body)).length, 0);
    assert(!events.some(event => event.type === 'session_state'));
    assert(events.some(event => event.type === 'run_end' && !event.success));
  } finally {
    site.close();
  }
});

test('摘要被截断（length）不采用，回退摘要只有一层标签', async () => {
  const site = await serveFixture((_req, res) => chatText(res, '<compacted-summary>\n## 原始需求与核心约束\n- 前半段', 'length'));
  const history = [U('用户需求一：保留全部约束'), AT('c1'), T('c1', 'r'.repeat(6000)), U('用户需求二'), AT('c2'), T('c2', 'ok')];
  const events = [];
  try {
    const saved = await runManualCompaction({ provider: { id: 'p', name: 'n', protocol: 'openai-chat', baseUrl: `${site.url}/v1`, apiKey: 'k-len' },
      model: { id: 'fixture', contextWindow: 8192 }, systemPrompt: 's', tools: [stateTool], messages: history },
    event => events.push(event));
    assert.equal(site.bodies.length, 1);
    const compacted = events.find(event => event.type === 'context_compacted');
    assert(compacted && !compacted.summary.includes('前半段'));
    assert(compacted.summary.includes('用户需求一：保留全部约束'));
    const summaryText = saved.find(m => m.forge_injection === 'compaction_summary').content[0].text;
    assert.equal(summaryText.split('<compacted-summary>').length, 2);
    assert.equal(summaryText.split('</compacted-summary>').length, 2);
  } finally {
    site.close();
  }
});

test('Claude 思考参数：新型号发 adaptive + effort，旧型号发 budget_tokens，各档位不合并', async () => {
  assert.equal(anthropicThinkingProfile('claude-opus-5-5').adaptive, true);
  assert.equal(anthropicThinkingProfile('us.anthropic.claude-opus-4-7-v1:0').adaptive, true);
  assert.equal(anthropicThinkingProfile('anthropic/claude-sonnet-4.6').adaptive, true);
  assert.equal(anthropicThinkingProfile('claude-fable-5-2').adaptive, true);
  assert.equal(anthropicThinkingProfile('claude-haiku-5-5').adaptive, true);
  assert.equal(anthropicThinkingProfile('claude-opus-4-20250514').adaptive, false);
  assert.equal(anthropicThinkingProfile('claude-sonnet-4-5-20250929').adaptive, false);
  assert.equal(anthropicThinkingProfile('claude-3-7-sonnet-20250219').adaptive, false);
  assert.equal(anthropicThinkingProfile('claude-haiku-4-5').adaptive, false);

  const site = await serveFixture((_req, res) => anthropicText(res));
  const run = async (id, effort) => {
    await runHarness({ provider: { id: 'a', name: 'n', protocol: 'anthropic', baseUrl: site.url, apiKey: 'k-claude' },
      model: { id, contextWindow: 200000, supportedThinkingLevels: ['low', 'medium', 'high', 'xhigh', 'max'], thinkingEffort: effort },
      prompt: 'p', systemPrompt: 's', tools: [] }, () => {}, async () => ({}));
    return site.bodies.at(-1).body;
  };
  try {
    for (const [id, effort, expected] of [
      ['claude-opus-5-5', 'xhigh', 'xhigh'], ['claude-opus-5-5', 'max', 'max'], ['claude-opus-5-5', 'minimal', 'low'],
      ['claude-fable-5-2', 'high', 'high'], ['claude-sonnet-4-6', 'xhigh', 'high'], ['claude-sonnet-4-6', 'max', 'max']]) {
      const body = await run(id, effort);
      assert.equal(body.thinking?.type, 'adaptive', `${id}/${effort}`);
      assert.equal(body.thinking.budget_tokens, undefined);
      assert.equal(body.output_config?.effort, expected, `${id}/${effort}`);
    }
    const budgets = {};
    for (const effort of ['high', 'xhigh', 'max']) {
      const body = await run('claude-sonnet-4-5', effort);
      assert.equal(body.thinking?.type, 'enabled');
      assert.equal(body.output_config, undefined);
      assert(body.thinking.budget_tokens < body.max_tokens);
      budgets[effort] = body.thinking.budget_tokens;
    }
    assert.equal(budgets.high, 16384);
    assert(budgets.xhigh > budgets.high && budgets.max > budgets.xhigh, JSON.stringify(budgets));
  } finally {
    site.close();
  }
  // 兼容网关拒绝思考参数时，本次运行改为不发送思考参数重试，而不是整次任务失败。
  const gateway = await serveFixture((_req, res, body) => (body.output_config || body.thinking
    ? badRequest(res, 'output_config: Extra inputs are not permitted')
    : anthropicText(res)));
  try {
    const events = [];
    await runHarness({ provider: { id: 'g', name: 'n', protocol: 'anthropic', baseUrl: gateway.url, apiKey: 'k-gw' },
      model: { id: 'claude-opus-5-5', contextWindow: 200000, supportedThinkingLevels: ['low', 'high'], thinkingEffort: 'high' },
      prompt: 'p', systemPrompt: 's', tools: [] }, event => events.push(event), async () => ({}));
    assert.equal(gateway.bodies.length, 2);
    assert.equal(gateway.bodies[1].body.thinking, undefined);
    assert(events.some(event => event.type === 'run_end' && event.success));
  } finally {
    gateway.close();
  }
});

test('未选择思考强度：Gemini、Responses、Anthropic 都不发送任何思考参数', async () => {
  const cases = [
    ['gemini', 'gemini-2.5-pro', geminiText, body => assert.equal(body.generationConfig?.thinkingConfig, undefined)],
    ['gemini', 'gemini-3-flash-preview', geminiText, body => assert.equal(body.generationConfig?.thinkingConfig, undefined)],
    ['openai-responses', 'o3', responsesText, body => { assert.equal(body.reasoning, undefined); assert.equal(body.include, undefined); }],
    ['anthropic', 'claude-opus-5-5', anthropicText, body => { assert.equal(body.thinking, undefined); assert.equal(body.output_config, undefined); }],
    ['anthropic', 'claude-sonnet-4-5', anthropicText, body => assert.equal(body.thinking, undefined)],
  ];
  for (const [protocol, id, respond, check] of cases) {
    const site = await serveFixture((_req, res) => respond(res));
    const events = [];
    try {
      await runHarness({ provider: { id: 'p', name: 'n', protocol, baseUrl: site.url, apiKey: 'k-default' },
        model: { id, contextWindow: 200000, supportedThinkingLevels: ['low', 'medium', 'high'] },
        prompt: 'p', systemPrompt: 's', tools: [] }, event => events.push(event), async () => ({}));
      assert.equal(site.bodies.length, 1, `${protocol}/${id}`);
      check(site.bodies[0].body);
      assert(events.some(event => event.type === 'run_end' && event.success), `${protocol}/${id}`);
    } finally {
      site.close();
    }
  }
});

test('输出上限降级在整次运行内保持，思考预算始终低于 max_tokens', async () => {
  assert.equal(parseMaxTokensRangeError('max_tokens: 32768 > 4096, which is the maximum allowed number of output tokens for claude-3-haiku-20240307'), 4096);
  assert.equal(parseMaxTokensRangeError('Invalid max_tokens: valid range [1, 8000]'), 8000);
  assert.equal(parseMaxTokensRangeError('unrelated error'), null);
  const cases = [
    ['openai-chat', 'gpt-x', {}, (res, i) => chatTool(res, `call-${i}`, 'editor_state'), res => chatText(res, 'ok')],
    ['openai-responses', 'gpt-x', {}, null, responsesText],
    ['anthropic', 'claude-sonnet-4-5', { supportedThinkingLevels: ['off', 'low', 'medium', 'high'], thinkingEffort: 'high' },
      (res, i) => anthropicTool(res, `call-${i}`), anthropicText],
  ];
  for (const [protocol, id, extra, firstTurn, laterTurn] of cases) {
    let good = 0;
    const site = await serveFixture((_req, res, body) => {
      const limit = maxOutputOf(body);
      if (limit > 8000) return badRequest(res, 'Invalid max_tokens / max_output_tokens: valid range [1, 8000]');
      if (body.thinking?.budget_tokens >= limit) return badRequest(res, 'thinking.budget_tokens must be less than max_tokens');
      ++good;
      if (good === 1 && firstTurn) return firstTurn(res, good);
      return laterTurn(res);
    });
    const events = [];
    try {
      await runHarness({ provider: { id: 'p', name: 'n', protocol, baseUrl: protocol === 'openai-chat' ? `${site.url}/v1` : site.url, apiKey: 'k-cap' },
        model: { id, contextWindow: 200000, ...extra }, prompt: 'p', systemPrompt: 's', tools: [stateTool] },
      event => events.push(event), async () => ({ ok: true }));
      const limits = site.bodies.map(entry => maxOutputOf(entry.body));
      assert(limits[0] > 8000, `${protocol}: ${limits}`);
      assert(limits.slice(1).every(limit => limit <= 8000), `${protocol}: ${limits}`);
      assert.equal(site.bodies.length, firstTurn ? 3 : 2, `${protocol}: ${limits}`);
      assert(events.some(event => event.type === 'run_end' && event.success), protocol);
    } finally {
      site.close();
    }
  }
  // 旧 Claude 的 `max_tokens: N > 4096` 报错形式也能解析并降级成功。
  const haiku = await serveFixture((_req, res, body) => (body.max_tokens > 4096
    ? badRequest(res, `max_tokens: ${body.max_tokens} > 4096, which is the maximum allowed number of output tokens for claude-x`)
    : anthropicText(res)));
  try {
    const events = [];
    await runHarness({ provider: { id: 'p', name: 'n', protocol: 'anthropic', baseUrl: haiku.url, apiKey: 'k-h' },
      model: { id: 'claude-custom-gateway', contextWindow: 200000 }, prompt: 'p', systemPrompt: 's', tools: [] },
    event => events.push(event), async () => ({}));
    assert.deepEqual(haiku.bodies.map(entry => entry.body.max_tokens).slice(-1), [4096]);
    assert(events.some(event => event.type === 'run_end' && event.success));
  } finally {
    haiku.close();
  }
});

test('输出截断续跑有共同上限：工具参数截断与正文截断都最多续跑 3 次', async () => {
  const toolSite = await serveFixture((_req, res, _body, index) => chatTool(res, `call-${index}`, 'editor_state', '{"op":"del', 'length'));
  const events = [];
  let executed = 0;
  try {
    await assert.rejects(runHarness({ provider: { id: 'p', name: 'n', protocol: 'openai-chat', baseUrl: `${toolSite.url}/v1`, apiKey: 'k-l1' },
      model: { id: 'fixture', contextWindow: 64000 }, prompt: 'p', systemPrompt: 's', tools: [stateTool], maxSteps: 50 },
    event => events.push(event), async () => { ++executed; return {}; }), /长度上限/);
    assert.equal(toolSite.bodies.length, 4);
    assert.equal(executed, 0);
    assert(events.some(event => event.type === 'session_state' && event.partial));
  } finally {
    toolSite.close();
  }
  const textSite = await serveFixture((_req, res, _body, index) => chatText(res, `第${index}段`, 'length'));
  try {
    await assert.rejects(runHarness({ provider: { id: 'p', name: 'n', protocol: 'openai-chat', baseUrl: `${textSite.url}/v1`, apiKey: 'k-l2' },
      model: { id: 'fixture', contextWindow: 64000 }, prompt: 'p', systemPrompt: 's', tools: [] },
    () => {}, async () => ({})), /长度上限/);
    assert.equal(textSite.bodies.length, 4);
    const nudge = JSON.stringify(textSite.bodies[1].body.messages.at(-1));
    assert(nudge.includes('从中断处'));
    assert(!nudge.includes('工具调用'));
  } finally {
    textSite.close();
  }
});

test('确定性兜底摘要不缩小时如实报告未压缩，同一区间不再重复请求；用户原文逐字保留', async () => {
  const bigRequirement = `需求开头${'甲'.repeat(9000)}中段关键约束XYZ${'乙'.repeat(9000)}需求结尾`;
  const history = [U(bigRequirement), A('好'), U('当前')];
  for (let i = 0; i < 3; ++i) history.push(AT(`c${i}`), T(`c${i}`, 'ok'));
  let calls = 0;
  const memo = new Set();
  const failing = async () => { ++calls; throw new Error('network'); };
  const first = await compactWithWarmCache({ messages: history, contextWindow: 8192, summarizeFn: failing, attemptMemo: memo });
  assert.equal(first.compacted, false);
  assert.equal(first.savedTokens, 0);
  assert.equal(first.messages[0].content[0].text, bigRequirement);
  const second = await compactWithWarmCache({ messages: history, contextWindow: 8192, summarizeFn: failing, attemptMemo: memo });
  assert.equal(second.compacted, false);
  assert.equal(calls, 1);

  // 区间里有可归纳的大工具结果时兜底摘要会缩小，用户长需求中段不会被截掉。
  const withTools = [U(bigRequirement), AT('t1'), T('t1', 'z'.repeat(60000)), U('当前'), AT('t2'), T('t2', 'ok')];
  const compacted = await compactWithWarmCache({ messages: withTools, contextWindow: 16000, force: true, summarizeFn: failing });
  assert.equal(compacted.compacted, true);
  assert(compacted.savedTokens > 0);
  assert(compacted.summary.includes('中段关键约束XYZ'));
  assert(!compacted.summary.includes('中间省略'));
  // 既有检查点整段继承，不再截到 2500 字。
  const longCheckpoint = `前${'丙'.repeat(5000)}后`;
  const inherited = buildDeterministicSummaryBody([createCompactedSummaryMessage(longCheckpoint)]);
  assert(inherited.includes(longCheckpoint));
  assert(!inherited.includes('<compacted-summary>'));
});

test('手动压缩按历史工具名还原：已停用工具与同形历史名都保持原名', async () => {
  const history = [U('一'), AT('c1', 'old.tool'), T('c1', 'r'.repeat(20000), 'old.tool'), U('二'),
    AT('c2', 'a_b'), T('c2', 'r', 'a_b'), AT('c3', 'old.tool'), T('c3', 'r', 'old.tool'), A('完')];
  const events = [];
  const saved = await runManualCompaction({ provider: {}, model: { contextWindow: 8192 }, systemPrompt: 's',
    tools: [{ name: 'a.b', description: 'd', inputSchema: { type: 'object', properties: {} } }], messages: history },
  event => events.push(event));
  assert(events.some(event => event.type === 'compact_end' && event.compacted));
  const names = saved.flatMap(m => m.role === 'toolResult' ? [m.toolName]
    : m.role === 'assistant' ? (m.content ?? []).filter(b => b.type === 'toolCall').map(b => b.name) : []);
  assert.deepEqual(names, ['a_b', 'a_b', 'old.tool', 'old.tool']);
});
