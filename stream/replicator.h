/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2016 Grigory Nikolaenko <nikolaenko.grigory@gmail.com>
          (c) 2017 Maxim Zhurovich <zhurovich@gmail.com>
          (c) 2018 Dima Korolev <dmitry.korolev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#ifndef CURRENT_STREAM_REPLICATOR_H
#define CURRENT_STREAM_REPLICATOR_H

#include <functional>
#include <string>
#include <thread>

#include "exceptions.h"
#include "stream.h"
#include "stream_impl.h"

#include "../blocks/http/api.h"
#include "../blocks/ss/ss.h"

#include "../bricks/sync/owned_borrowed.h"
#include "../bricks/sync/waitable_atomic.h"

#include "../typesystem/reflection/types.h"

namespace current {
namespace stream {

template <typename STREAM_ENTRY>
class SubscribableRemoteStream final {
 public:
  using entry_t = STREAM_ENTRY;

  class RemoteStream final {
   public:
    RemoteStream(const std::string& url, const std::string& entry_name, const std::string& namespace_name)
        : url_(url),
          schema_(Value<reflection::ReflectedTypeBase>(reflection::Reflector().ReflectType<entry_t>()).type_id,
                  entry_name,
                  namespace_name) {}

    void CheckSchema() const {
      const auto response = HTTP(GET(url_ + "/schema.simple"));
      if (response.code == HTTPResponseCode.OK) {
        const auto remote_schema = ParseJSON<SubscribableStreamSchema>(response.body);
        if (remote_schema != schema_) {
          CURRENT_THROW(RemoteStreamInvalidSchemaException());
        }
      } else {
        CURRENT_THROW(RemoteStreamDoesNotRespondException());
      }
    }

    uint64_t GetNumberOfEntries() const {
      const auto response = HTTP(GET(url_ + "?sizeonly"));
      if (response.code == HTTPResponseCode.OK) {
        return FromString<uint64_t>(response.body);
      } else {
        CURRENT_THROW(RemoteStreamDoesNotRespondException());
      }
    }

    std::string GetURLToSubscribe(uint64_t index, bool checked_subscription) const {
      return url_ + "?i=" + current::ToString(index) + (checked_subscription ? "&checked" : "");
    }

    std::string GetURLToTerminate(const std::string& subscription_id) const {
      return url_ + "?terminate=" + subscription_id;
    }

   private:
    const std::string url_;
    const SubscribableStreamSchema schema_;
  };

  template <typename F, typename TYPE_SUBSCRIBED_TO, SubscriptionMode SM>
  class RemoteSubscriberThread final : public current::stream::SubscriberScope::SubscriberThread {
    static_assert(current::ss::IsEntrySubscriber<F, TYPE_SUBSCRIBED_TO>::value, "");

   public:
    RemoteSubscriberThread(Borrowed<RemoteStream> remote_stream,
                           F& subscriber,
                           uint64_t start_idx,
                           bool checked_subscription,
                           std::function<void()> done_callback)
        : valid_(false),
          borrowed_remote_stream_(std::move(remote_stream), [this]() { TerminateSubscription(); }),
          done_callback_(done_callback),
          subscriber_(subscriber),
          index_(start_idx),
          checked_subscription_(checked_subscription),
          unused_idxts_(),
          terminate_subscription_requested_(false),
          thread_([this]() { Thread(); }) {
      valid_ = true;
    }

    ~RemoteSubscriberThread() {
      if (valid_) {
        TerminateSubscription();
        thread_.join();
      } else {
        if (done_callback_) {
          done_callback_();
        }
      }
    }

   private:
    RemoteSubscriberThread() = delete;
    RemoteSubscriberThread(const RemoteSubscriberThread&) = delete;
    RemoteSubscriberThread(RemoteSubscriberThread&&) = delete;
    void operator=(const RemoteSubscriberThread&) = delete;
    void operator=(RemoteSubscriberThread&&) = delete;

    void Thread() {
      ThreadImpl();
      subscriber_thread_done_ = true;
      subscription_id_.MutableScopedAccessor()->clear();
      if (done_callback_) {
        done_callback_();
      }
    }

    void ThreadImpl() {
      const RemoteStream& bare_stream = *borrowed_remote_stream_;
      bool terminate_sent = false;
      while (true) {
        if (!terminate_sent && terminate_subscription_requested_) {
          terminate_sent = true;
          if (subscriber_.Terminate() != ss::TerminationResponse::Wait) {
            return;
          }
        }
        try {
          bare_stream.CheckSchema();
          HTTP(ChunkedGET(bare_stream.GetURLToSubscribe(index_, checked_subscription_),
                          [this](const std::string& header, const std::string& value) { OnHeader(header, value); },
                          [this](const std::string& chunk_body) { OnChunk(chunk_body); },
                          [this]() {}));
        } catch (StreamTerminatedBySubscriber&) {
          break;
        } catch (RemoteStreamMalformedChunkException&) {
          if (++consecutive_malformed_chunks_count_ == 3) {
            fprintf(stderr,
                    "Constantly receiving malformed chunks from \"%s\"\n",
                    bare_stream.GetURLToSubscribe(index_, checked_subscription_).c_str());
          }
        } catch (current::Exception&) {
        }
        carried_over_data_.clear();
        subscription_id_.MutableScopedAccessor()->clear();
      }
    }

    template <SubscriptionMode MODE = SM>
    ENABLE_IF<MODE == SubscriptionMode::Checked> PassEntryToSubscriber(const std::string& raw_log_line) {
      const auto split = current::strings::Split(raw_log_line, '\t');
      if (split.empty()) {
        CURRENT_THROW(RemoteStreamMalformedChunkException());
      }
      const auto tsoptidx = ParseJSON<ts_optidx_t>(split[0]);
      if (Exists(tsoptidx.index)) {
        const auto idxts = idxts_t(Value(tsoptidx.index), tsoptidx.us);
        if (split.size() != 2u || idxts.index != index_) {
          CURRENT_THROW(RemoteStreamMalformedChunkException());
        }
        auto entry = ParseJSON<TYPE_SUBSCRIBED_TO>(split[1]);
        ++index_;
        if (subscriber_(std::move(entry), idxts, unused_idxts_) == ss::EntryResponse::Done) {
          CURRENT_THROW(StreamTerminatedBySubscriber());
        }
      } else {
        if (split.size() != 1u) {
          CURRENT_THROW(RemoteStreamMalformedChunkException());
        }
        if (subscriber_(tsoptidx.us) == ss::EntryResponse::Done) {
          CURRENT_THROW(StreamTerminatedBySubscriber());
        }
      }
      consecutive_malformed_chunks_count_ = 0;
    }

    template <SubscriptionMode MODE = SM>
    ENABLE_IF<MODE == SubscriptionMode::Unchecked> PassEntryToSubscriber(const std::string& raw_log_line) {
      const auto tab_pos = raw_log_line.find('\t');
      if (tab_pos != std::string::npos) {
        if (subscriber_(raw_log_line, index_++, unused_idxts_) == ss::EntryResponse::Done) {
          CURRENT_THROW(StreamTerminatedBySubscriber());
        }
      } else {
        const auto tsoptidx = ParseJSON<ts_only_t>(raw_log_line);
        if (subscriber_(tsoptidx.us) == ss::EntryResponse::Done) {
          CURRENT_THROW(StreamTerminatedBySubscriber());
        }
      }
    }

    void OnHeader(const std::string& header, const std::string& value) {
      if (header == "X-Current-Stream-Subscription-Id") {
        subscription_id_.SetValue(value);
      }
    }

    void OnChunk(const std::string& chunk) {
      if (terminate_subscription_requested_) {
        return;
      }

      const size_t chunk_size = chunk.size();
      size_t end_pos = 0;
      if (!carried_over_data_.empty()) {
        while (end_pos < chunk_size && chunk[end_pos] != '\n' && chunk[end_pos] != '\r') {
          ++end_pos;
        }
        if (end_pos == chunk_size) {
          carried_over_data_ += chunk;
          return;
        }
        PassEntryToSubscriber(carried_over_data_ + chunk.substr(0, end_pos));
      }

      size_t start_pos = end_pos;
      for (;;) {
        while (start_pos < chunk_size && (chunk[start_pos] == '\n' || chunk[start_pos] == '\r')) {
          ++start_pos;
        }
        end_pos = start_pos + 1;
        while (end_pos < chunk_size && chunk[end_pos] != '\n' && chunk[end_pos] != '\r') {
          ++end_pos;
        }
        if (end_pos >= chunk_size) {
          break;
        }
        PassEntryToSubscriber(chunk.substr(start_pos, end_pos - start_pos));
        start_pos = end_pos + 1;
      }
      if (start_pos < chunk_size) {
        carried_over_data_ = chunk.substr(start_pos);
      } else {
        carried_over_data_.clear();
      }
    }

    void TerminateSubscription() {
      subscription_id_.Wait([this](const std::string& subscription_id) {
        if (subscriber_thread_done_ || terminate_subscription_requested_) {
          return true;
        } else if (!subscription_id.empty()) {
          terminate_subscription_requested_ = true;
          const std::string terminate_url = borrowed_remote_stream_->GetURLToTerminate(subscription_id);
          try {
            HTTP(GET(terminate_url));
          } catch (current::Exception&) {
          }
          return true;
        }
        return false;
      });
    }

   private:
    bool valid_;
    BorrowedWithCallback<RemoteStream> borrowed_remote_stream_;
    const std::function<void()> done_callback_;
    F& subscriber_;
    uint64_t index_;
    const bool checked_subscription_;
    const idxts_t unused_idxts_;
    current::WaitableAtomic<std::string> subscription_id_;
    std::atomic_bool terminate_subscription_requested_;
    std::thread thread_;
    std::string carried_over_data_;
    uint32_t consecutive_malformed_chunks_count_;
  };

  template <typename F, typename TYPE_SUBSCRIBED_TO, SubscriptionMode SM>
  class RemoteSubscriberScopeImpl final : public current::stream::SubscriberScope {
   private:
    static_assert(current::ss::IsStreamSubscriber<F, TYPE_SUBSCRIBED_TO>::value, "");
    using base_t = current::stream::SubscriberScope;

   public:
    using subscriber_thread_t = RemoteSubscriberThread<F, TYPE_SUBSCRIBED_TO, SM>;

    RemoteSubscriberScopeImpl(Borrowed<RemoteStream> remote_stream,
                              F& subscriber,
                              uint64_t start_idx,
                              bool checked_subscription,
                              std::function<void()> done_callback)
        : base_t(std::move(std::make_unique<subscriber_thread_t>(
              std::move(remote_stream), subscriber, start_idx, checked_subscription, done_callback))) {}
  };
  template <typename F>
  using RemoteSubscriberScope = RemoteSubscriberScopeImpl<F, entry_t, SubscriptionMode::Checked>;
  template <typename F>
  using RemoteSubscriberScopeUnchecked = RemoteSubscriberScopeImpl<F, entry_t, SubscriptionMode::Unchecked>;

  explicit SubscribableRemoteStream(const std::string& remote_stream_url)
      : stream_(MakeOwned<RemoteStream>(
            remote_stream_url, stream::constants::kDefaultTopLevelName, stream::constants::kDefaultNamespaceName)) {
    stream_->CheckSchema();
  }

  explicit SubscribableRemoteStream(const std::string& remote_stream_url,
                                    const std::string& entry_name,
                                    const std::string& namespace_name)
      : stream_(MakeOwned<RemoteStream>(remote_stream_url, entry_name, namespace_name)) {
    stream_->CheckSchema();
  }

  template <typename F>
  RemoteSubscriberScope<F> Subscribe(F& subscriber,
                                     uint64_t start_idx = 0u,
                                     bool checked_subscription = false,
                                     std::function<void()> done_callback = nullptr) const {
    static_assert(current::ss::IsStreamSubscriber<F, entry_t>::value, "");
    return RemoteSubscriberScope<F>(stream_, subscriber, start_idx, checked_subscription, done_callback);
  }

  template <typename F>
  RemoteSubscriberScopeUnchecked<F> SubscribeUnchecked(F& subscriber,
                                                       uint64_t start_idx = 0u,
                                                       bool checked_subscription = false,
                                                       std::function<void()> done_callback = nullptr) const {
    static_assert(current::ss::IsStreamSubscriber<F, entry_t>::value, "");
    return RemoteSubscriberScopeUnchecked<F>(stream_, subscriber, start_idx, checked_subscription, done_callback);
  }

  uint64_t GetNumberOfEntries() const {
    return stream_.ObjectAccessorDespitePossiblyDestructing().GetNumberOfEntries();
  }

 private:
  Owned<RemoteStream> stream_;
};

template <typename STREAM>
struct StreamReplicatorImpl {
  using EntryResponse = current::ss::EntryResponse;
  using TerminationResponse = current::ss::TerminationResponse;
  using stream_t = STREAM;
  using entry_t = typename stream_t::entry_t;
  using publisher_t = typename stream_t::publisher_t;

  StreamReplicatorImpl(Borrowed<stream_t> stream)
      : stream_(stream), publisher_(std::move(stream_->BecomeFollowingStream())) {}
  virtual ~StreamReplicatorImpl() {
    publisher_ = nullptr;
    // NOTE(dkorolev): The destructor should not automatically order the stream to re-acquire data authority.
    // Otherwise the stream will SUDDENLY become the master one again, w/o any action from the user. Not cool.
    // The user should be responsible for restoring the stream's data authority as an instance
    // of `StreamReplicator` is being destructed.
    // NOTE(dkorolev): Master flip logic also plays well here.
  }

  EntryResponse operator()(entry_t&& entry, idxts_t current, idxts_t) {
    Value(publisher_)->Publish(std::move(entry), current.us);
    return EntryResponse::More;
  }

  EntryResponse operator()(const entry_t& entry, idxts_t current, idxts_t) {
    Value(publisher_)->Publish(entry, current.us);
    return EntryResponse::More;
  }

  EntryResponse operator()(std::string&& raw_log_line, uint64_t, idxts_t) {
    Value(publisher_)->PublishUnsafe(std::move(raw_log_line));
    return EntryResponse::More;
  }

  EntryResponse operator()(const std::string& raw_log_line, uint64_t, idxts_t) {
    Value(publisher_)->PublishUnsafe(raw_log_line);
    return EntryResponse::More;
  }

  EntryResponse operator()(std::chrono::microseconds ts) {
    Value(publisher_)->UpdateHead(ts);
    return EntryResponse::More;
  }

  EntryResponse EntryResponseIfNoMorePassTypeFilter() const { return EntryResponse::More; }
  TerminationResponse Terminate() const { return TerminationResponse::Terminate; }

 private:
  Borrowed<stream_t> stream_;

  // `Optional` to destroy the publisher before requesting the stream to reacquire data authority.
  Optional<Borrowed<publisher_t>> publisher_;
};

template <typename STREAM>
using StreamReplicator = current::ss::StreamSubscriber<StreamReplicatorImpl<STREAM>, typename STREAM::entry_t>;

}  // namespace stream
}  // namespace current

#endif  // CURRENT_STREAM_REPLICATOR_H
