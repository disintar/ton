#ifndef DUMPER_DISK_H
#define DUMPER_DISK_H

#include "dumper-interface.h"
#include <fstream>
#include <sstream>
#include <chrono>

class DumperDisk : public Dumper {
 public:
  explicit DumperDisk(std::string prefix, std::size_t buffer_size);
  ~DumperDisk() override;

  void storeBlock(std::string id, std::string block) override;
  void storeState(std::string id, std::string state) override;
  void addError(std::string id, std::string type) override;
  void forceDump() override;

 protected:
  void dump() override;
  void dumpError() override;
  void dumpLoners() override;
};

#endif  // DUMPER_DISK_H
