# Microsoft Detours 4.0.1

本目录保存Microsoft Detours 4.0.1中构建本机Win32注入桥所需的源文件、头文件及MIT许可证。上游项目：[microsoft/Detours](https://github.com/microsoft/Detours)。本次原始输入来自作者已授权只读使用的本机Detours依赖副本，版权和MIT许可证逐字保留。

本地兼容改动仅在`creatwth.cpp`的`DetourProcessViaHelperDllsA/W`两个函数：将`DWORD nLen`声明上移至各函数局部变量段，`GetEnvironmentVariableA/W`赋值保持原位置。这样避免MSVC 2026的`goto Cleanup`跨越初始化所报C2362，不改变控制流、返回值或Windows调用。其余源文件未改。

`uimports.cpp`由`creatwth.cpp`包含，不作为独立编译单元。`disol*.cpp`提供Detours自身引用的离线指令解码实现。仅独立`src/WarcraftRuntime32`目标链接该库；Core和主Qt产品不引入Detours或旧插件实现。

该第三方目标单独按C++14构建，使用Microsoft文档规定的[`/Zc:gotoScope-`](https://learn.microsoft.com/cpp/build/reference/zc-gotoscope)旧代码兼容选项，避免其他平凡局部变量在goto路径上的初始化被新工具链拒绝。原创启动器、bootstrap与其测试仍使用C++20及严格模式，该选项不作用于它们。
