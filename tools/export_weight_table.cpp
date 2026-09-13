// 权重表导出工具：把引擎的权重表导出成前端 game.js 需要的字面量。
//
// 为什么需要它：前端 `web/js/game.js` 里有一张**硬编码的权重表**，它是从
// 引擎（`core/game.cpp` 的 `WeightTable`）导出的副本 —— 为了让浏览器在
// 连不上引擎、退化为 LocalTransport 时，生成分布与引擎**逐位一致**。
//
// ⚠️ **改引擎的 strength / 饱和上限 / 下界时，必须重新导出并替换前端那张表。**
// 否则前端与引擎的难度分布会静默分叉：界面上看着正常，但同一难度下
// 浏览器本地生成的位置与引擎不一致。这个坑本项目踩过一次
// （"走子后的生成退回均匀分布，难度只在开局生效"）。
//
// ⚠️ 表里会出现 **0**：负分对应的权重会下溢截断成 0（那种格子不该被抽中）。
// 这是**预期值，不是错误**。前端抽样的区间累加逻辑要能正确处理 0 权重的格子，
// 以及"所有格权重都是 0"的退化情形（那时应退回均匀分布）。
//
// 用法：
//     g++ -std=c++20 -O2 -I engine/include -I engine/src \
//         tools/export_weight_table.cpp engine/build/libai2048_core.a -o export.exe
//     export.exe > table.txt
// 然后把输出粘进 web/js/game.js 的 WEIGHT_TABLE。
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/game.h"

using namespace ai2048;

int main() {
  const std::vector<std::int64_t> easy = ExportWeightTableForTesting(/*hard=*/false);
  const std::vector<std::int64_t> hard = ExportWeightTableForTesting(/*hard=*/true);

  std::printf("// score 下界 = %d，饱和上限 = %d，共 %zu 项\n", ExportScoreFloor(),
              ExportScoreSaturation(), easy.size());
  std::printf("// 索引 = score - (%d)，score 单位是百分数（100 = 1.00）\n", ExportScoreFloor());

  const auto dump = [](const char* name, const std::vector<std::int64_t>& table) {
    std::printf("\n// %s\nconst %s_WEIGHT_TABLE = [\n", name, name);
    for (std::size_t i = 0; i < table.size(); ++i) {
      if (i % 16 == 0) std::printf("  ");
      std::printf("%lld,", static_cast<long long>(table[i]));
      if (i % 16 == 15) std::printf("\n");
    }
    if (table.size() % 16 != 0) std::printf("\n");
    std::printf("];\n");
  };
  dump("EASY", easy);
  dump("HARD", hard);
  return 0;
}
