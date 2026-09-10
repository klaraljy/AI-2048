#include "ai2048/ai2048.h"

namespace ai2048 {

namespace {
// 由 CMake 的 project(VERSION ...) 注入；见 CMakeLists.txt。
constexpr std::string_view kVersion = AI2048_VERSION_STRING;
}  // namespace

std::string_view VersionString() noexcept { return kVersion; }

int RulesetVersion() noexcept {
  // 1 = 标准 2048：4x4；新块落在随机空格；90% 出 2、10% 出 4；
  //     单次移动内合并出的块不参与二次合并；无空格且四方向皆不可动即终局。
  //
  // 改动 src/core 里任何影响走子或生成的逻辑时，必须把这个数字加一，
  // 并在 docs/ 里记录原因、重建 benchmarks/ 下的基准种子集。
  return 1;
}

}  // namespace ai2048
