#ifndef TON_BLOCKPUBLISHERKAFKA_HPP
#define TON_BLOCKPUBLISHERKAFKA_HPP

#include "IBlockParser.hpp"
#include <cppkafka/cppkafka.h>

namespace ton::validator {

class BlockPublisherKafka : public IBLockPublisher {
 public:
  explicit BlockPublisherKafka(const std::string& endpoint);

  void publishBlockApplied(int wc, unsigned long long shard, std::string json) override;
  void publishBlockData(int wc, unsigned long long shard, std::string json) override;
  void publishBlockState(int wc, unsigned long long shard, std::string json) override;
  void publishOutMsgs(int wc, unsigned long long shard, std::string data) override;
  void deliver() override;
  void merge_new_shards(std::map<unsigned long long, int> new_shards) {
    for (const auto& pair : new_shards) {
      LOG(WARNING) << "Shard: " << pair.first << " mapped to: " << pair.second;
      shard_to_partition[pair.first] = pair.second;
    }
  }

 private:
  void publishBlockError(const std::string& id, const std::string& error);

 private:
  std::mutex net_mtx;
  cppkafka::Producer producer;
  std::map<unsigned long long, int> shard_to_partition{};
  int max_partition = 0;
};

}  // namespace ton::validator

#endif  //TON_BLOCKPUBLISHERKAFKA_HPP
