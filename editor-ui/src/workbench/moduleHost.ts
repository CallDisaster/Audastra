// ---- 模块宿主与滚动位置 ----
// 编辑器始终渲染在各自固定的宿主节点里，布局变化只搬动宿主节点，所以组件状态不丢。
// 但节点离开可见位置（停放区 display:none 或脱离文档）时浏览器会把其中所有滚动条归零：
// 搬走前同步记下有滚动偏移的元素，重新放回时恢复。不依赖 scroll 事件（后台标签页不派发）。
const scrollMemory = new WeakMap<HTMLElement, [Element, number, number][]>();

export function createModuleHost(): HTMLElement {
  const host = document.createElement('div');
  host.className = 'wb-module-host';
  return host;
}
/** 搬走前调用：记录宿主内所有滚动过的元素。 */
export function saveScroll(host: HTMLElement) {
  if (!host.isConnected || host.offsetParent === null) return;
  const list: [Element, number, number][] = [];
  for (const element of host.querySelectorAll('*')) if (element.scrollTop || element.scrollLeft) list.push([element, element.scrollTop, element.scrollLeft]);
  scrollMemory.set(host, list);
}
/** 放回后调用：恢复仍在宿主内的元素的滚动位置。 */
export function restoreScroll(host: HTMLElement) {
  for (const [element, top, left] of scrollMemory.get(host) ?? []) {
    if (!host.contains(element)) continue;
    element.scrollTop = top; element.scrollLeft = left;
  }
}
