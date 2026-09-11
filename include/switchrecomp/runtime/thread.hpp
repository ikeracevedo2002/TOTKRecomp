#pragma once

#include "switchrecomp/common/result.hpp"
#include "switchrecomp/runtime/atomic_memory.hpp"
#include "switchrecomp/runtime/context.hpp"
#include "switchrecomp/runtime/cpu_state.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace switchrecomp::runtime
{

enum class GuestThreadState : std::uint8_t
{
    Created,
    Running,
    Exited,
    Joined,
};

class GuestThread
{
  public:
    using EntryPoint = std::function<Result<void>(GuestThread&)>;

    GuestThread(std::uint64_t id, SharedRuntimeState& shared, EntryPoint entry);
    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;
    GuestThread(GuestThread&&) = delete;
    GuestThread& operator=(GuestThread&&) = delete;
    ~GuestThread();

    [[nodiscard]] Result<void> start();
    [[nodiscard]] Result<void> join();

    [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
    [[nodiscard]] GuestThreadState state() const noexcept;
    [[nodiscard]] CpuState& cpu() noexcept { return cpu_; }
    [[nodiscard]] const CpuState& cpu() const noexcept { return cpu_; }
    [[nodiscard]] RuntimeContext& runtime() noexcept { return runtime_; }
    [[nodiscard]] const RuntimeContext& runtime() const noexcept { return runtime_; }
    [[nodiscard]] bool has_error() const noexcept;
    [[nodiscard]] Error error() const;

  private:
    void run() noexcept;

    std::uint64_t id_ = 0U;
    CpuState cpu_{};
    RuntimeContext runtime_{};
    EntryPoint entry_;
    mutable std::mutex lifecycle_mutex_;
    GuestThreadState state_ = GuestThreadState::Created;
    Error thread_error_{ErrorCode::InvalidThreadState, "guest thread has no error"};
    bool has_thread_error_ = false;
    std::thread worker_;
};

class ThreadManager
{
  public:
    explicit ThreadManager(SharedRuntimeState& shared) : shared_(&shared) {}
    ThreadManager(const ThreadManager&) = delete;
    ThreadManager& operator=(const ThreadManager&) = delete;
    ~ThreadManager();

    [[nodiscard]] Result<GuestThread*> create(GuestThread::EntryPoint entry);
    [[nodiscard]] Result<GuestThread*> find(std::uint64_t id) noexcept;
    [[nodiscard]] Result<const GuestThread*> find(std::uint64_t id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Result<void> join_all();

  private:
    SharedRuntimeState* shared_ = nullptr;
    mutable std::mutex mutex_;
    std::uint64_t next_id_ = 1U;
    std::vector<std::unique_ptr<GuestThread>> threads_;
};

[[nodiscard]] const char* guest_thread_state_name(GuestThreadState state) noexcept;

} // namespace switchrecomp::runtime
