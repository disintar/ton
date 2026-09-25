// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once
#include "common/errorcode.h"
#include "validator/validator.h"

namespace ton::validator::fullnode {

// Do not let an invalid fast response win the race. The normal downstream
// proof/state checks remain enabled as well.
inline td::Promise<td::BufferSlice> validated_proof_download(
    td::actor::ActorId<ValidatorManagerInterface> manager, BlockIdExt block, bool link,
    td::Timestamp deadline, td::Promise<td::BufferSlice> promise) {
  return td::PromiseCreator::lambda(
      [manager, block, link, deadline, promise = std::move(promise)](td::Result<td::BufferSlice> result) mutable {
        if (result.is_error()) {
          promise.set_error(result.move_as_error());
          return;
        }
        if (deadline.is_in_past()) {
          promise.set_error(td::Status::Error(ErrorCode::timeout, "proof download deadline expired"));
          return;
        }
        auto bytes = result.move_as_ok();
        auto copy = bytes.clone();
        auto checked = td::PromiseCreator::lambda(
            [bytes = std::move(bytes), deadline, promise = std::move(promise)](td::Result<td::Unit> status) mutable {
              if (status.is_error()) promise.set_error(status.move_as_error());
              else if (deadline.is_in_past()) {
                promise.set_error(td::Status::Error(ErrorCode::timeout, "proof validation deadline expired"));
              } else promise.set_value(std::move(bytes));
            });
        if (link) {
          td::actor::send_closure(manager, &ValidatorManagerInterface::validate_block_proof_link,
                                  block, std::move(copy), std::move(checked));
        } else {
          td::actor::send_closure(manager, &ValidatorManagerInterface::validate_block_proof,
                                  block, std::move(copy), std::move(checked));
        }
      });
}

}  // namespace ton::validator::fullnode
