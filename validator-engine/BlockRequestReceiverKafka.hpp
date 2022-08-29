#ifndef TON_BLOCKREQUESTRECEIVERKAFKA_HPP
#define TON_BLOCKREQUESTRECEIVERKAFKA_HPP

#include "IBlockRequestReceiver.hpp"
#include <cppkafka/cppkafka.h>


namespace ton::validator {

class BlockRequestReceiverKafka : public IBlockRequestReceiver {
 public:
  explicit BlockRequestReceiverKafka(const std::string& endpoint);

  td::Result<BlockId> getRequest() override;

 private:
  std::mutex net_mtx;
  cppkafka::Consumer consumer;
};

}


#endif //TON_BLOCKREQUESTRECEIVERKAFKA_HPP
