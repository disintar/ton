#include <fstream>
#include "BlockPublisherFS.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

void BlockPublisherFS::publishBlockApplied(std::string j) {
  const auto json_id = json::parse(j)["id"];
  const std::string id = to_string(json_id["workchain"]) + ":" + to_string(json_id["shard"]) + ":" + to_string(json_id["seqno"]);
  std::ostringstream oss;
  oss << "applied" << "_" << id << ".json";
  std::ofstream file(oss.str());
  file << j;
}

void BlockPublisherFS::publishBlockData(std::string j) {
  const std::string id = json::parse(j)["id"];
  LOG(ERROR) << id;
  std::ostringstream oss;
  oss << "data" << "_" << id << ".json";
  std::ofstream file(oss.str());
  file << j;
}

void BlockPublisherFS::publishBlockState(std::string j) {
  const std::string id = json::parse(j)["id"];
  LOG(ERROR) << id;
  std::ostringstream oss;
  oss << "state" << "_" << id << ".json";
  std::ofstream file(oss.str());
  file << j;
}

}
