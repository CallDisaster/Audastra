// 命令面板排序：精确 > 开头 > 包含 > 子序列（拼音首字母以外的中文按字匹配）；多个词须全部命中。
// 纯逻辑，原生端的命令面板与 Agent 的命令检索共用同一规则。

export function fuzzyScore(query: string, text: string): number | null {
  const q = query.trim().toLowerCase(), t = text.toLowerCase();
  if (!q) return 0;
  let total = 0;
  for (const token of q.split(/\s+/)) {
    if (t === token) { total += 1000; continue; }
    if (t.startsWith(token)) { total += 800 - t.length; continue; }
    const at = t.indexOf(token);
    if (at >= 0) { total += 600 - at * 4 - t.length; continue; }
    // 子序列：每个字符依次出现，跨度越短分越高。
    let from = 0, first = -1, last = -1;
    for (const ch of token) { const found = t.indexOf(ch, from); if (found < 0) return null; if (first < 0) first = found; last = found; from = found + 1; }
    total += 200 - (last - first - token.length) * 3 - first;
  }
  return total;
}
export function rank<T>(items: readonly T[], query: string, keyOf: (item: T) => string, boost: (item: T) => number = () => 0, limit = 40): T[] {
  return items.map((item, order) => ({ item, order, score: fuzzyScore(query, keyOf(item)) }))
    .filter((entry): entry is { item: T; order: number; score: number } => entry.score !== null)
    .map(entry => ({ ...entry, score: entry.score + boost(entry.item) }))
    .sort((a, b) => b.score - a.score || a.order - b.order).slice(0, limit).map(entry => entry.item);
}
