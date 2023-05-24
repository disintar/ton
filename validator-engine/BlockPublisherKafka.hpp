#ifndef TON_BLOCKPUBLISHERKAFKA_HPP
#define TON_BLOCKPUBLISHERKAFKA_HPP

#include "IBlockParser.hpp"
#include <cppkafka/cppkafka.h>

namespace ton::validator {

class BlockPublisherKafka : public IBLockPublisher {
 public:
  explicit BlockPublisherKafka(const std::string& endpoint);

  void publishBlockApplied(unsigned long long shard, std::string json) override;
  void publishBlockData(unsigned long long shard, std::string json) override;
  void publishBlockState(unsigned long long shard, std::string json) override;
  void deliver() override;

 private:
  void publishBlockError(const std::string& id, const std::string& error);

 private:
  std::mutex net_mtx;
  cppkafka::Producer producer;
  std::map<unsigned long long, int> shard_to_partition{};
  int max_partition = -1;
};

}  // namespace ton::validator

#endif  //TON_BLOCKPUBLISHERKAFKA_HPP
