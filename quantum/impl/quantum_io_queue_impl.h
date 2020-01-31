/*
** Copyright 2018 Bloomberg Finance L.P.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
//NOTE: DO NOT INCLUDE DIRECTLY

//##############################################################################################
//#################################### IMPLEMENTATIONS #########################################
//##############################################################################################
#include <cmath>

namespace Bloomberg {
namespace quantum {

inline
IoQueue::IoQueue() :
    IoQueue(Configuration(), nullptr)
{
}

inline
IoQueue::IoQueue(const Configuration& config,
                 std::vector<IoQueue>* sharedIoQueues) :
    _sharedIoQueues(sharedIoQueues),
    _loadBalanceSharedIoQueues(config.getLoadBalanceSharedIoQueues()),
    _loadBalancePollIntervalMs(config.getLoadBalancePollIntervalMs()),
    _loadBalancePollIntervalBackoffPolicy(config.getLoadBalancePollIntervalBackoffPolicy()),
    _loadBalancePollIntervalNumBackoffs(config.getLoadBalancePollIntervalNumBackoffs()),
    _loadBalanceBackoffNum(0),
    _queue(Allocator<IoQueueListAllocator>::instance(AllocatorTraits::ioQueueListAllocSize())),
    _isEmpty(true),
    _isInterrupted(false),
    _isIdle(true)
{
    if (_sharedIoQueues) {
        //The shared queue doesn't have its own thread
        _thread = std::make_shared<std::thread>(std::bind(&IoQueue::run, this));
    }
}

inline
IoQueue::IoQueue(const IoQueue& other) :
    ITerminate(other),
    _sharedIoQueues(other._sharedIoQueues),
    _loadBalanceSharedIoQueues(other._loadBalanceSharedIoQueues),
    _loadBalancePollIntervalMs(other._loadBalancePollIntervalMs),
    _loadBalancePollIntervalBackoffPolicy(other._loadBalancePollIntervalBackoffPolicy),
    _loadBalancePollIntervalNumBackoffs(other._loadBalancePollIntervalNumBackoffs),
    _loadBalanceBackoffNum(0),
    _queue(Allocator<IoQueueListAllocator>::instance(AllocatorTraits::ioQueueListAllocSize())),
    _isEmpty(true),
    _isInterrupted(false),
    _isIdle(true)
{
    if (_sharedIoQueues) {
        //The shared queue doesn't have its own thread
        _thread = std::make_shared<std::thread>(std::bind(&IoQueue::run, this));
    }
}

inline
IoQueue::~IoQueue()
{
    terminate();
}

inline
void IoQueue::run()
{
    while (true)
    {
        try
        {
            Maybe<IoTask> task;
            if (_loadBalanceSharedIoQueues)
            {
                do
                {
                    task = grabWorkItemFromAll();
                    if (task)
                    {
                        _loadBalanceBackoffNum = 0; //reset
                        break;
                    }
                    YieldingThread()(getBackoffInterval());
                } while (!_isInterrupted);
            }
            else if (_isEmpty)
            {
                std::unique_lock<std::mutex> lock(_notEmptyMutex);
                //========================= BLOCK WHEN EMPTY =========================
                //Wait for the queue to have at least one element
                _notEmptyCond.wait(lock, [this]() -> bool { return !_isEmpty || _isInterrupted; });
            }
            
            if (_isInterrupted)
            {
                break;
            }

            if (!_loadBalanceSharedIoQueues)
            {
                //Iterate to the next runnable task
                task = grabWorkItem();
                if (!task)
                {
                    continue;
                }
            }
            
            //========================= START TASK =========================
            int rc = task.value().run();
            //========================== END TASK ==========================

            if (rc == (int)Task::Status::Success)
            {
                if (task.value().getQueueId() == (int)Queue::Id::Any)
                {
                    _stats.incSharedQueueCompletedCount();
                }
                else
                {
                    _stats.incCompletedCount();
                }
            }
            else
            {
                //IO task ended with error
                if (task.value().getQueueId() == (int)Queue::Id::Any)
                {
                    _stats.incSharedQueueErrorCount();
                }
                else
                {
                    _stats.incErrorCount();
                }

#ifdef __QUANTUM_PRINT_DEBUG
                std::lock_guard<std::mutex> guard(Util::LogMutex());
                if (rc == (int)Task::Status::Exception)
                {
                    std::cerr << "IO task exited with user exception." << std::endl;
                }
                else
                {
                    std::cerr << "IO task exited with error : " << rc << std::endl;
                }
#endif
            }
        }
        catch (std::exception& ex)
        {
            UNUSED(ex);
#ifdef __QUANTUM_PRINT_DEBUG
            std::lock_guard<std::mutex> guard(Util::LogMutex());
            std::cerr << "Caught exception: " << ex.what() << std::endl;
#endif
        }
        catch (...)
        {
#ifdef __QUANTUM_PRINT_DEBUG
            std::lock_guard<std::mutex> guard(Util::LogMutex());
            std::cerr << "Caught unknown exception." << std::endl;
#endif
        }
    } //while
}

inline
void IoQueue::enqueue(IoTask&& task)
{
    //========================= LOCKED SCOPE =========================
    SpinLock::Guard lock(_spinlock);
    doEnqueue(std::move(task));
}

inline
bool IoQueue::tryEnqueue(IoTask&& task)
{
    //========================= LOCKED SCOPE =========================
    SpinLock::Guard lock(_spinlock, SpinLock::TryToLock{});
    if (lock.ownsLock())
    {
        doEnqueue(std::move(task));
    }
    return lock.ownsLock();
}

inline
void IoQueue::doEnqueue(IoTask&& task)
{
    bool isEmpty = _queue.empty();
    if (task.isHighPriority())
    {
        _stats.incHighPriorityCount();
        _queue.emplace_front(std::move(task));
    }
    else
    {
        _queue.emplace_back(std::move(task));
    }
    _stats.incPostedCount();
    _stats.incNumElements();
    if (!_loadBalanceSharedIoQueues && isEmpty)
    {
        //signal on transition from 0 to 1 element only
        signalEmptyCondition(false);
    }
}

inline
Maybe<IoTask> IoQueue::dequeue(std::atomic_bool& hint)
{
    if (_loadBalanceSharedIoQueues)
    {
        //========================= LOCKED SCOPE =========================
        SpinLock::Guard lock(_spinlock);
        return doDequeue(hint);
    }
    return doDequeue(hint);
}

inline
Maybe<IoTask> IoQueue::tryDequeue(std::atomic_bool& hint)
{
    //========================= LOCKED SCOPE =========================
    SpinLock::Guard lock(_spinlock, SpinLock::TryToLock{});
    if (lock.ownsLock())
    {
        return doDequeue(hint);
    }
    return {};
}

inline
Maybe<IoTask> IoQueue::doDequeue(std::atomic_bool& hint)
{
    hint = _queue.empty();
    if (!hint)
    {
        Maybe<IoTask> task = std::move(_queue.front());
        _queue.pop_front();
        _stats.decNumElements();
        return task;
    }
    return {};
}

inline
Maybe<IoTask> IoQueue::tryDequeueFromShared()
{
    static size_t index = 0;
    size_t size = 0;
    
    for (size_t i = 0; i < (*_sharedIoQueues).size(); ++i)
    {
        IoQueue& queue = (*_sharedIoQueues)[++index % (*_sharedIoQueues).size()];
        size += queue.size();
        Maybe<IoTask> task = queue.tryDequeue(_isIdle);
        if (task)
        {
            return task;
        }
    }

    if (size) {
        //try again
        return tryDequeueFromShared();
    }
    return {};
}

inline
std::chrono::milliseconds IoQueue::getBackoffInterval()
{
    if (_loadBalanceBackoffNum < _loadBalancePollIntervalNumBackoffs) {
        ++_loadBalanceBackoffNum;
    }
    if (_loadBalancePollIntervalBackoffPolicy == Configuration::BackoffPolicy::Linear)
    {
        return _loadBalancePollIntervalMs + (_loadBalancePollIntervalMs * _loadBalanceBackoffNum);
    }
    else
    {
        return _loadBalancePollIntervalMs * static_cast<size_t>(std::exp2(_loadBalanceBackoffNum));
    }
}

inline
size_t IoQueue::size() const
{
    //Add +1 to account for the currently executing task which is no longer on the queue.
#if defined(_GLIBCXX_USE_CXX11_ABI) && (_GLIBCXX_USE_CXX11_ABI==0)
    return _isIdle ? _stats.numElements() : _stats.numElements() + 1;
#else
    return _isIdle ? _queue.size() : _queue.size() + 1;
#endif
}

inline
bool IoQueue::empty() const
{
    // If the queue is empty and there are no executing tasks
    return _queue.empty() && _isIdle;
}

inline
void IoQueue::terminate()
{
    bool value{false};
    if (_terminated.compare_exchange_strong(value, true) && _sharedIoQueues)
    {
        {
            std::unique_lock<std::mutex> lock(_notEmptyMutex);
            _isInterrupted = true;
        }
        if (!_loadBalanceSharedIoQueues) {
            _notEmptyCond.notify_all();
        }
        _thread->join();
        _queue.clear();
    }
}

inline
IQueueStatistics& IoQueue::stats()
{
    return _stats;
}

inline
SpinLock& IoQueue::getLock()
{
    return _spinlock;
}

inline
void IoQueue::signalEmptyCondition(bool value)
{
    {
        //========================= LOCKED SCOPE =========================
        std::lock_guard<std::mutex> lock(_notEmptyMutex);
        _isEmpty = value;
    }
    if (!value)
    {
        _notEmptyCond.notify_all();
    }
}

inline
Maybe<IoTask> IoQueue::grabWorkItem()
{
    static bool grabFromShared = false;
    Maybe<IoTask> task;
    grabFromShared = !grabFromShared;
    if (grabFromShared) {
        //========================= LOCKED SCOPE (SHARED QUEUE) =========================
        SpinLock::Guard lock((*_sharedIoQueues)[0].getLock());
        task = (*_sharedIoQueues)[0].dequeue(_isIdle);
        if (!task)
        {
            //========================= LOCKED SCOPE =========================
            SpinLock::Guard lock(_spinlock);
            task = dequeue(_isIdle);
            if (!task)
            {
                signalEmptyCondition(true);
            }
        }
    }
    else {
        //========================= LOCKED SCOPE =========================
        SpinLock::Guard lock(_spinlock);
        task = dequeue(_isIdle);
        if (!task)
        {
            //========================= LOCKED SCOPE (SHARED QUEUE) =========================
            SpinLock::Guard lock((*_sharedIoQueues)[0].getLock());
            task = (*_sharedIoQueues)[0].dequeue(_isIdle);
            if (!task)
            {
                signalEmptyCondition(true);
            }
        }
    }
    return task;
}

inline
Maybe<IoTask> IoQueue::grabWorkItemFromAll()
{
    static bool grabFromShared = false;
    Maybe<IoTask> task;
    grabFromShared = !grabFromShared;
    if (grabFromShared)
    {
        task = tryDequeueFromShared();
        if (!task)
        {
            task = dequeue(_isIdle);
        }
    }
    else
    {
        task = dequeue(_isIdle);
        if (!task)
        {
            task = tryDequeueFromShared();
        }
    }
    return task;
}

inline
bool IoQueue::isIdle() const
{
    return _isIdle;
}

inline
const std::shared_ptr<std::thread>& IoQueue::getThread() const
{
    return _thread;
}

}}
