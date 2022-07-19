#include "BlockPublisherKafka.hpp"

namespace ton::validator {

BlockPublisherKafka::BlockPublisherKafka(const std::string& endpoint) : producer(
    cppkafka::Configuration{
        { "metadata.broker.list", endpoint }
    }
) {}

void BlockPublisherKafka::publishBlockData(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "Sending " << json.size() << " bytes to Kafka";
  producer.produce(cppkafka::MessageBuilder("block-data").partition(0).payload(json));
  producer.flush();
}

void BlockPublisherKafka::publishBlockState(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "Sending " << json.size() << " bytes to Kafka";
  producer.produce(cppkafka::MessageBuilder("block-state").partition(0).payload(json));
  producer.flush();
}

}
