import type { EditorAsset, ModuleId, WorkbenchProject } from './types';

export type AssetKind = 'image' | 'audio' | 'model' | 'text' | 'other';
export const assetKinds: { id: AssetKind; label: string }[] = [
    { id: 'image', label: '图像与纹理' }, { id: 'model', label: '模型' },
    { id: 'audio', label: '声音与音乐' }, { id: 'text', label: '脚本与文本' },
    { id: 'other', label: '其他文件' },
];
export const MAX_FILE_BYTES = 2 * 1024 * 1024;
export const MAX_EMBEDDED_BYTES = 3 * 1024 * 1024;
export function assetKind(asset: Pick<EditorAsset, 'name' | 'type'>): AssetKind {
    const ext = asset.name.split('.').pop()?.toLowerCase() || '';
    const type = asset.type.toLowerCase();
    if (/^(png|jpe?g|gif|webp|svg|bmp|blp|tga|dds)$/.test(ext) || /image|texture|图像|纹理/.test(type)) return 'image';
    if (/^(wav|mp3|ogg|m4a|flac|aac)$/.test(ext) || /audio|sound|声音|音乐/.test(type)) return 'audio';
    if (/^(mdx|mdl|obj|gltf|glb|fbx)$/.test(ext) || /model|模型/.test(type)) return 'model';
    if (/^(j|lua|txt|json|xml|ini|slk|csv|fdf|toc|ai|md)$/.test(ext) || /text|script|文本|脚本/.test(type)) return 'text';
    return 'other';
}
export function readableSize(bytes: number) {
    return bytes >= 1024 * 1024 ? `${(bytes / 1024 / 1024).toFixed(1)} MB` : bytes >= 1024 ? `${(bytes / 1024).toFixed(1)} KB` : `${bytes} B`;
}
export function normalizeAssetPath(path: string) {
    return path.trim().replace(/\//g, '\\').replace(/^\\+/, '').replace(/\\+/g, '\\');
}
export const assetPathKey = (path: string) => normalizeAssetPath(path).toLowerCase();
export function pathError(path: string) {
    const normalized = normalizeAssetPath(path);
    if (!normalized || /(^|\\)\.\.?(\\|$)/.test(normalized) || /[:*?"<>|]/.test(normalized) || [...normalized].some(char => char.charCodeAt(0) < 32) || normalized.endsWith('\\')) return '请输入有效的包内相对路径';
    return '';
}
export function readAssetFile(file: File): Promise<string> {
    if (file.size > MAX_FILE_BYTES) return Promise.reject(new Error(`${file.name} 超过 2 MB`));
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(String(reader.result));
        reader.onerror = () => reject(new Error(`无法读取 ${file.name}`));
        reader.readAsDataURL(file);
    });
}
export function embeddedSize(assets: EditorAsset[]) {
    return assets.reduce((sum, asset) => sum + (asset.dataUrl?.length || 0), 0);
}
export interface AssetReference { id: string; label: string; detail: string; module: ModuleId; target: string }
function includesReference(value: unknown, keys: string[]): boolean {
    if (typeof value === 'string') return keys.some(key => value === key || assetPathKey(value) === assetPathKey(key));
    if (Array.isArray(value)) return value.some(item => includesReference(item, keys));
    if (value && typeof value === 'object') return Object.values(value).some(item => includesReference(item, keys));
    return false;
}
export function assetReferences(project: WorkbenchProject, asset: EditorAsset): AssetReference[] {
    const keys = [asset.id, asset.path];
    const refs: AssetReference[] = [];
    project.sounds.filter(sound => sound.assetId === asset.id).forEach(sound => refs.push({ id: sound.id, label: sound.name, detail: sound.music ? '音乐定义' : '声音定义', module: 'sounds', target: sound.id }));
    project.objects.forEach(object => {
        Object.entries(object.fields).filter(([, value]) => includesReference(value, keys)).forEach(([field]) => refs.push({ id: `${object.id}.${field}`, label: object.name, detail: field, module: 'objects', target: object.id }));
    });
    project.graphs.forEach(graph => graph.nodes.filter(node => includesReference(node.values, keys)).forEach(node => refs.push({ id: `${graph.id}.${node.id}`, label: graph.name, detail: project.definitions.find(def => def.id === node.definition)?.title || node.definition, module: 'triggers', target: graph.id })));
    return refs;
}
export function createDemoAudio(): string {
    // 示例工程使用的原创短提示音，不依赖游戏资源或外部文件。
    const sampleRate = 16000;
    const length = sampleRate * 2;
    const buffer = new ArrayBuffer(44 + length * 2);
    const view = new DataView(buffer);
    const writeText = (offset: number, value: string) => [...value].forEach((char, i) => view.setUint8(offset + i, char.charCodeAt(0)));
    writeText(0, 'RIFF'); view.setUint32(4, buffer.byteLength - 8, true); writeText(8, 'WAVE'); writeText(12, 'fmt ');
    view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
    view.setUint32(24, sampleRate, true); view.setUint32(28, sampleRate * 2, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true);
    writeText(36, 'data'); view.setUint32(40, length * 2, true);
    for (let i = 0; i < length; i++) {
        const t = i / sampleRate;
        const envelope = Math.min(t * 16, 1) * Math.exp(-t * 2.4) * Math.min((2 - t) * 10, 1);
        const sample = (Math.sin(t * Math.PI * 2 * 392) + Math.sin(t * Math.PI * 2 * 587.33) * 0.35 + Math.sin(t * Math.PI * 2 * 784) * 0.18) * envelope * 7000;
        view.setInt16(44 + i * 2, sample, true);
    }
    let binary = '';
    new Uint8Array(buffer).forEach(byte => { binary += String.fromCharCode(byte); });
    return `data:audio/wav;base64,${btoa(binary)}`;
}
