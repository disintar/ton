#pragma once

#include <map>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <deque>
#include <set>
#include "validator/interfaces/block-handle.h"
#include "validator/interfaces/block.h"
#include "validator/interfaces/shard.h"
#include "blockchain-indexer/json.hpp"
#include "validator-engine/ClusterSyncer.hpp"

using json = nlohmann::json;

namespace ton {
    class ClusterPublishSync;

    namespace validator {

        class IBLockPublisher {
        public:
            virtual ~IBLockPublisher() = default;

            virtual void publishBlockApplied(int wc, unsigned long long shard, std::string json) = 0;

            virtual void publishBlockData(int wc, unsigned long long shard, std::string json) = 0;

            virtual void publishBlockState(int wc, unsigned long long shard, std::string json) = 0;

            virtual void publishOutMsgs(int wc, unsigned long long shard, std::string out_msg_data) = 0;

            virtual void deliver() = 0;

            virtual void merge_new_shards(std::map<unsigned long long, int> new_shards) = 0;
        };

        class BlockParser {
        public:
            explicit BlockParser(std::unique_ptr<IBLockPublisher> publisher);

            ~BlockParser();

	        public:
	            void storeBlockApplied(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P);

            void set_cluster_sync(td::actor::ActorId<ClusterPublishSync> sync_actor) {
              cluster_sync_ = std::move(sync_actor);
            }

            void set_startup_replay_mode(bool enabled) {
              startup_replay_mode_ = enabled;
            }

	            void storeBlockData(ConstBlockHandle handle, td::Ref<BlockData> block,
	                                td::Promise<std::tuple<td::string, td::string>> P);
	            void storeComputedBlockState(ConstBlockHandle handle, td::Ref<BlockData> block, td::Ref<vm::Cell> state,
	                                         td::optional<td::Ref<vm::Cell>> prev_state,
	                                         td::optional<td::Ref<vm::Cell>> prev_state_2,
	                                         td::Promise<std::tuple<td::string, td::string>> P);

            bool process_out_msgs(const std::vector<json> &data);

            void merge_new_shards(std::map<unsigned long long, int> new_shards) {
              for (const auto &pair: new_shards) {
                shard_to_partition[pair.first] = pair.second;
              }

              publisher_->merge_new_shards(std::move(new_shards));
            }

            bool check_allowed_shard_parse(int wc, unsigned long long shard) {
              int p;
              if (wc == -1) {
                p = 0;
              } else {
                if (shard_to_partition.find(shard) == shard_to_partition.end()) {
                  max_partition++;
                  if (max_partition > 16) {
                    max_partition = 1;
                  }
                  p = max_partition;
                  shard_to_partition[shard] = max_partition;
                } else {
                  p = shard_to_partition[shard];
                }
              }

              std::string env_var_name = "PARSE_SHARDE_" + std::to_string(p);
              const char *env_var_name_cstr = env_var_name.c_str();
              bool allow;
              const char *env_var_value = std::getenv(env_var_name_cstr);
              if (env_var_value == nullptr) {
                LOG(ERROR) << "Environment variable " << env_var_name << " is not set. Allow shard.";
                allow = true;
              } else {
                try {
                  allow = std::stoi(env_var_value) == 1;
                } catch (const std::invalid_argument &e) {
                  LOG(WARNING)
                  << "Invalid value for environment variable" << env_var_name << ": not an integer. Allow shard.";
                  allow = true;
                } catch (const std::out_of_range &e) {
                  LOG(WARNING)
                  << "Invalid value for environment variable " << env_var_name << ": out of range. Allow shard.";
                  allow = true;
                }
              }

              return allow;
            }

            void storeBlockState(const ConstBlockHandle &handle, td::Ref<vm::Cell> state,
                                 td::Promise<std::tuple<td::string, td::string>> P);

            void storeBlockStateWithPrev(const ConstBlockHandle &handle, td::Ref<vm::Cell> prev_state,
                                         td::Ref<vm::Cell> state,
                                         td::Promise<std::tuple<td::string, td::string>> P);

            void enqueuePublishBlockApplied(td::int32 wc, unsigned long long shard, const std::string &json);

            void enqueuePublishBlockData(td::int32 wc, unsigned long long shard, const std::string &json);

            void enqueuePublishBlockState(td::int32 wc, unsigned long long shard, const std::string &json);

	        private:
	            struct ParsedBlockState {
		              BlockIdExt id;
		              td::string block_json;
		              td::string state_json;
		              bool publish_allowed{false};
		              bool apply_published{false};
		              bool block_published{false};
		              bool state_published{false};
		              bool skip_due_to_sync{false};
		              bool live_mode{false};
		              double state_ready_at{0.0};
		              double parse_started_at{0.0};
		              double parse_finished_at{0.0};
		              double sync_started_at{0.0};
		            };

		            struct PendingAppliedState {
		              BlockIdExt id;
		              td::string apply_json;
		              bool ready{false};
		            };

		            struct CachedLiveState {
		              BlockIdExt id;
		              td::Ref<vm::Cell> state;
		              std::string lineage_key;
		            };

		            static std::string getKey(const BlockIdExt &id);
		            static std::string getLineageKey(const BlockIdExt &id);
		            void startStatePublishLocked(const std::string &key, const BlockIdExt &id, ConstBlockHandle handle,
		                                         td::Ref<BlockData> data, td::Ref<vm::Cell> state,
		                                         td::optional<td::Ref<vm::Cell>> prev_state_opt,
		                                         td::optional<td::Ref<vm::Cell>> prev_state_opt_2,
		                                         double state_ready_at, bool live_mode);
		            void maybeStartStatePublish(const BlockIdExt &id);
	            void cacheLiveStateLocked(const BlockIdExt &id, td::Ref<vm::Cell> state);
	            td::optional<td::Ref<vm::Cell>> findLiveStateLocked(const BlockIdExt &id) const;
	            void evictLiveStatesLocked(const std::string &lineage_key);
	            void onStateParsed(std::string key, BlockIdExt id,
	                               td::Result<std::tuple<td::Bits256, td::string, td::string>> R);
	            void onStateSyncResult(std::string key, td::Result<std::tuple<td::string, td::string>> R);
	            void maybePublishBundle(std::string key);
	            void cleanupPublishedStateLocked(const std::string &key);
	            bool isCompletedLocked(const std::string &key) const;
	            void markCompletedLocked(const std::string &key, const BlockIdExt &id);
	            void handleBlockProgress(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P);

	            std::string parseBlockApplied(BlockIdExt id);

            void publish_applied_worker();

            void publish_blocks_worker();

            void publish_states_worker();

        private:
            std::shared_ptr<IBLockPublisher> publisher_;
            td::actor::ActorId<ClusterPublishSync> cluster_sync_;
            std::function<std::string(std::string)> post_processor_;
            std::map<unsigned long long, int> shard_to_partition{};
            int max_partition = 0;

	            std::mutex maps_mtx_;
	            std::map<std::string, PendingAppliedState> pending_applied_;
	            std::map<std::string, std::vector<std::pair<ConstBlockHandle, td::Ref<BlockData>>>> stored_blocks_;      // multimap?
	            std::map<std::string, std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>>> stored_states_;       // multimap?
	            std::map<std::string, std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>>> stored_prev_states_;  // multimap?
	            std::map<std::string, ParsedBlockState> parsed_states_;
	            std::set<std::string> state_parse_started_;
	            std::map<std::string, CachedLiveState> live_state_cache_;
	            std::map<std::string, std::deque<std::string>> live_state_lineages_;
	            std::set<std::string> completed_keys_;
	            std::map<std::string, std::deque<std::string>> completed_lineages_;
	            std::atomic_bool startup_replay_mode_{false};

            // mb rewrite with https://github.com/andreiavrammsd/cpp-channel

            std::atomic_bool running_ = true;  // TODO: stop_token when c++20
            std::mutex publish_out_msg_mtx_;

            std::mutex publish_applied_mtx_;
            std::condition_variable publish_applied_cv_;
            std::queue<std::tuple<td::int32, unsigned long long, std::string>> publish_applied_queue_;
            std::thread publish_applied_thread_;

            std::mutex publish_blocks_mtx_;
            std::condition_variable publish_blocks_cv_;
            std::queue<std::tuple<td::int32, unsigned long long, std::string>> publish_blocks_queue_;
            std::thread publish_blocks_thread_;

            std::mutex publish_states_mtx_;
            std::condition_variable publish_states_cv_;
            std::queue<std::tuple<td::int32, unsigned long long, std::string>> publish_states_queue_;
            std::thread publish_states_thread_;
        };

    }
}  // namespace ton::validator
