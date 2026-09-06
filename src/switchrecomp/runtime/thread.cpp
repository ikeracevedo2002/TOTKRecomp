#include "switchrecomp/runtime/thread.hpp"

#include <exception>
#include <new>
#include <utility>

namespace switchrecomp::runtime
{

GuestThread::GuestThread(std::uint64_t id, SharedRuntimeState& shared, EntryPoint entry)
    : id_(id), entry_(std::move(entry))
{
    runtime_.memory = shared.memory;
    runtime_.shared = &shared;
    runtime_.cpu = &cpu_;
    runtime_.guest_thread_id = id_;
}

GuestThread::~GuestThread()
{
    if (worker_.joinable())
    {
        worker_.join();
    }
}

Result<void> GuestThread::start()
{
    std::lock_guard lock(lifecycle_mutex_);
    if (state_ != GuestThreadState::Created)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidThreadState, "guest thread can only be started once"));
    }
    if (!entry_)
    {
        return Result<void>::failure(
            make_error(ErrorCode::InvalidArgument, "guest thread requires an entry callback"));
    }
    try
    {
        state_ = GuestThreadState::Running;
        worker_ = std::thread([this] { run(); });
    }
    catch (const std::exception& ex)
    {
        state_ = GuestThreadState::Created;
        return Result<void>::failure(
            make_error(ErrorCode::ThreadCreationFailed, ex.what()));
    }
    return Result<void>::success();
}

void GuestThread::run() noexcept
{
    try
    {
        const auto result = entry_(*this);
        if (!result)
        {
            std::lock_guard lock(lifecycle_mutex_);
            has_thread_error_ = true;
            thread_error_ = result.error();
        }
    }
    catch (const std::exception& ex)
    {
        std::lock_guard lock(lifecycle_mutex_);
        has_thread_error_ = true;
        try { thread_error_ = make_error(ErrorCode::InterpreterError, ex.what()); }
        catch (...) { thread_error_ = Error{ErrorCode::ResourceLimit, "guest worker exception diagnostic failed"}; }
    }
    catch (...)
    {
        std::lock_guard lock(lifecycle_mutex_);
        has_thread_error_ = true;
        thread_error_ = Error{ErrorCode::InterpreterError, "guest worker failed with an unknown exception"};
    }
    std::lock_guard lock(lifecycle_mutex_);
    state_ = GuestThreadState::Exited;
}

Result<void> GuestThread::join()
{
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_ == GuestThreadState::Created)
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidThreadState, "cannot join a guest thread before it starts"));
        }
        if (state_ == GuestThreadState::Joined)
        {
            return Result<void>::failure(
                make_error(ErrorCode::InvalidThreadState, "guest thread has already been joined"));
        }
    }
    if (worker_.joinable())
    {
        worker_.join();
    }
    std::lock_guard lock(lifecycle_mutex_);
    state_ = GuestThreadState::Joined;
    return Result<void>::success();
}

GuestThreadState GuestThread::state() const noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    return state_;
}

bool GuestThread::has_error() const noexcept
{
    std::lock_guard lock(lifecycle_mutex_);
    return has_thread_error_ || runtime_.has_error;
}

Error GuestThread::error() const
{
    std::lock_guard lock(lifecycle_mutex_);
    return has_thread_error_ ? thread_error_ : runtime_.last_error;
}

ThreadManager::~ThreadManager()
{
    (void)join_all();
}

Result<GuestThread*> ThreadManager::create(GuestThread::EntryPoint entry)
{
    if (shared_ == nullptr)
    {
        return Result<GuestThread*>::failure(
            make_error(ErrorCode::InvalidRuntimeContext, "thread manager has no shared runtime state"));
    }
    std::lock_guard lock(mutex_);
    if (next_id_ == 0U)
    {
        return Result<GuestThread*>::failure(
            make_error(ErrorCode::ResourceLimit, "guest thread ID space exhausted"));
    }
    try
    {
        auto thread = std::make_unique<GuestThread>(next_id_++, *shared_, std::move(entry));
        auto* raw = thread.get();
        threads_.push_back(std::move(thread));
        return Result<GuestThread*>::success(raw);
    }
    catch (const std::bad_alloc&)
    {
        return Result<GuestThread*>::failure(
            make_error(ErrorCode::ResourceLimit, "guest thread allocation failed"));
    }
}

Result<GuestThread*> ThreadManager::find(std::uint64_t id) noexcept
{
    std::lock_guard lock(mutex_);
    for (auto& thread : threads_)
        if (thread->id() == id) return Result<GuestThread*>::success(thread.get());
    return Result<GuestThread*>::failure(
        make_error(ErrorCode::InvalidThreadId, "guest thread ID does not exist"));
}

Result<const GuestThread*> ThreadManager::find(std::uint64_t id) const noexcept
{
    std::lock_guard lock(mutex_);
    for (const auto& thread : threads_)
        if (thread->id() == id) return Result<const GuestThread*>::success(thread.get());
    return Result<const GuestThread*>::failure(
        make_error(ErrorCode::InvalidThreadId, "guest thread ID does not exist"));
}

std::size_t ThreadManager::size() const noexcept
{
    std::lock_guard lock(mutex_);
    return threads_.size();
}

Result<void> ThreadManager::join_all()
{
    std::vector<GuestThread*> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot.reserve(threads_.size());
        for (auto& thread : threads_) snapshot.push_back(thread.get());
    }
    for (auto* thread : snapshot)
    {
        const auto state = thread->state();
        if (state == GuestThreadState::Running || state == GuestThreadState::Exited)
        {
            const auto joined = thread->join();
            if (!joined) return joined;
        }
    }
    return Result<void>::success();
}

const char* guest_thread_state_name(GuestThreadState state) noexcept
{
    switch (state)
    {
    case GuestThreadState::Created: return "created";
    case GuestThreadState::Running: return "running";
    case GuestThreadState::Exited: return "exited";
    case GuestThreadState::Joined: return "joined";
    }
    return "unknown";
}

} // namespace switchrecomp::runtime
