#include <gtest/gtest.h>

#include "ai2048/ai2048.h"

namespace {

TEST(Version, ReportsNonEmptyVersion) { EXPECT_FALSE(ai2048::VersionString().empty()); }

TEST(Ruleset, StartsAtOne) {
  // 规则集版本是"契约版本"：改动 src/core 的规则逻辑必须提升它并重建基准种子集。
  EXPECT_EQ(ai2048::RulesetVersion(), 1);
}

}  // namespace
