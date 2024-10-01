#include "liteserver.hpp"
#include "validator/interfaces/validator-manager.h"
#include "td/actor/actor.h"
#include "td/actor/ActorId.h"
#include "validator-engine/BlockParserAsync.hpp"
#include "auto/tl/lite_api.h"
#include "auto/tl/lite_api.hpp"
#include "ton/lite-tl.hpp"
#include "tl-utils/lite-utils.hpp"

namespace ton {
    std::string blkid_to_text(BlockId id) {
      return "(" + std::to_string(id.workchain) + ":" + std::to_string(id.shard) + ":" + std::to_string(id.seqno) + ")";
    }

    std::string blkid_to_text(BlockIdExt id) {
      return blkid_to_text(id.id) + " root_hash: " + id.root_hash.to_hex() + " file_hash: " + id.file_hash.to_hex();
    }

    namespace validator {
        void LiteQuery::perform_getParsedBlock(BlockId blkid) {
          LOG(INFO) << "Perform getParsedBlock: " << blkid_to_text(blkid);

          timeout_ = td::Timestamp::in(default_timeout_msec);

          auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<ConstBlockHandle> R) {
              if (R.is_error()) {
                td::actor::send_closure(SelfId, &LiteQuery::abort_query, td::Status::Error(
                        "Can't get handle from db: " + R.move_as_error().to_string()));
              } else {
                auto handle = R.move_as_ok();
                auto prev_block = handle->prev();
                td::actor::send_closure_later(SelfId, &LiteQuery::set_handle, handle);

                td::actor::send_closure(SelfId, &LiteQuery::set_continuation,
                                        [SelfId, prev_block = std::move(prev_block)]() -> void {
                                            td::actor::send_closure_later(SelfId, &LiteQuery::continue_getParsedBlock,
                                                                          prev_block);
                                        });

                td::actor::send_closure_later(SelfId, &LiteQuery::request_block_data, handle->id());
                td::actor::send_closure_later(SelfId, &LiteQuery::request_block_state, handle->id());
              }
          });

          ton::AccountIdPrefixFull pfx{blkid.workchain, blkid.shard};
          td::actor::send_closure(manager_, &ValidatorManagerInterface::get_block_by_seqno_from_db, pfx,
                                  blkid.seqno, std::move(P));
        }

        void LiteQuery::set_handle(ConstBlockHandle handle) {
          parse_handle_ = handle;
        }

        void LiteQuery::continue_getParsedBlock(std::vector<BlockIdExt> blkids_prev) {
          // todo: seqno:0
          current_state_ = std::move(state_);
          auto SelfId = actor_id(this);

          if (blkids_prev.size() > 1) {
            prev_accounts_left_shard = blkids_prev[0].id.shard;
            prev_accounts_right_shard = blkids_prev[1].id.shard;

            td::actor::send_closure(SelfId, &LiteQuery::set_continuation,
                                    [SelfId, right_state_id = blkids_prev[1]]() -> void {
                                        td::actor::send_closure_later(SelfId, &LiteQuery::continue_prev_getParsedBlock,
                                                                      right_state_id);
                                    });
          } else {
            prev_accounts_left_shard = blkids_prev[0].id.shard;

            td::actor::send_closure(SelfId, &LiteQuery::set_continuation, [SelfId]() -> void {
                td::actor::send_closure_later(SelfId, &LiteQuery::finish_getParsedBlock, false);
            });
          }

          LOG(INFO) << "Perform getParsedBlock, get left prev state: " << blkid_to_text(blkids_prev[0]);
          td::actor::send_closure_later(actor_id(this), &LiteQuery::request_block_state, blkids_prev[0]);
        }

        void LiteQuery::continue_prev_getParsedBlock(BlockIdExt blkid_prev_right) {
          LOG(INFO) << "Perform getParsedBlock, get right prev state: " << blkid_to_text(blkid_prev_right);
          left_prev_state_ = std::move(state_);

          td::actor::send_closure(actor_id(this), &LiteQuery::set_continuation, [SelfId = actor_id(this)]() -> void {
              td::actor::send_closure_later(SelfId, &LiteQuery::finish_getParsedBlock, true);
          });
        }

        void LiteQuery::finish_getParsedBlock(bool after_merge) {
          LOG(INFO) << "Perform getParsedBlock, run index";
          if (!after_merge) {
            left_prev_state_ = std::move(state_);
          }

          auto P0 = td::PromiseCreator::lambda(
                  [](td::Result<std::tuple<td::vector<json>, td::Bits256, unsigned long long, int>> R) {});
          auto P = td::PromiseCreator::lambda(
                  [SelfId = actor_id(this)](td::Result<std::tuple<td::Bits256, td::string, td::string>> R) mutable {
                      if (R.is_ok()) {
                        auto result = R.move_as_ok();

                        std::string block = std::get<1>(result);
                        std::string state = std::get<2>(result);

                        std::string data =
                                "{\"root_hash\": \"" + std::get<0>(result).to_hex() + "\", \"block\": " + block +
                                ", \"state\": " + state + "}";

                        td::actor::send_closure(SelfId, &LiteQuery::send_getParsedBlock, std::move(data));
                      } else {
                        td::actor::send_closure(SelfId, &LiteQuery::abort_query, R.move_as_error());
                      }
                  });

          td::actor::create_actor<BlockParserAsync>("BlockParserAsync",
                                                    parse_handle_->id(),
                                                    parse_handle_,
                                                    block_,
                                                    current_state_->root_cell(),
                                                    left_prev_state_->root_cell(),
                                                    std::move(P),
                                                    std::move(P0))
                  .release();
        }

        void LiteQuery::send_getParsedBlock(std::string data) {
          auto b = ton::create_serialize_tl_object<ton::lite_api::liteServer_parsedBlockData>(std::move(data));
          finish_query(std::move(b));
        }
    }
}