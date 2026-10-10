#pragma once

// argc/argv包含程序名及archive-session-inspect；无新参数时保留既有文本格式。
// 新参数仅查询指定成员的存在/字节数/散列，不输出正文，不修改归档。
[[nodiscard]] int RunArchiveInspectCommand(int argc, wchar_t** argv);
