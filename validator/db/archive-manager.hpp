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

    Copyright 2019-2020 Telegram Systems LLP
*/
#pragma once

#include "archive-slice.hpp"

namespace ton {

namespace validator {

class RootDb;

class ArchiveManager : public td::actor::Actor {
 public:
  ArchiveManager(td::actor::ActorId<RootDb> root, std::string db_root, bool read_only = false);

  void add_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void update_handle(BlockHandle handle, td::Promise<td::Unit> promise);
  void add_file(BlockHandle handle, FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void add_key_block_proof(BlockSeqno seqno, UnixTime ts, LogicalTime lt, FileReference ref_id, td::BufferSlice data,
                           td::Promise<td::Unit> promise);
  void add_temp_file_short(FileReference ref_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void get_handle(BlockIdExt block_id, td::Promise<BlockHandle> promise);
  void get_key_block_proof(FileReference ref_id, td::Promise<td::BufferSlice> promise);
  void get_temp_file_short(FileReference ref_id, td::Promise<td::BufferSlice> promise);
  void get_file_short(FileReference ref_id, td::Promise<td::BufferSlice> promise);
  void get_file(ConstBlockHandle handle, FileReference ref_id, td::Promise<td::BufferSlice> promise);

  void add_zero_state(BlockIdExt block_id, td::BufferSlice data, td::Promise<td::Unit> promise);
  void add_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::BufferSlice data,
                            td::Promise<td::Unit> promise);
  void add_persistent_state_gen(BlockIdExt block_id, BlockIdExt masterchain_block_id,
                                std::function<td::Status(td::FileFd&)> write_state,
                                td::Promise<td::Unit> promise);
  void get_zero_state(BlockIdExt block_id, td::Promise<td::BufferSlice> promise);
  void get_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<td::BufferSlice> promise);
  void get_persistent_state_slice(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::int64 offset,
                                  td::int64 max_size, td::Promise<td::BufferSlice> promise);
  void check_persistent_state(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<bool> promise);
  void check_zero_state(BlockIdExt block_id, td::Promise<bool> promise);

  void truncate(BlockSeqno masterchain_seqno, ConstBlockHandle handle, td::Promise<td::Unit> promise);
  //void truncate_continue(BlockSeqno masterchain_seqno, td::Promise<td::Unit> promise);

  void run_gc(UnixTime ts, UnixTime archive_ttl);

  /* from LTDB */
  void get_block_by_unix_time(AccountIdPrefixFull account_id, UnixTime ts, td::Promise<ConstBlockHandle> promise);
  void get_block_by_lt(AccountIdPrefixFull account_id, LogicalTime lt, td::Promise<ConstBlockHandle> promise);
  void get_block_by_seqno(AccountIdPrefixFull account_id, BlockSeqno seqno, td::Promise<ConstBlockHandle> promise);

  void get_archive_id(BlockSeqno masterchain_seqno, td::Promise<td::uint64> promise);
  void get_archive_slice(td::uint64 archive_id, td::uint64 offset, td::uint32 limit,
                         td::Promise<td::BufferSlice> promise);

  void start_up() override;

  void commit_transaction();
  void set_async_mode(bool mode, td::Promise<td::Unit> promise);

  static constexpr td::uint32 archive_size() {
    return 20000;
  }
  static constexpr td::uint32 key_archive_size() {
    return 200000;
  }

 private:
  struct FileDescription {
    struct Desc {
      BlockSeqno seqno;
      UnixTime ts;
      LogicalTime lt;
    };
    FileDescription(PackageId id, bool deleted) : id(id), deleted(deleted) {
    }
    auto file_actor_id() const {
      return file.get();
    }
    bool has_account_prefix(AccountIdPrefixFull account_id) const;
    PackageId id;
    mutable bool deleted;

    std::map<ShardIdFull, Desc> first_blocks;
    mutable td::actor::ActorOwn<ArchiveSlice> file;
  };

  class FileMap {
   public:
    std::map<PackageId, FileDescription>::const_iterator begin() const {
      return files_.cbegin();
    }
    std::map<PackageId, FileDescription>::const_iterator end() const {
      return files_.cend();
    }
    std::map<PackageId, FileDescription>::const_reverse_iterator rbegin() const {
      return files_.crbegin();
    }
    std::map<PackageId, FileDescription>::const_reverse_iterator rend() const {
      return files_.crend();
    }
    std::map<PackageId, FileDescription>::const_iterator find(PackageId x) const {
      return files_.find(x);
    }
    size_t count(const PackageId &x) const {
      return files_.count(x);
    }
    size_t size() const {
      return files_.size();
    }
    std::map<PackageId, FileDescription>::const_iterator lower_bound(const PackageId &x) const {
      return files_.lower_bound(x);
    }
    std::map<PackageId, FileDescription>::const_iterator upper_bound(const PackageId &x) const {
      return files_.upper_bound(x);
    }
    void clear() {
      files_.clear();
      shards_.clear();
    }
    const FileDescription &emplace(const PackageId &id, FileDescription desc) {
      auto it = files_.emplace(id, std::move(desc));
      if (it.second) {
        shard_index_add(it.first->second);
      }
      return it.first->second;
    }
    void erase(std::map<PackageId, FileDescription>::const_iterator it) {
      shard_index_del(it->second);
      files_.erase(it);
    }
    void set_shard_first_block(const FileDescription &desc, ShardIdFull shard, FileDescription::Desc v);
    const FileDescription *get_file_desc_by_seqno(ShardIdFull shard, BlockSeqno seqno) const;
    const FileDescription *get_file_desc_by_lt(ShardIdFull shard, LogicalTime lt) const;
    const FileDescription *get_file_desc_by_unix_time(ShardIdFull shard, UnixTime ts) const;

   private:
    std::map<PackageId, FileDescription> files_;
    struct ShardIndex {
      std::map<BlockSeqno, const FileDescription *> seqno_index_;
      std::map<LogicalTime, const FileDescription *> lt_index_;
      std::map<UnixTime, const FileDescription *> unix_time_index_;
    };
    std::map<ShardIdFull, ShardIndex> shards_;

    void shard_index_add(const FileDescription &desc);
    void shard_index_del(const FileDescription &desc);
  };
  FileMap files_, key_files_, temp_files_;
  BlockSeqno finalized_up_to_{0};
  bool async_mode_ = false;
  bool huge_transaction_started_ = false;
  td::uint32 huge_transaction_size_ = 0;

  FileMap &get_file_map(const PackageId &p) {
    return p.key ? key_files_ : p.temp ? temp_files_ : files_;
  }

  std::map<FileHash, FileReferenceShort> perm_states_;

  void load_package(PackageId seqno);
  void delete_package(PackageId seqno, td::Promise<td::Unit> promise);
  void deleted_package(PackageId seqno, td::Promise<td::Unit> promise);
  void get_handle_cont(BlockIdExt block_id, PackageId id, td::Promise<BlockHandle> promise);
  void get_handle_finish(BlockHandle handle, td::Promise<BlockHandle> promise);
  void get_file_short_cont(FileReference ref_id, PackageId idx, td::Promise<td::BufferSlice> promise);

  const FileDescription *get_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno, UnixTime ts, LogicalTime lt,
                                       bool force);
  const FileDescription *add_file_desc(ShardIdFull shard, PackageId id, BlockSeqno seqno, UnixTime ts, LogicalTime lt);
  void update_desc(FileMap &f, const FileDescription &desc, ShardIdFull shard, BlockSeqno seqno, UnixTime ts,
                   LogicalTime lt);
  const FileDescription *get_file_desc_by_seqno(ShardIdFull shard, BlockSeqno seqno, bool key_block);
  const FileDescription *get_file_desc_by_lt(ShardIdFull shard, LogicalTime lt, bool key_block);
  const FileDescription *get_file_desc_by_unix_time(ShardIdFull shard, UnixTime ts, bool key_block);
  const FileDescription *get_file_desc_by_seqno(AccountIdPrefixFull shard, BlockSeqno seqno, bool key_block);
  const FileDescription *get_file_desc_by_lt(AccountIdPrefixFull shard, LogicalTime lt, bool key_block);
  const FileDescription *get_file_desc_by_unix_time(AccountIdPrefixFull shard, UnixTime ts, bool key_block);
  const FileDescription *get_next_file_desc(const FileDescription *f);
  const FileDescription *get_temp_file_desc_by_idx(PackageId idx);
  PackageId get_max_temp_file_desc_idx();
  PackageId get_prev_temp_file_desc_idx(PackageId id);

  void add_persistent_state_impl(BlockIdExt block_id, BlockIdExt masterchain_block_id, td::Promise<td::Unit> promise,
                                 std::function<void(std::string, td::Promise<std::string>)> create_writer);
  void written_perm_state(FileReferenceShort id);

  void persistent_state_gc(FileHash last);
  void got_gc_masterchain_handle(ConstBlockHandle handle, FileHash hash);

  std::string db_root_;
  bool read_only_;

  std::shared_ptr<td::KeyValue> index_;

  PackageId get_package_id(BlockSeqno seqno) const;
  PackageId get_package_id_force(BlockSeqno masterchain_seqno, ShardIdFull shard, BlockSeqno seqno, UnixTime ts,
                                 LogicalTime lt, bool is_key);
  PackageId get_temp_package_id() const;
  PackageId get_key_package_id(BlockSeqno seqno) const;
  PackageId get_temp_package_id_by_unixtime(UnixTime ts) const;
};

}  // namespace validator

}  // namespace ton
