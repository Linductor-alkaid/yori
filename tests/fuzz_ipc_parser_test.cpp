#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <yori/ipc/ipc_protocol.hpp>

#include "yori_test.hpp"

// ---------------------------------------------------------------------------
// IPC parser 确定性 fuzz（M5 起步，RULE-11/设计第 17 节第 9 条）：
// 种子化变异集（字节翻转、截断、长度放大、随机拼接）驱动请求/响应解码器，
// 断言任意输入下 (1) 不 crash（sanitizer 生效）；(2) 结果只有"稳定错误码"或
// "完整解析"两种；(3) 完整解析的输出可再编码 roundtrip。独立 libFuzzer/
// OSS-Fuzz 基础设施另行立项；本集覆盖 CI 常规与 sanitizer 路径。
// ---------------------------------------------------------------------------

namespace {

using namespace yori::ipc;

constexpr int kIterations = 2000;

// 合法种子语料：覆盖全部请求/响应 kind 与字段形态。
std::vector<std::vector<std::uint8_t>> seed_corpus() {
  std::vector<std::vector<std::uint8_t>> corpus;

  IpcRequest submit;
  submit.kind = IpcRequestKind::kSubmit;
  submit.submit.argv = {"python", "train.py", "--flag", "value with spaces"};
  submit.submit.cwd = "/srv/training";
  submit.submit.env = {{"A", "1"}, {"B", "2"}};
  submit.submit.gpu_request = 1;
  submit.submit.tensorboard_logdir = std::string("runs/x");
  std::vector<std::uint8_t> frame;
  YORI_CHECK(append_request_frame(submit, frame));
  corpus.push_back(frame);

  for (const IpcRequestKind kind :
       {IpcRequestKind::kPs, IpcRequestKind::kQueue, IpcRequestKind::kGpu}) {
    IpcRequest request;
    request.kind = kind;
    frame.clear();
    YORI_CHECK(append_request_frame(request, frame));
    corpus.push_back(frame);
  }

  IpcRequest cancel;
  cancel.kind = IpcRequestKind::kCancel;
  cancel.cancel.job_id = 42;
  frame.clear();
  YORI_CHECK(append_request_frame(cancel, frame));
  corpus.push_back(frame);

  IpcRequest logs;
  logs.kind = IpcRequestKind::kLogs;
  logs.logs.job_id = 7;
  logs.logs.max_bytes = 4096;
  frame.clear();
  YORI_CHECK(append_request_frame(logs, frame));
  corpus.push_back(frame);

  IpcResponse ps;
  ps.kind = IpcRequestKind::kPs;
  ps.error = IpcError::kNone;
  ps.jobs.push_back(IpcJobSummary{
      1, 2, 1000, 3, false, {"python"}, "/srv", std::string("r"), IpcExitStatus{true, 0}});
  ps.jobs.push_back(IpcJobSummary{2, 0, 1001, 0, true, {}, "", std::nullopt, std::nullopt});
  frame.clear();
  YORI_CHECK(append_response_frame(ps, frame));
  corpus.push_back(frame);

  IpcResponse gpu;
  gpu.kind = IpcRequestKind::kGpu;
  gpu.error = IpcError::kNone;
  gpu.gpu_revision = 3;
  IpcGpuDevice device;
  device.uuid = "GPU-seed";
  device.index = 1;
  device.observed_state = 1;
  device.logical_state = 2;
  device.utilization_percent = 50;
  device.memory_used_bytes = 10;
  device.memory_total_bytes = 20;
  device.leased_by_job = 9;
  gpu.devices.push_back(device);
  frame.clear();
  YORI_CHECK(append_response_frame(gpu, frame));
  corpus.push_back(frame);

  IpcResponse logs_response;
  logs_response.kind = IpcRequestKind::kLogs;
  logs_response.logs.stdout_tail = {1, 2, 3, 0, 255};
  frame.clear();
  YORI_CHECK(append_response_frame(logs_response, frame));
  corpus.push_back(frame);

  return corpus;
}

class Mutator final {
 public:
  explicit Mutator(std::uint32_t seed) : generator_(seed) {}

  std::uint32_t next(std::uint32_t bound) { return generator_() % bound; }

 private:
  std::mt19937 generator_;
};

std::vector<std::uint8_t> mutate(const std::vector<std::uint8_t>& input, Mutator& mutator) {
  std::vector<std::uint8_t> output = input;
  const std::uint32_t operations = 1 + mutator.next(4);
  for (std::uint32_t op = 0; op < operations; ++op) {
    switch (mutator.next(6)) {
      case 0:  // 单字节翻转
        if (!output.empty()) {
          const std::size_t index = mutator.next(static_cast<std::uint32_t>(output.size()));
          output[index] = static_cast<std::uint8_t>(~output[index]);
        }
        break;
      case 1:  // 随机字节替换
        if (!output.empty()) {
          const std::size_t index = mutator.next(static_cast<std::uint32_t>(output.size()));
          output[index] = static_cast<std::uint8_t>(mutator.next(256));
        }
        break;
      case 2:  // 截断
        if (!output.empty()) {
          output.resize(mutator.next(static_cast<std::uint32_t>(output.size())));
        }
        break;
      case 3:  // 帧长度放大（首 4 字节写大值）
        if (output.size() >= 4) {
          const std::uint32_t big = 0x00ffffffu & mutator.next(0x00ffffffu);
          output[0] = static_cast<std::uint8_t>(big & 0xff);
          output[1] = static_cast<std::uint8_t>((big >> 8) & 0xff);
          output[2] = static_cast<std::uint8_t>((big >> 16) & 0xff);
          output[3] = static_cast<std::uint8_t>((big >> 24) & 0xff);
        }
        break;
      case 4:  // 追加随机垃圾
        output.push_back(static_cast<std::uint8_t>(mutator.next(256)));
        break;
      case 5:  // 头部字节平移（破坏 version/kind 对齐）
        if (output.size() > 5) {
          const std::size_t from = mutator.next(static_cast<std::uint32_t>(output.size() - 1));
          const std::size_t to = mutator.next(static_cast<std::uint32_t>(output.size() - 1));
          std::swap(output[from], output[to]);
        }
        break;
    }
  }
  return output;
}

// 解码入口：请求与响应两个方向都驱动。
void drive_decode(const std::vector<std::uint8_t>& payload) {
  if (payload.size() > IpcProtocolLimits::kMaxPayloadBytes + 1) {
    return;  // 长度域放大后无真实缓冲，跳过（decode 以 size 判定已单独覆盖）
  }
  const IpcRequestDecodeResult request = decode_request_payload(payload.data(), payload.size());
  if (request.ok()) {
    // 完整解析必须可再编码 roundtrip。
    std::vector<std::uint8_t> frame;
    YORI_CHECK(append_request_frame(request.value, frame));
  } else {
    YORI_CHECK(request.error != IpcDecodeError::kNone);
  }

  const IpcResponseDecodeResult response = decode_response_payload(payload.data(), payload.size());
  if (response.ok()) {
    std::vector<std::uint8_t> frame;
    YORI_CHECK(append_response_frame(response.value, frame));
  } else {
    YORI_CHECK(response.error != IpcDecodeError::kNone);
  }
}

}  // namespace

int main() {
  const std::vector<std::vector<std::uint8_t>> corpus = seed_corpus();
  YORI_CHECK(!corpus.empty());

  std::uint64_t valid_requests = 0;
  std::uint64_t valid_responses = 0;
  for (std::uint32_t seed = 1; seed <= kIterations; ++seed) {
    Mutator mutator(seed);
    const std::vector<std::uint8_t>& base =
        corpus[mutator.next(static_cast<std::uint32_t>(corpus.size()))];
    const std::vector<std::uint8_t> mutated = mutate(base, mutator);

    // 变异体作用于 payload（跳过 4 字节长度前缀模拟真实解码入口）。
    if (mutated.size() > 4) {
      std::vector<std::uint8_t> payload(mutated.begin() + 4, mutated.end());
      const IpcRequestDecodeResult request = decode_request_payload(payload.data(), payload.size());
      valid_requests += request.ok() ? 1 : 0;
      const IpcResponseDecodeResult response =
          decode_response_payload(payload.data(), payload.size());
      valid_responses += response.ok() ? 1 : 0;
      drive_decode(payload);
    }

    // 原始变异体（含前缀）也驱动一遍：覆盖 version/kind 被破坏的形态。
    drive_decode(mutated);
  }

  std::printf(
      "ipc parser fuzz: %d iterations, %llu valid request parses, %llu valid response parses\n",
      kIterations, static_cast<unsigned long long>(valid_requests),
      static_cast<unsigned long long>(valid_responses));

  if (yori::testing::failure_count != 0) {
    std::fprintf(stderr, "ipc parser fuzz: %d failure(s)\n", yori::testing::failure_count);
    return 1;
  }
  return 0;
}
