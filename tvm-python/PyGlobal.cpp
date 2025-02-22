// Copyright 2025 Disintar LLP / andrey@head-labs.com

#include <memory>
#include <mutex>

#include "PyGlobal.h"

std::mutex scheduler_init_mutex;
bool scheduler_running = false;
static std::unique_ptr<td::actor::Scheduler> thread_local_scheduler;
static std::unique_ptr<td::thread> scheduler_thread;

namespace pyglobal {
    class Runner : public td::actor::Actor {
    public:
        explicit Runner(std::function<void()> f) : f_(std::move(f)) {
        }

        void start_up() override {
          f_();
          stop();
        }

    private:
        std::function<void()> f_;
    };


    void init_thread_scheduler(int thread_count) {
      std::lock_guard<std::mutex> lock(scheduler_init_mutex);
      if (!thread_local_scheduler) {
        thread_local_scheduler = std::unique_ptr<td::actor::Scheduler>(new td::actor::Scheduler({thread_count}));
        scheduler_running = true;

        scheduler_thread = std::make_unique<td::thread>([&] {
            thread_local_scheduler->run();
        });
        scheduler_thread->detach();
      }
    }

    td::actor::Scheduler *get_thread_scheduler() {
      if (!scheduler_running) {
        init_thread_scheduler(6);
      }
      return thread_local_scheduler.get();
    }

    void stop_scheduler_thread() {
      if (scheduler_running) {
        std::lock_guard<std::mutex> lock(scheduler_init_mutex);
        thread_local_scheduler->run_in_context_external([] {
            td::actor::SchedulerContext::get()->stop();
        });
        scheduler_thread->join();
      }
    }

    void execute_async(std::function<void()> f) {
      get_thread_scheduler()->run_in_context_external([&] {
          td::actor::create_actor<pyglobal::Runner>("executeasync", std::move(f)).release();
      });
    }
}
