#ifndef TON_BLOCKPUBLISHER_HPP
#define TON_BLOCKPUBLISHER_HPP

#include <map>
#include <string>
#include <zmq.hpp>
#include <AMQPcpp.h>
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
  void storeBlockData(BlockHandle handle, td::Ref<BlockData> block) override;
  void storeBlockState(BlockHandle handle, td::Ref<ShardState> state) override;
 private:
  void gotState(BlockHandle handle, td::Ref<ShardState> state, std::vector<td::Bits256> accounts_keys);

  virtual void publishBlockData(const std::string& json) = 0;
  virtual void publishBlockState(const std::string& json) = 0;

 private:
  std::mutex maps_mtx;
  std::map<std::string, std::pair<BlockHandle, td::Ref<ShardState>>> states_;
  std::map<std::string, std::vector<td::Bits256>> accounts_keys_;
};

// TODO: separate files

class BlockPublisherZMQ : public BlockPublisherParser {
 public:
  explicit BlockPublisherZMQ(const std::string& endpoint);

 private:
  void publishBlockData(const std::string& json) override;
  void publishBlockState(const std::string& json) override;

 private:
  zmq::context_t ctx;
  zmq::socket_t socket;
  std::mutex net_mtx;
};

class BlockPublisherRMQ : public BlockPublisherParser {
 public:
  explicit BlockPublisherRMQ(const std::string& endpoint);

 private:
  void publishBlockData(const std::string& json) override;
  void publishBlockState(const std::string& json) override;

 private:
  AMQP amqp;
  std::unique_ptr<AMQPExchange> exchange;
  std::unique_ptr<AMQPQueue> queue_data;
  std::unique_ptr<AMQPQueue> queue_state;
  std::mutex net_mtx;
};

}
#endif  //TON_BLOCKPUBLISHER_HPP
