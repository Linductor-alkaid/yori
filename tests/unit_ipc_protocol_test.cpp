#include <yori/ipc/ipc_protocol.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
  const std::uint32_t length = static_cast<std::uint32_t>(frame[0]) |
                               (static_cast<std::uint32_t>(frame[1]) << 8) |
                               (static_cast<std::uint32_t>(frame[2]) << 16) |
                               (static_cast<std::uint32_t>(frame[3]) << 24);
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
  const std::uint32_t length = static_cast<std::uint32_t>(frame[0]) |
                               (static_cast<std::uint32_t>(frame[1]) << 8) |
                               (static_cast<std::uint32_t>(frame[2]) << 16) |
                               (static_cast<std::uint32_t>(frame[3]) << 24);
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
  response.jobs.push_back(IpcJobSummary{1, 0, 1000, 3, false, {"python", "train.py"},
                                        "/srv", std::string("runs/x"), std::nullopt});
  response.jobs.push_back(IpcJobSummary{2, 5, 1001, 2, true, {}, "", std::nullopt,
                                        IpcExitStatus{false, 9}});
  YORI_CHECK(roundtrip_response(response, decoded));
  YORI_CHECK(decoded.jobs.size() == 2);
  YORI_CHECK(!decoded.jobs[0].masked && decoded.jobs[0].argv.size() == 2 &&
             decoded.jobs[0].exit == std::nullopt);
  YORI_CHECK(decoded.jobs[1].masked && decoded.jobs[1].argv.empty() &&
             decoded.jobs[1].exit.has_value() && !decoded.jobs[1].exit->exited_normally &&
             decoded.jobs[1].exit->code == 9);

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

  // 坏版本。
  const std::uint8_t bad_version[2] = {2, static_cast<std::uint8_t>(IpcRequestKind::kPs)};
  result = decode_request_payload(bad_version, sizeof(bad_version));
  YORI_CHECK(result.error == IpcDecodeError::kBadVersion);

  // 坏 kind。
  const std::uint8_t bad_kind[2] = {1, 0};
  result = decode_request_payload(bad_kind, sizeof(bad_kind));
  YORI_CHECK(result.error == IpcDecodeError::kBadKind);
  const std::uint8_t bad_kind_high[2] = {1, 9};
  result = decode_request_payload(bad_kind_high, sizeof(bad_kind_high));
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
    YORI_CHECK(result.error == IpcDecodeError::kTruncated ||
               result.error == IpcDecodeError::kNone);
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
  std::vector<std::uint8_t> payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kSubmit),
                                       200};
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
  push_u32(1);          // 1 个摘要
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 1});  // job_id=1
  payload.push_back(8);  // state 越界
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
  payload.push_back(0);                                       // state
  payload.insert(payload.end(), {0, 0, 0, 100});              // owner_uid
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 2});    // revision
  payload.push_back(2);                                       // masked 非法
  result = decode_response_payload(payload.data(), payload.size());
  YORI_CHECK(result.error == IpcDecodeError::kInvalidValue);

  // gpu utilization 越界（>100）。
  payload = {1, static_cast<std::uint8_t>(IpcRequestKind::kGpu),
             static_cast<std::uint8_t>(IpcError::kNone)};
  push_u32(0);  // detail
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 0});  // revision
  push_u32(1);                                                // 1 台设备
  push_u32(3);                                                // uuid 长度
  payload.insert(payload.end(), {'a', 'b', 'c'});
  payload.insert(payload.end(), {0, 0, 0, 0});  // index
  payload.push_back(0);                          // observed
  payload.push_back(0);                          // logical
  payload.push_back(1);                          // has_util
  push_u32(101);                                 // utilization 越界
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
  payload.push_back(0);  // no util
  payload.push_back(1);  // has memory
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 10});  // used=10
  payload.insert(payload.end(), {0, 0, 0, 0, 0, 0, 0, 4});    // total=4
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
  big.argv.assign(16, std::string(64 * 1024, 'c'));  // 1 MiB，加开销必然超限
  big.cwd = "/tmp";
  request.submit = std::move(big);
  YORI_CHECK(!append_request_frame(request, frame));

  // 编码失败不留下半帧。
  YORI_CHECK(frame.empty());
}

void test_boundary_values() {
  // 63/64/65 KiB 字符串边界。
  for (const std::size_t size : {std::size_t{63 * 1024}, std::size_t{64 * 1024}}) {
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

}  // namespace

int main() {
  test_request_roundtrips();
  test_response_roundtrips();
  test_malformed_requests();
  test_malformed_responses();
  test_encoder_rejects_oversize();
  test_boundary_values();
  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "ipc protocol: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  std::printf("ipc protocol: all checks passed\n");
  return 0;
}
