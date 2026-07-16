#pragma once

#include "adnl/adnl.h"
#include "rldp2/rldp.h"
#include "td/actor/actor.h"
#include "ton/ton-types.h"
#include "validator/interfaces/block-handle.h"
#include "validator/interfaces/validator-manager.h"

namespace ton {
namespace validator {

class CollationManager : public td::actor::Actor {
 public:
  CollationManager(adnl::AdnlNodeIdShort, td::Ref<ValidatorManagerOptions>, td::actor::ActorId<ValidatorManager>,
                   td::actor::ActorId<adnl::Adnl>, td::actor::ActorId<rldp2::Rldp>) {
  }

  void validator_group_started(ShardIdFull) {
  }
  void validator_group_finished(ShardIdFull) {
  }
  void collate_block(ShardIdFull, BlockIdExt, td::Promise<GeneratedCandidate> promise) {
    promise.set_error(td::Status::Error("collator-node compatibility stub is disabled"));
  }
  void ban_collator(adnl::AdnlNodeIdShort, double) {
  }
  void update_options(td::Ref<ValidatorManagerOptions>) {
  }
  void get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_collationManagerStats_localId>> promise) {
    promise.set_value(create_tl_object<ton_api::engine_validator_collationManagerStats_localId>());
  }
};

}  // namespace validator
}  // namespace ton
