import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { once } from 'node:events';
const { runHarness, compactHistory, redact, providerToolNames } = await import(process.env.FORGE_RUNTIME_TEST_ENTRY || '../worker.mjs');

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
