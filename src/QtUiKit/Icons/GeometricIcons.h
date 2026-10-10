#pragma once

#include <QIcon>

// 程序化几何图标库：命运2 极简科幻信息图形风格。
// 所有图标均为 QPainter 矢量绘制——细线条、几何构型、低饱和冷色 + 金色点缀。
// 不使用任何外部图标字体/图标库，保证与项目艺术风格完全一致。
namespace GeometricIcons {

// ── 侧栏导航图标（20×20，线性构型） ──
QIcon Overview();       // 概览：六边形轮廓 + 中心点（命运2式据点符号）
QIcon Trigger();        // 触发器：角状闪电折线（几何化，非 emoji）
QIcon ObjectEditor();   // 物体编辑器：三层堆叠菱形（数据层叠）
QIcon MapInfo();        // 地图信息：圆 + 十字准线 + 弧段
QIcon ImportAssets();   // 导入资源：上三角 + 底线（入库符号）
QIcon Validate();       // 验证：角状对勾（几何折线）

// ── ECA 逻辑节点图标（16×16，技术制图风） ──
QIcon EcaEvent();       // 事件：角状脉冲线（信号触发）
QIcon EcaCondition();   // 条件：菱形 + 内十字（判定门）
QIcon EcaAction();      // 动作：双箭头角标（执行流）
QIcon EcaCall();        // 调用：圆 + 内三角（函数入口）
QIcon EcaDisabled();    // 禁用：断裂圆环 + 斜线
QIcon EcaComment();     // 注释：双横线（文本标记）

// ── 通用功能图标 ──
QIcon Undo();           // 撤销：左回折箭头
QIcon Redo();           // 重做：右回折箭头
QIcon Add();            // 添加：细十字
QIcon Edit();           // 编辑：角状笔尖
QIcon Export();         // 导出：右箭头 + 门框
QIcon Search();         // 搜索：圆 + 柄线

} // namespace GeometricIcons
