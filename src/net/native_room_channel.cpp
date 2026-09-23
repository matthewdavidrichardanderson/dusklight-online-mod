#include "dusklight_online/net/sdk_room_channel.hpp"

#include <curl/curl.h>
#include <curl/websockets.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace dusklight_online::net {
namespace {

constexpr size_t kMaxMessageBytes = 16 * 1024;
constexpr size_t kMaxQueuedMessages = 256;
constexpr size_t kMaxQueuedBytes = 1024 * 1024;
constexpr auto kSendTimeout = std::chrono::seconds(5);

class NativeRoomChannel final : public RoomChannel {
public:
    ~NativeRoomChannel() override { close(); }

    bool open(std::string_view url, std::string& error) override {
        close();
        if (!url.starts_with("wss://")) {
            error = "Cloud room connection requires a secure WebSocket URL";
            return false;
        }
        stopping_.store(false);
        try {
            worker_ = std::thread([this, address = std::string(url)] {
                run(address);
            });
        } catch (const std::system_error&) {
            stopping_.store(true);
            error = "Could not start the cloud room connection";
            return false;
        }
        return true;
    }

    bool send(std::string_view message) override {
        if (message.size() > kMaxMessageBytes) return false;
        std::lock_guard lock(mutex_);
        if (stopping_.load() || !connected_ ||
            outgoing_.size() >= kMaxQueuedMessages ||
            outgoingBytes_ + message.size() > kMaxQueuedBytes) return false;
        outgoing_.emplace_back(message);
        outgoingBytes_ += message.size();
        wake_.notify_one();
        return true;
    }

    bool poll(Event& event) override {
        std::lock_guard lock(mutex_);
        if (events_.empty()) return false;
        event = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    void close() override {
        stopping_.store(true);
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
        std::lock_guard lock(mutex_);
        connected_ = false;
        outgoing_.clear();
        outgoingBytes_ = 0;
        events_.clear();
    }

private:
    bool queue_event(Event event) {
        std::lock_guard lock(mutex_);
        if (stopping_.load()) return false;
        if (events_.size() >= kMaxQueuedMessages) {
            events_.clear();
            events_.push_back({EventKind::Closed, "Cloud room event queue limit reached"});
            return false;
        }
        events_.push_back(std::move(event));
        return true;
    }

    bool send_frame(CURL* curl, const std::string& message, std::string& error) {
        size_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + kSendTimeout;
        do {
            if (stopping_.load()) return false;
            size_t sent = 0;
            const CURLcode result = curl_ws_send(curl, message.data() + offset,
                                                  message.size() - offset, &sent,
                                                  0, CURLWS_TEXT);
            offset += sent;
            if (result != CURLE_OK && result != CURLE_AGAIN) {
                error = std::string("Cloud room send failed: ") + curl_easy_strerror(result);
                return false;
            }
            if (offset == message.size()) return true;
            if (std::chrono::steady_clock::now() >= deadline) {
                error = "Cloud room send timed out";
                return false;
            }
            if (result == CURLE_AGAIN || sent == 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (true);
    }

    void run(const std::string& address) {
        static std::once_flag curlInitOnce;
        static CURLcode curlInitResult = CURLE_FAILED_INIT;
        std::call_once(curlInitOnce, [] { curlInitResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
        if (curlInitResult != CURLE_OK) {
            queue_event({EventKind::Closed, "Could not initialize cloud room networking"});
            return;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            queue_event({EventKind::Closed, "Could not create cloud room connection"});
            return;
        }
        curl_easy_setopt(curl, CURLOPT_URL, address.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        const CURLcode connectResult = curl_easy_perform(curl);
        if (connectResult != CURLE_OK) {
            queue_event({EventKind::Closed, std::string("Cloud room connection failed: ") +
                                              curl_easy_strerror(connectResult)});
            curl_easy_cleanup(curl);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            if (!stopping_.load()) connected_ = true;
        }
        if (!queue_event({EventKind::Open, {}})) {
            curl_easy_cleanup(curl);
            return;
        }

        std::string incoming;
        std::string failure;
        std::array<char, 4096> buffer{};
        while (!stopping_.load()) {
            bool worked = false;
            std::string outgoing;
            {
                std::lock_guard lock(mutex_);
                if (!outgoing_.empty()) {
                    outgoing = std::move(outgoing_.front());
                    outgoingBytes_ -= outgoing.size();
                    outgoing_.pop_front();
                }
            }
            if (!outgoing.empty()) {
                if (!send_frame(curl, outgoing, failure)) break;
                worked = true;
            }

            for (int n = 0; n < 16 && !stopping_.load(); ++n) {
                size_t received = 0;
                const curl_ws_frame* frame = nullptr;
                const CURLcode result = curl_ws_recv(curl, buffer.data(), buffer.size(),
                                                     &received, &frame);
                if (result == CURLE_AGAIN) break;
                if (result == CURLE_GOT_NOTHING) {
                    failure = "Cloud room connection closed";
                    break;
                }
                if (result != CURLE_OK || !frame) {
                    failure = std::string("Cloud room receive failed: ") +
                              curl_easy_strerror(result);
                    break;
                }
                worked = true;
                if (frame->flags & CURLWS_CLOSE) {
                    failure = "Cloud room connection closed";
                    break;
                }
                if (frame->flags & (CURLWS_PING | CURLWS_PONG)) continue;
                if (!(frame->flags & CURLWS_TEXT)) {
                    failure = "Room service sent a non-text message";
                    break;
                }
                if (received > kMaxMessageBytes - incoming.size() ||
                    frame->bytesleft > static_cast<curl_off_t>(kMaxMessageBytes - incoming.size() - received)) {
                    failure = "Cloud room message exceeds limit";
                    break;
                }
                incoming.append(buffer.data(), received);
                if (frame->bytesleft == 0 && !(frame->flags & CURLWS_CONT)) {
                    if (!queue_event({EventKind::Message, std::move(incoming)})) {
                        failure = "Cloud room event queue limit reached";
                        break;
                    }
                    incoming.clear();
                }
            }
            if (!failure.empty()) break;
            if (!worked) {
                std::unique_lock lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                    return stopping_.load() || !outgoing_.empty();
                });
            }
        }
        curl_easy_cleanup(curl);
        {
            std::lock_guard lock(mutex_);
            connected_ = false;
        }
        if (!stopping_.load())
            queue_event({EventKind::Closed, failure.empty() ? "Cloud room connection closed" : failure});
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_{true};
    bool connected_ = false;
    std::deque<std::string> outgoing_;
    size_t outgoingBytes_ = 0;
    std::deque<Event> events_;
    std::thread worker_;
};

}  // namespace

std::unique_ptr<RoomChannel> make_sdk_room_channel() {
    return std::make_unique<NativeRoomChannel>();
}

}  // namespace dusklight_online::net
