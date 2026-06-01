/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <atomic>
#include <cstdint>

namespace ton {

namespace validator {

namespace fullnode {

inline std::atomic<std::uint64_t> custom_overlay_block_broadcasts_received_total{0};
inline std::atomic<std::uint64_t> custom_overlay_block_broadcasts_applied_total{0};

inline void record_custom_overlay_block_broadcast_received() {
  custom_overlay_block_broadcasts_received_total.fetch_add(1, std::memory_order_relaxed);
}

inline void record_custom_overlay_block_broadcast_applied() {
  custom_overlay_block_broadcasts_applied_total.fetch_add(1, std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_block_broadcasts_received_total() {
  return custom_overlay_block_broadcasts_received_total.load(std::memory_order_relaxed);
}

inline std::uint64_t get_custom_overlay_block_broadcasts_applied_total() {
  return custom_overlay_block_broadcasts_applied_total.load(std::memory_order_relaxed);
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
