/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright (C) 2011 - 2015                                                  *
 * Dominik Charousset <dominik.charousset (at) haw-hamburg.de>                *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "test.hpp"

#include "caf/all.hpp"

using namespace caf;

namespace {

std::atomic<size_t> s_ctors;
std::atomic<size_t> s_dtors;

} // namespace <anonymous>

class worker : public event_based_actor {
 public:
  worker();
  ~worker();
  behavior make_behavior() override;
};

worker::worker() {
  ++s_ctors;
}

worker::~worker() {
  ++s_dtors;
}

behavior worker::make_behavior() {
  return {
    [](int x, int y) {
      return x + y;
    }
  };
}

actor spawn_worker() {
  return spawn<worker>();
}

void test_actor_pool() {
  scoped_actor self;
  auto w = actor_pool::make(5, spawn_worker, actor_pool::round_robin{});
  self->monitor(w);
  self->send(w, sys_atom::value, put_atom::value, spawn_worker());
  std::vector<actor_addr> workers;
  for (int i = 0; i < 6; ++i) {
    self->sync_send(w, i, i).await(
      [&](int res) {
        CAF_CHECK_EQUAL(res, i + i);
        auto sender = self->current_sender();
        self->monitor(sender);
        workers.push_back(sender);
      }
    );
  }
  CAF_CHECK(workers.size() == 6);
  CAF_CHECK(std::unique(workers.begin(), workers.end()) == workers.end());
  auto is_invalid = [](const actor_addr& addr) {
    return addr == invalid_actor_addr;
  };
  CAF_CHECK(std::none_of(workers.begin(), workers.end(), is_invalid));
  self->sync_send(w, sys_atom::value, get_atom::value).await(
    [&](std::vector<actor>& ws) {
      std::sort(workers.begin(), workers.end());
      std::sort(ws.begin(), ws.end());
      CAF_CHECK(workers.size() == ws.size()
                && std::equal(workers.begin(), workers.end(), ws.begin()));
    }
  );
  anon_send_exit(workers.back(), exit_reason::user_shutdown);
  self->receive(
    after(std::chrono::milliseconds(25)) >> [] {
      // wait some time to give the pool time to remove the failed worker
    }
  );
  self->receive(
    [&](const down_msg& dm) {
      CAF_CHECK(dm.source == workers.back());
      workers.pop_back();
      // check whether actor pool removed failed worker
      self->sync_send(w, sys_atom::value, get_atom::value).await(
        [&](std::vector<actor>& ws) {
          std::sort(ws.begin(), ws.end());
          CAF_CHECK(workers.size() == ws.size()
                    && std::equal(workers.begin(), workers.end(), ws.begin()));
        }
      );
    },
    after(std::chrono::milliseconds(250)) >> [] {
      CAF_PRINTERR("didn't receive a down message");
    }
  );
  CAF_CHECKPOINT();
  self->send_exit(w, exit_reason::user_shutdown);
  for (int i = 0; i < 6; ++i) {
    self->receive(
      [&](const down_msg& dm) {
        auto last = workers.end();
        auto src = dm.source;
        CAF_CHECK(src != invalid_actor_addr);
        auto pos = std::find(workers.begin(), last, src);
        CAF_CHECK(pos != last || src == w);
        if (pos != last) {
          workers.erase(pos);
        }
      },
      after(std::chrono::milliseconds(250)) >> [] {
        CAF_PRINTERR("didn't receive a down message");
      }
    );
  }
}

void test_broadcast_actor_pool() {
  scoped_actor self;
  auto spawn5 = []() {
    return actor_pool::make(5, spawn_worker, actor_pool::broadcast{});
  };
  auto w = actor_pool::make(5, spawn5, actor_pool::broadcast{});
  self->send(w, 1, 2);
  std::vector<int> results;
  int i = 0;
  self->receive_for(i, 25)(
    [&](int res) {
      results.push_back(res);
    },
    after(std::chrono::milliseconds(250)) >> [] {
      CAF_PRINTERR("didn't receive a result");
    }
  );
  CAF_CHECK_EQUAL(results.size(), 25);
  CAF_CHECK(std::all_of(results.begin(), results.end(),
                        [](int res) { return res == 3; }));
  self->send_exit(w, exit_reason::user_shutdown);
  self->await_all_other_actors_done();
}

void test_random_actor_pool() {
  scoped_actor self;
  auto w = actor_pool::make(5, spawn_worker, actor_pool::random{});
  for (int i = 0; i < 5; ++i) {
    self->sync_send(w, 1, 2).await(
      [&](int res) {
        CAF_CHECK_EQUAL(res, 3);
      },
      after(std::chrono::milliseconds(250)) >> [] {
        CAF_PRINTERR("didn't receive a down message");
      }
    );
  }
  self->send_exit(w, exit_reason::user_shutdown);
  self->await_all_other_actors_done();
}

int main() {
  CAF_TEST(test_actor_pool);
  test_actor_pool();
  test_broadcast_actor_pool();
  test_random_actor_pool();
  await_all_actors_done();
  shutdown();
  CAF_CHECK_EQUAL(s_dtors.load(), s_ctors.load());
  return CAF_TEST_RESULT();
}

