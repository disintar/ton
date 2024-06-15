#include "dumper-kafka.h"
#include "td/utils/logging.h"

DumperKafka::DumperKafka(std::string prefix, std::size_t buffer_size)
    : Dumper(std::move(prefix), buffer_size) {

}

DumperKafka::~DumperKafka() {
  forceDump();
  // Close Kafka producer
}

void DumperKafka::storeBlock(std::string id, std::string block) {

}

void DumperKafka::storeState(std::string id, std::string state) {

}

void DumperKafka::addError(std::string id, std::string type) {

}

void DumperKafka::forceDump() {

}

void DumperKafka::dump() {

}

void DumperKafka::dumpError() {

}

void DumperKafka::dumpLoners() {

}
