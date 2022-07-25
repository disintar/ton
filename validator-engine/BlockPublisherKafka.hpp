#ifndef TON_BLOCKPUBLISHERKAFKA_HPP
#define TON_BLOCKPUBLISHERKAFKA_HPP

#include "IBlockPublisher.hpp"
#include <cppkafka/cppkafka.h>


namespace ton::validator {

class BlockPublisherKafka : public BlockPublisherParser {
 public:
  explicit BlockPublisherKafka(const std::string& endpoint);

 private:
  void publishBlockData(const std::string& json) override;
  void publishBlockState(const std::string& json) override;

  void publishBlockError(const std::string& id, const std::string& error);

 private:
  std::mutex net_mtx;
  cppkafka::Producer producer;
};

}

#endif  //TON_BLOCKPUBLISHERKAFKA_HPP
