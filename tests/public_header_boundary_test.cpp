#include <yori/version.h>

#include <yori/gpu/gpu_provider.hpp>
#include <yori/ipc/ipc_transport.hpp>
#include <yori/job/job.hpp>
#include <yori/launch/launch_adapter.hpp>
#include <yori/observe/observe.hpp>
#include <yori/process/process_supervisor.hpp>
#include <yori/queue/job_queue.hpp>
#include <yori/scheduler/scheduler.hpp>
#include <yori/store/state_store.hpp>

int main() { return yori::version()[0] == '\0' ? 1 : 0; }
