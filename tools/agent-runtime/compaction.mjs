// DSH 风格三级上下文压缩与分项 Token 计量模块（原创实现，参考 MIT 协议 deepseek-harness 设计）：
// 1. TokenMeter：区分中日韩字符、拉丁字符与工具 JSON Schema 的分项启发式计量，支持提供商真实 usage 锚点校准；
// 2. 第 1 级（ToolResultPruner）：确定性修剪早期超大工具输出（头 4096 + 尾 1024 字符），零模型调用开销；
// 3. 第 2 级（Warm-Cache Summarizer）：保持工具调用配对平衡，复用提供商前缀 KV Cache 生成结构化检查点，完整保留用户原始需求；
// 4. 第 3 级（Overflow / Manual Compact）：上下文超限紧急恢复与用户手动「压缩会话」。

export const DEFAULT_THRESHOLD_RATIO = 0.80;
export const DEFAULT_RETAIN_RATIO = 0.16;
export const PRUNE_MAX_CODEPOINTS = 8192;
export const PRUNE_HEAD_CHARS = 4096;
export const PRUNE_TAIL_CHARS = 1024;
export const COMPACTED_SUMMARY_TAG_OPEN = '<compacted-summary>';
export const COMPACTED_SUMMARY_TAG_CLOSE = '</compacted-summary>';

export const COMPACTION_DIRECTIVE = [
  '当前对话上下文即将达到窗口上限。请基于以上对话生成一份结构化上下文检查点摘要，供后续步骤接续执行。',
  '注意：禁止调用任何工具；直接输出包裹在 <compacted-summary> 与 </compacted-summary> 之间的 Markdown 文本，严格包含以下章节：',
  '',
  '## 原始需求与核心约束',
  '- 逐条完整保留用户提出的所有任务要求、设计规则、触发器/物编/命名/数值约束，以及作图习惯说明，严禁遗漏任何未完成需求。',
  '',
  '## 当前地图与关键对象身份',
  '- 记录已确认的地图名称、修订号（revision，标注继续写操作前需重新回读）、相关触发器/变量/单位/技能/物品 ID 与名称。',
  '',
  '## 已完成修改与验证结论',
  '- 按顺序列出已实际执行成功的修改、回读结果与测试状态。',
  '',
  '## 错误与修正记录',
  '- 记录遇到过的报错、门禁拒绝原因及已验证的避坑做法（无则写“无”）。',
  '',
  '## 待办清单与下一步行动',
  '- 明确列出尚未完成的触发器/物编/校验步骤，以及恢复后紧接着要执行的第一步操作。',
].join('\n');

export function estimateTextTokens(text) {
  if (!text) return 0;
  const str = String(text);
  let cjk = 0;
  let other = 0;
  for (let i = 0; i < str.length; ++i) {
    const code = str.charCodeAt(i);
    if ((code >= 0x2E80 && code <= 0x9FFF)
      || (code >= 0xAC00 && code <= 0xD7AF)
      || (code >= 0xF900 && code <= 0xFAFF)
      || (code >= 0xFF00 && code <= 0xFFEF)) {
      ++cjk;
    } else {
      ++other;
    }
  }
  return Math.ceil(cjk * 0.68 + other * 0.28);
}

export function extractMessageText(message) {
  if (!message) return '';
  if (typeof message.content === 'string') return message.content;
  if (!Array.isArray(message.content)) return '';
  return message.content
    .filter(block => block?.type === 'text' && typeof block.text === 'string')
    .map(block => block.text)
    .join('\n');
}

export function estimateMessageTokens(message) {
  if (!message || typeof message !== 'object') return 0;
  let tokens = 4; // 角色与帧结构开销
  if (typeof message.content === 'string') {
    tokens += estimateTextTokens(message.content);
  } else if (Array.isArray(message.content)) {
    for (const block of message.content) {
      if (!block || typeof block !== 'object') continue;
      if (block.type === 'text') {
        tokens += estimateTextTokens(block.text);
      } else if (block.type === 'thinking') {
        tokens += estimateTextTokens(block.thinking);
      } else if (block.type === 'toolCall') {
        tokens += 12 + estimateTextTokens(block.name) + estimateTextTokens(JSON.stringify(block.arguments ?? {}));
      } else if (block.type === 'image') {
        tokens += 768;
      } else {
        tokens += estimateTextTokens(JSON.stringify(block));
      }
    }
  }
  if (message.role === 'system') {
    if (Array.isArray(message.toolsAdded)) {
      for (const t of message.toolsAdded) tokens += estimateToolTokens(t);
    }
  }
  return tokens;
}

export function estimateToolTokens(tool) {
  if (!tool || typeof tool !== 'object') return 0;
  const schema = tool.inputSchema ?? tool.parameters ?? {};
  return 16
    + estimateTextTokens(tool.name ?? '')
    + estimateTextTokens(tool.description ?? '')
    + Math.ceil(JSON.stringify(schema).length * 0.28);
}

export function estimateSurfaceTokens({ systemPrompt = '', tools = [], messages = [] }) {
  const hasSystemInMessages = messages[0]?.role === 'system';
  let total = hasSystemInMessages ? 0 : estimateTextTokens(systemPrompt);
  const systemDeclaresTools = hasSystemInMessages && Array.isArray(messages[0]?.toolsAdded) && messages[0].toolsAdded.length > 0;
  if (!systemDeclaresTools) {
    for (const tool of tools) total += estimateToolTokens(tool);
  }
  for (const message of messages) total += estimateMessageTokens(message);
  return total;
}

export function isCompactionSummaryMessage(message) {
  if (!message || message.role !== 'user') return false;
  if (message.forge_injection === 'compaction_summary' || message.forge_injection === 'context_summary') return true;
  const text = extractMessageText(message);
  return text.startsWith(COMPACTED_SUMMARY_TAG_OPEN)
    || text.startsWith('较早完整回合已从模型上下文压缩；完整记录仍由宿主保存。');
}

export function isToolPairingBalanced(messages) {
  const pending = new Set();
  for (const message of messages) {
    if (!message || typeof message !== 'object') return false;
    const role = message.role;
    if (role === 'system' || role === 'user') {
      if (pending.size > 0) return false;
    } else if (role === 'assistant') {
      if (message.stopReason === 'aborted' || message.stopReason === 'error') return false;
      if (Array.isArray(message.content)) {
        for (const block of message.content) {
          if (block?.type !== 'toolCall') continue;
          if (!block.id || pending.has(block.id)) return false;
          pending.add(block.id);
        }
      }
    } else if (role === 'toolResult') {
      if (!pending.delete(message.toolCallId)) return false;
    } else {
      return false;
    }
  }
  return pending.size === 0;
}

// 第 1 级：确定性修剪较早轮次中的超大工具返回（保留头 4096 + 尾 1024 字符）。
export function pruneLargeToolResults(messages, options = {}) {
  const maxCodePoints = options.maxCodePoints ?? PRUNE_MAX_CODEPOINTS;
  const headChars = options.headChars ?? PRUNE_HEAD_CHARS;
  const tailChars = options.tailChars ?? PRUNE_TAIL_CHARS;
  const includeRecent = Boolean(options.includeRecent);

  let protectFrom = messages.length;
  if (!includeRecent) {
    // 默认保护最后一个用户回合内的最后 4 条工具结果，避免刚查出来就修剪导致重复查询。
    let seenToolResults = 0;
    for (let i = messages.length - 1; i >= 0; --i) {
      if (messages[i]?.role === 'toolResult') {
        ++seenToolResults;
        if (seenToolResults >= 4) {
          protectFrom = i;
          break;
        }
      }
      if (messages[i]?.role === 'user' && !messages[i]?.forge_hidden) {
        protectFrom = Math.min(protectFrom, i);
        break;
      }
    }
  }

  let prunedCount = 0;
  let savedTokens = 0;
  const next = messages.map((message, index) => {
    if (index >= protectFrom || message?.role !== 'toolResult' || !Array.isArray(message.content)) return message;
    let changed = false;
    const content = message.content.map(block => {
      if (block?.type !== 'text' || typeof block.text !== 'string' || block.forge_pruned) return block;
      const chars = Array.from(block.text);
      if (chars.length <= maxCodePoints) return block;
      const omitted = chars.length - headChars - tailChars;
      if (omitted <= 256) return block;
      const beforeTokens = estimateTextTokens(block.text);
      const prunedText = `${chars.slice(0, headChars).join('')}\n\n[…已修剪早期工具结果中间 ${omitted} 个字符以释放上下文；继续修改前请按需分页查询最新状态…]\n\n${chars.slice(chars.length - tailChars).join('')}`;
      const afterTokens = estimateTextTokens(prunedText);
      if (afterTokens >= beforeTokens) return block;
      changed = true;
      ++prunedCount;
      savedTokens += (beforeTokens - afterTokens);
      return { ...block, text: prunedText, forge_pruned: true };
    });
    return changed ? { ...message, content } : message;
  });

  return { messages: prunedCount > 0 ? next : messages, prunedCount, savedTokens };
}

// 选择满足工具配对平衡的待压缩区间 [regionStart, regionEnd)。
export function selectCompactionRegion(messages, contextWindow, options = {}) {
  const retainRatio = options.retainRatio ?? DEFAULT_RETAIN_RATIO;
  const headEnd = messages[0]?.role === 'system' ? 1 : 0;
  if (messages.length - headEnd < 2) return null;
  const totalMsgTokens = messages.slice(headEnd).reduce((sum, m) => sum + estimateMessageTokens(m), 0);
  const baseRetainBudget = Math.max(1024, Math.floor(contextWindow * retainRatio));
  const retainBudget = options.force
    ? Math.min(baseRetainBudget, Math.max(128, Math.floor(totalMsgTokens * 0.25)))
    : baseRetainBudget;

  // 收集所有合法的平衡切分点（工具配对已闭合的位置）。
  const validCuts = [];
  const pending = new Set();
  for (let i = headEnd; i < messages.length; ++i) {
    if (pending.size === 0 && i > headEnd) validCuts.push(i);
    const msg = messages[i];
    if (msg?.role === 'assistant' && Array.isArray(msg.content)) {
      for (const b of msg.content) if (b?.type === 'toolCall' && b.id) pending.add(b.id);
    } else if (msg?.role === 'toolResult' && msg.toolCallId) {
      pending.delete(msg.toolCallId);
    }
  }
  if (validCuts.length === 0) return null;

  // 找到所有真实用户消息边界（排除已有压缩摘要与隐藏消息）。
  const userCuts = validCuts.filter(idx => messages[idx]?.role === 'user' && !messages[idx]?.forge_hidden && !isCompactionSummaryMessage(messages[idx]));

  // 优先在真实用户消息边界切分，保留最近约 retainBudget 的尾部。
  if (userCuts.length >= 1) {
    let chosen = userCuts[userCuts.length - 1];
    for (let k = userCuts.length - 1; k >= 0; --k) {
      const candidate = userCuts[k];
      const tailTokens = messages.slice(candidate).reduce((sum, m) => sum + estimateMessageTokens(m), 0);
      chosen = candidate;
      if (tailTokens >= retainBudget) break;
    }
    if (chosen > headEnd && isToolPairingBalanced(messages.slice(headEnd, chosen)) && isToolPairingBalanced(messages.slice(chosen))) {
      return { regionStart: headEnd, regionEnd: chosen };
    }
  }

  // 单次长任务内已产生大量工具回合（只有 1 条真实用户提问，在 headEnd 或紧随压缩摘要之后）：
  // 绝不裁掉开头的用户原始长需求，而是保留该用户提问在头部，压缩中间早期已闭合的 [assistant + toolResult] 轮次！
  let firstRealUser = -1;
  for (let i = headEnd; i < messages.length; ++i) {
    if (messages[i]?.role === 'user' && !isCompactionSummaryMessage(messages[i]) && !messages[i]?.forge_hidden) {
      firstRealUser = i;
      break;
    }
  }
  const midStart = firstRealUser >= 0 ? firstRealUser + 1 : headEnd;
  const midCuts = validCuts.filter(idx => idx > midStart + 1);
  if (midCuts.length === 0) return null;

  let chosenMid = midCuts[midCuts.length - 1];
  for (let k = midCuts.length - 1; k >= 0; --k) {
    const candidate = midCuts[k];
    const tailTokens = messages.slice(candidate).reduce((sum, m) => sum + estimateMessageTokens(m), 0);
    chosenMid = candidate;
    if (tailTokens >= retainBudget) break;
  }
  if (chosenMid > midStart
    && isToolPairingBalanced(messages.slice(midStart, chosenMid))
    && isToolPairingBalanced(messages.slice(chosenMid))) {
    return { regionStart: midStart, regionEnd: chosenMid, midTurn: true };
  }
  return null;
}

export function unwrapSummaryTags(rawText) {
  const text = String(rawText ?? '').trim();
  const openIdx = text.indexOf(COMPACTED_SUMMARY_TAG_OPEN);
  const closeIdx = text.lastIndexOf(COMPACTED_SUMMARY_TAG_CLOSE);
  if (openIdx >= 0 && closeIdx > openIdx) {
    return text.slice(openIdx + COMPACTED_SUMMARY_TAG_OPEN.length, closeIdx).trim();
  }
  return text;
}

// 确定性兜底摘要（离线单测或摘要请求失败时使用）：完整保留被压缩区间内的所有用户原始需求 + 关键工具记录！
export function buildDeterministicSummaryBody(removedMessages, maxUserChars = 6000) {
  const previousSummaries = [];
  const userRequirements = [];
  const assistantConclusions = [];
  const toolFacts = [];

  for (const m of removedMessages) {
    if (!m || typeof m !== 'object') continue;
    if (isCompactionSummaryMessage(m)) {
      const unwrapped = unwrapSummaryTags(extractMessageText(m));
      if (unwrapped) previousSummaries.push(unwrapped.slice(0, 2500));
      continue;
    }
    if (m.role === 'user' && !m.forge_hidden) {
      const text = extractMessageText(m).trim();
      if (text) userRequirements.push(text);
    } else if (m.role === 'assistant') {
      const text = extractMessageText(m).trim();
      if (text) assistantConclusions.push(text.slice(0, 400));
    } else if (m.role === 'toolResult') {
      const text = extractMessageText(m).replace(/\s+/g, ' ').trim();
      toolFacts.push(`- ${m.toolName || 'tool'}（${m.isError ? '失败' : '成功'}）：${text.slice(0, 320)}`);
    }
  }

  const sections = [];
  if (previousSummaries.length > 0) {
    sections.push(`## 既有检查点继承\n${previousSummaries.join('\n\n').slice(0, 2500)}`);
  }
  if (userRequirements.length > 0) {
    let combinedUsers = userRequirements.map((req, idx) => `${idx + 1}. ${req}`).join('\n\n');
    if (combinedUsers.length > maxUserChars) {
      combinedUsers = `${combinedUsers.slice(0, Math.floor(maxUserChars * 0.75))}\n…[中间省略]…\n${combinedUsers.slice(-Math.floor(maxUserChars * 0.2))}`;
    }
    sections.push(`## 原始需求与核心约束\n${combinedUsers}`);
  }
  if (toolFacts.length > 0) {
    sections.push(`## 已完成修改与工具记录（最近 ${Math.min(12, toolFacts.length)} 条）\n${toolFacts.slice(-12).join('\n')}`);
  }
  if (assistantConclusions.length > 0) {
    sections.push(`## 阶段结论摘录\n${assistantConclusions.slice(-4).map(t => `- ${t}`).join('\n')}`);
  }
  sections.push('## 续接提醒\n- 历史对象修订号可能已过期，继续编辑前请重新查询当前地图身份与修订。');
  return sections.join('\n\n');
}

export function createCompactedSummaryMessage(bodyText, meta = {}) {
  const cleanBody = unwrapSummaryTags(bodyText);
  const wrapped = `${COMPACTED_SUMMARY_TAG_OPEN}\n${cleanBody}\n${COMPACTED_SUMMARY_TAG_CLOSE}`;
  return {
    role: 'user',
    timestamp: Date.now(),
    forge_hidden: true,
    forge_injection: 'compaction_summary',
    forge_compacted_count: meta.removedCount ?? 0,
    forge_saved_tokens: meta.savedTokens ?? 0,
    content: [{ type: 'text', text: wrapped }],
  };
}

// DSH 三级压缩主入口：先确定性修剪工具输出，仍超阈值（或手动强制）则复用热前缀 KV Cache 生成结构化检查点摘要。
export async function compactWithWarmCache({
  messages,
  systemPrompt = '',
  tools = [],
  contextWindow,
  observedPromptTokens = 0,
  thresholdRatio = DEFAULT_THRESHOLD_RATIO,
  retainRatio = DEFAULT_RETAIN_RATIO,
  force = false,
  summarizeFn = null,
  signal = undefined,
}) {
  const thresholdTokens = Math.max(1024, Math.floor(contextWindow * thresholdRatio));
  const initialEstimate = estimateSurfaceTokens({ systemPrompt, tools, messages });
  const effectiveBefore = Math.max(initialEstimate, observedPromptTokens);

  if (!force && effectiveBefore <= thresholdTokens) {
    return {
      messages,
      compacted: false,
      pruned: false,
      prunedCount: 0,
      removedMessages: 0,
      savedTokens: 0,
      summary: '',
    };
  }

  // 第 1 级：先修剪早期大型 toolResult。
  const pruned = pruneLargeToolResults(messages, { includeRecent: force && messages.length <= 4 });
  const afterPruneEstimate = estimateSurfaceTokens({ systemPrompt, tools, messages: pruned.messages });
  const effectiveAfterPrune = Math.max(afterPruneEstimate, Math.max(0, observedPromptTokens - pruned.savedTokens));

  if (!force && effectiveAfterPrune <= thresholdTokens) {
    return {
      messages: pruned.messages,
      compacted: false,
      pruned: pruned.prunedCount > 0,
      prunedCount: pruned.prunedCount,
      removedMessages: 0,
      savedTokens: pruned.savedTokens,
      summary: '',
    };
  }

  // 第 2 级：选择平衡压缩区间并执行热前缀摘要。
  const region = selectCompactionRegion(pruned.messages, contextWindow, {
    force,
    retainRatio: force ? Math.min(retainRatio, 0.10) : retainRatio,
  });
  if (!region) {
    return {
      messages: pruned.messages,
      compacted: false,
      pruned: pruned.prunedCount > 0,
      prunedCount: pruned.prunedCount,
      removedMessages: 0,
      savedTokens: pruned.savedTokens,
      summary: '',
    };
  }

  const { regionStart, regionEnd } = region;
  const removedSlice = pruned.messages.slice(regionStart, regionEnd);
  const removedTokens = removedSlice.reduce((sum, m) => sum + estimateMessageTokens(m), 0);

  let summaryBody = '';
  let usedLlmSummary = false;
  if (typeof summarizeFn === 'function') {
    try {
      // 复用提供商热前缀：直接把 messages[0..regionEnd) 原样作为前缀，仅在末尾追加一条压缩指令 user 消息！
      const prefixMessages = pruned.messages.slice(0, regionEnd);
      const llmText = await summarizeFn(prefixMessages, COMPACTION_DIRECTIVE, signal);
      const candidate = unwrapSummaryTags(llmText);
      if (candidate && candidate.length >= 16) {
        summaryBody = candidate;
        usedLlmSummary = true;
      }
    } catch {
      // 模型摘要失败时回退到不丢用户需求的确定性检查点摘要，不阻断用户任务。
    }
  }
  if (!summaryBody) {
    summaryBody = buildDeterministicSummaryBody(removedSlice);
  }

  const provisionalMessage = createCompactedSummaryMessage(summaryBody, { removedCount: removedSlice.length, savedTokens: 0 });
  const summaryTokens = estimateMessageTokens(provisionalMessage);

  // 非强制压缩时，若摘要反而比被压缩区间更大，则退回确定性精简摘要或跳过替换。
  if (!force && summaryTokens >= removedTokens && usedLlmSummary) {
    const fallbackBody = buildDeterministicSummaryBody(removedSlice, 3000);
    const fallbackMsg = createCompactedSummaryMessage(fallbackBody, { removedCount: removedSlice.length, savedTokens: 0 });
    if (estimateMessageTokens(fallbackMsg) < removedTokens) {
      summaryBody = fallbackBody;
    } else {
      return {
        messages: pruned.messages,
        compacted: false,
        pruned: pruned.prunedCount > 0,
        prunedCount: pruned.prunedCount,
        removedMessages: 0,
        savedTokens: pruned.savedTokens,
        summary: '',
      };
    }
  }

  const finalSummaryTokens = estimateTextTokens(summaryBody) + 24;
  const regionSaved = Math.max(0, removedTokens - finalSummaryTokens);
  const totalSaved = pruned.savedTokens + regionSaved;
  const summaryMessage = createCompactedSummaryMessage(summaryBody, {
    removedCount: removedSlice.length,
    savedTokens: totalSaved,
  });

  const nextMessages = [
    ...pruned.messages.slice(0, regionStart),
    summaryMessage,
    ...pruned.messages.slice(regionEnd),
  ];

  if (!isToolPairingBalanced(nextMessages)) {
    return {
      messages: pruned.messages,
      compacted: false,
      pruned: pruned.prunedCount > 0,
      prunedCount: pruned.prunedCount,
      removedMessages: 0,
      savedTokens: pruned.savedTokens,
      summary: '',
    };
  }

  return {
    messages: nextMessages,
    compacted: true,
    pruned: pruned.prunedCount > 0,
    prunedCount: pruned.prunedCount,
    removedMessages: removedSlice.length,
    savedTokens: totalSaved,
    summary: unwrapSummaryTags(summaryBody),
    usedLlmSummary,
  };
}

export function isContextOverflowError(errorOrMessage) {
  const text = String(errorOrMessage?.message ?? errorOrMessage ?? '').toLowerCase();
  return text.includes('context_length_exceeded')
    || text.includes('maximum context length')
    || text.includes('prompt is too long')
    || text.includes('context window')
    || text.includes('input is too long')
    || text.includes('tokens exceeds')
    || text.includes('exceeds the maximum number of tokens');
}
