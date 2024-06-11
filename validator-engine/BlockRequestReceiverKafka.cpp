#include "BlockRequestReceiverKafka.hpp"
#include "../blockchain-indexer/json-utils.hpp"

namespace ton::validator {

BlockRequestReceiverKafka::BlockRequestReceiverKafka(const std::string &endpoint) : consumer(
  cppkafka::Configuration{
    { "metadata.broker.list", endpoint },
    { "enable.auto.commit", false },
//    { "allow.auto.create.topics", true },
    { "group.id", "BlockRequestReceiver" }  // ?
  }
) {
  consumer.subscribe({"block-request"});
}

td::Result<BlockId> BlockRequestReceiverKafka::getRequest() {
  std::lock_guard lock(net_mtx);

  const auto msg = consumer.poll(std::chrono::milliseconds(10000));

  if (!msg) {
    return td::Status::Error("Failed to poll kafka message");
  }

  if (msg.get_error()) {
    if (!msg.is_eof()) {  // Ignore EOF notifications from rdkafka
      LOG(ERROR) << "Received error notification: " << msg.get_error().to_string();
      return td::Status::Error("Received error notification: " + msg.get_error().to_string());
    }
  }

  const std::string payload = msg.get_payload();

  try {
    const auto j = json::parse(payload);
    const auto workchain = j.at("workchain").get<WorkchainId>();
    const auto seqno = j.at("seqno").get<BlockSeqno>();
    const auto shard = j.at("shard").get<ShardId>();

    try {
      consumer.commit(msg); // TODO: or do not | return some sophisticated handle to .commit() in the outer world
    } catch (std::exception& e) {
      return td::Status::Error(std::string("Failed to commit kafka message") + e.what());
    }

    return {BlockId(workchain, shard, seqno)};

  } catch (std::exception& e) {
    return td::Status::Error(std::string("Failed to parse json payload: ") + e.what());
  }
}

}
