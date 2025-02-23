// Copyright 2025 Disintar LLP / andrey@head-labs.com

#ifndef PYGLOB_H
#define PYGLOB_H

#include <memory>
#include <mutex>
#include <thread>

#include "ton/ton-types.h"
#include "td/actor/PromiseFuture.h"
#include "td/actor/actor.h"
#include "td/actor/common.h"


namespace pyglobal {
    void init_thread_scheduler(int thread_count);

    td::actor::Scheduler *get_thread_scheduler();

    void stop_scheduler_thread();

    void execute_async(std::function<void()> f);
}

#endif