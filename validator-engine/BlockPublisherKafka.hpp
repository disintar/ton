#ifndef TON_BLOCKPUBLISHERKAFKA_HPP
#define TON_BLOCKPUBLISHERKAFKA_HPP

#include "IBlockParser.hpp"
#include <cppkafka/cppkafka.h>


namespace ton::validator {

class BlockPublisherKafka : public IBLockPublisher {
 public:
  explicit BlockPublisherKafka(const std::string& endpoint);

  void publishBlockApplied(std::string json) override;
  void publishBlockData(std::string json) override;
  void publishBlockState(std::string json) override;

 private:
  void publishBlockError(const std::string& id, const std::string& error);

 private:
  std::mutex net_mtx;
  cppkafka::Producer producer;
};

}

#endif  //TON_BLOCKPUBLISHERKAFKA_HPP
