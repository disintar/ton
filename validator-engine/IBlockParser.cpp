#include "IBlockParser.hpp"
#include "BlockParserAsync.hpp"
#include "blockchain-indexer/json-utils.hpp"

namespace ton::validator {

BlockParser::BlockParser(std::unique_ptr<IBLockPublisher> publisher)
    : publisher_(std::move(publisher))
    , publish_applied_thread_(&BlockParser::publish_applied_worker, this)
    , publish_blocks_thread_(&BlockParser::publish_blocks_worker, this)
    , publish_states_thread_(&BlockParser::publish_states_worker, this) {
}

BlockParser::~BlockParser() {
  running_ = false;
  publish_blocks_cv_.notify_all();
  publish_states_cv_.notify_all();
  publish_blocks_thread_.join();
  publish_states_thread_.join();
}

void BlockParser::storeBlockApplied(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P) {
  std::lock_guard<std::mutex> lock(maps_mtx_);
  LOG(WARNING) << "Store applied: " << id.to_str();
  const std::string key =
      std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard) + ":" + std::to_string(id.id.seqno);
  stored_applied_.insert({key, id});
  handleBlockProgress(id, std::move(P));
}

void BlockParser::storeBlockData(ConstBlockHandle handle, td::Ref<BlockData> block,
                                 td::Promise<std::tuple<td::string, td::string>> P) {
  std::lock_guard<std::mutex> lock(maps_mtx_);
  LOG(WARNING) << "Store block: " << block->block_id().to_str();
  const std::string key = std::to_string(handle->id().id.workchain) + ":" + std::to_string(handle->id().id.shard) +
                          ":" + std::to_string(handle->id().id.seqno);
  auto blocks_vec = stored_blocks_.find(key);
  if (blocks_vec == stored_blocks_.end()) {
    std::vector<std::pair<ConstBlockHandle, td::Ref<BlockData>>> vec;
    vec.emplace_back(std::pair{handle, block});
    stored_blocks_.insert({key, vec});
  } else {
    blocks_vec->second.emplace_back(std::pair{handle, block});
  }

  handleBlockProgress(handle->id(), std::move(P));
  LOG(WARNING) << "Stored block: " << block->block_id().to_str();
}

void BlockParser::storeBlockState(const ConstBlockHandle& handle, td::Ref<vm::Cell> state,
                                  td::Promise<std::tuple<td::string, td::string>> P) {
  std::lock_guard<std::mutex> lock(maps_mtx_);
  LOG(WARNING) << "Store state: " << handle->id().to_str();
  const std::string key = std::to_string(handle->id().id.workchain) + ":" + std::to_string(handle->id().id.shard) +
                          ":" + std::to_string(handle->id().id.seqno);
  auto states_vec = stored_states_.find(key);
  if (states_vec == stored_states_.end()) {
    std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>> vec;
    vec.emplace_back(std::pair{handle, std::move(state)});
    stored_states_.insert({key, vec});
  } else {
    states_vec->second.emplace_back(std::pair{handle, std::move(state)});
  }

  handleBlockProgress(handle->id(), std::move(P));
  LOG(WARNING) << "Stored state: " <<  handle->id().to_str();
}

void BlockParser::storeBlockStateWithPrev(const ConstBlockHandle& handle, td::Ref<vm::Cell> prev_state, td::Ref<vm::Cell> state,
                                          td::Promise<std::tuple<td::string, td::string>> P) {
  std::lock_guard<std::mutex> lock(maps_mtx_);
  LOG(WARNING) << "Store prev state: " << handle->id().to_str();
  const std::string key = std::to_string(handle->id().id.workchain) + ":" + std::to_string(handle->id().id.shard) +
                          ":" + std::to_string(handle->id().id.seqno);
  auto states_vec = stored_states_.find(key);
  if (states_vec == stored_states_.end()) {
    std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>> vec;
    vec.emplace_back(std::pair{handle, std::move(state)});
    stored_states_.insert({key, vec});
  } else {
    states_vec->second.emplace_back(std::pair{handle, std::move(state)});
  }

  auto prev_states_vec = stored_prev_states_.find(key);
  if (prev_states_vec == stored_prev_states_.end()) {
    std::vector<std::pair<ConstBlockHandle, td::Ref<vm::Cell>>> prev_state_vec;
    prev_state_vec.emplace_back(std::pair{handle, std::move(prev_state)});
    stored_prev_states_.insert({key, prev_state_vec});
  } else {
    prev_states_vec->second.emplace_back(std::pair{handle, prev_state});
  }

  handleBlockProgress(handle->id(), std::move(P));
  LOG(WARNING) << "Stored prev state: " << handle->id().to_str();
}

void BlockParser::handleBlockProgress(BlockIdExt id, td::Promise<std::tuple<td::string, td::string>> P) {
  const std::string key =
      std::to_string(id.id.workchain) + ":" + std::to_string(id.id.shard) + ":" + std::to_string(id.id.seqno);

  auto applied_found = stored_applied_.find(key);
  if (applied_found == stored_applied_.end()) {
    P.set_value(std::make_tuple("", ""));
    return;
  }
  const auto applied = applied_found->second;

  auto blocks_vec_found = stored_blocks_.find(key);
  if (blocks_vec_found == stored_blocks_.end()) {
    P.set_value(std::make_tuple("", ""));
    return;
  }
  const auto blocks_vec = blocks_vec_found->second;
  auto block_found_iter =
      std::find_if(blocks_vec.begin(), blocks_vec.end(), [&id](const auto& b) { return b.first->id() == id; });
  if (block_found_iter == blocks_vec.end()) {
    P.set_value(std::make_tuple("", ""));
    return;
  }

  auto states_vec_found = stored_states_.find(key);
  if (states_vec_found == stored_states_.end()) {
    P.set_value(std::make_tuple("", ""));
    return;
  }
  const auto states_vec = states_vec_found->second;
  auto state_found_iter =
      std::find_if(states_vec.begin(), states_vec.end(), [&id](const auto& s) { return s.first->id() == id; });
  if (state_found_iter == states_vec.end()) {
    P.set_value(std::make_tuple("", ""));
    return;
  }

  bool with_prev_state = false;
  td::Ref<vm::Cell> prev_state;

  auto prev_states_vec_found = stored_prev_states_.find(key);
  if (!(prev_states_vec_found == stored_prev_states_.end())) {
    const auto prev_states_vec = prev_states_vec_found->second;

    auto prev_state_found_iter = std::find_if(prev_states_vec.begin(), prev_states_vec.end(),
                                              [&id](const auto& s) { return s.first->id() == id; });
    if (!(prev_state_found_iter == prev_states_vec.end())) {
      with_prev_state = true;
      prev_state = prev_state_found_iter->second;
    }
  }

  td::optional<td::Ref<vm::Cell>> prev_state_opt;

  if (with_prev_state) {
    prev_state_opt = prev_state;
  }

  const auto applied_parsed = parseBlockApplied(id);
  const auto shard = id.id.shard;
  enqueuePublishBlockApplied(shard, applied_parsed);

  ConstBlockHandle handle = block_found_iter->first;
  td::Ref<BlockData> data = block_found_iter->second;
  td::Ref<vm::Cell> state = state_found_iter->second;

  td::actor::create_actor<BlockParserAsync>("BlockParserAsync", id, handle, data, state, prev_state_opt, std::move(P))
      .release();

  stored_applied_.erase(stored_applied_.find(key));
  stored_blocks_.erase(stored_blocks_.find(key));
  stored_states_.erase(stored_states_.find(key));

  if (with_prev_state) {
    stored_prev_states_.erase(stored_prev_states_.find(key));
  }
}

std::string BlockParser::parseBlockApplied(BlockIdExt id) {
  LOG(DEBUG) << "Parse Applied" << id.to_str();

  json to_dump = {{"file_hash", id.file_hash.to_hex()},
                  {"root_hash", id.root_hash.to_hex()},
                  {"id",
                   {
                       {"workchain", id.id.workchain},
                       {"seqno", id.id.seqno},
                       {"shard", id.id.shard},
                   }}};

  std::string dump = to_dump.dump();
  if (post_processor_) {
    dump = post_processor_(dump);
  }

  return dump;
}

void BlockParser::enqueuePublishBlockApplied(unsigned long long shard, const std::string& json) {
  std::unique_lock lock(publish_applied_mtx_);
  publish_applied_queue_.emplace(shard, json);
  lock.unlock();
  publish_applied_cv_.notify_one();
}

void BlockParser::enqueuePublishBlockData(unsigned long long shard, const std::string& json) {
  std::unique_lock lock(publish_blocks_mtx_);
  publish_blocks_queue_.emplace(shard, json);
  lock.unlock();
  publish_blocks_cv_.notify_one();
}

void BlockParser::enqueuePublishBlockState(unsigned long long shard, const std::string& json) {
  std::unique_lock lock(publish_states_mtx_);
  publish_states_queue_.emplace(shard, json);
  lock.unlock();
  publish_states_cv_.notify_one();
}

void BlockParser::publish_applied_worker() {
  bool should_run = running_;
  while (should_run) {
    std::unique_lock lock(publish_applied_mtx_);
    publish_applied_cv_.wait(lock, [this] { return !publish_applied_queue_.empty() || !running_; });
    if (publish_applied_queue_.empty()) {
      continue;
    }

    auto block = std::move(publish_applied_queue_.front());
    publish_applied_queue_.pop();

    should_run = running_ || !publish_applied_queue_.empty();
    lock.unlock();

    publisher_->publishBlockApplied(std::get<0>(block), std::get<1>(block));
  }
}

void BlockParser::publish_blocks_worker() {
  bool should_run = running_;
  while (should_run) {
    std::unique_lock lock(publish_blocks_mtx_);
    publish_blocks_cv_.wait(lock, [this] { return !publish_blocks_queue_.empty() || !running_; });
    if (publish_blocks_queue_.empty()) {
      continue;
    }

    auto block = std::move(publish_blocks_queue_.front());
    publish_blocks_queue_.pop();

    should_run = running_;
    lock.unlock();

    publisher_->publishBlockData(std::get<0>(block), std::get<1>(block));
  }
}

void BlockParser::publish_states_worker() {
  bool should_run = running_;
  while (should_run) {
    std::unique_lock lock(publish_states_mtx_);
    publish_states_cv_.wait(lock, [this] { return !publish_states_queue_.empty() || !running_; });
    if (publish_states_queue_.empty()) {
      continue;
    }
    auto state = std::move(publish_states_queue_.front());
    publish_states_queue_.pop();

    should_run = running_;
    lock.unlock();

    publisher_->publishBlockState(std::get<0>(state), std::get<1>(state));
  }
}

}  // namespace ton::validator
