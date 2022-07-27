#ifndef TON_IBLOCKPUBLISHER_HPP
#define TON_IBLOCKPUBLISHER_HPP

#include <map>
#include <string>
#include "validator/interfaces/block-handle.h"
#include "validator/interfaces/block.h"
#include "validator/interfaces/shard.h"

namespace ton::validator {

class IBlockPublisher {
 public:
  virtual ~IBlockPublisher() = default;

  virtual void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) = 0;
  virtual void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) = 0;
};

class BlockPublisherIgnore : public IBlockPublisher {
 public:
  void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) override {};
  void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) override {};
};

// helper class, do not use it
class BlockPublisherParser : public IBlockPublisher {
 public:
  void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) override final;
  void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) override final;
 private:
  void gotState(BlockHandle handle, td::Ref<ShardState> state, std::vector<td::Bits256> accounts_keys);

  void enqueuePublishBlockData(std::string json);
  void enqueuePublishBlockState(std::string json);

  virtual void publishBlockData(const std::string& json) = 0;
  virtual void publishBlockState(const std::string& json) = 0;

 private:
  std::mutex maps_mtx;
  std::map<std::string, std::pair<BlockHandle, td::Ref<ShardState>>> states_;
  std::map<std::string, std::vector<td::Bits256>> accounts_keys_;
};

}
#endif  //TON_IBLOCKPUBLISHER_HPP
