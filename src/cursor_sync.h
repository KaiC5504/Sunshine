/**
 * @file src/cursor_sync.h
 * @brief Declarations for sending the host pointer to a Moonlight client that draws it locally.
 */
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cursor_sync {
  constexpr std::string_view sequence_header = "X-Cursor-Seq";  ///< Response header carrying the pointer sequence number.
  constexpr std::string_view visible_header = "X-Cursor-Visible";  ///< Response header that is `1` while the host shows its pointer.
  constexpr std::string_view hotspot_header = "X-Cursor-Hotspot";  ///< Response header with the hotspot as `x,y` in image pixels.
  constexpr std::string_view png_mime_type = "image/png";  ///< MIME type of the pointer image.
  constexpr std::chrono::seconds poll_timeout {10};  ///< How long a poll waits for the pointer to change.
  constexpr std::chrono::seconds lease {15};  ///< How long after its last poll a client is trusted to still draw the pointer.
  constexpr std::chrono::milliseconds check_interval {4};  ///< How often the pointer is checked while a client waits.

  /**
   * @brief Pointer image scaled for a stream.
   */
  struct image_t {
    std::string png;  ///< PNG of the pointer.
    int hotspot_x = 0;  ///< Hotspot column in image pixels.
    int hotspot_y = 0;  ///< Hotspot row in image pixels.
  };

  /**
   * @brief Native pointer read by the sync endpoint.
   */
  class backend_t {
  public:
    virtual ~backend_t() = default;

    /**
     * @brief Return a value that changes whenever the pointer image does.
     *
     * @return Current image seed.
     */
    virtual std::uint64_t image_seed() = 0;

    /**
     * @brief Report whether the host shows its pointer.
     *
     * @return `false` while an app or the system hides the pointer.
     */
    virtual bool visible() = 0;

    /**
     * @brief Render the current pointer at the scale of a stream.
     *
     * @param stream_width Width of the client's video stream in pixels.
     * @return Pointer image, or `std::nullopt` when it cannot be read.
     */
    virtual std::optional<image_t> image(int stream_width) = 0;
  };

  /**
   * @brief Platform-independent HTTP reply to a pointer poll.
   */
  struct reply_t {
    int status = 200;  ///< HTTP status code.
    std::int64_t sequence = 0;  ///< Pointer sequence number to report.
    bool visible = true;  ///< Whether the host shows its pointer.
    std::string png;  ///< Pointer image, empty when hidden or unreadable.
    int hotspot_x = 0;  ///< Hotspot column in `png` pixels.
    int hotspot_y = 0;  ///< Hotspot row in `png` pixels.
  };

  /**
   * @brief Callback that writes a reply to a waiting client.
   */
  using respond_t = std::function<void(const reply_t &)>;

  /**
   * @brief Watches the pointer and answers client polls when it changes.
   *
   * A poll with `local` set also tells the host that the client draws the pointer, so captures
   * leave it out of the video until the client stops polling or asks for it back.
   */
  class hub_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock used for timeouts and leases.

    /**
     * @brief Wrap a native pointer.
     *
     * @param backend Native pointer.
     */
    explicit hub_t(std::unique_ptr<backend_t> backend);

    /**
     * @brief Stop the watcher thread and answer waiting polls.
     */
    ~hub_t();

    hub_t(const hub_t &) = delete;
    hub_t &operator=(const hub_t &) = delete;

    /**
     * @brief Handle a client poll.
     *
     * @param since Sequence number the client last saw, if any.
     * @param stream_width Width of the client's video stream in pixels.
     * @param local Whether the client draws the pointer itself.
     * @param respond Callback for the reply, called now or once the pointer changes.
     * @param now Current time.
     */
    void poll(std::optional<std::int64_t> since, int stream_width, bool local, respond_t respond, clock::time_point now);

    /**
     * @brief Check the pointer and answer the polls it concerns.
     *
     * Called by the watcher thread; tests call it directly.
     *
     * @param now Current time.
     */
    void update(clock::time_point now);

    /**
     * @brief Report whether a client currently draws the pointer.
     *
     * @param now Current time.
     * @return `true` while the lease of the last local poll lasts.
     */
    bool client_draws_pointer(clock::time_point now);

    /**
     * @brief Note that the client moved the pointer.
     *
     * macOS hides the pointer while the user types and shows it on the next mouse move, but
     * keeps reporting it hidden until another app changes it, so a move counts as showing it.
     */
    void pointer_moved();

    /**
     * @brief Start the thread that calls update() while polls wait.
     */
    void start_watching();

  private:
    /**
     * @brief Poll waiting for the pointer to change.
     */
    struct waiter_t {
      int stream_width;  ///< Width of the client's video stream.
      respond_t respond;  ///< Reply callback.
      clock::time_point deadline;  ///< When to answer even without a change.
    };

    /**
     * @brief Check the pointer and take the polls that are due.
     *
     * @param now Current time.
     * @return Callbacks paired with their replies, to be called without the lock held.
     */
    std::vector<std::pair<respond_t, reply_t>> collect(clock::time_point now);

    /**
     * @brief Build the reply for the current pointer.
     *
     * @param stream_width Width of the client's video stream.
     * @return Reply carrying the current pointer.
     */
    reply_t current(int stream_width);

    /**
     * @brief Refresh the sequence number from the backend.
     *
     * @return `true` when the pointer changed.
     */
    bool refresh();

    /**
     * @brief Body of the watcher thread.
     */
    void watch();

    std::unique_ptr<backend_t> backend;  ///< Native pointer.
    std::mutex mutex;  ///< Guards everything below.
    std::condition_variable wake;  ///< Signals the watcher about new polls or shutdown.
    std::vector<waiter_t> waiters;  ///< Polls waiting for a change.
    std::map<int, reply_t> replies;  ///< Replies for the current sequence, by stream width.
    std::int64_t sequence = 1;  ///< Increases whenever the pointer image or visibility changes.
    std::uint64_t seed = 0;  ///< Image seed seen at the last check.
    bool visible = true;  ///< Visibility reported to clients at the last check.
    bool backend_visible = true;  ///< Visibility the backend reported at the last check.
    bool moved_while_hidden = false;  ///< Whether the pointer moved since the backend last hid it.
    bool checked = false;  ///< Whether the backend has been read yet.
    std::optional<clock::time_point> lease_until;  ///< End of the local-drawing lease.
    bool stopping = false;  ///< Set when the watcher must exit.
    std::thread watcher;  ///< Thread that calls update() while polls wait.
  };

  /**
   * @brief Parse a decimal query parameter.
   *
   * @param value Raw parameter value.
   * @return Parsed number, or `std::nullopt` when absent or malformed.
   */
  std::optional<std::int64_t> parse_number(std::string_view value);

  /**
   * @brief Return the pointer hub of this host.
   *
   * @return Hub, or `nullptr` on platforms without pointer sync.
   */
  hub_t *hub();

  /**
   * @brief Report whether a client currently draws the pointer, so captures should leave it out.
   *
   * @return `true` while a client's local-drawing lease lasts.
   */
  bool client_draws_pointer();

  /**
   * @brief Tell this host's pointer hub that the client moved the pointer.
   */
  void pointer_moved();
}  // namespace cursor_sync

namespace platf {
  /**
   * @brief Open the native pointer.
   *
   * @return Pointer backend, or `nullptr` on platforms without pointer sync.
   */
  std::unique_ptr<cursor_sync::backend_t> make_cursor();
}  // namespace platf
