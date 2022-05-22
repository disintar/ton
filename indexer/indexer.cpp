//
// Created by tvorogme on 5/21/22.
//

#include "td/utils/logging.h"
#include "td/actor/actor.h"
#include "td/utils/Time.h"
#include "td/utils/Random.h"
#include "crypto/vm/cp0.h"
#include "validator/validator.h"
#include "validator/manager-disk.h"

namespace ton {

namespace validator {
class Indexer : public td::actor::Actor {
 private:
  std::string db_root_ = "/mnt/ton/";

 public:
  void run() {
    LOG(DEBUG) << "Use db root: " << db_root_;
    std::function<bool(ton::ShardIdFull, ton::CatchainSeqno, ton::validator::ValidatorManagerOptions::ShardCheckMode)>
        check_shard = [](ton::ShardIdFull, ton::CatchainSeqno,
                         ton::validator::ValidatorManagerOptions::ShardCheckMode) { return true; };
    bool allow_blockchain_init = false;
    double sync_blocks_before = 86400;
    double block_ttl = 86400 * 7;
    double state_ttl = 3600;
    double archive_ttl = 86400 * 365;
    double key_proof_ttl = 86400 * 3650;
    double max_mempool_num = 999999;
    bool initial_sync_disabled = true;

    auto opts_ = ton::validator::ValidatorManagerOptions::create(
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()},
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()},
        check_shard, allow_blockchain_init, sync_blocks_before, block_ttl, state_ttl, archive_ttl, key_proof_ttl,
        max_mempool_num, initial_sync_disabled);

    auto shard = ton::ShardIdFull(ton::masterchainId, ton::shardIdAll);
    auto shard_top =
        ton::BlockIdExt{ton::masterchainId, ton::shardIdAll, 0, ton::RootHash::zero(), ton::FileHash::zero()};

    auto id = PublicKeyHash::zero();

    auto validator_manager_ =
        ton::validator::ValidatorManagerDiskFactory::create(id, opts_, shard, shard_top, db_root_);

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ConstBlockHandle> R) {
      LOG(DEBUG) << "Got Answer!";

      R.ensure();
      auto block = R.move_as_ok();

      LOG(DEBUG) << "Block: Seqno: " << block->id().seqno();
    });

    auto to_find = BlockId{ton::masterchainId, 0x8000000000000000, 20750618};
    ton::AccountIdPrefixFull pfx{ton::masterchainId, 0x8000000000000000};
    td::actor::send_closure(validator_manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx, to_find.seqno,
                            std::move(P));
  }
};
}  // namespace validator
}  // namespace ton

int main(int argc, char **argv) {
  SET_VERBOSITY_LEVEL(verbosity_DEBUG);

  LOG(DEBUG) << "Let's rock!";

  CHECK(vm::init_op_cp0());

  td::actor::ActorOwn<ton::validator::Indexer> main;

  //td::actor::send_closure(main, &Indexer::run);

  td::actor::Scheduler scheduler({7});
  scheduler.run_in_context([&] { main = td::actor::create_actor<ton::validator::Indexer>("cool"); });
  scheduler.run_in_context([&] { td::actor::send_closure(main, &ton::validator::Indexer::run); });
  scheduler.run();
  return 0;
}