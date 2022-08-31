#ifndef TON_BLOCKPUBLISHERFS_HPP
#define TON_BLOCKPUBLISHERFS_HPP

#include "IBlockParser.hpp"

namespace ton::validator {

class BlockPublisherFS : public IBLockPublisher {
 public:
  explicit BlockPublisherFS(std::size_t batch_size);
  ~BlockPublisherFS() noexcept override;

  void publishBlockApplied(std::string json) override;
  void publishBlockData(std::string json) override;
  void publishBlockState(std::string json) override;

 private:
  void dump(bool force = false);

 private:
  const std::size_t batch_size;
  std::mutex buffer_mtx;
  std::vector<std::string> applied_buffer;
  std::vector<std::string> block_data_buffer;
  std::vector<std::string> state_buffer;
};

}


#endif //TON_BLOCKPUBLISHERFS_HPP
