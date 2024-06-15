#ifndef DUMPER_INTERFACE_H
#define DUMPER_INTERFACE_H

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "json.hpp"
#include "td/utils/check.h"

using json = nlohmann::json;

class Dumper {
 public:
  explicit Dumper(std::string prefix_, std::size_t buffer_size_) : prefix(prefix_), buffer_size(buffer_size_){};
  virtual ~Dumper() {
    UNREACHABLE();
  };

  virtual void storeBlock(std::string id, std::string block) = 0;
  virtual void storeState(std::string id, std::string state) = 0;
  virtual void addError(std::string id, std::string type) = 0;
  virtual void forceDump() = 0;

 protected:
  virtual void dump() = 0;
  virtual void dumpError() = 0;
  virtual void dumpLoners() = 0;

  std::string prefix;
  std::mutex store_mtx;
  std::mutex dump_mtx;
  std::unordered_map<std::string, std::string> blocks;
  std::unordered_map<std::string, std::string> states;
  std::vector<std::string> joined;
  std::vector<std::string> joined_ids;
  std::vector<json> error;
  std::size_t buffer_size;
};

#endif  // DUMPER_INTERFACE_H
