#include <grp.h>
#include <pwd.h>
// POSIX sigwait/sigprocmask/sigset_t 在 signal.h（非 csignal）
#include <signal.h>  // NOLINT(modernize-deprecated-headers)
#include <unistd.h>
#include <yori/version.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <yori/gpu/nvml_gpu_provider.hpp>
#include <yori/store/sqlite_state_store.hpp>

#include "runtime/daemon.hpp"
#include "runtime/executor_runtime.hpp"
#include "runtime/ipc_server.hpp"
#include "runtime/serial_state_store.hpp"

namespace {

constexpr const char* kDefaultSocketPath = "/run/yori/yori.sock";
constexpr const char* kDefaultStateDb = "/var/lib/yori/state.db";
constexpr const char* kDefaultLogRoot = "/var/lib/yori/jobs";
constexpr const char* kDefaultGpuLibrary = "libnvidia-ml.so.1";
constexpr const char* kDefaultSqliteLibrary = "libsqlite3.so.0";

constexpr std::uint32_t kDefaultSocketMode = 0660;

struct DaemonArguments final {
  std::string socket_path{kDefaultSocketPath};
  std::uint32_t socket_mode{kDefaultSocketMode};
  bool socket_group_set{false};
  std::uint32_t socket_group{0};
  std::vector<std::uint32_t> admin_gids;
  std::string state_db{kDefaultStateDb};
  std::string log_root{kDefaultLogRoot};
  std::string gpu_library{kDefaultGpuLibrary};
  std::string sqlite_library{kDefaultSqliteLibrary};
};

void print_usage() {
  std::fprintf(stderr,
               "usage: yorid [options]\n"
               "options:\n"
               "  --socket PATH          IPC endpoint (default %s)\n"
               "  --socket-mode OCTAL    endpoint mode (default 0660)\n"
               "  --socket-group GID     endpoint group; applies root:GID ownership\n"
               "                         (root only, DEC-010)\n"
               "  --admin-gid GID        admin group; members may view and cancel any\n"
               "                         job (repeatable)\n"
               "  --state-db PATH        SQLite state database (default %s)\n"
               "  --log-root PATH        job log root (default %s)\n"
               "  --gpu-library PATH     NVML library (default %s)\n"
               "  --sqlite-library PATH  SQLite library (default %s)\n"
               "  --version              print version and exit\n",
               kDefaultSocketPath, kDefaultStateDb, kDefaultLogRoot, kDefaultGpuLibrary,
               kDefaultSqliteLibrary);
}

bool parse_u32(const char* text, int base, std::uint32_t& out) {
  errno = 0;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, base);
  if (errno != 0 || end == text || *end != '\0' || value > 0xffffffffUL) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

// DEC-010：admin 组成员在启动时解析一次（主 GID 匹配之外补充补充组成员）。
void resolve_admin_members(const std::vector<std::uint32_t>& gids,
                           std::vector<std::uint32_t>& admin_uids) {
  for (const std::uint32_t gid : gids) {
    const group* group_entry = ::getgrgid(static_cast<gid_t>(gid));
    if (group_entry == nullptr || group_entry->gr_mem == nullptr) {
      continue;
    }
    for (const char* const* member = group_entry->gr_mem; *member != nullptr; ++member) {
      const passwd* member_entry = ::getpwnam(*member);
      if (member_entry != nullptr) {
        admin_uids.push_back(static_cast<std::uint32_t>(member_entry->pw_uid));
      }
    }
  }
}

int parse_arguments(int argc, char* argv[], DaemonArguments& args, bool& valid) {
  valid = true;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto take_value = [&](const char* name, std::string& out) -> bool {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "yorid: %s requires a value\n", name);
        return false;
      }
      out = argv[++i];
      return true;
    };
    if (flag == "--version") {
      std::printf("yorid %s\n", yori::version());
      return 0;
    }
    if (flag == "--help" || flag == "-h") {
      print_usage();
      return 2;
    }
    if (flag == "--socket") {
      if (!take_value("--socket", args.socket_path)) {
        valid = false;
      }
      continue;
    }
    if (flag == "--socket-mode") {
      std::string value;
      if (!take_value("--socket-mode", value) || !parse_u32(value.c_str(), 8, args.socket_mode) ||
          args.socket_mode > 0777) {
        std::fprintf(stderr, "yorid: --socket-mode expects an octal mode <= 0777\n");
        valid = false;
      }
      continue;
    }
    if (flag == "--socket-group") {
      std::string value;
      if (!take_value("--socket-group", value) ||
          !parse_u32(value.c_str(), 10, args.socket_group)) {
        std::fprintf(stderr, "yorid: --socket-group expects a numeric GID\n");
        valid = false;
      } else {
        args.socket_group_set = true;
      }
      continue;
    }
    if (flag == "--admin-gid") {
      std::string value;
      std::uint32_t gid = 0;
      if (!take_value("--admin-gid", value) || !parse_u32(value.c_str(), 10, gid)) {
        std::fprintf(stderr, "yorid: --admin-gid expects a numeric GID\n");
        valid = false;
      } else {
        args.admin_gids.push_back(gid);
      }
      continue;
    }
    if (flag == "--state-db") {
      if (!take_value("--state-db", args.state_db)) {
        valid = false;
      }
      continue;
    }
    if (flag == "--log-root") {
      if (!take_value("--log-root", args.log_root)) {
        valid = false;
      }
      continue;
    }
    if (flag == "--gpu-library") {
      if (!take_value("--gpu-library", args.gpu_library)) {
        valid = false;
      }
      continue;
    }
    if (flag == "--sqlite-library") {
      if (!take_value("--sqlite-library", args.sqlite_library)) {
        valid = false;
      }
      continue;
    }
    std::fprintf(stderr, "yorid: unknown option %s\n", flag.c_str());
    valid = false;
  }
  if (!valid) {
    print_usage();
  }
  return -1;
}

}  // namespace

// yorid 主生命周期：Executor owner（EXEC-01）。启动序为 store 打开 -> 恢复
// -> GPU 观察 -> IPC（由 Daemon 承载）；停止序为 SIGTERM/SIGINT -> Daemon
// 停止（EXEC-10 ①③）-> Executor shutdown（主线程，非 worker）。
int main(int argc, char* argv[]) {
  DaemonArguments args;
  bool arguments_valid = false;
  const int parse_result = parse_arguments(argc, argv, args, arguments_valid);
  if (parse_result == 0) {
    return 0;
  }
  if (parse_result == 2) {
    return 0;
  }
  if (!arguments_valid) {
    return 2;
  }

  // 信号处理在创建任何 worker 之前完成：主线程 sigwait，不安装异步 handler。
  // （sigset_t 经 signal.h 的 glibc 内部 typedef 提供，include-cleaner 误报）
  sigset_t wait_signals;  // NOLINT(misc-include-cleaner)
  static_cast<void>(sigemptyset(&wait_signals));
  static_cast<void>(sigaddset(&wait_signals, SIGINT));
  static_cast<void>(sigaddset(&wait_signals, SIGTERM));
  if (::sigprocmask(SIG_BLOCK, &wait_signals, nullptr) != 0) {
    std::fprintf(stderr, "yorid: cannot block signals: %s\n", std::strerror(errno));
    return 1;
  }

  yori::runtime::ExecutorRuntime executor_runtime;
  std::string runtime_error;
  if (!executor_runtime.initialize({}, runtime_error)) {
    std::fprintf(stderr, "yorid: executor init failed: %s\n", runtime_error.c_str());
    return 1;
  }

  // 失败路径统一：打印、从主线程关闭 Executor、退出。
  const auto fail = [&executor_runtime](const char* what, const std::string& code,
                                        const std::string& detail) {
    std::fprintf(stderr, "yorid: %s failed: %s%s%s\n", what, code.c_str(),
                 detail.empty() ? "" : " (", detail.c_str());
    static_cast<void>(executor_runtime.shutdown());
    return 1;
  };

  // StateStore（SQLite，DEC-009）；运行期读（IPC worker）与写（JobManager，
  // EXEC-08）经 SerialStateStore 的所有权互斥串行化（M7 守护总装）。
  yori::store::SqliteStateStoreConfig store_config;
  store_config.library_path = args.sqlite_library;
  store_config.database_path = args.state_db;
  auto store = std::make_unique<yori::store::SqliteStateStore>(store_config);
  const yori::store::SqliteStoreOpenResult store_open = store->open();
  if (!store_open.ok()) {
    return fail("state store open", yori::store::to_string(store_open.code), store_open.detail);
  }
  auto serialized_store = std::make_unique<yori::runtime::SerialStateStore>(std::move(store));

  // GpuProvider（NVML 适配，管理员配置的库路径，基线 20）。
  yori::gpu::NvmlGpuProviderConfig gpu_config;
  gpu_config.library_path = args.gpu_library;
  yori::gpu::NvmlGpuProvider gpu_provider(gpu_config);
  const yori::gpu::NvmlProviderLoadResult gpu_load = gpu_provider.load();
  if (!gpu_load.ok()) {
    return fail("nvml load", yori::gpu::to_string(gpu_load.code), gpu_load.detail);
  }

  // 端点治理（DEC-010）：root + --socket-group 应用 root:GID；非 root 显式
  // 拒绝配置他人的属主。
  yori::runtime::UdsIpcServerConfig ipc_config;
  ipc_config.socket_path = args.socket_path;
  ipc_config.socket_mode = args.socket_mode;
  if (args.socket_group_set && ::geteuid() != 0) {
    std::fprintf(stderr, "yorid: --socket-group requires root (DEC-010: non-root cannot chown)\n");
    static_cast<void>(executor_runtime.shutdown());
    return 1;
  }
  if (args.socket_group_set) {
    ipc_config.apply_ownership = true;
    ipc_config.socket_owner_uid = 0;
    ipc_config.socket_owner_gid = args.socket_group;
  }

  yori::runtime::DaemonConfig daemon_config;
  daemon_config.ipc = ipc_config;
  daemon_config.service.admin_gids = args.admin_gids;
  resolve_admin_members(args.admin_gids, daemon_config.service.admin_uids);
  daemon_config.job_manager.log_root = args.log_root;
  // DEC-006：daemon 环境快照（白名单继承源）。
  for (char* const* entry = ::environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view text{*entry};
    const auto separator = text.find('=');
    if (separator == std::string_view::npos || separator == 0) {
      continue;
    }
    daemon_config.job_manager.daemon_environment.emplace_back(
        std::string{text.substr(0, separator)}, std::string{text.substr(separator + 1)});
  }

  yori::runtime::Daemon daemon(executor_runtime.executor(), gpu_provider,
                               std::move(serialized_store), daemon_config);
  const yori::runtime::DaemonStartResult daemon_start = daemon.start();
  if (!daemon_start.ok()) {
    return fail("daemon start", yori::runtime::to_string(daemon_start.code), daemon_start.message);
  }

  std::fprintf(stderr, "yorid: serving on %s\n", args.socket_path.c_str());

  int received_signal = 0;
  while (::sigwait(&wait_signals, &received_signal) != 0) {
    // EINTR 等瞬态错误后继续等待。
  }

  std::fprintf(stderr, "yorid: received signal %d, shutting down\n", received_signal);
  static_cast<void>(daemon.stop());
  const auto shutdown_code = executor_runtime.shutdown();
  return shutdown_code == yori::runtime::ExecutorRuntimeShutdownResult::kRequestedFromWorker ? 1
                                                                                             : 0;
}
