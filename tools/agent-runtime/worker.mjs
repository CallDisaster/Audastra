import { Agent } from '@earendil-works/pi-agent-core';
import { createInitialSystemMessage, normalizeContext, toToolDeclaration } from '@earendil-works/pi-ai';
import { createModels, createProvider } from '@earendil-works/pi-ai/models';
import { ANTHROPIC_MODELS } from '@earendil-works/pi-ai/providers/anthropic.models';
import { createInterface } from 'node:readline';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import {
  DEFAULT_THRESHOLD_RATIO,
  DEFAULT_RETAIN_RATIO,
  compactWithWarmCache,
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

// 工具名映射同时收集当前启用工具与历史中出现过的工具（含已停用/已删除），保证历史名称双向还原一致。
export function createToolNameMaps(rawTools, messages) {
  const names = new Set((rawTools ?? []).map(tool => tool.name));
  for (const msg of messages ?? []) {
    if (msg?.role === 'toolResult' && msg.toolName) names.add(msg.toolName);
    if (msg?.role === 'assistant' && Array.isArray(msg.content)) {
      for (const b of msg.content) if (b?.type === 'toolCall' && b.name) names.add(b.name);
    }
    if (msg?.role === 'system') {
      for (const t of msg.toolsAdded ?? []) if (t?.name) names.add(t.name);
      for (const t of msg.toolsRemoved ?? []) if (t?.name) names.add(t.name);
    }
  }
  const toProvider = providerToolNames([...names]);
  const toEditor = new Map([...toProvider].map(([editor, provider]) => [provider, editor]));
  return {
    providerName: name => toProvider.get(name) ?? (String(name).replace(/[^a-zA-Z0-9_-]/g, '_').slice(0, 64) || 'tool'),
    editorName: name => toEditor.get(name) ?? name,
  };
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

// Claude 型号 ID 归一：去掉 OpenRouter/Bedrock/Vertex 的前缀与日期、版本后缀，版本号中的点号换成连字符。
function normalizeClaudeSlug(modelId) {
  let slug = String(modelId ?? '').trim().toLowerCase();
  slug = slug.slice(slug.lastIndexOf('/') + 1);
  slug = slug.replace(/\[[^\]]*\]$/, '').replace(/@.*$/, '').replace(/:.*$/, '');
  slug = slug.replace(/^(?:[a-z]{2,6}\.)?anthropic\./, '');
  slug = slug.replace(/-v\d+$/, '').replace(/-latest$/, '').replace(/-\d{8}$/, '');
  return slug.replace(/(\d)\.(\d)/g, '$1-$2');
}

// Claude 思考参数形态的唯一判定入口。
// adaptive：只接受 thinking.type=adaptive + output_config.effort，budget_tokens 会被拒绝或已废弃
//   （Opus/Sonnet 4.6 起、Haiku 5 起、Fable、Mythos）；其余旧型号与第三方 Anthropic 兼容模型走 budget_tokens。
// xhigh：接口是否接受 effort=xhigh；canDisable：是否接受 thinking.type=disabled。
// 优先读 pi-ai 内置目录的 compat.forceAdaptiveThinking 与 thinkingLevelMap，目录未收录时按型号族与版本推断。
export function anthropicThinkingProfile(modelId) {
  const slug = normalizeClaudeSlug(modelId);
  const known = Object.hasOwn(ANTHROPIC_MODELS, slug) ? ANTHROPIC_MODELS[slug] : undefined;
  if (known?.reasoning) {
    const map = known.thinkingLevelMap ?? {};
    return {
      adaptive: known.compat?.forceAdaptiveThinking === true,
      xhigh: map.xhigh === 'xhigh',
      canDisable: map.off !== null,
      source: 'catalog',
    };
  }
  if (/(?:^|[-_.])(?:fable|mythos)(?:$|[-_.\d])/.test(slug)) {
    return { adaptive: true, xhigh: true, canDisable: false, source: 'pattern' };
  }
  const family = slug.match(/claude-(opus|sonnet|haiku)(?:-(\d{1,2})(?:-(\d{1,2}))?(?!\d))?/);
  if (family) {
    // 不带版本号的别名按最新一代处理。
    const version = family[2] === undefined ? Infinity : Number(family[2]) + Number(family[3] ?? 0) / 10;
    if (family[1] === 'haiku') {
      return version >= 5
        ? { adaptive: true, xhigh: true, canDisable: false, source: 'pattern' }
        : { adaptive: false, xhigh: false, canDisable: true, source: 'pattern' };
    }
    if (version >= 4.6) {
      return {
        adaptive: true,
        xhigh: family[1] === 'opus' ? version >= 4.7 : version >= 5,
        canDisable: family[1] === 'opus' ? version < 5 : version < 5.5,
        source: 'pattern',
      };
    }
  }
  return { adaptive: false, xhigh: false, canDisable: true, source: 'pattern' };
}

// 新 Claude 的 effort 档位：minimal 并入 low；不支持 xhigh 的型号降到 high，max 始终保持独立。
function anthropicAdaptiveLevelMap(profile) {
  return {
    off: profile.canDisable ? undefined : null,
    minimal: 'low',
    low: 'low',
    medium: 'medium',
    high: 'high',
    xhigh: profile.xhigh ? 'xhigh' : 'high',
    max: 'max',
  };
}

// 旧 Claude 的 budget_tokens：每个档位独立取值，pi-ai 会再压到 max_tokens 以下并留出作答空间。
const ANTHROPIC_THINKING_BUDGETS = { minimal: 1024, low: 2048, medium: 8192, high: 16384, xhigh: 24576, max: 32768 };

function claudeDefaultMaxTokens(modelId) {
  const slug = normalizeClaudeSlug(modelId);
  if (/claude-3-(?:opus|sonnet|haiku)\b|claude-3-haiku/.test(slug)) return 4096;
  if (/claude-3-5-/.test(slug)) return 8192;
  return /claude-|(?:^|[-_.])(?:fable|mythos)/.test(slug) ? 32000 : 0;
}

export function resolveDefaultMaxTokens(inputModel, baseUrl, contextWindow) {
  const explicit = Number(inputModel?.maxOutputTokens ?? 0);
  if (Number.isFinite(explicit) && explicit > 0) {
    return Math.min(explicit, Math.max(256, contextWindow));
  }
  const id = String(inputModel?.id ?? '').toLowerCase();
  const host = String(baseUrl ?? '').toLowerCase();
  const claudeCap = claudeDefaultMaxTokens(id);
  if (claudeCap > 0) return Math.min(claudeCap, Math.max(4096, Math.floor(contextWindow / 2)));
  if (isReasoningCapableModel(id, host) || /gpt-5|gemini-/.test(id)) {
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

const MAX_TOKENS_FIELD_RE = /max_tokens|max_completion_tokens|max_output_tokens|maxoutputtokens|max output tokens/i;

// 解析站点返回的输出上限错误，得到允许的最大值；只认出字段名、认不出数值时退到 8192。
export function parseMaxTokensRangeError(errorMessage) {
  const msg = String(errorMessage ?? '');
  if (!MAX_TOKENS_FIELD_RE.test(msg)) return null;
  const patterns = [
    [/\[\s*1\s*,\s*(\d+)\s*\)/, -1],
    [/\[\s*1\s*,\s*(\d+)\s*\]/, 0],
    [/to\s+(\d+)\s*\(exclusive\)/i, -1],
    [/to\s+(\d+)\s*\(inclusive\)/i, 0],
    [/<=\s*(\d+)/, 0],
    [/>\s*(\d+)/, 0],
    [/at most\s+(\d+)/i, 0],
    [/maximum[^0-9]*?\b(\d{3,7})\b/i, 0],
  ];
  for (const [pattern, delta] of patterns) {
    const match = msg.match(pattern);
    if (!match) continue;
    const parsed = Number(match[1]) + delta;
    if (Number.isSafeInteger(parsed) && parsed >= 256) return parsed;
  }
  return 8192;
}

// 站点拒绝思考/推理参数（字段未知、取值不支持等）的 400。
function isUnsupportedThinkingParamError(errorMessage) {
  const msg = String(errorMessage ?? '');
  return /\b400\b|invalid|unknown|unrecognized|unsupported|not supported|not permitted|not allowed|extra inputs|does not support/i.test(msg)
    && /reasoning_effort|reasoning|thinking|output_config|effort|budget_tokens/i.test(msg);
}

function createAbortError() {
  const error = new Error('已停止。');
  error.name = 'AbortError';
  return error;
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
  // 模型具备思考能力（只用于选择线协议形态）；是否真的发送思考参数由每次请求的强度决定。
  const reasoningCapable = supportedLevels.some(lv => lv !== 'off')
    || (requestedEffort !== '' && requestedEffort !== 'off');

  const thinkingLevelMap = {};
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
    // 注册时不声明思考能力：未选择强度时任何协议都不应由 pi-ai 自动补发思考参数。
    reasoning: false,
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

  return {
    api,
    contextWindow,
    maxTokens,
    reasoningCapable,
    isDeepSeek,
    requestedEffort,
    supportedLevels,
    thinkingLevelMap,
    anthropicProfile: api === 'anthropic-messages' ? anthropicThinkingProfile(input.model.id) : undefined,
    model,
    models,
  };
}

// 按协议生成一次请求的思考参数（模型字段覆盖 + 流选项）。
// effort 为空：任何协议都不发送思考/推理参数，交给接口默认；fallbackLevel>=2 表示站点拒绝过思考参数，同样不发送。
function thinkingRequestFor(ctx, effort, fallbackLevel) {
  if (!effort || fallbackLevel >= 2) {
    return { model: { reasoning: false }, options: { reasoning: undefined, thinkingBudgets: undefined } };
  }
  if (ctx.api === 'anthropic-messages') {
    const profile = ctx.anthropicProfile;
    if (profile.adaptive) {
      // 不能关闭思考的新型号把 off 压到最低档 low。
      const reasoning = effort === 'off' ? (profile.canDisable ? undefined : 'low') : effort;
      return {
        model: {
          reasoning: true,
          thinkingLevelMap: anthropicAdaptiveLevelMap(profile),
          compat: { ...(ctx.model.compat ?? {}), forceAdaptiveThinking: true },
        },
        options: { reasoning, thinkingBudgets: undefined },
      };
    }
    if (effort === 'off') {
      return { model: { reasoning: true, thinkingLevelMap: {} }, options: { reasoning: undefined, thinkingBudgets: undefined } };
    }
    const piLevel = ['minimal', 'low', 'medium', 'high'].includes(effort) ? effort : 'high';
    return {
      model: { reasoning: true, thinkingLevelMap: {} },
      options: { reasoning: piLevel, thinkingBudgets: { [piLevel]: ANTHROPIC_THINKING_BUDGETS[effort] } },
    };
  }
  return {
    model: { reasoning: ctx.reasoningCapable || effort !== 'off', thinkingLevelMap: ctx.thinkingLevelMap },
    options: { reasoning: effort !== 'off' ? effort : undefined, thinkingBudgets: undefined },
  };
}

function normalizeWirePayload(params, { api, isDeepSeek, reasoningModel, requestedEffort, stripReasoningEffort }) {
  if (!params || typeof params !== 'object') return params;
  const next = { ...params };
  // 对齐 deepseek-harness serialize.ts 的思考强度线协议：
  // - 未显式选择强度（默认 ""）：不覆写接口默认，移除 thinking 与 reasoning_effort；
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

// 一次运行内共用的请求执行器：统一思考参数、输出上限与失败回退。
// - 站点拒绝输出上限时解析允许值，本次运行后续所有请求（含摘要）都沿用降级后的上限；
// - 站点拒绝思考参数时，Chat 旧网关先只剥离 reasoning_effort，其余情况改为不发送思考参数，同样在本次运行内保持。
function createRequestRunner(ctx, input, onStatus = undefined) {
  const state = { maxTokens: ctx.maxTokens, thinkingFallback: 0 };

  const adjustAfterError = (errorMessage, effort) => {
    const text = String(errorMessage ?? '');
    const mentionsMaxTokens = MAX_TOKENS_FIELD_RE.test(text);
    if (!mentionsMaxTokens && effort && state.thinkingFallback < 2 && isUnsupportedThinkingParamError(text)) {
      const stripOnly = state.thinkingFallback === 0 && ctx.api === 'openai-completions' && /reasoning_effort/i.test(text);
      state.thinkingFallback = stripOnly ? 1 : 2;
      if (!stripOnly) onStatus?.('站点不接受所选思考强度参数，本次运行改用接口默认思考设置。');
      return true;
    }
    if (mentionsMaxTokens) {
      const cap = parseMaxTokensRangeError(text);
      if (cap && cap < state.maxTokens) {
        state.maxTokens = cap;
        onStatus?.(`站点限制单次输出上限为 ${cap} tokens，本次运行已按此上限重试。`);
        return true;
      }
    }
    return false;
  };

  const stream = (targetModel, llmContext, options = {}) => {
    const { forgeCompaction, ...rest } = options;
    const effort = forgeCompaction ? '' : ctx.requestedEffort;
    const start = () => {
      const thinking = thinkingRequestFor(ctx, effort, state.thinkingFallback);
      const modelCap = Math.min(targetModel.maxTokens || state.maxTokens, state.maxTokens);
      const callModel = { ...targetModel, ...thinking.model, maxTokens: modelCap };
      return ctx.models.streamSimple(callModel, llmContext, {
        ...rest,
        ...thinking.options,
        maxTokens: Math.min(rest.maxTokens ?? modelCap, modelCap),
        cacheRetention: 'short',
        sessionId: input.sessionId || rest.sessionId,
        onPayload: async (params, m) => {
          const base = rest.onPayload ? (await rest.onPayload(params, m)) ?? params : params;
          return normalizeWirePayload(base, {
            api: ctx.api,
            isDeepSeek: ctx.isDeepSeek,
            reasoningModel: ctx.reasoningCapable,
            requestedEffort: state.thinkingFallback >= 2 ? '' : effort,
            stripReasoningEffort: state.thinkingFallback >= 1,
          });
        },
      });
    };
    let active = start();
    return {
      async *[Symbol.asyncIterator]() {
        for (let attempt = 0; ; ++attempt) {
          let yielded = false;
          let retry = false;
          for await (const ev of active) {
            // 只在尚未向上游交付任何事件（站点直接拒绝请求）时调整参数重试。
            if (!yielded && ev.type === 'error' && attempt < 3 && !rest.signal?.aborted
              && adjustAfterError(ev.error?.errorMessage, effort)) {
              retry = true;
              break;
            }
            yielded = true;
            yield ev;
          }
          if (!retry) return;
          active = start();
        }
      },
      result: () => active.result(),
    };
  };

  return { state, stream };
}

const SUMMARY_MAX_TOKENS = 8192;
const ZERO_USAGE = { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, totalTokens: 0,
  cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };

// 旧版或外部导入的历史可能缺少 usage；pi-ai 估算上下文时会读取它，缺失会让整次请求失败。补零占位，零值不会被当作用量。
function withUsagePlaceholders(messages) {
  return messages.map(m => (m?.role === 'assistant' && !m.usage ? { ...m, usage: ZERO_USAGE } : m));
}

// 摘要请求：原样复用待压缩前缀（热缓存），末尾追加压缩指令。
// 只有正常结束（stop）才算成功；截断、中断、调用工具或报错都抛出，由压缩模块保持历史或回退确定性摘要。
function createSummarizer({ runner, model, systemPrompt, toolDecls, sessionId, onStart }) {
  return async (prefixMessages, directive, sumSignal) => {
    if (sumSignal?.aborted) throw createAbortError();
    onStart?.();
    const hasSystem = prefixMessages[0]?.role === 'system';
    const llmMessages = [
      ...(hasSystem ? [] : [createInitialSystemMessage(systemPrompt, toolDecls)].filter(Boolean)),
      ...withUsagePlaceholders(prefixMessages.filter(m => m.role === 'system' || m.role === 'user' || m.role === 'assistant' || m.role === 'toolResult')),
      { role: 'user', content: [{ type: 'text', text: directive }], timestamp: Date.now() },
    ];
    const response = runner.stream(model, normalizeContext({ messages: llmMessages }), {
      signal: sumSignal,
      maxTokens: SUMMARY_MAX_TOKENS,
      forgeCompaction: true,
      sessionId,
    });
    let text = '';
    for await (const ev of response) {
      if (ev.type === 'text_delta') text += ev.delta;
    }
    const finalMsg = await response.result();
    if (sumSignal?.aborted || finalMsg?.stopReason === 'aborted') throw createAbortError();
    if (finalMsg?.stopReason !== 'stop') {
      throw new Error(finalMsg?.errorMessage || `摘要未正常结束（${finalMsg?.stopReason ?? '未知'}）。`);
    }
    return text;
  };
}

const MAX_LENGTH_CONTINUATIONS = 3;
const MAX_OVERFLOW_RECOVERIES = 3;

export async function runHarness(input, emit, invokeTool, signal) {
  const key = input.provider?.apiKey ?? '';
  const ctx = await createProviderContext(input, key);
  const { api, contextWindow, requestedEffort, models } = ctx;
  const safeEmit = event => emit(JSON.parse(redact(event, [key])));

  const rawTools = input.tools ?? [];
  const { providerName, editorName } = createToolNameMaps(rawTools, input.messages);

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

  const runner = createRequestRunner(ctx, input, text => safeEmit({ type: 'status', text }));
  const resolvedModel = models.getModel(input.provider.id, ctx.model.id);
  const summarizeWithCache = createSummarizer({
    runner,
    model: resolvedModel,
    systemPrompt: input.systemPrompt,
    toolDecls: tools.map(toToolDeclaration),
    sessionId: input.sessionId || undefined,
    onStart: () => safeEmit({ type: 'compaction_status', active: true, text: '正在归纳早期上下文检查点…' }),
  });
  // 本次运行内共享的压缩参数；attemptMemo 记录已被拒绝的区间，避免每回合重复无效摘要请求。
  const compactionOptions = {
    systemPrompt: input.systemPrompt,
    tools,
    contextWindow,
    thresholdRatio: Number(input.compactionThresholdRatio) || DEFAULT_THRESHOLD_RATIO,
    summarizeFn: summarizeWithCache,
    attemptMemo: new Set(),
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

  let initialMessages = withUsagePlaceholders(renameHistoryTools(input.messages ?? [], providerName));
  const preCompact = await compactWithWarmCache({
    ...compactionOptions,
    messages: initialMessages,
    retainRatio: DEFAULT_RETAIN_RATIO,
    signal,
  });
  if (signal?.aborted) {
    // 预压缩阶段收到停止：不再发送主请求，历史保持原样（本次提交的输入由宿主保留）。
    safeEmit({ type: 'run_end', success: false, text: '已停止，未发送模型请求。' });
    return sanitizeTranscriptForSave(initialMessages, editorName);
  }
  initialMessages = preCompact.messages;

  const maxSteps = Number(input.maxSteps) > 0 ? Math.min(512, Number(input.maxSteps)) : 128;
  let requestCount = 0;
  let lastObservedPromptTokens = 0;
  let lengthContinuationCount = 0;
  let dsmlRetryCount = 0;
  let incompleteReason = '';
  let stepLimitReached = false;

  const agent = new Agent({
    sessionId: input.sessionId || undefined,
    initialState: {
      systemPrompt: input.systemPrompt,
      model: resolvedModel,
      messages: initialMessages,
      tools,
      // 只用于记录到助手消息；实际思考参数由请求执行器按所选强度生成。
      thinkingLevel: requestedEffort || 'off',
    },
    streamFn: runner.stream,
    toolExecution: 'sequential',
    prepareRequest: async ({ context }, loopSignal) => {
      if (signal?.aborted || loopSignal?.aborted) {
        // 停止后不再发出新请求；中止当前运行，由运行时记为 aborted 而不是错误。
        agent.abort();
        throw createAbortError();
      }
      if (++requestCount > maxSteps) {
        stepLimitReached = true;
        throw new Error(`单次自动运行已达 ${maxSteps} 步上限，已完成进度与会话均已保存；可直接点击继续接着执行。`);
      }
      return { context };
    },
    prepareNextTurnWithContext: async (turn, turnSignal) => {
      // 每轮工具返回后检查上下文压力，超过阈值自动运行三级压缩。
      const compacted = await compactWithWarmCache({
        ...compactionOptions,
        messages: turn.context.messages,
        observedPromptTokens: lastObservedPromptTokens,
        retainRatio: DEFAULT_RETAIN_RATIO,
        signal: turnSignal && signal ? AbortSignal.any([turnSignal, signal]) : (turnSignal ?? signal),
      });
      if (compacted.aborted) return undefined;
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
        // 截断续跑与工具参数截断共用一个连续次数上限，避免每步都耗尽输出上限直到步数用完。
        if (lengthContinuationCount >= MAX_LENGTH_CONTINUATIONS || signal?.aborted) {
          incompleteReason = '输出连续达到单次最大长度上限被截断，未完成全部操作；当前进度已保存，可发送“继续”分批执行或在设置中调高输出上限。';
          return { action: 'end' };
        }
        ++lengthContinuationCount;
        safeEmit({ type: 'status', text: `单次输出达到长度上限，正在自动分批续跑（${lengthContinuationCount}/${MAX_LENGTH_CONTINUATIONS}）…` });
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
        // 正文或思考在中途被截断：请模型从中断处接着输出。
        agent.steer({
          role: 'user',
          timestamp: Date.now(),
          forge_hidden: true,
          forge_injection: 'length_continue',
          content: [{
            type: 'text',
            text: tools.length > 0
              ? '上一步输出达到单次长度上限被截断。请从中断处直接接着写，不要重复已输出的内容；如果接下来需要修改地图，直接发起工具调用并分批执行。'
              : '上一步输出达到单次长度上限被截断。请从中断处直接接着写，不要重复已输出的内容。',
          }],
        });
        return { action: 'continue' };
      }
      // 正常完成的一步说明已有进展，重新计算连续截断次数。
      if (msg?.stopReason === 'stop' || msg?.stopReason === 'toolUse') lengthContinuationCount = 0;

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
      // 停止后 pi-ai 的请求准备阶段会以 error 结束（"This operation was aborted"），按停止处理而不是失败。
      if (message.stopReason === 'error' && !signal?.aborted) lastError = message.errorMessage || '模型请求失败。';
    }
  });

  safeEmit({ type: 'run_start', text: '正在处理地图任务', context_window: contextWindow });
  if (preCompact.compacted || preCompact.pruned) emitCompactionIfAny(preCompact);

  try {
    await agent.prompt(input.prompt);

    // 第 3 级：提供商判定上下文超限时强制紧急压缩并自动续跑；每次都必须真正缩小，次数有上限。
    for (let recovery = 0; recovery < MAX_OVERFLOW_RECOVERIES
      && lastError && isContextOverflowError(lastError) && !signal?.aborted; ++recovery) {
      const cleanedBeforeRetry = sanitizeTranscriptForSave(agent.state.messages, x => x);
      const emergency = await compactWithWarmCache({
        ...compactionOptions,
        messages: cleanedBeforeRetry,
        force: true,
        retainRatio: 0.10,
        signal,
      });
      if (emergency.aborted || signal?.aborted || !(emergency.compacted || emergency.pruned)) break;
      lastError = undefined;
      emitCompactionIfAny(emergency);
      agent.state.messages = emergency.messages;
      const tailRole = emergency.messages[emergency.messages.length - 1]?.role;
      if (tailRole !== 'user' && tailRole !== 'toolResult') break;
      await agent.continue();
    }

    // 用户停止不是错误：保存已完成进度并以“已停止”收口。
    const stopped = Boolean(signal?.aborted);
    if (stopped) {
      lastError = undefined;
      incompleteReason = '';
    }
    if (lastError) throw new Error(lastError);
    if (incompleteReason) throw new Error(incompleteReason);

    const savedMessages = sanitizeTranscriptForSave(agent.state.messages, editorName);
    safeEmit({ type: 'session_state', messages: savedMessages });
    safeEmit({
      type: 'run_end',
      success: !stopped,
      text: stopped ? '已停止，保留已完成改动。' : '本次任务结束。',
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
  const { providerName, editorName } = createToolNameMaps(rawTools, input.messages);

  let summarizeFn = null;
  let contextWindow = Number(input.model?.contextWindow ?? 262144);
  if (input.provider?.protocol && input.provider?.baseUrl && input.model?.id) {
    try {
      const ctx = await createProviderContext(input, key);
      contextWindow = ctx.contextWindow;
      const toolDecls = rawTools.map(t => toToolDeclaration({
        name: providerName(t.name),
        description: t.description || t.name,
        parameters: (ctx.api === 'google-generative-ai' ? inlineSchemaRefs(t.inputSchema) : t.inputSchema)
          ?? { type: 'object', properties: {} },
      }));
      summarizeFn = createSummarizer({
        runner: createRequestRunner(ctx, input),
        model: ctx.models.getModel(input.provider.id, ctx.model.id),
        systemPrompt: input.systemPrompt,
        toolDecls,
        sessionId: input.sessionId || undefined,
        onStart: () => safeEmit({ type: 'compaction_status', active: true, text: '正在压缩会话上下文…' }),
      });
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

  if (result.aborted || signal?.aborted) {
    // 停止的手动压缩不产出会话状态，宿主保留原历史。
    safeEmit({ type: 'compact_end', success: false, compacted: false, cancelled: true, text: '已停止压缩，原上下文保留。' });
    return sanitizeTranscriptForSave(providerMessages, editorName);
  }

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
    safeEmit({ type: 'compact_end', success: true, compacted: false, text: '当前会话没有可以缩小的早期内容，未压缩。' });
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
