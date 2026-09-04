#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <yori/job/job.hpp>

#include "yori_test.hpp"

namespace {

using yori::job::Job;
using yori::job::JobState;
using yori::job::TransitionOutcome;

constexpr std::array<JobState, 8> kStates{
    JobState::kQueued,   JobState::kStarting, JobState::kRunning,   JobState::kStopping,
    JobState::kFinished, JobState::kFailed,   JobState::kCancelled, JobState::kLost,
};

yori::job::JobSpec valid_spec() {
  yori::job::JobSpec spec;
  spec.owner_uid = 1000;
  spec.owner_gid = 1000;
  spec.argv = {"train"};
  spec.cwd = "/srv/training";
  spec.submit_time = std::chrono::system_clock::time_point{std::chrono::seconds{1}};
  return spec;
}

std::unique_ptr<Job> make_job(std::uint64_t id = 1) {
  yori::job::JobCreationError error;
  auto job = Job::create(yori::job::JobId{id}, valid_spec(), error);
  YORI_CHECK(job != nullptr);
  return job;
}

void apply(Job& job, JobState next) {
  const auto result = job.transition_to(next);
  YORI_CHECK(result.outcome == TransitionOutcome::kApplied);
  YORI_CHECK(job.state() == next);
}

std::unique_ptr<Job> make_job_in_state(JobState state) {
  auto job = make_job(static_cast<std::uint64_t>(state) + 1);
  if (state == JobState::kQueued) {
    return job;
  }
  if (state == JobState::kCancelled) {
    apply(*job, JobState::kCancelled);
    return job;
  }

  apply(*job, JobState::kStarting);
  if (state == JobState::kStarting) {
    return job;
  }
  if (state == JobState::kFailed) {
    apply(*job, JobState::kFailed);
    return job;
  }
  if (state == JobState::kLost) {
    apply(*job, JobState::kLost);
    return job;
  }

  apply(*job, JobState::kRunning);
  if (state == JobState::kRunning) {
    return job;
  }
  if (state == JobState::kFinished) {
    apply(*job, JobState::kFinished);
    return job;
  }

  apply(*job, JobState::kStopping);
  return job;
}

bool allowed(JobState from, JobState to) {
  switch (from) {
    case JobState::kQueued:
      return to == JobState::kStarting || to == JobState::kCancelled;
    case JobState::kStarting:
      return to == JobState::kRunning || to == JobState::kStopping || to == JobState::kFailed ||
             to == JobState::kLost;
    case JobState::kRunning:
      return to == JobState::kStopping || to == JobState::kFinished || to == JobState::kFailed ||
             to == JobState::kLost;
    case JobState::kStopping:
      return to == JobState::kCancelled || to == JobState::kFailed || to == JobState::kLost;
    case JobState::kFinished:
    case JobState::kFailed:
    case JobState::kCancelled:
    case JobState::kLost:
      return false;
  }
  return false;
}

}  // namespace

int main() {
  for (const auto from : kStates) {
    for (const auto requested : kStates) {
      auto job = make_job_in_state(from);
      const auto revision_before = job->revision();
      const auto result = job->transition_to(requested);

      YORI_CHECK(result.from == from);
      YORI_CHECK(result.requested == requested);
      YORI_CHECK(std::string(yori::job::to_string(result.outcome)) != "UNKNOWN");
      if (yori::job::is_terminal(from)) {
        const auto expected = requested == from ? TransitionOutcome::kIdempotentTerminal
                                                : TransitionOutcome::kIgnoredAfterTerminal;
        YORI_CHECK(result.outcome == expected);
        YORI_CHECK(job->state() == from);
        YORI_CHECK(job->revision() == revision_before);
      } else if (allowed(from, requested)) {
        YORI_CHECK(result.outcome == TransitionOutcome::kApplied);
        YORI_CHECK(job->state() == requested);
        YORI_CHECK(job->revision() == revision_before + 1);
      } else {
        YORI_CHECK(result.outcome == TransitionOutcome::kRejected);
        YORI_CHECK(job->state() == from);
        YORI_CHECK(job->revision() == revision_before);
      }
      YORI_CHECK(result.revision == job->revision());
    }
  }

  auto cancelled = make_job(99);
  apply(*cancelled, JobState::kCancelled);
  const auto revision = cancelled->revision();
  YORI_CHECK(cancelled->transition_to(JobState::kStarting).outcome ==
             TransitionOutcome::kIgnoredAfterTerminal);
  YORI_CHECK(cancelled->transition_to(JobState::kFinished).outcome ==
             TransitionOutcome::kIgnoredAfterTerminal);
  YORI_CHECK(cancelled->state() == JobState::kCancelled);
  YORI_CHECK(cancelled->revision() == revision);

  for (const auto state : kStates) {
    YORI_CHECK(std::string(yori::job::to_string(state)) != "UNKNOWN");
  }

  return yori::testing::failure_count == 0 ? 0 : 1;
}
