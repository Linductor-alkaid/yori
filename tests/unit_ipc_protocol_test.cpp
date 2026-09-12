#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <yori/ipc/ipc_protocol.hpp>

#include "yori_test.hpp"

namespace {

using namespace yori::ipc;

// 编码 -> 解码 roundtrip 辅助。
bool roundtrip_request(const IpcRequest& request, IpcRequest& decoded) {
  std::vector<std::uint8_t> frame;
  if (!append_request_frame(request, frame)) {
    return false;
  }
  // 帧布局：u32 长度 + payload。
  if (frame.size() < 4) {
    return false;
  }
  const std::uint32_t length =
      static_cast<std::uint32_t>(frame[0]) | (static_cast<std::uint32_t>(frame[1]) << 8) |
      (static_cast<std::uint32_t>(frame[2]) << 16) | (static_cast<std::uint32_t>(frame[3]) << 24);
  if (length != frame.size() - 4 || !ipc_payload_length_valid(length)) {
    return false;
  }
  const IpcRequestDecodeResult result = decode_request_payload(frame.data() + 4, length);
  if (!result.ok()) {
    return false;
  }
  decoded = result.value;
  return true;
}

bool roundtrip_response(const IpcResponse& response, IpcResponse& decoded) {
  std::vector<std::uint8_t> frame;
  if (!append_response_frame(response, frame)) {
    return false;
  }
  const std::uint32_t length =
      static_cast<std::uint32_t>(frame[0]) | (static_cast<std::uint32_t>(frame[1]) << 8) |
      (static_cast<std::uint32_t>(frame[2]) << 16) | (static_cast<std::uint32_t>(frame[3]) << 24);
  const IpcResponseDecodeResult result = decode_response_payload(frame.data() + 4, length);
  if (!result.ok()) {
    return false;
  }
  decoded = result.value;
  return true;
}

IpcSubmitRequest sample_submit() {
  IpcSubmitRequest submit;
  submit.argv = {"python", "train.py", "--epochs", "10"};
  submit.cwd = "/srv/training";
  submit.env = {{"TRAIN_DIR", "/data"}, {"LOG_LEVEL", "info"}};
  submit.gpu_request = 1;
  submit.launch_profile = std::string("pytorch");
  submit.tensorboard_logdir = std::string("runs/exp1");
  return submit;
}

void test_request_roundtrips() {
  IpcRequest request;
  request.kind = IpcRequestKind::kSubmit;
  request.submit = sample_submit();
  IpcRequest decoded;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.kind == IpcRequestKind::kSubmit);
  YORI_CHECK(decoded.submit.argv == request.submit.argv);
  YORI_CHECK(decoded.submit.cwd == request.submit.cwd);
  YORI_CHECK(decoded.submit.env == request.submit.env);
  YORI_CHECK(decoded.submit.gpu_request == 1);
  YORI_CHECK(decoded.submit.launch_profile == std::string("pytorch"));
  YORI_CHECK(decoded.submit.tensorboard_logdir == std::string("runs/exp1"));

  request = IpcRequest{};
  request.kind = IpcRequestKind::kPs;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.kind == IpcRequestKind::kPs);

  request.kind = IpcRequestKind::kQueue;
  YORI_CHECK(roundtrip_request(request, decoded));
  request.kind = IpcRequestKind::kGpu;
  YORI_CHECK(roundtrip_request(request, decoded));

  request.kind = IpcRequestKind::kCancel;
  request.cancel.job_id = 42;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.cancel.job_id == 42);

  request.kind = IpcRequestKind::kLogs;
  request.logs.job_id = 7;
  request.logs.max_bytes = 4096;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.logs.job_id == 7 && decoded.logs.max_bytes == 4096);

  // submit 的空可选字段（无 profile / logdir、空 env）。
  IpcSubmitRequest bare;
  bare.argv = {"train"};
  bare.cwd = "/tmp";
  request.kind = IpcRequestKind::kSubmit;
  request.submit = std::move(bare);
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(!decoded.submit.launch_profile.has_value());
  YORI_CHECK(!decoded.submit.tensorboard_logdir.has_value());
  YORI_CHECK(decoded.submit.env.empty());
}

void test_response_roundtrips() {
  IpcResponse response;
  response.kind = IpcRequestKind::kSubmit;
  response.error = IpcError::kNone;
  response.job_id = 99;
  IpcResponse decoded;
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.job_id == 99 && decoded.error == IpcError::kNone);

  response.kind = IpcRequestKind::kPs;
  response.jobs.push_back(IpcJobSummary{
      1, 0, 1000, 3, false, {"python", "train.py"}, "/srv", std::string("runs/x"), std::nullopt});
  response.jobs.push_back(
      IpcJobSummary{2, 5, 1001, 2, true, {}, "", std::nullopt, IpcExitStatus{false, 9}});
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.jobs.size() == 2);
  YORI_CHECK(!decoded.jobs[0].masked && decoded.jobs[0].argv.size() == 2 &&
             decoded.jobs[0].exit == std::nullopt);
  YORI_CHECK(decoded.jobs[1].masked && decoded.jobs[1].argv.empty());
  const auto& second_exit = decoded.jobs[1].exit;
  YORI_CHECK(second_exit.has_value() && !second_exit->exited_normally && second_exit->code == 9);

  response = IpcResponse{};
  response.kind = IpcRequestKind::kQueue;
  response.queue.push_back(IpcQueueEntry{5, 1000, 1725900000123456789ULL, 0});
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.queue.size() == 1 && decoded.queue[0].job_id == 5 &&
             decoded.queue[0].submit_time_unix_ns == 1725900000123456789ULL);

  response = IpcResponse{};
  response.kind = IpcRequestKind::kGpu;
  response.gpu_revision = 12;
  IpcGpuDevice device;
  device.uuid = "GPU-abc";
  device.index = 3;
  device.observed_state = 0;
  device.logical_state = 2;
  device.utilization_percent = 77;
  device.memory_used_bytes = 1000;
  device.memory_total_bytes = 4000;
  device.leased_by_job = 5;
  response.devices.push_back(device);
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.devices.size() == 1 && decoded.gpu_revision == 12);
  YORI_CHECK(decoded.devices[0].uuid == "GPU-abc" && decoded.devices[0].index == 3 &&
             decoded.devices[0].logical_state == 2);
  YORI_CHECK(decoded.devices[0].utilization_percent == std::uint32_t{77});
  YORI_CHECK(decoded.devices[0].leased_by_job == std::uint64_t{5});

  response = IpcResponse{};
  response.kind = IpcRequestKind::kCancel;
  response.error = IpcError::kInvalidState;
  response.detail = "running";
  response.state = 3;
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.error == IpcError::kInvalidState && decoded.state == 3 &&
             decoded.detail == "running");

  response = IpcResponse{};
  response.kind = IpcRequestKind::kLogs;
  response.logs.stdout_truncated = true;
  response.logs.stdout_tail = {0x61, 0x62, 0x63, 0x00, 0xff};
  response.logs.stderr_tail = {0x01};
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.logs.stdout_truncated && !decoded.logs.stderr_truncated);
  YORI_CHECK(decoded.logs.stdout_tail == response.logs.stdout_tail);
  YORI_CHECK(decoded.logs.stderr_tail == response.logs.stderr_tail);
}

void test_malformed_requests() {
  // 空 / 过短输入。
  IpcRequestDecodeResult result = decode_request_payload(nullptr, 0);
  YORI_CHECK(result.error == IpcDecodeError::kTruncated);

  const std::uint8_t only_version[1] = {1};
  result = decode_request_payload(only_version, 1);
  YORI_CHECK(result.error == IpcDecodeError::kTruncated);

  // 坏版本（v2 起接受 1/2，3 仍非法）。
  const std::uint8_t bad_version[2] = {3, static_cast<std::uint8_t>(IpcRequestKind::kPs)};
  result = decode_request_payload(bad_version, sizeof(bad_version));
  YORI_CHECK(result.error == IpcDecodeError::kBadVersion);

  // 坏 kind。
  const std::uint8_t bad_kind[2] = {1, 0};
  result = decode_request_payload(bad_kind, sizeof(bad_kind));
  YORI_CHECK(result.error == IpcDecodeError::kBadKind);
  const std::uint8_t bad_kind_high[2] = {1, 10};
  result = decode_request_payload(bad_kind_high, sizeof(bad_kind_high));
  YORI_CHECK(result.error == IpcDecodeError::kBadKind);
  // INSPECT 是 v2 kind：v1 帧携带判坏 kind。
  const std::uint8_t v1_inspect[2] = {1, static_cast<std::uint8_t>(IpcRequestKind::kInspect)};
  result = decode_request_payload(v1_inspect, sizeof(v1_inspect));
  YORI_CHECK(result.error == IpcDecodeError::kBadKind);

  // 空 kind 之后的尾部字节。
  const std::uint8_t trailing[3] = {1, static_cast<std::uint8_t>(IpcRequestKind::kPs), 0};
  result = decode_request_payload(trailing, sizeof(trailing));
  YORI_CHECK(result.error == IpcDecodeError::kTrailingBytes);

  // 截断的 submit body。
  IpcRequest request;
  request.kind = IpcRequestKind::kSubmit;
  request.submit = sample_submit();
  std::vector<std::uint8_t> frame;
  YORI_CHECK(append_request_frame(request, frame));
  for (std::size_t cut = 4; cut < frame.size(); ++cut) {
    result = decode_request_payload(frame.data() + 4, cut - 4);
    // 每个截断点要么截断错误，要么恰好在合法边界完整解析（不可能：最后
    // 字节属于最后字段时除外——统一断言不允许 crash 且错误受控）。
    YORI_CHECK(result.error == IpcDecodeError::kTruncated || result.error == IpcDecodeError::kNone);
  }

  // 字符串含 NUL：构造 submit，argv_count=1，argv[0]="a\0b"。
  std::vector<std::uint8_t> payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kSubmit)};
  const auto push_u32 = [&payload](std::uint32_t v) {
    payload.push_back(static_cast<std::uint8_t>(v & 0xff));
    payload.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    payload.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    payload.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
  };
  push_u32(1);
  push_u32(3);
  payload.push_back('a');
  payload.push_back('\0');
  payload.push_back('b');
  result = decode_request_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kBadString);

  // argv 数量超限。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kSubmit)};
  push_u32(IpcProtocolLimits::kMaxItemCount + 1);
  result = decode_request_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kTooManyItems);

  // 单字符串超限：长度声明 kMaxStringBytes + 1（不实际提供内容）。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kSubmit)};
  push_u32(1);
  push_u32(IpcProtocolLimits::kMaxStringBytes + 1);
  result = decode_request_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kOversize);
}

void test_malformed_responses() {
  // 坏错误码值域。
  std::vector<std::uint8_t> payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kSubmit), 200};
  const auto push_u32 = [&payload](std::uint32_t v) {
    payload.push_back(static_cast<std::uint8_t>(v & 0xff));
    payload.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    payload.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    payload.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
  };
  push_u32(0);
  push_u32(0);
  IpcResponseDecodeResult result = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kInvalidValue);

  // 状态字节越界（ps 摘要 state=8；提供完整后续字段以到达值域检查）。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kPs),
             static_cast<std::uint8_t>(IpcError::kNone)};
  push_u32(0);
  push_u32(1);                                              // 1 个摘要
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 1});  // job_id=1
  payload.push_back(8);                                     // state 越界
  payload.insert(payload.end(), {0, 0, 0, 0});              // owner_uid
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 0});  // revision
  payload.push_back(0);                                     // masked=0
  payload.push_back(0);                                     // 无退出状态
  result = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kInvalidValue);

  // 布尔标记非 0/1（ps 摘要 masked=2）。重造至 masked 字段。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kPs),
             static_cast<std::uint8_t>(IpcError::kNone)};
  push_u32(0);
  push_u32(1);
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 1});  // job_id
  payload.push_back(0);                                     // state
  payload.insert(payload.end(), {0, 0, 0, 100});            // owner_uid
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 2});  // revision
  payload.push_back(2);                                     // masked 非法
  result = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kInvalidValue);

  // gpu utilization 越界（>100）。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kGpu),
             static_cast<std::uint8_t>(IpcError::kNone)};
  push_u32(0);                                              // detail
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 0});  // revision
  push_u32(1);                                              // 1 台设备
  push_u32(3);                                              // uuid 长度
  payload.insert(payload.end(), {'a', 'b', 'c'});
  payload.insert(payload.end(), {0, 0, 0, 0});  // index
  payload.push_back(0);                         // observed
  payload.push_back(0);                         // logical
  payload.push_back(1);                         // has_util
  push_u32(101);                                // utilization 越界
  result = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kInvalidValue);

  // 显存 used > total。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kGpu),
             static_cast<std::uint8_t>(IpcError::kNone)};
  push_u32(0);
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 0});
  push_u32(1);
  push_u32(3);
  payload.insert(payload.end(), {'a', 'b', 'c'});
  payload.insert(payload.end(), {0, 0, 0, 0});
  payload.push_back(0);
  payload.push_back(0);
  payload.push_back(0);                                      // no util
  payload.push_back(1);                                      // has memory
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 10});  // used=10
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 4});   // total=4
  result = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kInvalidValue);

  // 列表计数超限。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kPs),
             static_cast<std::uint8_t>(IpcError::kNone)};
  push_u32(0);
  push_u32(IpcProtocolLimits::kMaxListItems + 1);
  result = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kTooManyItems);

  // 空输入与超上限输入长度（decode_*_payload 以 size 判定，无需真实缓冲）。
  result = decode_response_payload(nullptr, 0);
  YORI_CHECK(result.error == IpcDecodeError::kTruncated);
  const std::uint32_t huge = IpcProtocolLimits::kMaxPayloadBytes + 1;
  IpcRequestDecodeResult request_result = decode_request_payload(nullptr, huge);
  YORI_CHECK(request_result.error == IpcDecodeError::kOversize);
  IpcResponseDecodeResult response_result = decode_response_payload(nullptr, huge);
  YORI_CHECK(response_result.error == IpcDecodeError::kOversize);
}

void test_encoder_rejects_oversize() {
  IpcRequest request;
  request.kind = IpcRequestKind::kSubmit;
  request.submit.argv.assign(IpcProtocolLimits::kMaxItemCount + 1, "x");
  std::vector<std::uint8_t> frame;
  YORI_CHECK(!append_request_frame(request, frame));

  request.submit.argv.assign(2, std::string(IpcProtocolLimits::kMaxStringBytes + 1, 'a'));
  YORI_CHECK(!append_request_frame(request, frame));

  request.submit.argv = {"x"};
  request.submit.cwd = std::string(2, 'b');  // 合法
  request.submit.argv[0] = std::string("a\0b", 3);
  YORI_CHECK(!append_request_frame(request, frame));

  // 总负载超限：argv 总字节 > 1 MiB（计数合法 2 条、单条合法 64 KiB）。
  IpcSubmitRequest big;
  big.argv.assign(16, std::string(std::size_t{64} * 1024, 'c'));  // 1 MiB，加开销必然超限
  big.cwd = "/tmp";
  request.submit = std::move(big);
  YORI_CHECK(!append_request_frame(request, frame));

  // 编码失败不留下半帧。
  YORI_CHECK(frame.empty());
}

void test_boundary_values() {
  // 63/64/65 KiB 字符串边界。
  for (const std::size_t size : {std::size_t{63} * 1024, std::size_t{64} * 1024}) {
    IpcRequest request;
    request.kind = IpcRequestKind::kSubmit;
    request.submit.argv = {std::string(size, 'x')};
    request.submit.cwd = "/tmp";
    std::vector<std::uint8_t> frame;
    YORI_CHECK(append_request_frame(request, frame));
    IpcRequest decoded;
    YORI_CHECK(roundtrip_request(request, decoded));
    YORI_CHECK(decoded.submit.argv[0].size() == size);
  }
  {
    IpcRequest request;
    request.kind = IpcRequestKind::kSubmit;
    request.submit.argv = {std::string(64 * 1024 + 1, 'x')};
    request.submit.cwd = "/tmp";
    std::vector<std::uint8_t> frame;
    YORI_CHECK(!append_request_frame(request, frame));
  }

  // 帧长度边界辅助。
  YORI_CHECK(!ipc_payload_length_valid(1));
  YORI_CHECK(ipc_payload_length_valid(2));
  YORI_CHECK(ipc_payload_length_valid(IpcProtocolLimits::kMaxPayloadBytes));
  YORI_CHECK(!ipc_payload_length_valid(IpcProtocolLimits::kMaxPayloadBytes + 1));
}

// ---------------------------------------------------------------------------
// M6：LOGS_FOLLOW / TENSORBOARD 请求与响应、流式帧族。
// ---------------------------------------------------------------------------

bool roundtrip_stream_frame(const IpcStreamFrame& frame, IpcStreamFrame& decoded) {
  std::vector<std::uint8_t> buffer;
  if (!append_stream_frame(frame, buffer)) {
    return false;
  }
  const std::uint32_t length =
      static_cast<std::uint32_t>(buffer[0]) | (static_cast<std::uint32_t>(buffer[1]) << 8) |
      (static_cast<std::uint32_t>(buffer[2]) << 16) | (static_cast<std::uint32_t>(buffer[3]) << 24);
  if (length != buffer.size() - 4) {
    return false;
  }
  const IpcStreamFrameDecodeResult result = decode_stream_frame_payload(buffer.data() + 4, length);
  if (!result.ok()) {
    return false;
  }
  decoded = result.value;
  return true;
}

void test_m6_request_roundtrips() {
  IpcRequest request;
  request.kind = IpcRequestKind::kLogsFollow;
  request.logs_follow.job_id = 9;
  request.logs_follow.since_stdout = std::uint64_t{4096};
  request.logs_follow.since_stderr = std::uint64_t{0};
  IpcRequest decoded;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.kind == IpcRequestKind::kLogsFollow);
  YORI_CHECK(decoded.logs_follow.job_id == 9);
  YORI_CHECK(decoded.logs_follow.since_stdout == std::uint64_t{4096});
  YORI_CHECK(decoded.logs_follow.since_stderr == std::uint64_t{0});

  // 不带 since（两流都从当前末尾跟随）。
  request.logs_follow.since_stdout.reset();
  request.logs_follow.since_stderr.reset();
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(!decoded.logs_follow.since_stdout && !decoded.logs_follow.since_stderr);

  request.kind = IpcRequestKind::kTensorboard;
  request.tensorboard.job_id = 11;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.kind == IpcRequestKind::kTensorboard);
  YORI_CHECK(decoded.tensorboard.job_id == 11);
}

void test_m6_response_roundtrips() {
  IpcResponse response;
  response.kind = IpcRequestKind::kLogsFollow;
  response.error = IpcError::kNone;
  response.logs_follow.job_state = 3;  // RUNNING
  response.logs_follow.stdout_offset = 100;
  response.logs_follow.stderr_offset = 200;
  IpcResponse decoded;
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.kind == IpcRequestKind::kLogsFollow);
  YORI_CHECK(decoded.logs_follow.job_state == 3);
  YORI_CHECK(decoded.logs_follow.stdout_offset == 100);
  YORI_CHECK(decoded.logs_follow.stderr_offset == 200);

  response.kind = IpcRequestKind::kTensorboard;
  response.tensorboard.logdir = std::string("runs/exp1");
  response.tensorboard.cwd = "/srv/training";
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.tensorboard.logdir == std::string("runs/exp1"));
  YORI_CHECK(decoded.tensorboard.cwd == "/srv/training");

  // 无 logdir 的 TENSORBOARD 响应（回退 cwd）。
  response.tensorboard.logdir.reset();
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(!decoded.tensorboard.logdir);
  YORI_CHECK(decoded.tensorboard.cwd == "/srv/training");
}

void test_m6_stream_frame_roundtrips() {
  IpcStreamFrame frame;
  IpcStreamFrame decoded;

  frame = IpcStreamFrame{};
  frame.kind = IpcStreamFrameKind::kLogData;
  frame.stream = 0;
  frame.begin_offset = 10;
  frame.end_offset = 20;
  frame.data = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};
  YORI_CHECK(roundtrip_stream_frame(frame, decoded));
  YORI_CHECK(decoded.kind == IpcStreamFrameKind::kLogData);
  YORI_CHECK(decoded.stream == 0);
  YORI_CHECK(decoded.begin_offset == 10 && decoded.end_offset == 20);
  YORI_CHECK(decoded.data == frame.data);

  // 丢弃标记 chunk：begin == end 且 data 非空。
  frame.stream = 1;
  frame.begin_offset = 20;
  frame.end_offset = 20;
  frame.data = {'[', 'y', 'o', 'r', 'i', ']'};
  YORI_CHECK(roundtrip_stream_frame(frame, decoded));
  YORI_CHECK(decoded.begin_offset == 20 && decoded.end_offset == 20);
  YORI_CHECK(decoded.data.size() == 6);

  frame = IpcStreamFrame{};
  frame.kind = IpcStreamFrameKind::kLogGap;
  frame.stream = 0;
  frame.begin_offset = 5;
  frame.end_offset = 1000;
  YORI_CHECK(roundtrip_stream_frame(frame, decoded));
  YORI_CHECK(decoded.kind == IpcStreamFrameKind::kLogGap);
  YORI_CHECK(decoded.begin_offset == 5 && decoded.end_offset == 1000);

  frame.kind = IpcStreamFrameKind::kLogBackpressure;
  frame.begin_offset = 4096;
  YORI_CHECK(roundtrip_stream_frame(frame, decoded));
  YORI_CHECK(decoded.kind == IpcStreamFrameKind::kLogBackpressure);
  YORI_CHECK(decoded.begin_offset == 4096);

  frame = IpcStreamFrame{};
  frame.kind = IpcStreamFrameKind::kLogEof;
  frame.job_state = 4;  // FINISHED
  frame.exit = IpcExitStatus{true, 0};
  YORI_CHECK(roundtrip_stream_frame(frame, decoded));
  YORI_CHECK(decoded.kind == IpcStreamFrameKind::kLogEof);
  YORI_CHECK(decoded.job_state == 4);
  YORI_CHECK(decoded.exit && decoded.exit->exited_normally && decoded.exit->code == 0);

  frame.exit = IpcExitStatus{false, 9};
  YORI_CHECK(roundtrip_stream_frame(frame, decoded));
  YORI_CHECK(decoded.exit && !decoded.exit->exited_normally && decoded.exit->code == 9);

  frame.exit.reset();
  YORI_CHECK(roundtrip_stream_frame(frame, decoded));
  YORI_CHECK(!decoded.exit);
}

void test_m6_stream_frame_malformed() {
  IpcStreamFrame frame;
  frame.kind = IpcStreamFrameKind::kLogData;
  frame.stream = 0;
  frame.begin_offset = 0;
  frame.end_offset = 3;
  frame.data = {'x', 'y', 'z'};
  std::vector<std::uint8_t> buffer;
  YORI_CHECK(append_stream_frame(frame, buffer));
  const std::uint8_t* payload = buffer.data() + 4;
  const std::size_t size = buffer.size() - 4;

  // 逐字节截断：仅允许 kTruncated。
  for (std::size_t cut = 0; cut < size; ++cut) {
    const IpcStreamFrameDecodeResult result = decode_stream_frame_payload(payload, cut);
    YORI_CHECK(result.error == IpcDecodeError::kTruncated);
  }

  // 完整解码 + 尾部多余字节。
  std::vector<std::uint8_t> trailing(payload, payload + size);
  trailing.push_back(0);
  YORI_CHECK(decode_stream_frame_payload(trailing.data(), trailing.size()).error ==
             IpcDecodeError::kTrailingBytes);

  // 坏版本 / 坏帧 kind。
  std::vector<std::uint8_t> bad_version(payload, payload + size);
  bad_version[0] = 2;
  YORI_CHECK(decode_stream_frame_payload(bad_version.data(), bad_version.size()).error ==
             IpcDecodeError::kBadVersion);
  std::vector<std::uint8_t> bad_kind(payload, payload + size);
  bad_kind[1] = 5;
  YORI_CHECK(decode_stream_frame_payload(bad_kind.data(), bad_kind.size()).error ==
             IpcDecodeError::kBadKind);
  bad_kind[1] = 0;
  YORI_CHECK(decode_stream_frame_payload(bad_kind.data(), bad_kind.size()).error ==
             IpcDecodeError::kBadKind);

  // 坏流标识（> 1）。
  std::vector<std::uint8_t> bad_stream(payload, payload + size);
  bad_stream[2] = 2;
  YORI_CHECK(decode_stream_frame_payload(bad_stream.data(), bad_stream.size()).error ==
             IpcDecodeError::kInvalidValue);

  // EOF 帧的坏终态字节与坏退出标记。
  IpcStreamFrame eof_frame;
  eof_frame.kind = IpcStreamFrameKind::kLogEof;
  eof_frame.job_state = 4;
  eof_frame.exit = IpcExitStatus{true, 0};
  std::vector<std::uint8_t> eof_buffer;
  YORI_CHECK(append_stream_frame(eof_frame, eof_buffer));
  std::vector<std::uint8_t> bad_state(eof_buffer.begin() + 4, eof_buffer.end());
  bad_state[2] = 8;
  YORI_CHECK(decode_stream_frame_payload(bad_state.data(), bad_state.size()).error ==
             IpcDecodeError::kInvalidValue);
  std::vector<std::uint8_t> bad_exit(eof_buffer.begin() + 4, eof_buffer.end());
  bad_exit[4] = 2;  // exited_normally 标记非法
  YORI_CHECK(decode_stream_frame_payload(bad_exit.data(), bad_exit.size()).error ==
             IpcDecodeError::kInvalidValue);

  // 编码端拒绝：end < begin、超限数据、坏流标识、坏终态。
  IpcStreamFrame invalid = frame;
  invalid.begin_offset = 5;
  invalid.end_offset = 3;
  invalid.data = {'x', 'y'};
  std::vector<std::uint8_t> reject;
  YORI_CHECK(!encode_stream_frame_payload(invalid, reject));
  invalid = frame;
  invalid.data.assign(IpcProtocolLimits::kMaxStreamDataBytes + 1, 'x');
  YORI_CHECK(!encode_stream_frame_payload(invalid, reject));
  invalid = frame;
  invalid.stream = 7;
  YORI_CHECK(!encode_stream_frame_payload(invalid, reject));
  invalid = IpcStreamFrame{};
  invalid.kind = IpcStreamFrameKind::kLogEof;
  invalid.job_state = 200;
  YORI_CHECK(!encode_stream_frame_payload(invalid, reject));

  // 数据字段超过流式上限（但低于通用字节数组上限）时编码拒绝；解码端以
  // 手工构造的超限帧验证 kOversize。
  IpcStreamFrame oversize = frame;
  oversize.begin_offset = 0;
  oversize.end_offset = std::uint64_t{300} * 1024;
  oversize.data.assign(static_cast<std::size_t>(300) * 1024, 'x');
  std::vector<std::uint8_t> oversize_reject;
  YORI_CHECK(!encode_stream_frame_payload(oversize, oversize_reject));

  const std::uint32_t oversize_length = static_cast<std::uint32_t>(std::uint64_t{300} * 1024);
  std::vector<std::uint8_t> handcrafted = {0x01, 0x01, 0x00};
  handcrafted.insert(handcrafted.end(), 8, 0);  // begin = 0
  handcrafted.insert(handcrafted.end(), 8, 0);  // end = 0
  for (int shift = 0; shift < 32; shift += 8) {
    handcrafted.push_back(static_cast<std::uint8_t>((oversize_length >> shift) & 0xffu));
  }
  handcrafted.insert(handcrafted.end(), oversize_length, 'x');
  const IpcStreamFrameDecodeResult oversize_decoded =
      decode_stream_frame_payload(handcrafted.data(), handcrafted.size());
  YORI_CHECK(oversize_decoded.error == IpcDecodeError::kOversize);
}

// golden vector（M5 风险项收口）：固定字节序列锁定线格式。
void test_m6_golden_vectors() {
  // LOGS_FOLLOW 请求：version=1, kind=7, job=9, since_stdout 有（4096），
  // since_stderr 无。
  IpcRequest request;
  request.kind = IpcRequestKind::kLogsFollow;
  request.version = 1;  // v1 客户端形态（v2 daemon 仍接受）
  request.logs_follow.job_id = 9;
  request.logs_follow.since_stdout = std::uint64_t{4096};
  std::vector<std::uint8_t> buffer;
  YORI_CHECK(append_request_frame(request, buffer));
  const std::vector<std::uint8_t> expected_request = {
      0x14, 0x00, 0x00, 0x00,                          // 长度 20（2 头 + 8 + 1 + 8 + 1）
      0x01, 0x07,                                      // version 1, kind 7
      0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // job 9
      0x01,                                            // since_stdout 存在
      0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 4096
      0x00,                                            // since_stderr 不存在
  };
  YORI_CHECK(buffer == expected_request);

  // LOG_DATA 流式帧：stream=1, begin=2, end=5, data="abc"。
  IpcStreamFrame frame;
  frame.kind = IpcStreamFrameKind::kLogData;
  frame.stream = 1;
  frame.begin_offset = 2;
  frame.end_offset = 5;
  frame.data = {'a', 'b', 'c'};
  buffer.clear();
  YORI_CHECK(append_stream_frame(frame, buffer));
  const std::vector<std::uint8_t> expected_frame = {
      0x1a, 0x00, 0x00, 0x00,                          // 长度 26（2 头 + 1 + 8 + 8 + 4 + 3）
      0x01, 0x01,                                      // version 1, LOG_DATA
      0x01,                                            // stderr
      0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // begin 2
      0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // end 5
      0x03, 0x00, 0x00, 0x00,                          // 数据长度 3
      'a',  'b',  'c',
  };
  YORI_CHECK(buffer == expected_frame);

  // LOG_EOF 流式帧：job_state=4（FINISHED），exited 0。
  frame = IpcStreamFrame{};
  frame.kind = IpcStreamFrameKind::kLogEof;
  frame.job_state = 4;
  frame.exit = IpcExitStatus{true, 0};
  buffer.clear();
  YORI_CHECK(append_stream_frame(frame, buffer));
  const std::vector<std::uint8_t> expected_eof = {
      0x09, 0x00, 0x00, 0x00,  // 长度 9（2 头 + 1 + 1 + 1 + 4）
      0x01, 0x04,              // version 1, LOG_EOF
      0x04,                    // FINISHED
      0x01,                    // exit 存在
      0x01,                    // exited_normally
      0x00, 0x00, 0x00, 0x00,  // code 0
  };
  YORI_CHECK(buffer == expected_eof);
}


// ---------------------------------------------------------------------------
// M8：协议 v2（DEC-011）——SUBMIT 扩展字段、INSPECT、版本协商。
// ---------------------------------------------------------------------------

void test_m8_submit_v2_roundtrip() {
  // v2 SUBMIT：捕获 executable 与 env 元数据往返。
  IpcRequest request;
  request.kind = IpcRequestKind::kSubmit;
  request.submit = sample_submit();
  request.submit.executable = std::string("/opt/conda/envs/train/bin/python");
  request.submit.env_metadata =
      IpcEnvMetadata{1, std::string("3.11.5")};  // conda
  IpcRequest decoded;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.submit.executable == request.submit.executable);
  YORI_CHECK(decoded.submit.env_metadata.has_value());
  if (decoded.submit.env_metadata) {
    YORI_CHECK(decoded.submit.env_metadata->source == 1);
    YORI_CHECK(decoded.submit.env_metadata->python_version == std::string("3.11.5"));
  }

  // v2 无捕获（字段缺省）同样合法。
  request.submit.executable.reset();
  request.submit.env_metadata.reset();
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(!decoded.submit.executable.has_value());
  YORI_CHECK(!decoded.submit.env_metadata.has_value());

  // v1 SUBMIT：编码不含扩展字段，解码按无捕获处理。
  request.version = 1;
  std::vector<std::uint8_t> frame;
  YORI_CHECK(append_request_frame(request, frame));
  const IpcRequestDecodeResult v1_result =
      decode_request_payload(frame.data() + 4, frame.size() - 4);
  YORI_CHECK(v1_result.ok());
  YORI_CHECK(!v1_result.value.submit.executable.has_value());
  YORI_CHECK(!v1_result.value.submit.env_metadata.has_value());
  YORI_CHECK(v1_result.value.version == 1);

  // v1 帧携带 v2 字段：编码拒绝（版本与字段一致性）。
  IpcRequest inconsistent;
  inconsistent.kind = IpcRequestKind::kSubmit;
  inconsistent.version = 1;
  inconsistent.submit = sample_submit();
  inconsistent.submit.executable = std::string("/bin/true");
  std::vector<std::uint8_t> rejected;
  YORI_CHECK(!append_request_frame(inconsistent, rejected));

  // v2 SUBMIT 截断矩阵。
  request.version = 2;
  request.submit.executable = std::string("/bin/python3");
  request.submit.env_metadata = IpcEnvMetadata{2, std::string("3.12")};
  frame.clear();
  YORI_CHECK(append_request_frame(request, frame));
  for (std::size_t cut = 4; cut < frame.size(); ++cut) {
    const IpcRequestDecodeResult cut_result =
        decode_request_payload(frame.data() + 4, cut - 4);
    YORI_CHECK(cut_result.error == IpcDecodeError::kTruncated ||
               cut_result.error == IpcDecodeError::kNone);
  }
}

void test_m8_inspect_roundtrip() {
  IpcRequest request;
  request.kind = IpcRequestKind::kInspect;
  request.inspect.job_id = 77;
  IpcRequest decoded;
  YORI_CHECK(roundtrip_request(request, decoded));
  YORI_CHECK(decoded.kind == IpcRequestKind::kInspect);
  YORI_CHECK(decoded.inspect.job_id == 77);

  IpcResponse response;
  response.kind = IpcRequestKind::kInspect;
  IpcInspectPayload& inspect = response.inspect;
  inspect.job_id = 77;
  inspect.state = 2;  // RUNNING
  inspect.owner_uid = 1000;
  inspect.revision = 5;
  inspect.cwd = "/srv/training";
  inspect.executable = std::string("/opt/conda/bin/python");
  inspect.argv = {"python", "train.py"};
  inspect.env_metadata = IpcEnvMetadata{1, std::string("3.11.5")};
  inspect.env = {IpcEnvEntry{"PATH", false, "/opt/conda/bin"},
                 IpcEnvEntry{"HF_TOKEN", true, "***"}};
  inspect.gpu_uuid = std::string("GPU-abcdef");
  inspect.gpu_index = std::uint32_t{3};
  inspect.submit_time_unix_ns = std::uint64_t{1757600000} * 1000000000ULL;
  inspect.exit = IpcExitStatus{true, 0};
  inspect.log_path = std::string("/var/lib/yori/jobs/77");
  IpcResponse decoded_response;
  YORI_CHECK(roundtrip_response(response, decoded_response));
  const IpcInspectPayload& out = decoded_response.inspect;
  YORI_CHECK(out.job_id == 77 && out.state == 2 && out.owner_uid == 1000 && out.revision == 5);
  YORI_CHECK(out.cwd == inspect.cwd && out.executable == inspect.executable);
  YORI_CHECK(out.argv == inspect.argv);
  YORI_CHECK(out.env_metadata.has_value() && out.env_metadata->source == 1 &&
             out.env_metadata->python_version == std::string("3.11.5"));
  YORI_CHECK(out.env.size() == 2);
  if (out.env.size() == 2) {
    YORI_CHECK(out.env[0].name == "PATH" && !out.env[0].masked &&
               out.env[0].value == "/opt/conda/bin");
    YORI_CHECK(out.env[1].name == "HF_TOKEN" && out.env[1].masked && out.env[1].value == "***");
  }
  YORI_CHECK(out.gpu_uuid == std::string("GPU-abcdef") && out.gpu_index == std::uint32_t{3});
  YORI_CHECK(out.submit_time_unix_ns == inspect.submit_time_unix_ns);
  YORI_CHECK(out.exit.has_value() && out.exit->exited_normally && out.exit->code == 0);
  YORI_CHECK(out.log_path == inspect.log_path);

  // 响应回显请求版本。
  YORI_CHECK(decoded_response.version == response.version);

  // 无分配（QUEUED）与无元数据的极简载荷。
  response = IpcResponse{};
  response.kind = IpcRequestKind::kInspect;
  response.inspect.job_id = 78;
  YORI_CHECK(roundtrip_response(response, decoded_response));
  YORI_CHECK(!decoded_response.inspect.gpu_uuid.has_value());
  YORI_CHECK(!decoded_response.inspect.gpu_index.has_value());
  YORI_CHECK(!decoded_response.inspect.exit.has_value());
  YORI_CHECK(!decoded_response.inspect.log_path.has_value());

  // 越界拒绝：env 超列表上限、坏 source。
  IpcResponse invalid = response;
  invalid.inspect.env.assign(IpcProtocolLimits::kMaxListItems + 1, IpcEnvEntry{"K", false, "v"});
  std::vector<std::uint8_t> reject;
  YORI_CHECK(!append_response_frame(invalid, reject));

  invalid = response;
  invalid.inspect.env_metadata = IpcEnvMetadata{7, std::nullopt};
  YORI_CHECK(!append_response_frame(invalid, reject));

  // 手工坏帧：source 越界（payload: version 2, kind 9, error 0, detail "", ...）。
  std::vector<std::uint8_t> handcrafted = {
      0x02, 0x09,                                      // version 2, INSPECT
      0x00,                                            // error none
      0x00, 0x00, 0x00, 0x00,                          // detail ""
      0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // job 77
      0x02,                                            // RUNNING
      0xe8, 0x03, 0x00, 0x00,                          // owner 1000
      0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // revision 5
      0x00, 0x00, 0x00, 0x00,                          // cwd ""
      0x00,                                            // executable 无
      0x00, 0x00, 0x00, 0x00,                          // argv count 0
      0x01,                                            // env_metadata 有
      0x07,                                            // source 7（非法）
  };
  const IpcResponseDecodeResult bad_source =
      decode_response_payload(handcrafted.data(), handcrafted.size());
  YORI_CHECK(bad_source.error == IpcDecodeError::kInvalidValue);
}

void test_m8_golden_vectors() {
  // v2 SUBMIT 尾部追加字段：executable="/bin/true"，env_metadata 无。
  IpcRequest request;
  request.kind = IpcRequestKind::kSubmit;
  request.version = 2;
  request.submit.argv = {"true"};
  request.submit.cwd = "/tmp";
  request.submit.gpu_request = 1;
  request.submit.executable = std::string("/bin/true");
  std::vector<std::uint8_t> buffer;
  YORI_CHECK(append_request_frame(request, buffer));
  const std::vector<std::uint8_t> expected = {
      0x2f, 0x00, 0x00, 0x00,                          // 长度 47
      0x02, 0x01,                                      // version 2, SUBMIT
      0x01, 0x00, 0x00, 0x00,                          // argv count 1
      0x04, 0x00, 0x00, 0x00, 't', 'r', 'u', 'e',      // "true"
      0x04, 0x00, 0x00, 0x00, '/', 't', 'm', 'p',      // cwd "/tmp"
      0x00, 0x00, 0x00, 0x00,                          // env count 0
      0x01, 0x00, 0x00, 0x00,                          // gpu_request 1
      0x00,                                            // launch_profile 无
      0x00,                                            // tensorboard_logdir 无
      0x01,                                            // executable 有
      0x09, 0x00, 0x00, 0x00,                          // 长度 9
      '/', 'b', 'i', 'n', '/', 't', 'r', 'u', 'e',
      0x00,                                            // env_metadata 无
  };
  YORI_CHECK(buffer == expected);

  // INSPECT 请求：version 2, kind 9, job 5。
  request = IpcRequest{};
  request.kind = IpcRequestKind::kInspect;
  request.inspect.job_id = 5;
  buffer.clear();
  YORI_CHECK(append_request_frame(request, buffer));
  const std::vector<std::uint8_t> expected_inspect = {
      0x0a, 0x00, 0x00, 0x00,                          // 长度 10（2 头 + 8）
      0x02, 0x09,                                      // version 2, INSPECT
      0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // job 5
  };
  YORI_CHECK(buffer == expected_inspect);
}

}  // namespace

int main() {
  test_request_roundtrips();
  test_response_roundtrips();
  test_malformed_requests();
  test_malformed_responses();
  test_encoder_rejects_oversize();
  test_boundary_values();
  test_m6_request_roundtrips();
  test_m6_response_roundtrips();
  test_m6_stream_frame_roundtrips();
  test_m6_stream_frame_malformed();
  test_m6_golden_vectors();
  test_m8_submit_v2_roundtrip();
  test_m8_inspect_roundtrip();
  test_m8_golden_vectors();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "ipc protocol: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("ipc protocol: all checks passed\n");
  return 0;
}
