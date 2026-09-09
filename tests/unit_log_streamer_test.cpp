#include <cstdio>
#include <string>
#include <vector>
#include <yori/ipc/ipc_protocol.hpp>
#include <yori/job/job.hpp>
#include <yori/observe/log_sink.hpp>

#include "runtime/log_streamer.hpp"
#include "yori_test.hpp"

namespace {

using namespace yori;
using namespace yori::runtime;
using observe::LogStreamKind;

const std::string kMarker = "[yori] dropped 16 bytes\n";

// 直接驱动数据面（不经过泵）：按序发布块。
void publish(LogStreamer& streamer, job::JobId job, LogStreamKind stream, const std::string& data,
             std::uint64_t begin) {
  const std::uint64_t end = begin + data.size();
  YORI_CHECK(streamer.publish_chunk(job, stream, data, begin, end) == LogPublishCode::kPublished);
}

void test_register_publish_subscribe() {
  LogStreamer streamer;
  std::string error;
  const job::JobId job{1};

  YORI_CHECK(streamer.register_job(job, error) == LogRegisterCode::kRegistered);
  YORI_CHECK(streamer.register_job(job, error) == LogRegisterCode::kAlreadyRegistered);
  const job::JobId invalid{};
  YORI_CHECK(streamer.register_job(invalid, error) == LogRegisterCode::kInvalidJob);

  publish(streamer, job, LogStreamKind::kStdout, "hello", 0);
  publish(streamer, job, LogStreamKind::kStderr, "warn", 0);

  // 默认订阅（无 since）：从当前末尾跟随，无回放。
  LogSubscribeResult result = streamer.subscribe(job, std::nullopt, std::nullopt);
  YORI_CHECK(result.code == LogSubscribeCode::kSubscribed);
  YORI_CHECK(result.finished == false);
  YORI_CHECK(result.session.replay(LogStreamKind::kStdout).start_offset == 5);
  YORI_CHECK(result.session.replay(LogStreamKind::kStderr).start_offset == 4);
  YORI_CHECK(result.session.replay(LogStreamKind::kStdout).chunks.empty());
  {
    // 订阅后发布：直播可见。
    publish(streamer, job, LogStreamKind::kStdout, " world", 5);
    LogChunk chunk;
    YORI_CHECK(result.session.subscription().try_receive(chunk));
    YORI_CHECK(chunk.kind == LogChunk::Kind::kData);
    YORI_CHECK(chunk.stream == LogStreamKind::kStdout);
    YORI_CHECK(chunk.begin_offset == 5 && chunk.end_offset == 11);
    YORI_CHECK((chunk.data == std::vector<std::uint8_t>{' ', 'w', 'o', 'r', 'l', 'd'}));
    YORI_CHECK(!result.session.subscription().try_receive(chunk));
  }

  // since 回放：从 3 起回放 [3, 11) 的块。
  LogSubscribeResult replayed = streamer.subscribe(job, std::uint64_t{3}, std::nullopt);
  YORI_CHECK(replayed.code == LogSubscribeCode::kSubscribed);
  const LogReplayPlan& stdout_plan = replayed.session.replay(LogStreamKind::kStdout);
  YORI_CHECK(stdout_plan.start_offset == 3);
  YORI_CHECK(stdout_plan.gap_bytes == 0);
  YORI_CHECK(stdout_plan.chunks.size() == 2);  // "hello"(0-5) + " world"(5-11)
  YORI_CHECK(stdout_plan.chunks[0].begin_offset == 0);
  YORI_CHECK(stdout_plan.chunks[1].end_offset == 11);
  YORI_CHECK(replayed.session.replay(LogStreamKind::kStderr).start_offset == 4);
  YORI_CHECK(replayed.session.replay(LogStreamKind::kStderr).gap_bytes == 0);
  YORI_CHECK(replayed.session.replay(LogStreamKind::kStderr).chunks.empty());  // 无 since 不回放

  {
    // 回放快照与直播交界：订阅后发布的新块经订阅到达，offset 衔接回放终点。
    publish(streamer, job, LogStreamKind::kStdout, "!", 11);
    LogChunk chunk;
    YORI_CHECK(replayed.session.subscription().try_receive(chunk));
    YORI_CHECK(chunk.begin_offset == 11 && chunk.end_offset == 12);
  }

  // 未来 offset：钳制为当前（不回退、不伪造）。
  LogSubscribeResult future = streamer.subscribe(job, std::uint64_t{1000}, std::nullopt);
  YORI_CHECK(future.code == LogSubscribeCode::kSubscribed);
  YORI_CHECK(future.session.replay(LogStreamKind::kStdout).start_offset == 12);
  YORI_CHECK(future.session.replay(LogStreamKind::kStdout).chunks.empty());

  YORI_CHECK(streamer.unregister_job(job) == LogUnregisterCode::kUnregistered);
  YORI_CHECK(streamer.unregister_job(job) == LogUnregisterCode::kNotFound);
  YORI_CHECK(streamer.publish_chunk(job, LogStreamKind::kStdout, "x", 11, 12) ==
             LogPublishCode::kNotRegistered);
}

void test_backlog_trim_and_gap() {
  LogStreamerConfig config;
  config.backlog_bytes_per_stream = LogStreamerConfig::kMinBacklogBytes;  // 64 KiB
  LogStreamer streamer(config);
  std::string error;
  const job::JobId job{2};
  YORI_CHECK(streamer.register_job(job, error) == LogRegisterCode::kRegistered);

  // 发布 4 块 32 KiB（共 128 KiB > 64 KiB 窗口），保留最新两块。
  const std::string block(std::size_t{32} * 1024, 'x');
  std::uint64_t offset = 0;
  for (int i = 0; i < 4; ++i) {
    publish(streamer, job, LogStreamKind::kStdout, block, offset);
    offset += block.size();
  }

  LogSubscribeResult recent = streamer.subscribe(job, std::uint64_t{96} * 1024, std::nullopt);
  YORI_CHECK(recent.code == LogSubscribeCode::kSubscribed);
  YORI_CHECK(recent.session.replay(LogStreamKind::kStdout).gap_bytes == 0);
  YORI_CHECK(recent.session.replay(LogStreamKind::kStdout).start_offset ==
             std::uint64_t{96} * 1024);
  YORI_CHECK(recent.session.replay(LogStreamKind::kStdout).chunks.size() == 1);

  // since 落在淘汰区间：GAP 跳到窗口起点（64 KiB）。
  LogSubscribeResult old = streamer.subscribe(job, std::uint64_t{1024}, std::nullopt);
  YORI_CHECK(old.code == LogSubscribeCode::kSubscribed);
  YORI_CHECK(old.session.replay(LogStreamKind::kStdout).gap_bytes ==
             std::uint64_t{64} * 1024 - 1024);
  YORI_CHECK(old.session.replay(LogStreamKind::kStdout).start_offset == std::uint64_t{64} * 1024);
  YORI_CHECK(old.session.replay(LogStreamKind::kStdout).chunks.size() == 2);

  YORI_CHECK(streamer.statistics().backlog_trims > 0);
}

void test_finish_and_late_subscribe() {
  LogStreamer streamer;
  std::string error;
  const job::JobId job{3};
  YORI_CHECK(streamer.register_job(job, error) == LogRegisterCode::kRegistered);
  publish(streamer, job, LogStreamKind::kStdout, "data", 0);

  // 先订阅再终态：EOF chunk 到达订阅，此后 publish 拒绝；finish 幂等。
  LogSubscribeResult live = streamer.subscribe(job, std::nullopt, std::nullopt);
  YORI_CHECK(live.code == LogSubscribeCode::kSubscribed);
  const ipc::IpcExitStatus exit_status{true, 0};
  YORI_CHECK(streamer.finish_job(job, static_cast<std::uint8_t>(yori::job::JobState::kFinished),
                                 exit_status) == LogFinishCode::kFinished);
  YORI_CHECK(streamer.finish_job(job, static_cast<std::uint8_t>(yori::job::JobState::kFinished),
                                 exit_status) == LogFinishCode::kAlreadyFinished);
  YORI_CHECK(streamer.publish_chunk(job, LogStreamKind::kStdout, "x", 4, 5) ==
             LogPublishCode::kJobFinished);
  {
    LogChunk chunk;
    YORI_CHECK(live.session.subscription().try_receive(chunk));
    YORI_CHECK(chunk.kind == LogChunk::Kind::kEof);
    YORI_CHECK(chunk.job_state == static_cast<std::uint8_t>(yori::job::JobState::kFinished));
    YORI_CHECK(chunk.exit && chunk.exit->exited_normally);
    // Topic 已关闭：排空后不再有数据。
    YORI_CHECK(!live.session.subscription().try_receive(chunk));
    YORI_CHECK(live.session.subscription().is_closed());
  }

  // 迟到订阅：回放 + 终态上下文（会话侧回放后以 EOF 收尾）。
  LogSubscribeResult late = streamer.subscribe(job, std::uint64_t{0}, std::uint64_t{0});
  YORI_CHECK(late.code == LogSubscribeCode::kSubscribed);
  YORI_CHECK(late.finished);
  YORI_CHECK(late.job_state == static_cast<std::uint8_t>(yori::job::JobState::kFinished));
  YORI_CHECK(late.exit && late.exit->exited_normally);
  YORI_CHECK(late.session.replay(LogStreamKind::kStdout).chunks.size() == 1);

  YORI_CHECK(streamer.finish_job(job::JobId{77}, 0, std::nullopt) == LogFinishCode::kNotFound);
}

void test_session_limits() {
  LogStreamerConfig config;
  config.max_sessions_per_job = 2;
  config.max_total_sessions = 3;
  LogStreamer streamer(config);
  std::string error;
  const job::JobId first{4};
  const job::JobId second{5};
  YORI_CHECK(streamer.register_job(first, error) == LogRegisterCode::kRegistered);
  YORI_CHECK(streamer.register_job(second, error) == LogRegisterCode::kRegistered);

  auto session_a = streamer.subscribe(first, std::nullopt, std::nullopt);
  auto session_b = streamer.subscribe(first, std::nullopt, std::nullopt);
  YORI_CHECK(session_a.code == LogSubscribeCode::kSubscribed);
  YORI_CHECK(session_b.code == LogSubscribeCode::kSubscribed);

  // 每 Job 上限。
  auto session_c = streamer.subscribe(first, std::nullopt, std::nullopt);
  YORI_CHECK(session_c.code == LogSubscribeCode::kSessionLimitJob);

  // 全局上限（2/3 已用）。
  auto session_d = streamer.subscribe(second, std::nullopt, std::nullopt);
  YORI_CHECK(session_d.code == LogSubscribeCode::kSubscribed);
  auto session_e = streamer.subscribe(second, std::nullopt, std::nullopt);
  YORI_CHECK(session_e.code == LogSubscribeCode::kSessionLimitTotal);

  // 未知 Job。
  auto session_f = streamer.subscribe(job::JobId{99}, std::nullopt, std::nullopt);
  YORI_CHECK(session_f.code == LogSubscribeCode::kJobUnknown);

  // RAII 归还：释放一个会话后计数回落、可再入。
  session_b = LogSubscribeResult{};
  YORI_CHECK(streamer.statistics().active_sessions == 2);
  auto session_g = streamer.subscribe(first, std::nullopt, std::nullopt);
  YORI_CHECK(session_g.code == LogSubscribeCode::kSubscribed);
}

void test_drop_marker_and_offset_validation() {
  LogStreamer streamer;
  std::string error;
  const job::JobId job{6};
  YORI_CHECK(streamer.register_job(job, error) == LogRegisterCode::kRegistered);

  publish(streamer, job, LogStreamKind::kStdout, "abc", 0);
  YORI_CHECK(streamer.publish_drop_marker(job, LogStreamKind::kStdout, 3, 16) ==
             LogPublishCode::kPublished);
  // 标记不前进 offset：下块仍须衔接 3。
  publish(streamer, job, LogStreamKind::kStdout, "def", 3);

  LogSubscribeResult result = streamer.subscribe(job, std::uint64_t{0}, std::nullopt);
  const LogReplayPlan& plan = result.session.replay(LogStreamKind::kStdout);
  YORI_CHECK(plan.chunks.size() == 3);
  const std::string marker_text = observe::drop_marker_text(16);
  YORI_CHECK(plan.chunks[1].begin_offset == 3 && plan.chunks[1].end_offset == 3);
  YORI_CHECK(std::string(plan.chunks[1].data.begin(), plan.chunks[1].data.end()) == marker_text);
  YORI_CHECK(plan.chunks[2].begin_offset == 3 && plan.chunks[2].end_offset == 6);

  // offset 衔接违规显式拒绝（RULE-08：不静默接受错位数据）。
  YORI_CHECK(streamer.publish_chunk(job, LogStreamKind::kStdout, "zzz", 99, 102) ==
             LogPublishCode::kInvalidOffset);
  // 空块与 end < begin 拒绝。
  YORI_CHECK(streamer.publish_chunk(job, LogStreamKind::kStdout, "", 6, 6) ==
             LogPublishCode::kInvalidOffset);
  YORI_CHECK(streamer.publish_chunk(job, LogStreamKind::kStdout, "ab", 8, 7) ==
             LogPublishCode::kInvalidOffset);
}

}  // namespace

int main() {
  test_register_publish_subscribe();
  test_backlog_trim_and_gap();
  test_finish_and_late_subscribe();
  test_session_limits();
  test_drop_marker_and_offset_validation();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "log streamer: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("log streamer: all checks passed\n");
  return 0;
}
