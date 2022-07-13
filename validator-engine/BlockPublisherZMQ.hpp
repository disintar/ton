#ifndef TON_BLOCKPUBLISHERZMQ_HPP
#define TON_BLOCKPUBLISHERZMQ_HPP

#include "IBlockPublisher.hpp"
#include <zmq.hpp>

namespace ton::validator {

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

}

#endif  //TON_BLOCKPUBLISHERZMQ_HPP
