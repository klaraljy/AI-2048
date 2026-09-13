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
  // 2 = 难度规则加强（2026-09-13）。`normal` 的**玩法规则**没变（它只走均匀分支），
  //     但权重表的计算口径变了，所以版本一起进：
  //       - 困难档的**角落/边缘惩罚真正生效**。评分函数末尾的 `score < 0 ? 0`
  //         一直把这两条惩罚吃掉：空角落格本身没有任何加分、得 0 分，减去 80
  //         又被夹回 0，于是它与平庸格**完全同权** —— "角落对玩家有利、不该
  //         往那儿放"这条设计意图从未生效（实测落点份额与 0 分格毫无差别）。
  //       - 评分饱和上限 600 → 900，并新增下界 −400，即允许负分参与加权。
  //       - 实测效果（各 2 万次采样，接近满盘的盘面）：最高分格落点份额
  //         27.9% → **46.0%**（3.68 倍于均匀）；角落 4.4%（低于均匀 12.5%）。
  //
  // 改动 src/core 里任何影响走子或生成的逻辑时，必须把这个数字加一，
  // 并在 docs/ 里记录原因、重建 benchmarks/ 下的基准种子集。
  return 2;
}

}  // namespace ai2048
