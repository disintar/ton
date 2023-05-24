#include "BlockPublisherKafka.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

BlockPublisherKafka::BlockPublisherKafka(const std::string& endpoint)
    : producer(cppkafka::Configuration{{"metadata.broker.list", endpoint},
                                       {"message.max.bytes", "1000000000"},  // max
                                       {"acks", "1"},
                                       {"debug", "msg,broker,topic"},
                                       {"queue.buffering.max.ms", "500"}}) {
}

void BlockPublisherKafka::publishBlockApplied(unsigned long long shard, std::string json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "[block-applied] Sending " << json.size() << " bytes to Kafka";
  try {
    const char* value = getenv("KAFKA_APPLY_TOPIC");

    if (shard_to_partition.find(shard) == shard_to_partition.end()) {
      max_partition++;
      shard_to_partition[shard] = max_partition;
    }

    producer.produce(cppkafka::MessageBuilder(value ? value : "block-applied-mainnet")
                         .partition(shard_to_partition[shard])
                         .payload(json));
  } catch (std::exception& e) {
    const auto id = to_string(json::parse(json)["id"]);
    LOG(ERROR) << "Error while sending block applied (" << id << ") to kafka: " << e.what();
    publishBlockError(id, e.what());
  }
}

void BlockPublisherKafka::deliver() {
  try {
    producer.flush(std::chrono::milliseconds(100000));
  } catch (std::exception& e) {
    LOG(ERROR) << "Error while deliver to kafka: " << e.what();
  }
}

void BlockPublisherKafka::publishBlockData(unsigned long long shard, std::string json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "[block-data] Sending " << json.size() << " bytes to Kafka";
  try {
    const char* value = getenv("KAFKA_BLOCK_TOPIC");

    if (shard_to_partition.find(shard) == shard_to_partition.end()) {
      max_partition++;
      shard_to_partition[shard] = max_partition;
    }

    producer.produce(cppkafka::MessageBuilder(value ? value : "block-data-mainnet")
                         .partition(shard_to_partition[shard])
                         .payload(json));
  } catch (std::exception& e) {
    const auto id = to_string(json::parse(json)["id"]);
    LOG(ERROR) << "Error while sending block data (" << id << ") to kafka: " << e.what();
    publishBlockError(id, e.what());
  }
}

void BlockPublisherKafka::publishBlockState(unsigned long long shard, std::string json) {
  std::lock_guard<std::mutex> guard(net_mtx);
  LOG(INFO) << "[block-state] Sending " << json.size() << " bytes to Kafka";
  try {
    const char* value = getenv("KAFKA_STATE_TOPIC");

    if (shard_to_partition.find(shard) == shard_to_partition.end()) {
      max_partition++;
      shard_to_partition[shard] = max_partition;
    }

    producer.produce(cppkafka::MessageBuilder(value ? value : "block-state-mainnet")
                         .partition(shard_to_partition[shard])
                         .payload(json));
  } catch (std::exception& e) {
    const auto id = to_string(json::parse(json)["id"]);
    LOG(ERROR) << "Error while sending block state (" << id << ") to kafka: " << e.what();
    publishBlockError(id, e.what());
  }
}

void BlockPublisherKafka::publishBlockError(const std::string& id, const std::string& error) {
  const json json = {{"id", id}, {"error", error}};
  const std::string dump = json.dump();

  try {
    const char* value = getenv("KAFKA_ERROR_TOPIC");
    LOG(WARNING) << "[block-error] Sending " << json.size() << " bytes to Kafka";
    producer.produce(cppkafka::MessageBuilder(value ? value : "block-error-mainnet").partition(0).payload(dump));
    producer.flush(std::chrono::milliseconds(10000));
  } catch (std::exception& e) {
    LOG(ERROR) << "Error while sending block (" << id << ") error (" << error << ") to kafka: " << e.what();
  }
}

}  // namespace ton::validator
