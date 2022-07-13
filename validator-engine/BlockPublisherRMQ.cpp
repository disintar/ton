#include "BlockPublisherRMQ.hpp"

namespace ton::validator {

BlockPublisherRMQ::BlockPublisherRMQ(const std::string& endpoint)
    : amqp(endpoint)
    , exchange(amqp.createExchange("BlockPublisher"))
    , queue_data(amqp.createQueue("block/data"))
    , queue_state(amqp.createQueue("block/state")) {
  exchange->Declare("BlockPublisher", "direct");
  queue_data->Declare();
  queue_data->Bind("BlockPublisher", "");
  queue_state->Declare();
  queue_state->Bind("BlockPublisher", "");
}

void BlockPublisherRMQ::publishBlockData(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);

  exchange->Publish(json, "block/data");
}

void BlockPublisherRMQ::publishBlockState(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);

  exchange->Publish(json, "block/state");
}

}
