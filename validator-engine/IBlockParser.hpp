#ifndef TON_IBLOCKPARSER_HPP
#define TON_IBLOCKPARSER_HPP

#include <map>
#include <queue>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "validator/interfaces/block-handle.h"
#include "validator/interfaces/block.h"
#include "validator/interfaces/shard.h"

namespace ton::validator {

class IBLockPublisher {
 public:
  virtual ~IBLockPublisher() = default;

  virtual void publishBlockApplied(std::string json) = 0;
  virtual void publishBlockData(std::string json) = 0;
  virtual void publishBlockState(std::string json) = 0;
};

class BLockPublisherIgnore : public IBLockPublisher {
 public:
  void publishBlockApplied(std::string json) override {};
  void publishBlockData(std::string json) override {};
  void publishBlockState(std::string json) override {};
};

class IBlockParser {
 public:
  virtual ~IBlockParser() = default;

  virtual void setBlockPublisher(std::unique_ptr<IBLockPublisher> publisher) = 0;

  virtual void storeBlockApplied(BlockIdExt id) = 0;
  virtual void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) = 0;
  virtual void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) = 0;
};


class BlockParser : public IBlockParser {
public:
    BlockParser();
    ~BlockParser() override;

 public:
  void setBlockPublisher(std::unique_ptr<IBLockPublisher> publisher) final;

  void storeBlockApplied(BlockIdExt id) final;
  void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) final;
  void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) final;

 private:
  void gotState(BlockHandle handle, td::Ref<ShardState> state, std::vector<td::Bits256> accounts_keys);

  void enqueuePublishBlockApplied(std::string json);
  void enqueuePublishBlockData(std::string json);
  void enqueuePublishBlockState(std::string json);

  void publish_applied_worker();
  void publish_blocks_worker();
  void publish_states_worker();

 private:
  std::unique_ptr<IBLockPublisher> publisher_;

  std::mutex maps_mtx_;
  std::map<std::string, std::pair<BlockHandle, td::Ref<ShardState>>> stored_states_;
  std::map<std::string, std::vector<td::Bits256>> stored_accounts_keys_;

  // mb rewrite with https://github.com/andreiavrammsd/cpp-channel

  std::atomic_bool running_ = true; // TODO: stop_token when c++20

  std::mutex publish_applied_mtx_;
  std::condition_variable publish_applied_cv_;
  std::queue<std::string> publish_applied_queue_;
  std::thread publish_applied_thread_;

  std::mutex publish_blocks_mtx_;
  std::condition_variable publish_blocks_cv_;
  std::queue<std::string> publish_blocks_queue_;
  std::thread publish_blocks_thread_;

  std::mutex publish_states_mtx_;
  std::condition_variable publish_states_cv_;
  std::queue<std::string> publish_states_queue_;
  std::thread publish_states_thread_;
};

}
#endif  //TON_IBLOCKPARSER_HPP
