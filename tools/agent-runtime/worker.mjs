import { Agent } from '@earendil-works/pi-agent-core';
import { createInitialSystemMessage, normalizeContext, toToolDeclaration } from '@earendil-works/pi-ai';
import { createModels, createProvider } from '@earendil-works/pi-ai/models';
import { createInterface } from 'node:readline';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import {
  DEFAULT_THRESHOLD_RATIO,
  DEFAULT_RETAIN_RATIO,
  buildDeterministicSummaryBody,
  compactWithWarmCache,
  createCompactedSummaryMessage,
  estimateSurfaceTokens,
  extractMessageText,
  isContextOverflowError,
  isToolPairingBalanced,
} from './compaction.mjs';

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

// OpenAI 兼容站点（DeepSeek 等）与 Anthropic 只接受 ^[a-zA-Z0-9_-]{1,64}$ 的工具名；编辑器与外部 MCP 名可含点号。
// 发给模型前换成合规名，调用、检查点和保存状态仍还原为原名；冲突时加序号，映射在一次运行内固定。
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

// Gemini 对 $ref 的支持不完整，递归引用可能被拒绝整次请求。发给它的参数 schema 就地展开 $ref、去掉 $defs；
// 同一展开路径上再遇到同一定义时换成普通对象，打断递归。编辑器收到调用后仍按原 schema 严格校验。
export function inlineSchemaRefs(schema) {
  if (!schema || typeof schema !== 'object') return schema;
  const resolveRef = ref => {
    if (typeof ref !== 'string' || !ref.startsWith('#/')) return undefined;
    let current = schema;
    for (const raw of ref.slice(2).split('/')) {
      const part = raw.replaceAll('~1', '/').replaceAll('~0', '~');
      if (!current || typeof current !== 'object' || !Object.hasOwn(current, part)) return undefined;
      current = current[part];
    }
    return current && typeof current === 'object' ? current : undefined;
  };
  const walk = (node, stack) => {
    if (Array.isArray(node)) return node.map(item => walk(item, stack));
    if (!node || typeof node !== 'object') return node;
    if (typeof node.$ref === 'string') {
      const { $ref, ...siblings } = node;
      const target = resolveRef($ref);
      if (!target || stack.includes($ref))
        return { type: 'object', description: '结构与上层同名定义相同，按相同字段嵌套填写。', ...walk(siblings, stack) };
      return { ...walk(target, [...stack, $ref]), ...walk(siblings, stack) };
    }
    const result = {};
    for (const [key, value] of Object.entries(node)) {
      if (key === '$defs' || key === 'definitions') continue;
      result[key] = key === 'properties' && value && typeof value === 'object' && !Array.isArray(value)
        ? Object.fromEntries(Object.entries(value).map(([name, child]) => [name, walk(child, stack)]))
        : walk(value, stack);
    }
    return result;
  };
  return walk(schema, []);
}

// 双向转换历史中的工具名称（包含 system 的 toolsAdded / toolsRemoved、assistant toolCall 与 toolResult）。
export function renameHistoryTools(messages, rename) {
  return messages.map(message => {
    if (!message || typeof message !== 'object') return message;
    if (message.role === 'system') {
      let next = message;
      if (Array.isArray(message.toolsAdded)) {
        next = { ...next, toolsAdded: message.toolsAdded.map(t => t?.name ? { ...t, name: rename(t.name) } : t) };
      }
      if (Array.isArray(message.toolsRemoved)) {
        next = { ...next, toolsRemoved: message.toolsRemoved.map(t => t?.name ? { ...t, name: rename(t.name) } : t) };
      }
      return next;
    }
    if (message.role === 'toolResult' && message.toolName) {
      return { ...message, toolName: rename(message.toolName) };
    }
    if (message.role === 'assistant' && Array.isArray(message.content)) {
      return {
        ...message,
        content: message.content.map(part =>
          part?.type === 'toolCall' && part.name ? { ...part, name: rename(part.name) } : part),
      };
    }
    return message;
  });
}

// 保存前剥离尾部因错误/中断未完成的残缺助手轮次，确保落盘历史永远满足完整工具配对，同时保留全部早期用户提问与已完成工具步。
export function sanitizeTranscriptForSave(messages, restoreToolName = x => x) {
  const restored = renameHistoryTools(messages ?? [], restoreToolName);
  const cleaned = [];
  for (let i = 0; i < restored.length; ++i) {
    const msg = restored[i];
    if (!msg || typeof msg !== 'object') continue;
    if (msg.role === 'assistant') {
      if (msg.stopReason === 'aborted' || msg.stopReason === 'error') {
        continue;
      }
      const toolCalls = Array.isArray(msg.content) ? msg.content.filter(b => b?.type === 'toolCall' && b.id) : [];
      if (toolCalls.length > 0) {
        const needed = new Set(toolCalls.map(tc => tc.id));
        let j = i + 1;
        while (j < restored.length && restored[j]?.role === 'toolResult') {
          needed.delete(restored[j].toolCallId);
          ++j;
        }
        if (needed.size > 0) {
          // 尾部未配对工具调用（例如刚发出调用就中断）不写入活动表层。
          break;
        }
      }
    }
    cleaned.push(msg);
  }
  return isToolPairingBalanced(cleaned) ? cleaned : [];
}

// 同步确定性压缩入口（供离线单测及无模型连接时使用，完整保留用户原始需求）。
export function compactHistory(messages, contextWindow) {
  const budget = Math.max(1024, Math.floor(contextWindow * DEFAULT_THRESHOLD_RATIO));
  const estimate = JSON.stringify(messages).length / 2;
  if (estimate <= budget) return { messages, compacted: false };
  const boundaries = [];
  for (let i = 0; i < messages.length; ++i) {
    if (messages[i].role === 'user' && !messages[i].forge_hidden) boundaries.push(i);
  }
  if (boundaries.length < 2) throw new Error('最近单个回合超过上下文预算，请缩小查询范围或开始新任务。');
  let cut = boundaries[1];
  for (const boundary of boundaries.slice(1)) {
    cut = boundary;
    if (JSON.stringify(messages.slice(cut)).length / 2 <= budget) break;
  }
  const head = messages[0]?.role === 'system' ? [messages[0]] : [];
  const startIdx = head.length;
  const removed = messages.slice(startIdx, cut);
  const summary = createCompactedSummaryMessage(buildDeterministicSummaryBody(removed, 2000), {
    removedCount: removed.length,
  });
  const retained = [...head, summary, ...messages.slice(cut)];
  if (JSON.stringify(retained).length / 2 > budget) throw new Error('最近回合超过上下文预算，未发送不完整工具链。');
  return { messages: retained, compacted: true, removedMessages: removed.length };
}

const VALID_THINKING_LEVELS = ['off', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max'];

function normalizeThinkingEffort(raw) {
  const token = String(raw ?? '').trim().toLowerCase();
  if (!token || token === 'default' || token === 'auto') return '';
  if (token === 'none' || token === 'disabled') return 'off';
  return VALID_THINKING_LEVELS.includes(token) ? token : '';
}

function isReasoningCapableModel(modelId = '', baseUrl = '', modelName = '') {
  const id = `${String(modelId)} ${String(modelName)}`.toLowerCase();
  const url = String(baseUrl).toLowerCase();
  if (/reasoner|thinking|deepseek-r1|deepseek-v4|deepseek\s+v4|deepseek-flash|\bo1\b|\bo3\b|\bo4\b|qwq|gpt-5|gpt-6|claude-opus|claude-sonnet-4|claude-sonnet-5|claude-fable|gemini-2\.5|gemini-3/.test(id)) return true;
  if (url.includes('deepseek') && /reasoner|v4|flash|r1/.test(id)) return true;
  return false;
}

export function resolveDefaultMaxTokens(inputModel, baseUrl, contextWindow) {
  const explicit = Number(inputModel?.maxOutputTokens ?? 0);
  if (Number.isFinite(explicit) && explicit > 0) {
    return Math.min(explicit, Math.max(256, contextWindow));
  }
  const id = String(inputModel?.id ?? '').toLowerCase();
  const host = String(baseUrl ?? '').toLowerCase();
  if (isReasoningCapableModel(id, host) || /gpt-5|claude-|gemini-/.test(id)) {
    return Math.min(32768, Math.max(4096, Math.floor(contextWindow / 2)));
  }
  if (host.includes('deepseek.com') && id === 'deepseek-chat') {
    return 8192;
  }
  return Math.min(16384, Math.max(4096, Math.floor(contextWindow / 4)));
}

function hasRawDsmlToolLeak(text) {
  if (!text) return false;
  return text.includes('<｜DSML｜function_calls>')
    || text.includes('<|DSML|function_calls>')
    || text.includes('<｜DSML｜invoke')
    || text.includes('<|DSML|invoke');
}

function parseMaxTokensRangeError(errorMessage) {
  const msg = String(errorMessage ?? '');
  if (!/max_tokens|max_completion_tokens|max_output_tokens/i.test(msg)) return null;
  const bracketMatch = msg.match(/\[\s*1\s*,\s*(\d+)\s*\]/) || msg.match(/<=\s*(\d+)/) || msg.match(/maximum.*?\b(\d{4,6})\b/i);
  if (bracketMatch) {
    const parsed = Number(bracketMatch[1]);
    if (Number.isSafeInteger(parsed) && parsed >= 512) return parsed;
  }
  return 8192;
}

async function createProviderContext(input, key) {
  const api = protocols[input.provider?.protocol];
  if (!api) throw new Error('未支持的提供商协议。');
  const contextWindow = Number(input.model?.contextWindow ?? 262144);
  if (!Number.isSafeInteger(contextWindow) || contextWindow < 1024) throw new Error('上下文长度无效。');
  const endpoint = new URL(input.provider.baseUrl);
  if (!['http:', 'https:'].includes(endpoint.protocol)) throw new Error('API 地址必须是 HTTP 或 HTTPS。');

  const apiModule = await apiLoaders[api]();
  const maxTokens = resolveDefaultMaxTokens(input.model, input.provider.baseUrl, contextWindow);
  const isDeepSeek = String(input.provider.baseUrl).toLowerCase().includes('deepseek')
    || /deepseek/i.test(`${input.model?.id ?? ''} ${input.model?.name ?? ''}`);

  const requestedEffort = normalizeThinkingEffort(input.model?.thinkingEffort);
  const explicitLevels = Array.isArray(input.model?.supportedThinkingLevels)
    ? input.model.supportedThinkingLevels.map(normalizeThinkingEffort).filter(Boolean)
    : [];
  const thinkingCustomized = Boolean(input.model?.thinkingCustomized);
  const supportedLevels = (explicitLevels.length > 0 || thinkingCustomized)
    ? explicitLevels
    : (isReasoningCapableModel(input.model?.id, input.provider.baseUrl, input.model?.name)
      ? (isDeepSeek ? ['off', 'low', 'high', 'max'] : ['off', 'low', 'medium', 'high'])
      : []);
  const reasoningModel = supportedLevels.some(lv => lv !== 'off')
    || (requestedEffort !== '' && requestedEffort !== 'off');

  const thinkingLevelMap = {};
  if (reasoningModel) {
    for (const lv of VALID_THINKING_LEVELS) {
      if (supportedLevels.includes(lv) || lv === requestedEffort) {
        if (lv === 'off') {
          if (api === 'openai-responses') thinkingLevelMap.off = 'none';
        } else {
          thinkingLevelMap[lv] = lv;
        }
      } else {
        thinkingLevelMap[lv] = null;
      }
    }
  }

  const compat = (api === 'openai-completions' && isDeepSeek)
    ? {
      supportsStore: false,
      supportsDeveloperRole: false,
      supportsReasoningEffort: true,
      maxTokensField: 'max_tokens',
      requiresReasoningContentOnAssistantMessages: true,
      thinkingFormat: 'deepseek',
    }
    : undefined;

  const model = {
    id: input.model.id,
    name: input.model.name || input.model.id,
    api,
    provider: input.provider.id,
    baseUrl: input.provider.baseUrl,
    reasoning: reasoningModel,
    ...(reasoningModel ? { thinkingLevelMap } : {}),
    ...(compat ? { compat } : {}),
    input: input.model.imagesSupported ? ['text', 'image'] : ['text'],
    contextWindow,
    maxTokens,
    cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0 },
  };
  const models = createModels();
  models.setProvider(createProvider({
    id: input.provider.id,
    name: input.provider.name,
    baseUrl: input.provider.baseUrl,
    auth: { apiKey: { name: 'FORGE 本机凭据', resolve: async () => ({ auth: { apiKey: key } }) } },
    models: [model],
    api: { stream: apiModule.stream, streamSimple: apiModule.streamSimple },
  }));

  return { api, contextWindow, maxTokens, reasoningModel, isDeepSeek, requestedEffort, supportedLevels, model, models };
}

function isUnsupportedReasoningEffortError(errorMessage) {
  const msg = String(errorMessage ?? '');
  return /\b400\b|invalid|unknown|unrecognized|unsupported/i.test(msg)
    && /reasoning_effort/i.test(msg);
}

function normalizeWirePayload(params, { api, isDeepSeek, reasoningModel, requestedEffort, stripReasoningEffort, clampedMaxTokens }) {
  if (!params || typeof params !== 'object') return params;
  const next = { ...params };
  if (clampedMaxTokens > 0) {
    if (typeof next.max_tokens === 'number' && next.max_tokens > clampedMaxTokens) {
      next.max_tokens = clampedMaxTokens;
    }
    if (typeof next.max_completion_tokens === 'number' && next.max_completion_tokens > clampedMaxTokens) {
      next.max_completion_tokens = clampedMaxTokens;
    }
  }
  // 对齐 deepseek-harness serialize.ts 的思考强度线协议：
  // - 未显式选择强度（默认 ""）：不覆写接口默认，移除 pi-ai 因缺省 reasoning 误注入的 thinking: disabled；
  // - 显式选择 "off"：DeepSeek 发送 thinking: { type: "disabled" } 且不发 reasoning_effort；普通 OpenAI 不发 reasoning_effort；
  // - 显式选择 "low"/"high"/"max" 等：DeepSeek 发送 thinking: { type: "enabled" } + reasoning_effort（若旧网关拒绝 reasoning_effort 则自动剥离重试）。
  if (api === 'openai-completions') {
    if (!requestedEffort) {
      delete next.thinking;
      delete next.reasoning_effort;
    } else if (requestedEffort === 'off') {
      if (isDeepSeek && reasoningModel) {
        next.thinking = { type: 'disabled' };
      } else {
        delete next.thinking;
      }
      delete next.reasoning_effort;
    } else if (isDeepSeek) {
      next.thinking = { type: 'enabled' };
      if (stripReasoningEffort) {
        delete next.reasoning_effort;
      } else {
        next.reasoning_effort = requestedEffort;
      }
    } else if (stripReasoningEffort) {
      delete next.reasoning_effort;
    }
  }
  if (Array.isArray(next.messages)) {
    const anyReasoning = next.messages.some(m => m?.role === 'assistant' && typeof m.reasoning_content === 'string');
    next.messages = next.messages.map(m => {
      if (!m || m.role !== 'assistant') return m;
      const copy = { ...m };
      if (copy.content === null || copy.content === undefined) copy.content = '';
      if ((isDeepSeek || anyReasoning) && (reasoningModel || anyReasoning)
        && Array.isArray(copy.tool_calls) && copy.tool_calls.length > 0
        && copy.reasoning_content === undefined) {
        copy.reasoning_content = '';
      }
      return copy;
    });
  }
  return next;
}

export async function runHarness(input, emit, invokeTool, signal) {
  const key = input.provider?.apiKey ?? '';
  const { api, contextWindow, maxTokens, reasoningModel, isDeepSeek, requestedEffort, supportedLevels, model, models } = await createProviderContext(input, key);
  const safeEmit = event => emit(JSON.parse(redact(event, [key])));

  const rawTools = input.tools ?? [];
  const allKnownToolNames = new Set(rawTools.map(tool => tool.name));
  for (const msg of input.messages ?? []) {
    if (msg?.role === 'toolResult' && msg.toolName) allKnownToolNames.add(msg.toolName);
    if (msg?.role === 'assistant' && Array.isArray(msg.content)) {
      for (const b of msg.content) if (b?.type === 'toolCall' && b.name) allKnownToolNames.add(b.name);
    }
    if (msg?.role === 'system') {
      for (const t of msg.toolsAdded ?? []) if (t?.name) allKnownToolNames.add(t.name);
      for (const t of msg.toolsRemoved ?? []) if (t?.name) allKnownToolNames.add(t.name);
    }
  }
  const toProvider = providerToolNames([...allKnownToolNames]);
  const toEditor = new Map([...toProvider].map(([editor, provider]) => [provider, editor]));
  const providerName = name => toProvider.get(name) ?? (String(name).replace(/[^a-zA-Z0-9_-]/g, '_').slice(0, 64) || 'tool');
  const editorName = name => toEditor.get(name) ?? name;

  const tools = rawTools.map(tool => ({
    name: providerName(tool.name),
    label: tool.description?.split('\n')[0] || tool.name,
    description: tool.description || tool.name,
    parameters: api === 'google-generative-ai' ? inlineSchemaRefs(tool.inputSchema) : tool.inputSchema,
    executionMode: 'sequential',
    execute: async (id, arguments_, toolSignal) => {
      const result = await invokeTool({ id, name: tool.name, arguments: arguments_ }, toolSignal);
      const text = redact(JSON.stringify(result), [key]);
      const bounded = text.length > 65536 ? JSON.stringify({
        result_truncated: true,
        note: '结果超过单次工具上下文预算，请缩小分页后重新查询。',
        prefix: text.slice(0, 63000),
      }) : text;
      return { content: [{ type: 'text', text: bounded }], details: { isError: Boolean(result.isError) } };
    },
  }));

  let clampedMaxTokens = 0;
  let stripReasoningEffort = false;
  const resolvedModel = models.getModel(input.provider.id, model.id);

  // 支持自动降级输出上限与旧网关 reasoning_effort 回退的流式包装。
  const resilientStreamFn = (targetModel, llmContext, options) => {
    const isAuxCompaction = Boolean(options?.forgeCompaction);
    const callEffort = isAuxCompaction ? '' : requestedEffort;
    const buildStreamOptions = (extraMaxTokens = undefined) => ({
      ...options,
      ...(extraMaxTokens !== undefined ? { maxTokens: extraMaxTokens } : {}),
      reasoning: callEffort && callEffort !== 'off' ? callEffort : undefined,
      cacheRetention: 'short',
      sessionId: input.sessionId || options?.sessionId,
      onPayload: async (params, m) => {
        const base = options?.onPayload ? (await options.onPayload(params, m)) ?? params : params;
        return normalizeWirePayload(base, {
          api,
          isDeepSeek,
          reasoningModel,
          requestedEffort: callEffort,
          stripReasoningEffort,
          clampedMaxTokens,
        });
      },
    });
    const primary = models.streamSimple(targetModel, llmContext, buildStreamOptions());
    let activeStream = primary;
    return {
      async *[Symbol.asyncIterator]() {
        let sawOutput = false;
        for await (const ev of primary) {
          if (ev.type === 'text_delta' || ev.type === 'thinking_delta' || ev.type === 'toolcall_delta' || ev.type === 'toolcall_end') {
            sawOutput = true;
          }
          if (!sawOutput && ev.type === 'error') {
            const errMsg = ev.error?.errorMessage;
            if (!stripReasoningEffort && callEffort && callEffort !== 'off' && isUnsupportedReasoningEffortError(errMsg)) {
              stripReasoningEffort = true;
              activeStream = models.streamSimple(targetModel, llmContext, buildStreamOptions(clampedMaxTokens || undefined));
              for await (const retryEv of activeStream) yield retryEv;
              return;
            }
            if (!clampedMaxTokens) {
              const cap = parseMaxTokensRangeError(errMsg);
              if (cap && cap < (targetModel.maxTokens || maxTokens)) {
                clampedMaxTokens = cap;
                activeStream = models.streamSimple({ ...targetModel, maxTokens: cap }, llmContext, buildStreamOptions(cap));
                for await (const retryEv of activeStream) yield retryEv;
                return;
              }
            }
          }
          yield ev;
        }
      },
      result: () => activeStream.result(),
    };
  };

  // 热前缀摘要回调：原样发送待压缩前缀，仅追加压缩指令 user 消息以复用提供商 KV Cache。
  const summarizeWithCache = async (prefixMessages, directive, sumSignal) => {
    safeEmit({ type: 'compaction_status', active: true, text: '正在归纳早期上下文检查点…' });
    const toolDecls = tools.map(toToolDeclaration);
    const hasSystem = prefixMessages[0]?.role === 'system';
    const sysMessages = hasSystem
      ? []
      : [createInitialSystemMessage(input.systemPrompt, toolDecls)].filter(Boolean);
    const llmMessages = [
      ...sysMessages,
      ...prefixMessages.filter(m => m.role === 'system' || m.role === 'user' || m.role === 'assistant' || m.role === 'toolResult'),
      { role: 'user', content: [{ type: 'text', text: directive }], timestamp: Date.now() },
    ];
    const response = resilientStreamFn(resolvedModel, normalizeContext({ messages: llmMessages }), {
      signal: sumSignal ?? signal,
      maxTokens: Math.min(4096, clampedMaxTokens || maxTokens),
      forgeCompaction: true,
    });
    let text = '';
    for await (const ev of response) {
      if (ev.type === 'text_delta') text += ev.delta;
    }
    const finalMsg = await response.result();
    if (finalMsg?.stopReason === 'error') throw new Error(finalMsg.errorMessage || '摘要生成失败');
    return text;
  };

  const emitCompactionIfAny = result => {
    if (result.compacted) {
      safeEmit({
        type: 'context_compacted',
        removed_messages: result.removedMessages,
        pruned_results: result.prunedCount,
        saved_tokens: result.savedTokens,
        summary: result.summary,
        text: `已压缩早期 ${result.removedMessages} 条消息（释放约 ${result.savedTokens} tokens）；原始记录完整保留。`,
      });
    } else if (result.pruned) {
      safeEmit({
        type: 'context_compacted',
        removed_messages: 0,
        pruned_results: result.prunedCount,
        saved_tokens: result.savedTokens,
        summary: '',
        text: `已修剪 ${result.prunedCount} 条早期超大工具输出（释放约 ${result.savedTokens} tokens）。`,
      });
    }
  };

  let initialMessages = renameHistoryTools(input.messages ?? [], providerName);
  const preCompact = await compactWithWarmCache({
    messages: initialMessages,
    systemPrompt: input.systemPrompt,
    tools,
    contextWindow,
    thresholdRatio: Number(input.compactionThresholdRatio) || DEFAULT_THRESHOLD_RATIO,
    retainRatio: DEFAULT_RETAIN_RATIO,
    summarizeFn: summarizeWithCache,
    signal,
  });
  initialMessages = preCompact.messages;

  const maxSteps = Number(input.maxSteps) > 0 ? Math.min(512, Number(input.maxSteps)) : 128;
  let requestCount = 0;
  let lastObservedPromptTokens = 0;
  let lengthContinuationCount = 0;
  let dsmlRetryCount = 0;
  let incompleteReason = '';
  let stepLimitReached = false;

  const initialThinkingLevel = requestedEffort
    ? requestedEffort
    : (reasoningModel ? (supportedLevels.find(lv => lv !== 'off') || 'high') : 'off');

  const agent = new Agent({
    sessionId: input.sessionId || undefined,
    initialState: {
      systemPrompt: input.systemPrompt,
      model: resolvedModel,
      messages: initialMessages,
      tools,
      thinkingLevel: initialThinkingLevel,
    },
    streamFn: resilientStreamFn,
    toolExecution: 'sequential',
    prepareRequest: async ({ context }) => {
      if (++requestCount > maxSteps) {
        stepLimitReached = true;
        throw new Error(`单次自动运行已达 ${maxSteps} 步上限，已完成进度与会话均已保存；可直接点击继续接着执行。`);
      }
      return { context };
    },
    prepareNextTurnWithContext: async (turn, turnSignal) => {
      // 每轮工具返回后检查上下文压力，超过 80% 阈值自动运行 DSH 三级压缩。
      const compacted = await compactWithWarmCache({
        messages: turn.context.messages,
        systemPrompt: input.systemPrompt,
        tools,
        contextWindow,
        observedPromptTokens: lastObservedPromptTokens,
        thresholdRatio: Number(input.compactionThresholdRatio) || DEFAULT_THRESHOLD_RATIO,
        retainRatio: DEFAULT_RETAIN_RATIO,
        summarizeFn: summarizeWithCache,
        signal: turnSignal ?? signal,
      });
      if (compacted.compacted || compacted.pruned) {
        lastObservedPromptTokens = Math.max(0, lastObservedPromptTokens - compacted.savedTokens);
        agent.state.messages = compacted.messages;
        emitCompactionIfAny(compacted);
        return { context: { ...turn.context, messages: compacted.messages } };
      }
      return undefined;
    },
    finishTurn: async turn => {
      const msg = turn.message;
      const toolCalls = Array.isArray(msg?.content) ? msg.content.filter(b => b?.type === 'toolCall') : [];

      // 每完成一个完整平衡回合即向宿主发送增量检查点，防止后续步骤出错导致前面进度丢失。
      const checkpoint = sanitizeTranscriptForSave(turn.context.messages, editorName);
      if (checkpoint.length > 0) {
        safeEmit({ type: 'turn_checkpoint', messages: checkpoint });
      }

      if (msg?.stopReason === 'length') {
        if (toolCalls.length > 0) {
          // pi-agent-core 已将截断的工具调用标为失败并准备继续，补充提示引导模型分批小步修改。
          agent.steer({
            role: 'user',
            timestamp: Date.now(),
            forge_hidden: true,
            forge_injection: 'length_continue',
            content: [{
              type: 'text',
              text: '上一步因单次输出达到最大长度上限导致工具参数被截断。请将复杂修改（如大型触发器编辑）拆分为多次较小的工具调用分步执行：先创建触发器骨架，再分批添加事件、条件与动作。',
            }],
          });
          return undefined;
        }
        // 在思考链或正文中耗尽单次输出上限、尚未发出工具调用时（典型表现：规划完触发器后直接假“本轮完成”），自动续跑！
        if (lengthContinuationCount < 3 && !signal?.aborted) {
          ++lengthContinuationCount;
          safeEmit({ type: 'status', text: `单次输出达到长度上限，正在自动分批续跑（${lengthContinuationCount}/3）…` });
          agent.steer({
            role: 'user',
            timestamp: Date.now(),
            forge_hidden: true,
            forge_injection: 'length_continue',
            content: [{
              type: 'text',
              text: '上一步在思考或输出中途达到单次 token 上限被截断，尚未发出工具调用。请不要重复长篇解释，直接发出下一步工具调用；若触发器节点较多，请分批多次调用完成。',
            }],
          });
          return { action: 'continue' };
        }
        incompleteReason = '输出连续达到单次最大长度上限被截断，未完成全部操作；当前进度已保存，可发送“继续”分批执行或在设置中调高输出上限。';
        return { action: 'end' };
      }

      // 防御 DeepSeek 偶发将工具调用以纯文本 <｜DSML｜function_calls> 输出而未走 tool_calls 通道导致提前停机。
      if (msg?.stopReason === 'stop' && toolCalls.length === 0) {
        const text = extractMessageText(msg);
        if (hasRawDsmlToolLeak(text) && dsmlRetryCount < 2 && !signal?.aborted) {
          ++dsmlRetryCount;
          agent.steer({
            role: 'user',
            timestamp: Date.now(),
            forge_hidden: true,
            forge_injection: 'dsml_continue',
            content: [{
              type: 'text',
              text: '检测到你将工具调用写成了纯文本 DSML 标签，系统未执行该调用。请通过标准 function calling 工具调用通道重新发起操作。',
            }],
          });
          return { action: 'continue' };
        }
      }
      return undefined;
    },
  });

  const abort = () => agent.abort();
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) abort();

  let lastError;
  agent.subscribe(event => {
    if (event.type === 'message_update') {
      const sub = event.assistantMessageEvent;
      if (sub?.type === 'text_delta' && sub.delta) {
        safeEmit({ type: 'assistant_delta', text: sub.delta });
      } else if (sub?.type === 'thinking_delta' && sub.delta) {
        safeEmit({ type: 'thinking_delta', text: sub.delta });
      }
    } else if (event.type === 'tool_execution_start') {
      safeEmit({
        type: 'tool_start',
        id: event.toolCallId,
        tool_name: editorName(event.toolName),
        title: editorName(event.toolName),
        details: { arguments: event.args },
      });
    } else if (event.type === 'tool_execution_end') {
      const isError = Boolean(event.isError || event.result?.details?.isError);
      safeEmit({
        type: 'tool_end',
        id: event.toolCallId,
        tool_name: editorName(event.toolName),
        success: !isError,
        details: { result: event.result },
      });
    } else if (event.type === 'message_end' && event.message.role === 'assistant') {
      const message = event.message;
      // PI 在未收到站点用量时也保留全零占位，不能展示成真实的 0 tokens。
      const available = message.usage && ['input', 'output', 'cacheRead', 'cacheWrite', 'totalTokens']
        .some(k => Number.isFinite(message.usage[k]) && message.usage[k] > 0);
      if (available) {
        const { cost: _unavailableCost, ...tokenUsage } = message.usage;
        const uncachedInput = Math.max(0, Number(message.usage.input || 0));
        const cacheRead = Math.max(0, Number(message.usage.cacheRead || 0));
        const cacheWrite = Math.max(0, Number(message.usage.cacheWrite || 0));
        const totalInput = uncachedInput + cacheRead + cacheWrite;
        const outputTokens = Math.max(0, Number(message.usage.output || 0));
        const reasoningTokens = Math.max(0, Number(message.usage.reasoning || message.usage.reasoningTokens || 0));
        const contextTokens = totalInput + outputTokens;
        const totalTokens = Math.max(contextTokens, Number(message.usage.totalTokens || 0));
        lastObservedPromptTokens = contextTokens;
        safeEmit({
          type: 'usage',
          available: true,
          details: tokenUsage,
          cost_available: false,
          input_tokens: totalInput,
          uncached_input_tokens: uncachedInput,
          cache_read_tokens: cacheRead,
          cache_write_tokens: cacheWrite,
          output_tokens: outputTokens,
          reasoning_tokens: reasoningTokens,
          context_tokens: contextTokens,
          total_tokens: totalTokens,
          estimated: false,
        });
      } else {
        safeEmit({
          type: 'usage',
          available: false,
          cost_available: false,
          input_tokens: null,
          output_tokens: null,
          estimated: false,
        });
      }
      if (message.stopReason === 'error') lastError = message.errorMessage || '模型请求失败。';
    }
  });

  safeEmit({ type: 'run_start', text: '正在处理地图任务', context_window: contextWindow });
  if (preCompact.compacted || preCompact.pruned) emitCompactionIfAny(preCompact);

  try {
    await agent.prompt(input.prompt);

    // 第 3 级：若因提供商判定上下文超限报错，强制执行一次紧急压缩并自动续跑一次。
    if (lastError && isContextOverflowError(lastError) && !signal?.aborted) {
      const cleanedBeforeRetry = sanitizeTranscriptForSave(agent.state.messages, x => x);
      const emergency = await compactWithWarmCache({
        messages: cleanedBeforeRetry,
        systemPrompt: input.systemPrompt,
        tools,
        contextWindow,
        force: true,
        retainRatio: 0.10,
        summarizeFn: summarizeWithCache,
        signal,
      });
      if (emergency.compacted || emergency.pruned) {
        lastError = undefined;
        emitCompactionIfAny(emergency);
        agent.state.messages = emergency.messages;
        const tailRole = emergency.messages[emergency.messages.length - 1]?.role;
        if (tailRole === 'user' || tailRole === 'toolResult') {
          await agent.continue();
        }
      }
    }

    if (lastError) throw new Error(lastError);
    if (incompleteReason) throw new Error(incompleteReason);

    const savedMessages = sanitizeTranscriptForSave(agent.state.messages, editorName);
    safeEmit({ type: 'session_state', messages: savedMessages });
    safeEmit({
      type: 'run_end',
      success: !signal?.aborted,
      text: signal?.aborted ? '已停止，保留已完成改动。' : '本次任务结束。',
    });
    return savedMessages;
  } finally {
    if (lastError || incompleteReason) {
      const savedMessages = sanitizeTranscriptForSave(agent.state.messages, editorName);
      safeEmit({
        type: 'session_state',
        messages: savedMessages,
        partial: true,
        step_limit_reached: stepLimitReached,
      });
    }
    signal?.removeEventListener('abort', abort);
  }
}

export async function runManualCompaction(input, emit, signal) {
  const key = input.provider?.apiKey ?? '';
  const safeEmit = event => emit(JSON.parse(redact(event, [key])));
  const rawTools = input.tools ?? [];
  const toProvider = providerToolNames(rawTools.map(t => t.name));
  const toEditor = new Map([...toProvider].map(([editor, provider]) => [provider, editor]));
  const providerName = name => toProvider.get(name) ?? (String(name).replace(/[^a-zA-Z0-9_-]/g, '_').slice(0, 64) || 'tool');
  const editorName = name => toEditor.get(name) ?? name;

  let summarizeFn = null;
  let contextWindow = Number(input.model?.contextWindow ?? 262144);
  if (input.provider?.protocol && input.provider?.baseUrl && input.model?.id) {
    try {
      const ctx = await createProviderContext(input, key);
      contextWindow = ctx.contextWindow;
      const resolvedModel = ctx.models.getModel(input.provider.id, ctx.model.id);
      const toolDecls = rawTools.map(t => toToolDeclaration({
        name: providerName(t.name),
        description: t.description || t.name,
        parameters: t.inputSchema ?? { type: 'object', properties: {} },
      }));
      summarizeFn = async (prefixMessages, directive, sumSignal) => {
        safeEmit({ type: 'compaction_status', active: true, text: '正在压缩会话上下文…' });
        const hasSystem = prefixMessages[0]?.role === 'system';
        const sys = hasSystem ? [] : [createInitialSystemMessage(input.systemPrompt, toolDecls)].filter(Boolean);
        const llmMessages = [
          ...sys,
          ...prefixMessages.filter(m => m.role === 'system' || m.role === 'user' || m.role === 'assistant' || m.role === 'toolResult'),
          { role: 'user', content: [{ type: 'text', text: directive }], timestamp: Date.now() },
        ];
        const response = ctx.models.streamSimple(resolvedModel, normalizeContext({ messages: llmMessages }), {
          signal: sumSignal ?? signal,
          maxTokens: 4096,
          cacheRetention: 'short',
          sessionId: input.sessionId || undefined,
          onPayload: params => normalizeWirePayload(params, {
            api: ctx.api,
            isDeepSeek: ctx.isDeepSeek,
            reasoningModel: ctx.reasoningModel,
            requestedEffort: '',
            stripReasoningEffort: false,
            clampedMaxTokens: 0,
          }),
        });
        let text = '';
        for await (const ev of response) {
          if (ev.type === 'text_delta') text += ev.delta;
        }
        const finalMsg = await response.result();
        if (finalMsg?.stopReason === 'error') throw new Error(finalMsg.errorMessage || '摘要生成失败');
        return text;
      };
    } catch {
      summarizeFn = null;
    }
  }

  const providerMessages = renameHistoryTools(input.messages ?? [], providerName);
  const result = await compactWithWarmCache({
    messages: providerMessages,
    systemPrompt: input.systemPrompt ?? '',
    tools: rawTools,
    contextWindow,
    force: true,
    retainRatio: 0.12,
    summarizeFn,
    signal,
  });

  const savedMessages = sanitizeTranscriptForSave(result.messages, editorName);
  if (result.compacted || result.pruned) {
    safeEmit({
      type: 'context_compacted',
      removed_messages: result.removedMessages,
      pruned_results: result.prunedCount,
      saved_tokens: result.savedTokens,
      summary: result.summary,
      manual: true,
      text: result.compacted
        ? `已手动压缩会话：归纳早期 ${result.removedMessages} 条消息（释放约 ${result.savedTokens} tokens），原始记录完整保留。`
        : `已修剪 ${result.prunedCount} 条超大工具结果（释放约 ${result.savedTokens} tokens）。`,
    });
    safeEmit({ type: 'session_state', messages: savedMessages });
    safeEmit({ type: 'compact_end', success: true, compacted: true, text: '会话上下文已压缩。' });
  } else {
    safeEmit({ type: 'compact_end', success: true, compacted: false, text: '当前会话较短，无需压缩。' });
  }
  return savedMessages;
}

export { estimateSurfaceTokens };

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
    else if (input.type === 'compact') {
      if (active) { emit({ type: 'error', text: '已有任务正在运行。' }); continue; }
      aborter = new AbortController();
      active = runManualCompaction(input, emit, aborter.signal)
        .catch(error => {
          emit({ type: 'error', text: redact(error.message, [input.provider?.apiKey]) });
          emit({ type: 'compact_end', success: false, compacted: false, text: '压缩未完成，原上下文保留。' });
        })
        .finally(() => { active = undefined; aborter = undefined; pending.clear(); });
    } else if (input.type === 'start') {
      if (active) { emit({ type: 'error', text: '已有任务正在运行。' }); continue; }
      aborter = new AbortController();
      active = runHarness(input, emit, invokeTool, aborter.signal)
        .catch(error => {
          emit({ type: 'error', text: redact(error.message, [input.provider?.apiKey]) });
          emit({ type: 'run_end', success: false, text: '任务未完成，已保留现有改动。' });
        })
        .finally(() => { active = undefined; aborter = undefined; pending.clear(); });
    }
  }
  aborter?.abort();
  if (active) await active;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  runStdio().catch(() => process.exitCode = 1);
}
