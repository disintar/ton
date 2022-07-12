#ifndef TON_BLOCKPUBLISHER_HPP
#define TON_BLOCKPUBLISHER_HPP

#include <map>
#include <string>
#include <zmq.hpp>
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

class BlockPublisher : public IBlockPublisher {
 public:
  explicit BlockPublisher(const std::string& endpoint);

  void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) override;
  void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) override;

 private:
  void gotState(BlockHandle handle, td::Ref<ShardState> state, std::vector<td::Bits256> accounts_keys);
  void publishBlockData(const std::string& json);
  void publishBlockState(const std::string& json);

 private:
  zmq::context_t ctx;
  zmq::socket_t socket;
  std::mutex net_mtx;

  std::mutex maps_mtx;
  std::map<std::string, std::pair<BlockHandle, td::Ref<ShardState>>> states_;
  std::map<std::string, std::vector<td::Bits256>> accounts_keys_;
};

}
#endif  //TON_BLOCKPUBLISHER_HPP
