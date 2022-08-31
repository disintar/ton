#include <fstream>
#include "BlockPublisherFS.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

BlockPublisherFS::BlockPublisherFS(const std::size_t batch_size) : batch_size(batch_size) {
  applied_buffer.reserve(batch_size);
  block_data_buffer.reserve(batch_size);
  state_buffer.reserve(batch_size);
}

BlockPublisherFS::~BlockPublisherFS() noexcept {
  std::lock_guard lock(buffer_mtx);
  dump(true);
}

void BlockPublisherFS::publishBlockApplied(std::string j) {
  std::lock_guard lock(buffer_mtx);
  applied_buffer.emplace_back(j);
  dump();
}

void BlockPublisherFS::publishBlockData(std::string j) {
  std::lock_guard lock(buffer_mtx);
  block_data_buffer.emplace_back(j);
  dump();
}

void BlockPublisherFS::publishBlockState(std::string j) {
  std::lock_guard lock(buffer_mtx);
  state_buffer.emplace_back(j);
  dump();
}

void dump_inner(const std::string& prefix, const std::vector<std::string>& buffer) {
  auto to_dump = json::array();

  for (const auto& e : buffer) {
    to_dump.emplace_back(json::parse(e));
  }

  const auto tag =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();

    std::ostringstream oss;
    oss << prefix << "-" << tag << ".json";
    std::ofstream file(oss.str());
    file << to_dump.dump(4);
}

void BlockPublisherFS::dump(const bool force) {
  if ((applied_buffer.size() >= batch_size) || (!applied_buffer.empty() && force)) {
    dump_inner("applied", applied_buffer);
    applied_buffer.clear();
  }
  if ((block_data_buffer.size() >= batch_size) || (!block_data_buffer.empty() && force)) {
    dump_inner("block_data", block_data_buffer);
    block_data_buffer.clear();
  }
  if ((state_buffer.size() >= batch_size) || (!state_buffer.empty() && force)) {
    dump_inner("state", state_buffer);
    state_buffer.clear();
  }
}

}
