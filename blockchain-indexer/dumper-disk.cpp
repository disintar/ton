#include "dumper-disk.h"
#include "td/utils/logging.h"

DumperDisk::DumperDisk(std::string prefix, std::size_t buffer_size)
    : Dumper(std::move(prefix), buffer_size) {}

DumperDisk::~DumperDisk() {
  forceDump();
}

void DumperDisk::storeBlock(std::string id, std::string block) {
  LOG(DEBUG) << "Storing block " << id;
  {
    std::lock_guard<std::mutex> lock(store_mtx);

    auto state = states.find(id);
    if (state == states.end()) {
      blocks.insert({std::move(id), std::move(block)});
    } else {
      std::string tmp_id = id;
      joined_ids.emplace_back(std::move(id));

      std::string together = R"({"id": ")";
      std::string state_str = state->second;

      together += tmp_id;
      together += R"(", "block": )";
      together += block;
      together += R"(, "state": )";
      together += state_str;
      together += "}";

      joined.emplace_back(std::move(together));
      states.erase(state);
    }

    if (joined.size() >= buffer_size) {
      dump();
      dumpLoners();
    }
  }
}

void DumperDisk::storeState(std::string id, std::string state) {
  LOG(DEBUG) << "Storing state " << id;
  {
    std::lock_guard lock(store_mtx);

    auto block = blocks.find(id);
    if (block == blocks.end()) {
      states.insert({std::move(id), std::move(state)});
    } else {
      std::string tmp_id = id;
      joined_ids.emplace_back(std::move(id));

      std::string together = R"({"id": ")";
      std::string block_str = block->second;

      together += tmp_id;
      together += R"(", "block": )";
      together += block_str;
      together += R"(, "state": )";
      together += state;
      together += "}";

      joined.emplace_back(std::move(together));
      blocks.erase(block);
    }

    if (joined.size() >= buffer_size) {
      dump();
    }
  }
}

void DumperDisk::addError(std::string id, std::string type) {
  LOG(ERROR) << "We have error in " << id << " in " << type;
  json data;
  data = {
      {"id", id},
      {"type", type},
  };

  error.emplace_back(std::move(data));
}

void DumperDisk::forceDump() {
  LOG(INFO) << "Force dump of what is left";
  dump();
  dumpLoners();
  dumpError();
  LOG(INFO) << "Finished force dumping";
}

void DumperDisk::dump() {
  if (joined.empty()) {
    return;
  }

  std::lock_guard lock(dump_mtx);

  const auto dumped_amount = joined.size();

  std::string to_dump = "[";
  std::string to_dump_ids = "[";

  if (!joined.empty()) {
    while (!joined.empty()) {
      std::string tmp = joined.back();
      joined.pop_back();
      to_dump += tmp;
      to_dump += ",";
    }
    to_dump.pop_back();

    while (!joined_ids.empty()) {
      std::string tmp = joined_ids.back();
      joined_ids.pop_back();

      to_dump_ids += "\"";
      to_dump_ids += tmp;
      to_dump_ids += "\",";
    }
    to_dump_ids.pop_back();
  }

  to_dump += "]";
  to_dump_ids += "]";

  auto tag =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();

  std::ostringstream oss;
  oss << prefix << tag << ".json";
  std::ofstream file(oss.str());
  file << to_dump;
  file.close();

  std::ostringstream oss_ids;
  oss_ids << prefix << tag << "_ids.json";
  std::ofstream file_ids(oss_ids.str());
  file_ids << to_dump_ids;
  file_ids.close();

  std::ostringstream done_ids;
  done_ids << prefix << tag << "_done.json";
  std::ofstream file_done(done_ids.str());
  file_done << "done ids";
  file_done.close();

  LOG(WARNING) << "Dumped " << dumped_amount << " block/state pairs";
}

void DumperDisk::dumpError() {
  std::lock_guard lock(dump_mtx);

  if (!error.empty()) {
    auto tag =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    auto error_to_dump = json::array();
    for (auto &e : error) {
      error_to_dump.emplace_back(std::move(e));
    }
    error.clear();

    std::ostringstream oss_ids;
    oss_ids << prefix << tag << "_error.json";
    std::ofstream file_ids(oss_ids.str());
    file_ids << error_to_dump.dump(4);

    LOG(INFO) << "Dumped error data";
  }
}

void DumperDisk::dumpLoners() {
  std::lock_guard lock(dump_mtx);

  const auto lone_blocks_amount = blocks.size();
  const auto lone_states_amount = states.size();
  if ((lone_blocks_amount == 0) & (lone_states_amount == 0)) {
    return;
  }

  auto to_dump_ids = json::array();

  auto blocks_to_dump = json::array();
  for (auto &e : blocks) {
    json block_json = {{"id", e.first}, {"block", std::move(e.second)}};
    to_dump_ids.emplace_back(e.first);
    blocks_to_dump.emplace_back(std::move(block_json));
  }
  blocks.clear();

  auto states_to_dump = json::array();
  for (auto &e : states) {
    json state_json = {{"id", e.first}, {"state", std::move(e.second)}};
    to_dump_ids.emplace_back(e.first);
    states_to_dump.emplace_back(std::move(state_json));
  }
  states.clear();

  json to_dump = {{"blocks", std::move(blocks_to_dump)}, {"states", std::move(states_to_dump)}};

  if (lone_blocks_amount != 0 || lone_states_amount != 0) {
    auto tag =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    std::ostringstream oss;
    oss << prefix << "loners_" << tag << ".json";
    std::ofstream file(oss.str());
    file << to_dump.dump(-1);

    std::ostringstream oss_ids;
    oss_ids << prefix << "loners_" << tag << "_ids.json";
    std::ofstream file_ids(oss_ids.str());
    file_ids << to_dump_ids.dump(-1);

    LOG(WARNING) << "Dumped " << lone_blocks_amount << " blocks without pair";
    LOG(WARNING) << "Dumped " << lone_states_amount << " states without pair";
  }
}
