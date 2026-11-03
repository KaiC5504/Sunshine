/**
 * @file src/cursor_sync.cpp
 * @brief Definitions for sending the host pointer to a Moonlight client that draws it locally.
 */
// standard includes
#include <charconv>
#include <string_view>

// local includes
#include "cursor_sync.h"
#include "logging.h"

using namespace std::literals;

namespace cursor_sync {
  hub_t::hub_t(std::unique_ptr<backend_t> backend):
      backend(std::move(backend)) {
  }

  hub_t::~hub_t() {
    std::vector<waiter_t> pending;
    {
      std::scoped_lock lock {mutex};
      stopping = true;
      pending.swap(waiters);
    }
    wake.notify_all();
    if (watcher.joinable()) {
      watcher.join();
    }

    for (auto &waiter : pending) {
      waiter.respond(reply_t {.status = 204, .sequence = sequence, .visible = visible});
    }
  }

  void hub_t::poll(std::optional<std::int64_t> since, int stream_width, bool local, respond_t respond, clock::time_point now) {
    auto due = std::vector<std::pair<respond_t, reply_t>> {};
    {
      std::scoped_lock lock {mutex};
      if (local) {
        lease_until = now + lease;
      } else {
        lease_until.reset();
      }

      due = collect(now);
      if (!local || !since || *since != sequence) {
        due.emplace_back(std::move(respond), current(stream_width));
      } else {
        waiters.push_back({stream_width, std::move(respond), now + poll_timeout});
      }
    }

    wake.notify_all();
    for (auto &[callback, reply] : due) {
      callback(reply);
    }
  }

  void hub_t::update(clock::time_point now) {
    auto due = std::vector<std::pair<respond_t, reply_t>> {};
    {
      std::scoped_lock lock {mutex};
      due = collect(now);
    }

    for (auto &[callback, reply] : due) {
      callback(reply);
    }
  }

  bool hub_t::client_draws_pointer(clock::time_point now) {
    std::scoped_lock lock {mutex};
    return lease_until && now < *lease_until;
  }

  void hub_t::start_watching() {
    std::scoped_lock lock {mutex};
    if (!watcher.joinable()) {
      watcher = std::thread {&hub_t::watch, this};
    }
  }

  std::vector<std::pair<respond_t, reply_t>> hub_t::collect(clock::time_point now) {
    const bool changed = refresh();

    std::vector<std::pair<respond_t, reply_t>> due;
    for (auto it = waiters.begin(); it != waiters.end();) {
      if (changed) {
        due.emplace_back(std::move(it->respond), current(it->stream_width));
      } else if (now >= it->deadline) {
        due.emplace_back(std::move(it->respond), reply_t {.status = 204, .sequence = sequence, .visible = visible});
      } else {
        ++it;
        continue;
      }
      it = waiters.erase(it);
    }
    return due;
  }

  reply_t hub_t::current(int stream_width) {
    if (const auto it = replies.find(stream_width); it != replies.end()) {
      return it->second;
    }

    reply_t reply {.sequence = sequence, .visible = visible};
    if (visible) {
      if (auto image = backend->image(stream_width)) {
        reply.png = std::move(image->png);
        reply.hotspot_x = image->hotspot_x;
        reply.hotspot_y = image->hotspot_y;
      } else {
        BOOST_LOG(debug) << "Pointer sync: couldn't read the pointer"sv;
      }
    }
    BOOST_LOG(debug) << "Pointer sync: pointer "sv << sequence << (visible ? " shown, "sv : " hidden, "sv) << reply.png.size()
                     << "-byte image for a "sv << stream_width << "-pixel stream, hotspot "sv << reply.hotspot_x << ',' << reply.hotspot_y;
    return replies.emplace(stream_width, std::move(reply)).first->second;
  }

  bool hub_t::refresh() {
    const auto new_seed = backend->image_seed();
    const auto new_visible = backend->visible();
    if (checked && new_seed == seed && new_visible == visible) {
      return false;
    }

    const bool changed = checked;
    checked = true;
    seed = new_seed;
    visible = new_visible;
    replies.clear();
    if (changed) {
      ++sequence;
    }
    return changed;
  }

  void hub_t::watch() {
    std::unique_lock lock {mutex};
    while (!stopping) {
      if (waiters.empty()) {
        wake.wait(lock, [this] {
          return stopping || !waiters.empty();
        });
        continue;
      }

      if (wake.wait_for(lock, check_interval, [this] {
            return stopping;
          })) {
        break;
      }

      auto due = collect(clock::now());
      lock.unlock();
      for (auto &[callback, reply] : due) {
        callback(reply);
      }
      lock.lock();
    }
  }

  std::optional<std::int64_t> parse_number(std::string_view value) {
    std::int64_t number = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc {} || end != value.data() + value.size() || value.empty()) {
      return std::nullopt;
    }
    return number;
  }

  hub_t *hub() {
    // Never destroyed: waiting polls hold HTTP responses that must not outlive the server at exit.
    static hub_t *const instance = []() -> hub_t * {
      auto backend = platf::make_cursor();
      if (!backend) {
        return nullptr;
      }

      auto *created = new hub_t(std::move(backend));
      created->start_watching();
      return created;
    }();
    return instance;
  }

  bool client_draws_pointer() {
    auto *instance = hub();
    return instance && instance->client_draws_pointer(hub_t::clock::now());
  }
}  // namespace cursor_sync
