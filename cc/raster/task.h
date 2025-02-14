// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_RASTER_TASK_H_
#define CC_RASTER_TASK_H_

#include <stdint.h>

#include <vector>

#include "base/memory/ref_counted.h"
#include "cc/base/cc_export.h"

namespace cc {
class Task;

// States to manage life cycle of a task. Task gets created with NEW state and
// concludes either in FINISHED or CANCELLED state. So possible life cycle
// paths for task are -
//   NEW -> SCHEDULED -> RUNNING -> FINISHED
//   NEW -> SCHEDULED -> CANCELED
class CC_EXPORT TaskState {
 public:
  bool IsScheduled() const;
  bool IsRunning() const;
  bool IsFinished() const;
  bool IsCanceled() const;

  // Functions to change the state of task. These functions should be called
  // only from TaskGraphWorkQueue where the life cycle of a task is decided or
  // from tests. These functions are not thread-safe. Caller is responsible for
  // thread safety.
  void Reset();  // Sets state to NEW.
  void DidSchedule();
  void DidStart();
  void DidFinish();
  void DidCancel();

 private:
  friend class Task;

  // Let only Task class create the TaskState.
  TaskState();
  ~TaskState();

  enum class Value : uint16_t { NEW, SCHEDULED, RUNNING, FINISHED, CANCELED };

  Value value_;
};

// A task which can be run by a TaskGraphRunner. To run a Task, it should be
// inserted into a TaskGraph, which can then be scheduled on the
// TaskGraphRunner.
class CC_EXPORT Task : public base::RefCountedThreadSafe<Task> {
 public:
  typedef std::vector<scoped_refptr<Task>> Vector;

  TaskState& state() { return state_; }

  // Subclasses should implement this method. RunOnWorkerThread may be called
  // on any thread, and subclasses are responsible for locking and thread
  // safety.
  virtual void RunOnWorkerThread() = 0;

 protected:
  friend class base::RefCountedThreadSafe<Task>;

  Task();
  virtual ~Task();

 private:
  TaskState state_;
};

// A task dependency graph describes the order in which to execute a set
// of tasks. Dependencies are represented as edges. Each node is assigned
// a category, a priority and a run count that matches the number of
// dependencies. Priority range from 0 (most favorable scheduling) to UINT16_MAX
// (least favorable). Categories range from 0 to UINT16_MAX. It is up to the
// implementation and its consumer to determine the meaning (if any) of a
// category. A TaskGraphRunner implementation may chose to prioritize certain
// categories over others, regardless of the individual priorities of tasks.
struct CC_EXPORT TaskGraph {
  struct Node {
    typedef std::vector<Node> Vector;

    Node(Task* task,
         uint16_t category,
         uint16_t priority,
         uint32_t dependencies)
        : task(task),
          category(category),
          priority(priority),
          dependencies(dependencies) {}

    Task* task;
    uint16_t category;
    uint16_t priority;
    uint32_t dependencies;
  };

  struct Edge {
    typedef std::vector<Edge> Vector;

    Edge(const Task* task, Task* dependent)
        : task(task), dependent(dependent) {}

    const Task* task;
    Task* dependent;
  };

  TaskGraph();
  TaskGraph(const TaskGraph& other);
  ~TaskGraph();

  void Swap(TaskGraph* other);
  void Reset();

  Node::Vector nodes;
  Edge::Vector edges;
};

}  // namespace cc

#endif  // CC_RASTER_TASK_H_
