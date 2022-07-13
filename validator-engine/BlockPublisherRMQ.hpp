#ifndef TON_BLOCKPUBLISHERRMQ_HPP
#define TON_BLOCKPUBLISHERRMQ_HPP

#include "IBlockPublisher.hpp"
#include <AMQPcpp.h>

namespace ton::validator {

class BlockPublisherRMQ : public BlockPublisherParser {
 public:
  explicit BlockPublisherRMQ(const std::string& endpoint);

 private:
  void publishBlockData(const std::string& json) override;
  void publishBlockState(const std::string& json) override;

 private:
  AMQP amqp;
  AMQPExchange* exchange;
  AMQPQueue* queue_data;
  AMQPQueue* queue_state;
  std::mutex net_mtx;
};

}

#endif  //TON_BLOCKPUBLISHERRMQ_HPP
