/*
//@HEADER
// ************************************************************************
//
//                        Kokkos v. 2.0
//              Copyright (2014) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Christian R. Trott (crtrott@sandia.gov)
//
// ************************************************************************
//@HEADER
*/

#ifndef KOKKOS_IMPL_TASKQUEUECOMMON_HPP
#define KOKKOS_IMPL_TASKQUEUECOMMON_HPP

#include <Kokkos_Macros.hpp>
#if defined( KOKKOS_ENABLE_TASKDAG )


#include <Kokkos_TaskScheduler_fwd.hpp>
#include <Kokkos_Core_fwd.hpp>

#include <Kokkos_MemoryPool.hpp>

#include <impl/Kokkos_TaskNode.hpp>
#include <impl/Kokkos_TaskResult.hpp>

#include <impl/Kokkos_TaskQueueMemoryManager.hpp>
#include <impl/Kokkos_Memory_Fence.hpp>
#include <impl/Kokkos_Atomic_Increment.hpp>
#include <impl/Kokkos_OptionalRef.hpp>
#include <impl/Kokkos_LIFO.hpp>

#include <string>
#include <typeinfo>
#include <stdexcept>


//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

namespace Kokkos {
namespace Impl {

/// @brief CRTP Base class implementing the ready count parts common to most task queues
template <class Derived>
class TaskQueueCommonMixin
{
private:

  int32_t m_ready_count = 0;

  // CRTP boilerplate
  KOKKOS_INLINE_FUNCTION
  Derived& _self() { return *static_cast<Derived*>(this); }

public:

  //----------------------------------------------------------------------------
  // <editor-fold desc="Constructors, destructor, and assignment"> {{{2

  TaskQueueCommonMixin()
    : m_ready_count(0)
  {
    // TODO figure out if I need this store to be atomic
  }

  ~TaskQueueCommonMixin() {
    KOKKOS_EXPECTS(m_ready_count == 0);
  }

  // </editor-fold> end Constructors, destructor, and assignment }}}2
  //----------------------------------------------------------------------------


  //----------------------------------------------------------------------------
  // <editor-fold desc="Task and queue completion"> {{{2

private:

  // This would be more readable with a lambda, but that comes with
  // all the baggage associated with a lambda (compilation times, bugs with
  // nvcc, etc.), so we'll use a simple little helper functor here.
  template <class TaskQueueTraits>
  struct _schedule_waiting_tasks_operation {
    TaskNode<TaskQueueTraits> const& m_predecessor;
    Derived& m_queue;
    KOKKOS_INLINE_FUNCTION
    void operator()(TaskNode<TaskQueueTraits>&& task) const noexcept
      // requires Same<TaskType, Derived::task_base_type>
    {
      if(task.is_runnable()) // KOKKOS_LIKELY
      {
        if(m_predecessor.is_runnable()) {
          m_queue.update_scheduling_info_from_completed_predecessor(
            /* ready_task = */ task.as_runnable_task(),
            /* predecessor = */ m_predecessor.as_runnable_task()
          );
        }
        m_queue.schedule_runnable(std::move(task).as_runnable_task());
      }
      else {
        m_queue.schedule_aggregate(std::move(task).as_aggregate());
      }
    }
  };

protected:

  template <class TaskQueueTraits>
  KOKKOS_FUNCTION
  void _complete_finished_task(TaskNode<TaskQueueTraits>&& task) {
    using scheduling_info_type = typename Derived::scheduling_info_type;
    scheduling_info_type* scheduling_info_ptr = nullptr;
    task.consume_wait_queue(
      _schedule_waiting_tasks_operation<TaskQueueTraits>{
        task,
        _self()
      }
    );
    bool should_delete = task.decrement_and_check_reference_count();
    if(should_delete) {
      _self().deallocate(std::move(task));
    }
  }

  KOKKOS_INLINE_FUNCTION
  void _increment_ready_count() {
    // TODO memory order
    Kokkos::atomic_increment(&this->m_ready_count);
  }

  KOKKOS_INLINE_FUNCTION
  void _decrement_ready_count() {
    // TODO memory order
    Kokkos::atomic_decrement(&this->m_ready_count);
  }

public:

  KOKKOS_INLINE_FUNCTION
  bool is_done() const noexcept {
    // TODO Memory order, instead of volatile
    return (*(volatile int*)(&m_ready_count)) == 0;
  }

  template <class TaskQueueTraits>
  KOKKOS_FUNCTION
  void
  complete(RunnableTaskBase<TaskQueueTraits>&& task) {
    if(task.get_respawn_flag()) {
      _self().schedule_runnable(std::move(task));
    }
    else {
      _complete_finished_task(std::move(task));
    }
    // A runnable task was popped from a ready queue finished executing.
    // If respawned into a ready queue then the ready count was incremented
    // so decrement whether respawned or not.  If finished, all of the
    // tasks waiting on this have been enqueued (either in the ready queue
    // or the next waiting queue, in the case of an aggregate), and the
    // ready count has been incremented for each of those, preventing
    // quiescence.  Thus, it's safe to decrement the ready count here.
    // TODO memory order? (probably release)
    _decrement_ready_count();
  }

  template <class TaskQueueTraits>
  KOKKOS_FUNCTION
  void
  complete(AggregateTask<TaskQueueTraits>&& task) {
    // TODO old code has a ifndef __HCC_ACCELERATOR__ here; figure out why
    _complete_finished_task(std::move(task));
  }

  // </editor-fold> end Task and queue completion }}}2
  //----------------------------------------------------------------------------


  //----------------------------------------------------------------------------
  // <editor-fold desc="Scheduling"> {{{2

protected:

  template <class TaskQueueTraits, class ReadyQueueType>
  KOKKOS_INLINE_FUNCTION
  void
  _schedule_runnable_to_queue(
    RunnableTaskBase<TaskQueueTraits>&& task,
    ReadyQueueType& ready_queue
  )
  {
    bool task_is_ready = true;
    bool scheduling_info_updated = false;

    if(task.has_predecessor()) {
      // save the predecessor into a local variable, then clear it from the
      // task before adding it to the wait queue of the predecessor
      // (We have exclusive access to the task's predecessor, so we don't need
      // to do this atomically)
      // TODO document that we expect exclusive access to `task` in this function
      auto& predecessor = task.get_predecessor();
      // This needs a load/store fence here, technically
      // making this a release store would also do this
      task.clear_predecessor();

      // do this before enqueueing and potentially losing exclusive access to task
      bool task_is_respawning = task.get_respawn_flag();

      // clear the respawn flag, since we're handling the respawn (if any) here
      task.set_respawn_flag(false);

      // TODO remove this fence in favor of memory orders
      Kokkos::memory_fence(); // for now

      // Try to add the task to the predecessor's waiting queue.  If it fails,
      // the predecessor is already done
      bool predecessor_not_ready = predecessor.try_add_waiting(task);

      // NOTE: if the predecessor was not ready and the task was enqueued,
      // we've lost exclusive access and should nt touch task again

      // If the predecessor is not done, then task is not ready
      task_is_ready = not predecessor_not_ready;

      if(task_is_ready and predecessor.is_runnable()) {
        // this is our last chance to update the scheduling info before
        // predecessor is potentially deleted
        _self().update_scheduling_info_from_completed_predecessor(
          /* ready_task = */ task,
          /* predecessor = */ predecessor.as_runnable_task()
        );
        scheduling_info_updated = true;
      }

      if(task_is_respawning) {
        // Reference count for predecessor was incremented when
        // respawn called set_dependency()
        // so that if predecessor completed prior to the
        // above try_add_waiting(), predecessor would not be destroyed.
        // predecessor reference count can now be decremented,
        // which may deallocate it.
        bool should_delete = predecessor.decrement_and_check_reference_count();
        if(should_delete) {
          // TODO better encapsulation of this!
          _self().deallocate(std::move(predecessor));
        }
      }
      // Note! predecessor may be destroyed at this point, so don't add anything
      // here
    }

    if(scheduling_info_updated) {
      // We need to go back to the queue itself and see if it wants to schedule
      // somewhere else
      _self().schedule_runnable(std::move(task));
    }
    // Put it in the appropriate ready queue if it's ready
    else if(task_is_ready) {
      // Increment the ready count
      _self()._increment_ready_count();
      // and enqueue the task
      ready_queue.push(std::move(task));
    }

    // Task may be enqueued and may be run at any point; don't touch it (hence
    // the use of move semantics)
  }

public:

  template <class TaskQueueTraits>
  KOKKOS_FUNCTION
  void
  schedule_runnable(RunnableTaskBase<TaskQueueTraits>&& task) {
    // just forward to the version that takes info as a parameter with the
    // task's info
    _self().schedule_runnable(
      std::move(task),
      task.template scheduling_info_as<typename Derived::scheduling_info_type>()
    );
  }

  template <class TaskQueueTraits>
  KOKKOS_FUNCTION
  void
  schedule_aggregate(AggregateTask<TaskQueueTraits>&& aggregate) {
    // Because the aggregate is being scheduled, should not be in any queue
    KOKKOS_EXPECTS(not aggregate.is_enqueued());

    bool incomplete_dependence_found = false;

    for(auto*& predecessor_ptr_ref : aggregate) {

      // if a previous scheduling operation hasn't already set the predecessor
      // to nullptr, try to enqueue the aggregate into the predecessorendence's waiting
      // queue
      if(predecessor_ptr_ref != nullptr) {

        // Swap the pointer onto the stack and set the one in the aggregate VLA
        // to nullptr before we try to add it to the waiting queue so that some
        // other thread doesn't also get to here and find the pointer to be
        // not null (since as soon as we try and schedule the aggregate, we
        // potentially lose exclusive access to it if that enqueueing operation
        // succeeds.  The swap doesn't need to happen atomically since we have
        // exclusive access to aggregate until an insertion succeeds
        auto* predecessor_ptr = std::move(predecessor_ptr_ref);

        // TODO I think this needs to be a store release so that it doesn't get reordered after the queue insertion
        predecessor_ptr_ref = nullptr;

        // TODO remove this fence in favor of memory orders
        Kokkos::memory_fence();

        // If adding the aggregate to the waiting queue succeeds, the predecessor is not
        // complete
        bool pred_not_ready = predecessor_ptr->try_add_waiting(aggregate);

        // NOTE! At this point it is unsafe to access aggregate (unless the
        // enqueueing failed, so we can't use move semantics to expire it)

        // we found an incomplete dependence, so we can't make task's successors
        // ready yet
        incomplete_dependence_found = pred_not_ready;

        // the reference count for the predecessor was incremented when we put
        // it into the predecessor list, so decrement it here
        bool should_delete = predecessor_ptr->decrement_and_check_reference_count();
        if(should_delete) {
          // TODO better encapsulation of this!
          _self().deallocate(std::move(*predecessor_ptr));
        }

        // Stop the loop if we found an incomplete dependence
        if(incomplete_dependence_found) break;
      }
    }

    // NOTE: it's not safe to access aggregate any more if an incomplete dependence
    // was found, because some other thread could have already popped it off
    // of another waiting queue

    if(not incomplete_dependence_found) {
      // all of the predecessors were completed, so we can complete `task`
      _self().complete(std::move(aggregate));
    }
    // Note!! task may have been deleted at this point, so don't add anything here!
  }

  // Provide a sensible default that can be overridden
  template <class TaskQueueTraits>
  void update_scheduling_info_from_completed_predecessor(
    RunnableTaskBase<TaskQueueTraits>& ready_task,
    RunnableTaskBase<TaskQueueTraits> const& predecessor
  ) {
    // by default, tell a ready task to use the scheduling info of its most
    // recent predecessor
    using scheduling_info_type = typename Derived::scheduling_info_type;
    ready_task.template scheduling_info_as<scheduling_info_type>() =
      predecessor.template scheduling_info_as<scheduling_info_type>();
  }

  template <
    class ExecutionSpace,
    class MemorySpace,
    class MemoryPool
  >
  static /* KOKKOS_CONSTEXPR_14 */ size_t
  task_queue_allocation_size(
    ExecutionSpace const&,
    MemorySpace const&,
    MemoryPool const&
  )
    // requires Same<ExecutionSpace, typename Derived::execution_space>
    //            && Same<MemorySpace, typename Derived::memory_space>
    //            && Same<MemoryPool, typename Derived::memory_pool>
  {
    static_assert(
      std::is_same<ExecutionSpace, typename Derived::execution_space>::value
        && std::is_same<MemorySpace, typename Derived::memory_space>::value
        && std::is_same<MemoryPool, typename Derived::memory_pool>::value,
      "Type mismatch in task_queue_allocation_size customization point"
    );

    return sizeof(Derived);
  }

  // </editor-fold> end Scheduling }}}2
  //----------------------------------------------------------------------------

};

} /* namespace Impl */
} /* namespace Kokkos */

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

#endif /* #if defined( KOKKOS_ENABLE_TASKDAG ) */
#endif /* #ifndef KOKKOS_IMPL_TASKQUEUECOMMON_HPP */

