#include "BlockPublisherKafka.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

BlockPublisherKafka::BlockPublisherKafka(const std::string& endpoint) : producer(
    cppkafka::Configuration{
        { "metadata.broker.list", endpoint },
        { "message.max.bytes", "1000000000" },  // max
        { "acks", "1" }
    }
) {}

void BlockPublisherKafka::publishBlockApplied(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "Sending " << json.size() << " bytes to Kafka";
  try {
    producer.produce(cppkafka::MessageBuilder("block-applied").partition(0).payload(json));
    producer.flush(std::chrono::milliseconds(10000));
  } catch (std::exception& e) {
    const auto id = to_string(json::parse(json)["id"]);
    LOG(ERROR) << "Error while sending block applied (" << id << ") to kafka: " << e.what();
    publishBlockError(id, e.what());
  }
}

void BlockPublisherKafka::publishBlockData(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "Sending " << json.size() << " bytes to Kafka";
  try {
    producer.produce(cppkafka::MessageBuilder("block-data").partition(0).payload(json));
    producer.flush(std::chrono::milliseconds(10000));
  } catch (std::exception& e) {
    const auto id = to_string(json::parse(json)["id"]);
    LOG(ERROR) << "Error while sending block data (" << id << ") to kafka: " << e.what();
    publishBlockError(id, e.what());
  }
}

void BlockPublisherKafka::publishBlockState(const std::string& json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "Sending " << json.size() << " bytes to Kafka";
  try {
    producer.produce(cppkafka::MessageBuilder("block-state").partition(0).payload(json));
    producer.flush(std::chrono::milliseconds(10000));
  } catch (std::exception& e) {
    const auto id = to_string(json::parse(json)["id"]);
    LOG(ERROR) << "Error while sending block state (" << id << ") to kafka: " << e.what();
    publishBlockError(id, e.what());
  }
}

void BlockPublisherKafka::publishBlockError(const std::string& id, const std::string& error) {
  const json json = {
      {"id", id},
      {"error", error}
  };
  const std::string dump = json.dump();

  try {
    producer.produce(cppkafka::MessageBuilder("block-error").partition(0).payload(dump));
    producer.flush(std::chrono::milliseconds(10000));
  } catch (std::exception& e) {
    LOG(ERROR) << "Error while sending block (" << id << ") error (" << error << ") to kafka: " << e.what();
  }
}

}
