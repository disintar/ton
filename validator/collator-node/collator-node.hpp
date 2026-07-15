#pragma once

#include "adnl/adnl.h"
#include "rldp2/rldp.h"
#include "td/actor/actor.h"
#include "ton/ton-types.h"
#include "validator/interfaces/block-handle.h"
#include "validator/validator.h"

namespace ton {
namespace validator {

struct CollatorNodeResponseStats {
  tl_object_ptr<ton_api::validatorStats_collatorNodeResponse> tl() const {
    return create_tl_object<ton_api::validatorStats_collatorNodeResponse>();
  }
};

class CollatorNode : public td::actor::Actor {
 public:
  CollatorNode(adnl::AdnlNodeIdShort, td::Ref<ValidatorManagerOptions>, td::actor::ActorId<ValidatorManager>,
               td::actor::ActorId<adnl::Adnl>, td::actor::ActorId<rldp2::Rldp>) {
  }

  void new_shard_block_accepted(BlockIdExt, CatchainSeqno) {
  }
  void new_masterchain_block_notification(td::Ref<MasterchainState>) {
  }
  void update_shard_client_handle(BlockHandle) {
  }
  void update_options(td::Ref<ValidatorManagerOptions>) {
  }
  void add_shard(ShardIdFull) {
  }
  void del_shard(ShardIdFull) {
  }
};

}  // namespace validator
}  // namespace ton
