# Audastra 产品标识

`audastra.svg` 是作者在 2026-10-05 确认的几何 V2 原稿，逐字节保留，SHA-256 为 `dc52fcb428a365266f3ba801ddf731a431523045334fa982357dda61ffcf4944`。源稿 metadata 中的待批准字样是生成时的历史文字；本次作者已经批准。

应用导出只裁去外边距，并将徽章圆角外的背景设为透明；星、轨道、飞船、星球路径和颜色均沿用原稿。16/24/32/48/64/128/256 PNG 用于 Qt 窗口图标，同尺寸 ICO 用于 Windows EXE。512 PNG 用于启动页与关于页，避免在每次绘制时解析复杂 SVG。小尺寸保留同一构图，细密刻线会随缩放减弱。

`provenance.json` 记录源稿、导出尺寸、渲染版本和每个文件的 SHA。需要重导出时，在已安装 Sharp 的开发环境中运行：

```powershell
node tools/dev/export-brand.cjs assets/brand/audastra.svg assets/brand <本机已安装的Sharp模块绝对路径>
```

导出器仅使用 CPU，固定一个工作线程；不安装依赖、不修改源稿、不启动编辑器或游戏。导出无需成为普通构建的一部分，程序直接使用已提交的 PNG/ICO。
