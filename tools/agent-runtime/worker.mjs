import { Agent } from '@earendil-works/pi-agent-core';
import { createModels, createProvider } from '@earendil-works/pi-ai/models';
import { createInterface } from 'node:readline';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const protocols = {
  'openai-chat': 'openai-completions',
  'openai-responses': 'openai-responses',
  anthropic: 'anthropic-messages',
  gemini: 'google-generative-ai',
};

// Literal imports let the portable build carry only the four supported APIs.
const apiLoaders = {
  'openai-completions': () => import('@earendil-works/pi-ai/api/openai-completions'),
  'openai-responses': () => import('@earendil-works/pi-ai/api/openai-responses'),
  'anthropic-messages': () => import('@earendil-works/pi-ai/api/anthropic-messages'),
  'google-generative-ai': () => import('@earendil-works/pi-ai/api/google-generative-ai'),
};

export function redact(value, secrets = []) {
  let text = typeof value === 'string' ? value : JSON.stringify(value);
  for (const secret of secrets) if (secret) text = text.split(secret).join('[已隐藏密钥]');
  return text;
}

// OpenAI 兼容站点（DeepSeek 等）与 Anthropic 只接受 ^[a-zA-Z0-9_-]{1,64}$ 的工具名；编辑器 MCP 名可含点号。
// 发给模型前换成合规名，调用和事件仍用原名；冲突时加序号，映射在一次运行内固定。
export function providerToolNames(names) {
  const toProvider = new Map(), used = new Set();
  for (const name of names) {
    const base = (String(name).replace(/[^a-zA-Z0-9_-]/g, '_') || 'tool').slice(0, 64);
    let candidate = base;
    for (let n = 2; used.has(candidate); ++n) candidate = `${base.slice(0, 64 - String(n).length - 1)}_${n}`;
    used.add(candidate); toProvider.set(name, candidate);
  }
  return toProvider;
}

// 历史中的工具请求/结果使用与本次工具表一致的合规名，否则严格站点会拒绝整段历史。
function renameHistoryTools(messages, rename) {
  return messages.map(message => {
    if (message.role === 'toolResult' && message.toolName) return { ...message, toolName: rename(message.toolName) };
    if (message.role === 'assistant' && Array.isArray(message.content))
      return { ...message, content: message.content.map(part =>
        part?.type === 'toolCall' && part.name ? { ...part, name: rename(part.name) } : part) };
    return message;
  });
}

export function compactHistory(messages, contextWindow) {
  // 只移除完整用户回合；保留工具请求/响应配对，不截掉最近未结束的工具链。
  const budget = Math.max(1024, Math.floor(contextWindow * 0.65));
  const estimate = JSON.stringify(messages).length / 2;
  if (estimate <= budget) return { messages, compacted: false };
  const boundaries = [];
  for (let i = 0; i < messages.length; ++i) if (messages[i].role === 'user') boundaries.push(i);
  if (boundaries.length < 2) throw new Error('最近单个回合超过上下文预算，请缩小查询范围或开始新任务。');
  let cut = boundaries[1];
  for (const boundary of boundaries.slice(1)) {
    cut = boundary;
    if (JSON.stringify(messages.slice(cut)).length / 2 <= budget) break;
  }
  const removed = messages.slice(0, cut);
  const facts = removed.filter(m => m.role === 'toolResult').map(m => {
    const content = (m.content ?? []).filter(c => c.type === 'text').map(c => c.text).join('\n');
    return `${m.toolName}: ${m.isError ? '失败' : '已返回'} ${content.slice(0, 600)}`;
  }).slice(-12).join('\n');
  const summary = {
    role: 'user', timestamp: Date.now(),
    forge_hidden: true, forge_injection: 'context_summary',
    content: [{ type: 'text', text: `较早完整回合已从模型上下文压缩；完整记录仍由宿主保存。下列只是历史工具结果摘要，身份可能已过期，继续编辑前重新查询当前地图。\n${facts}` }],
  };
  const retained = [summary, ...messages.slice(cut)];
  if (JSON.stringify(retained).length / 2 > budget) throw new Error('最近回合超过上下文预算，未发送不完整工具链。');
  return { messages: retained, compacted: true, removedMessages: cut };
}

export async function runHarness(input, emit, invokeTool, signal) {
  const api = protocols[input.provider?.protocol];
  if (!api) throw new Error('未支持的提供商协议。');
  const contextWindow = Number(input.model?.contextWindow ?? 262144);
  if (!Number.isSafeInteger(contextWindow) || contextWindow < 1024) throw new Error('上下文长度无效。');
  const endpoint = new URL(input.provider.baseUrl);
  if (!['http:', 'https:'].includes(endpoint.protocol)) throw new Error('API 地址必须是 HTTP 或 HTTPS。');
  const key = input.provider.apiKey ?? '';
  const safeEmit = event => emit(JSON.parse(redact(event, [key])));
  const apiModule = await apiLoaders[api]();
  const maxTokens = Math.min(input.model.maxOutputTokens || 8192, Math.max(256, Math.floor(contextWindow / 4)));
  const model = {
    id: input.model.id, name: input.model.name || input.model.id, api,
    provider: input.provider.id, baseUrl: input.provider.baseUrl,
    reasoning: false, input: ['text'], contextWindow, maxTokens,
    cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
  };
  const models = createModels();
  models.setProvider(createProvider({
    id: input.provider.id, name: input.provider.name, baseUrl: input.provider.baseUrl,
    auth: { apiKey: { name: 'FORGE 本机凭据', resolve: async () => ({ auth: { apiKey: key } }) } },
    models: [model], api: { stream: apiModule.stream, streamSimple: apiModule.streamSimple },
  }));
  const toProvider = providerToolNames((input.tools ?? []).map(tool => tool.name));
  const toEditor = new Map([...toProvider].map(([editor, provider]) => [provider, editor]));
  const providerName = name => toProvider.get(name) ?? (String(name).replace(/[^a-zA-Z0-9_-]/g, '_').slice(0, 64) || 'tool');
  const editorName = name => toEditor.get(name) ?? name;
  const tools = (input.tools ?? []).map(tool => ({
    name: providerName(tool.name), label: tool.description?.split('\n')[0] || tool.name,
    description: tool.description || tool.name, parameters: tool.inputSchema,
    executionMode: 'sequential',
    execute: async (id, arguments_, toolSignal) => {
      const result = await invokeTool({ id, name: tool.name, arguments: arguments_ }, toolSignal);
      const text = redact(JSON.stringify(result), [key]);
      const bounded = text.length > 65536 ? JSON.stringify({ result_truncated: true,
        note: '结果超过模型工具上下文预算，请减小分页后重新查询。', prefix: text.slice(0, 63000) }) : text;
      return { content: [{ type: 'text', text: bounded }], details: { isError: Boolean(result.isError) } };
    },
  }));
  const history = compactHistory(renameHistoryTools(input.messages ?? [], providerName), contextWindow);
  if (history.compacted) safeEmit({ type: 'context_compacted', removed_messages: history.removedMessages,
    text: '已压缩较早完整回合；完整操作记录保留。' });
  let requestCount = 0;
  const agent = new Agent({
    initialState: { systemPrompt: input.systemPrompt, model: models.getModel(input.provider.id, model.id),
      messages: history.messages, tools, thinkingLevel: 'off' },
    streamFn: models.streamSimple.bind(models), toolExecution: 'sequential',
    prepareRequest: async ({ context }) => {
      if (++requestCount > 64) throw new Error('本次自动运行达到64个模型回合，已保留任务状态。');
      return { context };
    },
  });
  const abort = () => agent.abort();
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) abort();
  let lastError;
  agent.subscribe(event => {
    if (event.type === 'message_update' && event.assistantMessageEvent?.type === 'text_delta') {
      safeEmit({ type: 'assistant_delta', text: event.assistantMessageEvent.delta });
    } else if (event.type === 'tool_execution_start') {
      safeEmit({ type: 'tool_start', id: event.toolCallId, tool_name: editorName(event.toolName),
        title: editorName(event.toolName), details: { arguments: event.args } });
    } else if (event.type === 'tool_execution_end') {
      const isError = Boolean(event.isError || event.result?.details?.isError);
      safeEmit({ type: 'tool_end', id: event.toolCallId, tool_name: editorName(event.toolName),
        success: !isError, details: { result: event.result } });
    } else if (event.type === 'message_end' && event.message.role === 'assistant') {
      const message = event.message;
      // PI 在未收到站点用量时也保留全零占位，不能展示成真实的0 tokens。
      const available = message.usage && ['input', 'output', 'cacheRead', 'cacheWrite', 'totalTokens']
        .some(key => Number.isFinite(message.usage[key]) && message.usage[key] > 0);
      if (available) {
        const { cost: _unavailableCost, ...tokenUsage } = message.usage;
        safeEmit({ type: 'usage', available: true, details: tokenUsage, cost_available: false,
          input_tokens: message.usage.input, output_tokens: message.usage.output, estimated: false });
      } else {
        safeEmit({ type: 'usage', available: false, cost_available: false,
          input_tokens: null, output_tokens: null, estimated: false });
      }
      if (message.stopReason === 'error') lastError = message.errorMessage || '模型请求失败。';
    }
  });
  safeEmit({ type: 'run_start', text: '正在处理地图任务', context_window: contextWindow });
  try {
    await agent.prompt(input.prompt);
    if (lastError) throw new Error(lastError);
    safeEmit({ type: 'session_state', messages: agent.state.messages });
    safeEmit({ type: 'run_end', success: !signal?.aborted,
      text: signal?.aborted ? '已停止，保留已完成改动。' : '本次任务结束。' });
    return agent.state.messages;
  } finally {
    if (lastError) safeEmit({ type: 'session_state', messages: agent.state.messages });
    signal?.removeEventListener('abort', abort);
  }
}

async function runStdio() {
  const lines = createInterface({ input: process.stdin, crlfDelay: Infinity });
  let active;
  let aborter;
  const pending = new Map();
  const emit = event => process.stdout.write(JSON.stringify(event) + '\n');
  const invokeTool = (call, signal) => new Promise((resolve_, reject) => {
    const cancel = () => { pending.delete(call.id); reject(new Error('工具等待已取消。')); };
    signal?.addEventListener('abort', cancel, { once: true });
    pending.set(call.id, { resolve: value => { signal?.removeEventListener('abort', cancel); resolve_(value); } });
    if (signal?.aborted) cancel();
    else emit({ type: 'tool_call', ...call });
  });
  emit({ type: 'ready', runtime: 'pi', version: '1.0.0' });
  for await (const line of lines) {
    if (line.length > 8 * 1024 * 1024) { emit({ type: 'error', text: 'IPC 请求超过8MiB。' }); continue; }
    let input;
    try { input = JSON.parse(line); } catch { emit({ type: 'error', text: 'IPC JSON格式错误。' }); continue; }
    if (input.type === 'tool_result') { pending.get(input.id)?.resolve(input.result); pending.delete(input.id); }
    else if (input.type === 'stop') aborter?.abort();
    else if (input.type === 'start') {
      if (active) { emit({ type: 'error', text: '已有任务正在运行。' }); continue; }
      aborter = new AbortController();
      active = runHarness(input, emit, invokeTool, aborter.signal)
        .catch(error => { emit({ type: 'error', text: redact(error.message, [input.provider?.apiKey]) });
          emit({ type: 'run_end', success: false, text: '任务未完成，已保留现有改动。' }); })
        .finally(() => { active = undefined; aborter = undefined; pending.clear(); });
    }
  }
  aborter?.abort();
  if (active) await active;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  runStdio().catch(() => process.exitCode = 1);
}
