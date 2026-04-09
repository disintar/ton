#include "IBlockParser.hpp"
#include "BlockParserAsync.hpp"
#include "blockchain-indexer/json-utils.hpp"
#include "td/actor/ActorId.h"
#include "td/utils/Time.h"

namespace ton::validator {

    std::string BlockParser::getKey(const BlockIdExt &id) {
      return std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard) + ":" + std::to_string(id.id.seqno);
    }

    std::string BlockParser::getLineageKey(const BlockIdExt &id) {
      return std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard);
    }

    BlockParser::BlockParser(std::unique_ptr<IBLockPublisher> publisher)
            : publisher_(std::move(publisher)), publish_applied_thread_(&BlockParser::publish_applied_worker, this),
              publish_blocks_thread_(&BlockParser::publish_blocks_worker, this),
              publish_states_thread_(&BlockParser::publish_states_worker, this) {
    }

    BlockParser::~BlockParser() {
      running_ = false;
      publish_applied_cv_.notify_all();
      publish_blocks_cv_.notify_all();
      publish_states_cv_.notify_all();
      publish_applied_thread_.join();
      publish_blocks_thread_.join();
      publish_states_thread_.join();
    }

    void BlockParser::storeBlockApplied(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P) {
      if (!check_allowed_shard_parse(id.id.workchain, id.id.shard)) {
        LOG(WARNING) << "Skip applied: " << id.id.to_str();
        P.set_value(std::make_tuple("", ""));
        return;
      }

      const std::string key = getKey(id);
      const double started_at = td::Time::now();
      LOG(WARNING) << "[publish-apply] ready block=" << id.to_str() << " t=" << started_at;

      auto promise_try_sync = td::PromiseCreator::lambda(
          [this, key, id, started_at](td::Result<std::tuple<td::string, td::string>> sync_result) mutable {
            LOG(WARNING) << "[publish-apply] sync-done block=" << id.to_str()
                         << " duration_ms=" << (td::Time::now() - started_at) * 1000.0
                         << " root_hash=" << id.root_hash.to_hex();
            onAppliedSyncResult(std::move(key), id, std::move(sync_result));
          });
      td::actor::send_closure(cluster_sync_, &ClusterPublishSync::sync_block_state,
                              std::make_tuple(id.root_hash, parseBlockApplied(id), td::string{}),
                              std::move(promise_try_sync));
      P.set_value(std::make_tuple("", ""));
    }

    void BlockParser::storeBlockData(ConstBlockHandle handle, td::Ref<BlockData> block,
                                     td::Promise<std::tuple<td::string, td::string>> P) {
      if (!check_allowed_shard_parse(handle->id().id.workchain, handle->id().id.shard)) {
        LOG(WARNING) << "Skip block data: " << handle->id().id.to_str();
        P.set_value(std::make_tuple("", ""));
        return;
      }

      if (!startup_replay_mode_) {
        P.set_value(std::make_tuple("", ""));
        return;
      }

      std::lock_guard<std::mutex> lock(maps_mtx_);
      LOG(DEBUG) << "Store block: " << block->block_id().to_str();
      const std::string key = getKey(handle->id());
      auto blocks_vec = stored_blocks_.find(key);
      if (blocks_vec == stored_blocks_.end()) {
        std::vector<std::pair<ConstBlockHandle, td::Ref<BlockData>>> vec;
        vec.emplace_back(std::pair{handle, block});
        stored_blocks_.insert({key, vec});
      } else {
        blocks_vec->second.emplace_back(std::pair{handle, block});
      }

      P.set_value(std::make_tuple("", ""));
      maybeStartStatePublish(handle->id());
      LOG(DEBUG) << "Stored block: " << block->block_id().to_str();
    }

    void BlockParser::storeComputedBlockState(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state,
                                              td::optional<td::Ref<vm::Cell>> prev_state,
                                              td::optional<td::Ref<vm::Cell>> prev_state_2,
                                              td::Promise<std::tuple<td::string, td::string>> P) {
      if (!check_allowed_shard_parse(handle->id().id.workchain, handle->id().id.shard)) {
        LOG(WARNING) << "Skip computed state data: " << handle->id().id.to_str();
        P.set_value(std::make_tuple("", ""));
        return;
      }

      std::lock_guard<std::mutex> lock(maps_mtx_);
      const double now = td::Time::now();
      LOG(WARNING) << "[publish-state] ready block=" << handle->id().to_str() << " t=" << now;

      const auto prev_ids = handle->prev();
      cacheLiveStateLocked(handle->id(), state);

      if (prev_state && !prev_ids.empty()) {
        cacheLiveStateLocked(prev_ids[0], prev_state.value());
      }
      if (prev_state_2 && prev_ids.size() > 1) {
        cacheLiveStateLocked(prev_ids[1], prev_state_2.value());
      }

      td::optional<td::Ref<vm::Cell>> resolved_prev_state;
      td::optional<td::Ref<vm::Cell>> resolved_prev_state_2;
      if (!prev_ids.empty()) {
        resolved_prev_state = findLiveStateLocked(prev_ids[0]);
        if (!resolved_prev_state) {
          LOG(WARNING) << "[publish-state] cache-miss block=" << handle->id().to_str()
                       << " missing_prev=" << prev_ids[0].to_str();
          P.set_value(std::make_tuple("", ""));
          return;
        }
      }
      if (prev_ids.size() > 1) {
        resolved_prev_state_2 = findLiveStateLocked(prev_ids[1]);
        if (!resolved_prev_state_2) {
          LOG(WARNING) << "[publish-state] cache-miss block=" << handle->id().to_str()
                       << " missing_prev=" << prev_ids[1].to_str();
          P.set_value(std::make_tuple("", ""));
          return;
        }
      }

      startStatePublishLocked(getKey(handle->id()), handle->id(), handle, std::move(block), std::move(state),
                              std::move(resolved_prev_state), std::move(resolved_prev_state_2), now, true);
      P.set_value(std::make_tuple("", ""));
    }

    void BlockParser::storeBlockState(const ConstBlockHandle &handle, td::Ref<vm::Cell> state,
                                      td::Promise<std::tuple<td::string, td::string>> P) {
      if (!check_allowed_shard_parse(handle->id().id.workchain, handle->id().id.shard)) {
        LOG(WARNING) << "Skip state data: " << handle->id().id.to_str();
        P.set_value(std::make_tuple("", ""));
        return;
      }

      if (!startup_replay_mode_) {
        P.set_value(std::make_tuple("", ""));
        return;
      }

      std::lock_guard<std::mutex> lock(maps_mtx_);
      LOG(WARNING) << "[publish-state] rootdb-ready block=" << handle->id().to_str() << " t=" << td::Time::now();
      const std::string key = getKey(handle->id());
      auto states_vec = stored_states_.find(key);
      if (states_vec == stored_states_.end()) {
        std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>> vec;
        vec.emplace_back(std::pair{handle, std::move(state)});
        stored_states_.insert({key, vec});
      } else {
        states_vec->second.emplace_back(std::pair{handle, std::move(state)});
      }

      P.set_value(std::make_tuple("", ""));
      maybeStartStatePublish(handle->id());
      LOG(DEBUG) << "Stored state: " << handle->id().to_str();
    }

    void BlockParser::storeBlockStateWithPrev(const ConstBlockHandle &handle, td::Ref<vm::Cell> prev_state,
                                              td::Ref<vm::Cell> state,
                                              td::Promise<std::tuple<td::string, td::string>> P) {
      if (!check_allowed_shard_parse(handle->id().id.workchain, handle->id().id.shard)) {
        LOG(WARNING) << "Skip state data with prev: " << handle->id().id.to_str();
        P.set_value(std::make_tuple("", ""));
        return;
      }

      if (!startup_replay_mode_) {
        P.set_value(std::make_tuple("", ""));
        return;
      }

      std::lock_guard<std::mutex> lock(maps_mtx_);
      LOG(WARNING) << "[publish-state] rootdb-ready-with-prev block=" << handle->id().to_str() << " t="
                   << td::Time::now();
      const std::string key = getKey(handle->id());
      auto states_vec = stored_states_.find(key);
      if (states_vec == stored_states_.end()) {
        std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>> vec;
        vec.emplace_back(std::pair{handle, std::move(state)});
        stored_states_.insert({key, vec});
      } else {
        states_vec->second.emplace_back(std::pair{handle, std::move(state)});
      }

      auto prev_states_vec = stored_prev_states_.find(key);
      if (prev_states_vec == stored_prev_states_.end()) {
        std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>> prev_state_vec;
        prev_state_vec.emplace_back(std::pair{handle, std::move(prev_state)});
        stored_prev_states_.insert({key, prev_state_vec});
      } else {
        prev_states_vec->second.emplace_back(std::pair{handle, prev_state});
      }

      P.set_value(std::make_tuple("", ""));
      maybeStartStatePublish(handle->id());
      LOG(DEBUG) << "Stored prev state: " << handle->id().to_str();
    }

    void BlockParser::startStatePublishLocked(const std::string &key, const BlockIdExt &id, ConstBlockHandle handle,
                                              td::Ref<BlockData> data, td::Ref<vm::Cell> state,
                                              td::optional<td::Ref<vm::Cell>> prev_state_opt,
                                              td::optional<td::Ref<vm::Cell>> prev_state_opt_2,
                                              double state_ready_at, bool live_mode) {
      if (state_parse_started_.count(key) != 0 || parsed_states_.count(key) != 0) {
        return;
      }

      state_parse_started_.insert(key);
      parsed_states_[key].id = id;
      parsed_states_[key].live_mode = live_mode;
      parsed_states_[key].state_ready_at = state_ready_at;
      parsed_states_[key].parse_started_at = td::Time::now();

      LOG(WARNING) << "[publish-state] index-start block=" << id.to_str()
                   << " after_ready_ms=" << (parsed_states_[key].parse_started_at - state_ready_at) * 1000.0;

      const char *value = getenv("KAFKA_OUTMSG_TOPIC");
      bool allow_send_messages = bool(value);
      auto Po = td::PromiseCreator::lambda(
          [publisher = publisher_, allow_send_messages, cluster_sync = cluster_sync_](
              td::Result<std::tuple<td::vector<json>, td::Bits256, unsigned long long, int>> R) {
            if (R.is_ok() && allow_send_messages) {
              td::actor::send_closure(cluster_sync, &ClusterPublishSync::sync_block_trace, R.move_as_ok(), publisher);
            }
          });

      auto promise_try_sync = td::PromiseCreator::lambda(
          [this, key, id](td::Result<std::tuple<td::Bits256, td::string, td::string>> R) mutable {
            onStateParsed(std::move(key), id, std::move(R));
          });

      td::actor::create_actor<BlockParserAsync>("BlockParserAsync", id, handle, data, state, prev_state_opt,
                                                prev_state_opt_2, std::move(promise_try_sync), std::move(Po))
          .release();
    }

    void BlockParser::cacheLiveStateLocked(const BlockIdExt &id, td::Ref<vm::Cell> state) {
      const std::string key = getKey(id);
      if (live_state_cache_.find(key) != live_state_cache_.end()) {
        return;
      }
      const std::string lineage_key = getLineageKey(id);
      live_state_cache_.emplace(key, CachedLiveState{id, std::move(state), lineage_key});
      live_state_lineages_[lineage_key].push_back(key);
      evictLiveStatesLocked(lineage_key);
    }

    td::optional<td::Ref<vm::Cell>> BlockParser::findLiveStateLocked(const BlockIdExt &id) const {
      const auto it = live_state_cache_.find(getKey(id));
      if (it == live_state_cache_.end()) {
        return {};
      }
      return it->second.state;
    }

    void BlockParser::evictLiveStatesLocked(const std::string &lineage_key) {
      auto it = live_state_lineages_.find(lineage_key);
      if (it == live_state_lineages_.end()) {
        return;
      }
      while (it->second.size() > 20) {
        live_state_cache_.erase(it->second.front());
        it->second.pop_front();
      }
      if (it->second.empty()) {
        live_state_lineages_.erase(it);
      }
    }

    void BlockParser::maybeStartStatePublish(const BlockIdExt &id) {
      const std::string key = getKey(id);
      if (state_parse_started_.count(key) != 0 || parsed_states_.count(key) != 0) {
        return;
      }

      auto blocks_vec_found = stored_blocks_.find(key);
      if (blocks_vec_found == stored_blocks_.end()) {
        return;
      }
      auto block_found_iter = std::find_if(blocks_vec_found->second.begin(), blocks_vec_found->second.end(),
                                           [&id](const auto &b) { return b.first->id() == id; });
      if (block_found_iter == blocks_vec_found->second.end()) {
        return;
      }

      auto states_vec_found = stored_states_.find(key);
      if (states_vec_found == stored_states_.end()) {
        return;
      }
      auto state_found_iter = std::find_if(states_vec_found->second.begin(), states_vec_found->second.end(),
                                           [&id](const auto &s) { return s.first->id() == id; });
      if (state_found_iter == states_vec_found->second.end()) {
        return;
      }

      bool with_prev_state = false;
      td::Ref<vm::Cell> prev_state;
      auto prev_states_vec_found = stored_prev_states_.find(key);
      if (prev_states_vec_found != stored_prev_states_.end()) {
        auto prev_state_found_iter =
            std::find_if(prev_states_vec_found->second.begin(), prev_states_vec_found->second.end(),
                         [&id](const auto &s) { return s.first->id() == id; });
        if (prev_state_found_iter != prev_states_vec_found->second.end()) {
          with_prev_state = true;
          prev_state = prev_state_found_iter->second;
        }
      }

      ConstBlockHandle handle = block_found_iter->first;
      td::Ref<BlockData> data = block_found_iter->second;
      td::Ref<vm::Cell> state = state_found_iter->second;
      td::optional<td::Ref<vm::Cell>> prev_state_opt;
      if (with_prev_state) {
        prev_state_opt = prev_state;
      }

      stored_blocks_.erase(blocks_vec_found);
      stored_states_.erase(states_vec_found);
      if (with_prev_state) {
        stored_prev_states_.erase(prev_states_vec_found);
      }
      startStatePublishLocked(key, id, handle, std::move(data), std::move(state), std::move(prev_state_opt), {},
                              td::Time::now(), false);
    }

    void BlockParser::onStateParsed(std::string key, BlockIdExt id,
                                    td::Result<std::tuple<td::Bits256, td::string, td::string>> R) {
      if (R.is_error()) {
        LOG(ERROR) << "State parsing failed for " << key << ": " << R.error().message();
        std::lock_guard<std::mutex> lock(maps_mtx_);
        state_parse_started_.erase(key);
        return;
      }

      double parse_started_at = 0.0;
      auto result = R.move_as_ok();
      const auto root_hash = std::get<0>(result);
      const auto started_at = td::Time::now();

      {
        std::lock_guard<std::mutex> lock(maps_mtx_);
        auto &entry = parsed_states_[key];
        entry.id = id;
        entry.block_json = std::get<1>(result);
        entry.state_json = std::get<2>(result);
        entry.parse_finished_at = started_at;
        entry.sync_started_at = started_at;
        parse_started_at = entry.parse_started_at;
      }
      LOG(WARNING) << "[publish-state] index-done block=" << id.to_str()
                   << " duration_ms=" << (started_at - parse_started_at) * 1000.0;

      auto promise_try_sync = td::PromiseCreator::lambda(
          [this, key, root_hash, started_at, id](td::Result<std::tuple<td::string, td::string>> sync_result) mutable {
            LOG(WARNING) << "[publish-state] sync-done block=" << id.to_str()
                         << " duration_ms=" << (td::Time::now() - started_at) * 1000.0
                         << " root_hash=" << root_hash.to_hex();
            onStateSyncResult(std::move(key), std::move(sync_result));
          });
      td::actor::send_closure(cluster_sync_, &ClusterPublishSync::sync_block_state, std::move(result),
                              std::move(promise_try_sync));
    }

    void BlockParser::onStateSyncResult(std::string key, td::Result<std::tuple<td::string, td::string>> R) {
      td::string state_json;
      td::string block_json;
      td::int32 wc = 0;
      unsigned long long shard = 0;
      {
        std::lock_guard<std::mutex> lock(maps_mtx_);
        auto it = parsed_states_.find(key);
        if (it == parsed_states_.end()) {
          return;
        }

        if (R.is_error()) {
          it->second.skip_due_to_sync = true;
          cleanupPublishedStateLocked(key);
          return;
        }

        auto synced = R.move_as_ok();
        it->second.publish_allowed = true;
        it->second.block_json = std::get<0>(synced);
        it->second.state_json = std::get<1>(synced);
        if (!it->second.state_published) {
          it->second.state_published = true;
          state_json = it->second.state_json;
        }
        if (!it->second.block_published) {
          it->second.block_published = true;
          block_json = it->second.block_json;
        }
        wc = it->second.id.id.workchain;
        shard = it->second.id.id.shard;
        cleanupPublishedStateLocked(key);
      }

      if (!state_json.empty()) {
        LOG(WARNING) << "[publish-state] kafka-enqueue block=" << key;
        enqueuePublishBlockState(wc, shard, state_json);
      }
      if (!block_json.empty()) {
        LOG(WARNING) << "[publish-block] kafka-enqueue block=" << key;
        enqueuePublishBlockData(wc, shard, block_json);
      }
    }

    void BlockParser::onAppliedSyncResult(std::string key, BlockIdExt id,
                                          td::Result<std::tuple<td::string, td::string>> R) {
      td::string applied_json;
      {
        std::lock_guard<std::mutex> lock(maps_mtx_);
        if (R.is_error()) {
          return;
        }

        auto synced = R.move_as_ok();
        applied_json = std::get<0>(synced);
        stored_applied_[key] = id;
      }

      if (!applied_json.empty()) {
        LOG(WARNING) << "[publish-apply] kafka-enqueue block=" << key;
        enqueuePublishBlockApplied(id.id.workchain, id.id.shard, applied_json);
      }
    }

    void BlockParser::maybePublishBlockData(std::string key) {
      td::string block_json;
      td::int32 wc = 0;
      unsigned long long shard = 0;
      {
        std::lock_guard<std::mutex> lock(maps_mtx_);
        auto parsed_it = parsed_states_.find(key);
        if (parsed_it == parsed_states_.end()) {
          return;
        }
        if (!parsed_it->second.publish_allowed || parsed_it->second.block_published ||
            parsed_it->second.skip_due_to_sync) {
          cleanupPublishedStateLocked(key);
          return;
        }
        parsed_it->second.block_published = true;
        block_json = parsed_it->second.block_json;
        wc = parsed_it->second.id.id.workchain;
        shard = parsed_it->second.id.id.shard;
        cleanupPublishedStateLocked(key);
      }

      if (!block_json.empty()) {
        LOG(WARNING) << "[publish-block] kafka-enqueue block=" << key;
        enqueuePublishBlockData(wc, shard, block_json);
      }
    }

    void BlockParser::cleanupPublishedStateLocked(const std::string &key) {
      auto parsed_it = parsed_states_.find(key);
      if (parsed_it == parsed_states_.end()) {
        return;
      }

      const bool done = parsed_it->second.skip_due_to_sync ||
                        (parsed_it->second.state_published && parsed_it->second.block_published);
      if (!done) {
        return;
      }

      parsed_states_.erase(parsed_it);
      state_parse_started_.erase(key);
      stored_prev_states_.erase(key);
    }

    void BlockParser::handleBlockProgress(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P) {
      P.set_value(std::make_tuple("", ""));
      {
        std::lock_guard<std::mutex> lock(maps_mtx_);
        maybeStartStatePublish(id);
      }
    }

    std::string BlockParser::parseBlockApplied(BlockIdExt id) {
      json to_dump = {{"file_hash", id.file_hash.to_hex()},
                      {"root_hash", id.root_hash.to_hex()},
                      {"id",
                                    {
                                            {"workchain", id.id.workchain},
                                            {"seqno", id.id.seqno},
                                            {"shard", id.id.shard},
                                    }}};

      std::string dump = to_dump.dump();
      if (post_processor_) {
        dump = post_processor_(dump);
      }

      return dump;
    }

    void BlockParser::enqueuePublishBlockApplied(td::int32 wc, unsigned long long shard, const std::string &json) {
      std::unique_lock lock(publish_applied_mtx_);
      publish_applied_queue_.emplace(wc, shard, json);
      lock.unlock();
      publish_applied_cv_.notify_one();
    }

    void BlockParser::enqueuePublishBlockData(td::int32 wc, unsigned long long shard, const std::string &json) {
      std::unique_lock lock(publish_blocks_mtx_);
      publish_blocks_queue_.emplace(wc, shard, json);
      lock.unlock();
      publish_blocks_cv_.notify_one();
    }

    void BlockParser::enqueuePublishBlockState(td::int32 wc, unsigned long long shard, const std::string &json) {
      std::unique_lock lock(publish_states_mtx_);
      publish_states_queue_.emplace(wc, shard, json);
      lock.unlock();
      publish_states_cv_.notify_one();
    }

    void BlockParser::publish_applied_worker() {
      bool should_run = running_;
      while (should_run) {
        std::unique_lock lock(publish_applied_mtx_);
        publish_applied_cv_.wait(lock, [this] { return !publish_applied_queue_.empty() || !running_; });
        if (publish_applied_queue_.empty()) {
          continue;
        }

        auto block = std::move(publish_applied_queue_.front());
        publish_applied_queue_.pop();

        should_run = running_ || !publish_applied_queue_.empty();
        lock.unlock();

        LOG(WARNING) << "[publish-apply] kafka-send wc=" << std::get<0>(block) << " shard=" << std::get<1>(block);
        publisher_->publishBlockApplied(std::get<0>(block), std::get<1>(block), std::get<2>(block));
      }
    }

    void BlockParser::publish_blocks_worker() {
      bool should_run = running_;
      while (should_run) {
        std::unique_lock lock(publish_blocks_mtx_);
        publish_blocks_cv_.wait(lock, [this] { return !publish_blocks_queue_.empty() || !running_; });
        if (publish_blocks_queue_.empty()) {
          continue;
        }

        auto block = std::move(publish_blocks_queue_.front());
        publish_blocks_queue_.pop();

        should_run = running_;
        lock.unlock();

        LOG(WARNING) << "[publish-block] kafka-send wc=" << std::get<0>(block) << " shard=" << std::get<1>(block);
        publisher_->publishBlockData(std::get<0>(block), std::get<1>(block), std::get<2>(block));
      }
    }

    void BlockParser::publish_states_worker() {
      bool should_run = running_;
      while (should_run) {
        std::unique_lock lock(publish_states_mtx_);
        publish_states_cv_.wait(lock, [this] { return !publish_states_queue_.empty() || !running_; });
        if (publish_states_queue_.empty()) {
          continue;
        }
        auto state = std::move(publish_states_queue_.front());
        publish_states_queue_.pop();

        should_run = running_;
        lock.unlock();

        LOG(WARNING) << "[publish-state] kafka-send wc=" << std::get<0>(state) << " shard=" << std::get<1>(state);
        publisher_->publishBlockState(std::get<0>(state), std::get<1>(state), std::get<2>(state));
      }
    }

}  // namespace ton::validator
