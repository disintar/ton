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
  virtual void deliver() = 0;
};

class BlockParser {
 public:
  explicit BlockParser(std::unique_ptr<IBLockPublisher> publisher);
  ~BlockParser();

 public:
  void storeBlockApplied(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P);
  void storeBlockData(ConstBlockHandle handle, td::Ref<BlockData> block, td::Promise<std::tuple<td::string, td::string>> P);
  void storeBlockState(const ConstBlockHandle& handle, td::Ref<vm::Cell> state,
                       td::Promise<std::tuple<td::string, td::string>> P);
  void storeBlockStateWithPrev(const ConstBlockHandle& handle, td::Ref<vm::Cell> prev_state, td::Ref<vm::Cell> state,
                               td::Promise<std::tuple<td::string, td::string>> P);

  void enqueuePublishBlockApplied(unsigned long long shard, const std::string& json);
  void enqueuePublishBlockData(unsigned long long shard, const std::string& json);
  void enqueuePublishBlockState(unsigned long long shard, const std::string& json);

 private:
  void handleBlockProgress(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P);

  std::string parseBlockApplied(BlockIdExt id);

  void publish_applied_worker();
  void publish_blocks_worker();
  void publish_states_worker();

 private:
  std::unique_ptr<IBLockPublisher> publisher_;
  std::function<std::string(std::string)> post_processor_;

  std::mutex maps_mtx_;
  std::map<std::string, BlockIdExt> stored_applied_;
  std::map<std::string, std::vector<std::pair<ConstBlockHandle, td::Ref<BlockData>>>> stored_blocks_;      // multimap?
  std::map<std::string, std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>>> stored_states_;     // multimap?
  std::map<std::string, std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>>> stored_prev_states_;  // multimap?

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
