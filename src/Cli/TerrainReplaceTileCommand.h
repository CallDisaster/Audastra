#pragma once

// terrain-replace-tile <源地图> <原贴图> <新贴图> --game-root <魔兽目录> --tool-root <YDWE目录> [--out <新候选.w3x>]
// 新贴图不在地面贴图表里：直接把原贴图的槽位换成新贴图；已在表里：原贴图槽位上的全部顶点改用新贴图
// （变体与荒芜等标志不变，表本身不变）。两种都走 map-edit-save 的同一保存事务（通行性重算、脚本重建），
// 源地图只读，结果写到新候选文件；省略 --out 时写到源文件旁的 <名>.tiles<扩展名>，文件已存在则拒绝。
int RunTerrainReplaceTile(int argc, wchar_t** argv);
