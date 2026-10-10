# 把文本文件嵌入为 C++ std::string_view 常量（构建期运行）。
# 用法：cmake -DINPUT=<文件> -DOUTPUT=<cpp> -DSYMBOL=<名称> -DNAMESPACE=<命名空间> -P EmbedText.cmake
file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_length)
math(EXPR byte_count "${hex_length} / 2")
# 每 32 字节一行，避免超长源码行。
string(REGEX REPLACE "(................................................................)" "\\1\n" content "${content}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
file(WRITE "${OUTPUT}.tmp"
"// 由 cmake/EmbedText.cmake 生成，勿手改。
#include <string_view>
namespace ${NAMESPACE} {
namespace { const unsigned char kBytes[] = {
${bytes}0x00};
}
extern const std::string_view ${SYMBOL};
const std::string_view ${SYMBOL}(reinterpret_cast<const char*>(kBytes), ${byte_count});
}
")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
