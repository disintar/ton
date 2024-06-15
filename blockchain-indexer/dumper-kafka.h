#ifndef DUMPER_KAFKA_H
#define DUMPER_KAFKA_H

#include "dumper-interface.h"

class DumperKafka : public Dumper {
 public:
  explicit DumperKafka(std::string prefix, std::size_t buffer_size);
  ~DumperKafka() override;

  void storeBlock(std::string id, std::string block) override;
  void storeState(std::string id, std::string state) override;
  void addError(std::string id, std::string type) override;
  void forceDump() override;

 protected:
  void dump() override;
  void dumpError() override;
  void dumpLoners() override;

 private:
  // Kafka producer instance
};

#endif  // DUMPER_KAFKA_H
