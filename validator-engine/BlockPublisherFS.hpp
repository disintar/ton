#ifndef TON_BLOCKPUBLISHERFS_HPP
#define TON_BLOCKPUBLISHERFS_HPP

#include "IBlockParser.hpp"

namespace ton::validator {

class BlockPublisherFS : public IBLockPublisher {
public:
  void publishBlockApplied(std::string json) override;
  void publishBlockData(std::string json) override;
  void publishBlockState(std::string json) override;
};

}


#endif //TON_BLOCKPUBLISHERFS_HPP
