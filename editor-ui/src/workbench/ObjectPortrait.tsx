import { useState } from 'react';
import { Anvil, FlaskConical, Mountain, Shield, Sparkles, Swords, Trees } from 'lucide-react';
import type { EditorObject, ObjectKind, WorkbenchProject } from './types';
import { catalogIcon } from './objectCatalog';

interface ObjectPortraitProps {
    object: EditorObject;
    project: WorkbenchProject;
    size?: number;
    className?: string;
}

const samplePortraits: Record<string, string> = {
    'unit.soldier': '/portraits/guard.svg',
    'unit.archer': '/portraits/ranger.svg',
    'unit.knight': '/portraits/knight.svg',
    'unit.mage': '/portraits/arcanist.svg',
    'unit.worker': '/portraits/artisan.svg',
    'unit.hero': '/portraits/lord.svg',
    'ability.bolt': '/portraits/arcane-bolt.svg',
    'ability.heal': '/portraits/holy-light.svg',
    'ability.defend': '/portraits/defend.svg',
};
const fallbackIcons = { unit: Swords, item: FlaskConical, destructable: Trees, doodad: Mountain, ability: Sparkles, buff: Shield, upgrade: Anvil };
const fallbackColors: Record<ObjectKind, string> = {
    unit: '#9bacba', item: '#c5ac7d', destructable: '#a7b68c', doodad: '#a3afa0',
    ability: '#b5a0d5', buff: '#8fb5bf', upgrade: '#c0aa89',
};
const pathKey = (value: string) => value.trim().replace(/\\/g, '/').toLowerCase();

function supportedImage(value: string | undefined, imported = false): string | undefined {
    if (!value) return;
    const source = value.trim();
    if (/^data:image\/(?:png|jpe?g|webp|gif|svg\+xml|bmp|avif)(?:;|,)/i.test(source)) return source;
    // 无 MIME 的浏览器导入仍可交给 img 解码；游戏包内路径不作为网页地址请求。
    if (imported && /^data:application\/octet-stream(?:;|,)/i.test(source)) return source;
    if (/^\/portraits\/[a-z0-9_-]+\.svg$/i.test(source)) return source;
    // 本机原版图标（tools/import-war3-data.mjs 导出的 PNG）。
    if (/^(?:\.\/)?local-data\/war3\/icons\/[a-z0-9_]+\.png$/i.test(source)) return source;
    if (!/^https?:\/\//i.test(source)) return;
    try {
        const url = new URL(source);
        if (url.username || url.password || /\.(?:blp|tga|dds|mdx|mdl)(?:$|\/)/i.test(url.pathname)) return;
        return url.href;
    } catch { return; }
}

/** 图片引用优先；示例单位与技能使用本项目原创矢量资源。 */
export function ObjectPortrait({ object, project, size = 40, className }: ObjectPortraitProps) {
    const icon = typeof object.fields.icon === 'string' ? object.fields.icon.trim() : '';
    const asset = icon ? project.assets.find(item => item.id === icon || pathKey(item.path) === pathKey(icon)) : undefined;
    const custom = asset ? supportedImage(asset.dataUrl, true) : supportedImage(icon);
    const example = object.kind === 'unit' || object.kind === 'ability' ? samplePortraits[object.id] : undefined;
    const candidates = [custom, catalogIcon(object), example].filter((source): source is string => !!source);
    const [failures, setFailures] = useState<string[]>([]);
    const source = candidates.find(candidate => !failures.includes(candidate));
    const Icon = fallbackIcons[object.kind];
    const dimension = Number.isFinite(size) ? Math.max(16, size) : 40;

    return <span className={`object-portrait ${className ?? ''}`} style={{ width: dimension, height: dimension, minWidth: dimension, flex: '0 0 auto', display: 'inline-flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden', borderRadius: 5, background: 'var(--raised, #232729)', boxShadow: 'inset 0 0 0 1px var(--line, #303538)', verticalAlign: 'middle' }}>
        {source ? <img key={source} src={source} alt={object.name} loading="lazy" draggable={false} referrerPolicy="no-referrer" style={{ display: 'block', width: '100%', height: '100%', objectFit: 'cover' }} onError={() => setFailures(previous => previous.includes(source) ? previous : [...previous, source])} /> : <Icon role="img" aria-label={object.name} size={Math.round(dimension * 0.52)} strokeWidth={1.5} color={fallbackColors[object.kind]} />}
    </span>;
}
