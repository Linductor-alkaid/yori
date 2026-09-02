#pragma once

// 极简测试断言（M0-06）：仅服务于骨架示例用例；正式测试框架随 M1 契约测试
// 一并评估引入。skip 统一使用退出码 77（ctest SKIP_RETURN_CODE）。
#include <cstdio>
#include <cstdlib>

namespace yori::testing {

inline int failure_count = 0;

}  // namespace yori::testing

// 条件不满足时记录失败并继续；测试末尾聚合返回。
#define YORI_CHECK(condition)                                                         \
  do {                                                                                \
    if (!(condition)) {                                                               \
      std::fprintf(stderr, "CHECK 失败 %s:%d: %s\n", __FILE__, __LINE__, #condition); \
      ++yori::testing::failure_count;                                                 \
    }                                                                                 \
  } while (false)

// 显式 skip：打印原因与补跑条件并以 77 退出。
#define YORI_SKIP(reason)              \
  do {                                 \
    std::printf("SKIP: %s\n", reason); \
    std::exit(77);                     \
  } while (false)
