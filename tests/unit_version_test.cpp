#include <yori/version.h>

#include "yori_test.hpp"

namespace {

// 校验 MAJOR.MINOR.PATCH 前缀形状（允许后续里程碑追加后缀描述）。
bool has_semver_prefix(const char* s) {
  int digits = 0;
  int dots = 0;
  for (const char* p = s; *p != '\0'; ++p) {
    if (*p >= '0' && *p <= '9') {
      ++digits;
    } else if (*p == '.' && digits > 0) {
      ++dots;
      digits = 0;
    } else {
      break;
    }
  }
  return dots == 2 && digits > 0;
}

}  // namespace

int main() {
  YORI_CHECK(yori::version() != nullptr);
  YORI_CHECK(has_semver_prefix(yori::version()));
  YORI_CHECK(yori::kVersion[0] != '\0');
  return yori::testing::failure_count == 0 ? 0 : 1;
}
