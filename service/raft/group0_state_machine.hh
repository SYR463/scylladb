/*
 * Copyright (C) 2020-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#include "raft/raft.hh"
#include "utils/UUID_gen.hh"
#include "service/raft/raft_state_machine.hh"

namespace service {
class migration_manager;
class storage_proxy;

class migration_manager;

// Raft state machine implementation for managing group 0 changes (e.g. schema changes).
// NOTE: group 0 raft server is always instantiated on shard 0.
class group0_state_machine : public raft_state_machine {
    migration_manager& _mm;
    storage_proxy& _sp;
public:
    group0_state_machine(migration_manager& mm, storage_proxy& sp) : _mm(mm), _sp(sp) {}
    future<> apply(std::vector<raft::command_cref> command) override;
    future<raft::snapshot_id> take_snapshot() override;
    void drop_snapshot(raft::snapshot_id id) override;
    future<> load_snapshot(raft::snapshot_id id) override;
    future<> transfer_snapshot(gms::inet_address from, raft::snapshot_descriptor snp) override;
    future<> abort() override;
};

} // end of namespace service
