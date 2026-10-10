#pragma once

// map-edit-save：有界地形笔划或通用字段修改，经共用 MapEditSaveService 写出独立候选。
// 参数从 argv[2] 起；argv 为原始命令行（UTF-16）。
int RunMapEditSave(int argc, wchar_t** argv);
int RunMapTriggerTextQuery(int argc, wchar_t** argv);
