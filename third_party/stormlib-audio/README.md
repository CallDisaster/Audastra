# StormLib 音频格式派生代码

- 官方来源：https://github.com/ladislav-zezula/StormLib
- 固定提交：`44ebfbfc109d76e2a85bbd5d8b0c949df7e65c6f`
- 许可证：MIT；完整原文见同目录 `LICENSE`。
- 参考文件：`src/huffman/huff.cpp`、`src/huffman/huff.h`、`src/adpcm/adpcm.cpp`、`src/adpcm/adpcm.h`、`src/SCompression.cpp`。
- 本项目文件：`src/Core/Mpq/MpqAudioTables.h` 与 `MpqAudioDecode.cpp`。

保留格式需要的九组初始权重和 ADPCM 步长表；将解码流程改写为 C++20 span、固定节点池及有界 vector。移除压缩器、快速查找缓存、StarCraft beta 编码及 StormLib 运行时依赖。增加截断、未终止 Huffman、重复 NYT 符号、逐级输出容量及最终长度校验。版权归属保留在派生源码头部。

本机 StormLib DLL 只用于开发时独立 oracle，不链接到 Core，也不随本项目发布。游戏音频及本机 oracle 输出不进入版本库。
