#pragma once

// argc/argv包含程序名，以及imports-query或imports-get、归档/目录路径。
// 两命令均要求--expected-source-sha256；get另要求--expected-inventory-sha256和--import-id。
// query支持--query、--limit及--expected-inventory-sha256；续页必须同时传回
// --cursor <next_cursor_token>及--expected-inventory-sha256，并保持原query。
// 所有查询复用Core；不提供裸起点、正文、编辑、保存或队列参数。完整JSON最多256KiB。
[[nodiscard]] int RunImportQueryCommand(int argc, wchar_t** argv);
