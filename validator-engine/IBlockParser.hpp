#ifndef TON_IBLOCKPARSER_HPP
#define TON_IBLOCKPARSER_HPP

#include <map>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include "validator/interfaces/block-handle.h"
#include "validator/interfaces/block.h"
#include "validator/interfaces/shard.h"

namespace ton::validator {

class IBLockPublisher {
 public:
  virtual ~IBLockPublisher() = default;

  virtual void publishBlockApplied(unsigned long long shard, std::string json) = 0;
  virtual void publishBlockData(unsigned long long shard, std::string json) = 0;
  virtual void publishBlockState(unsigned long long shard, std::string json) = 0;
  virtual void deliver();
};

class IBlockParser {
 public:
  virtual ~IBlockParser() = default;

  virtual void storeBlockApplied(BlockIdExt id) = 0;
  virtual void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) = 0;
  virtual void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) = 0;
  virtual void storeBlockStateWithPrev(BlockHandle handle, td::Ref<vm::Cell> prev_state,
                                       td::Ref<ShardState> state) = 0;
};

class BlockParser : public IBlockParser {
 public:
  explicit BlockParser(std::unique_ptr<IBLockPublisher> publisher);
  ~BlockParser() override;

 public:
  void storeBlockApplied(BlockIdExt id) final;
  void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) final;
  void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) final;
  void storeBlockStateWithPrev(BlockHandle handle, td::Ref<vm::Cell> prev_state,
                               td::Ref<ShardState> state) final;

  void setPostProcessor(std::function<std::string(std::string)>);

 private:
  void gotState(BlockHandle handle, td::Ref<ShardState> state, std::vector<td::Bits256> accounts_keys);

  void handleBlockProgress(BlockIdExt id);

  std::string parseBlockApplied(BlockIdExt id);
  std::pair<std::string, std::vector<std::pair<td::Bits256, int>>> parseBlockData(BlockIdExt id, const BlockHandle& handle,
                                                                  const td::Ref<BlockData>& data);
  std::string parseBlockState(BlockIdExt id, const BlockHandle& handle, const td::Ref<ShardState>& state,
                                           const std::vector<std::pair<td::Bits256, int>>& accounts_keys,
                                           const td::optional<td::Ref<vm::Cell>>& prev_state);
  void enqueuePublishBlockApplied(unsigned long long shard, std::string json);
  void enqueuePublishBlockData(unsigned long long shard, std::string json);
  void enqueuePublishBlockState(unsigned long long shard, std::string json);

  void publish_applied_worker();
  void publish_blocks_worker();
  void publish_states_worker();

 private:
  std::unique_ptr<IBLockPublisher> publisher_;
  std::function<std::string(std::string)> post_processor_;

  std::mutex maps_mtx_;
  std::map<std::string, BlockIdExt> stored_applied_;
  std::map<std::string, std::vector<std::pair<BlockHandle, td::Ref<BlockData>>>> stored_blocks_;        // multimap?
  std::map<std::string, std::vector<std::pair<BlockHandle, td::Ref<ShardState>>>> stored_states_;       // multimap?
  std::map<std::string, std::vector<std::pair<BlockHandle, td::Ref<vm::Cell>>>> stored_prev_states_;  // multimap?

  // mb rewrite with https://github.com/andreiavrammsd/cpp-channel

  std::atomic_bool running_ = true;  // TODO: stop_token when c++20

  std::mutex publish_applied_mtx_;
  std::condition_variable publish_applied_cv_;
  std::queue<std::tuple<unsigned long long, std::string>> publish_applied_queue_;
  std::thread publish_applied_thread_;

  std::mutex publish_blocks_mtx_;
  std::condition_variable publish_blocks_cv_;
  std::queue<std::tuple<unsigned long long, std::string>> publish_blocks_queue_;
  std::thread publish_blocks_thread_;

  std::mutex publish_states_mtx_;
  std::condition_variable publish_states_cv_;
  std::queue<std::tuple<unsigned long long, std::string>> publish_states_queue_;
  std::thread publish_states_thread_;
};

}  // namespace ton::validator
#endif  //TON_IBLOCKPARSER_HPP
