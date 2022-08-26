#include <fstream>
#include "BlockPublisherFS.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

void BlockPublisherFS::publishBlockApplied(std::string j) {
  std::ostringstream oss;
  oss << "applied" << "_" << to_string(json::parse(j)["id"]) << ".json";
  std::ofstream file(oss.str());
  file << j;
}

void BlockPublisherFS::publishBlockData(std::string j) {
  std::ostringstream oss;
  oss << "data" << "_" << to_string(json::parse(j)["id"]) << ".json";
  std::ofstream file(oss.str());
  file << j;
}

void BlockPublisherFS::publishBlockState(std::string j) {
  std::ostringstream oss;
  oss << "state" << "_" << to_string(json::parse(j)["id"]) << ".json";
  std::ofstream file(oss.str());
  file << j;
}

}