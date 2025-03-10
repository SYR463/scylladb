/*
 *
 * Modified by ScyllaDB
 * Copyright (C) 2015-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#pragma once

#include <map>
#include <unordered_set>
#include <unordered_map>
#include "gms/inet_address.hh"
#include "dht/i_partitioner.hh"
#include "inet_address_vectors.hh"
#include "utils/UUID.hh"
#include <optional>
#include <memory>
#include <boost/range/iterator_range.hpp>
#include <boost/icl/interval.hpp>
#include "range.hh"
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/semaphore.hh>

// forward declaration since replica/database.hh includes this file
namespace replica {
class keyspace;
}

namespace locator {

class abstract_replication_strategy;

using inet_address = gms::inet_address;
using token = dht::token;

// Endpoint Data Center and Rack names
struct endpoint_dc_rack {
    sstring dc;
    sstring rack;
};

class topology {
public:
    topology() {}
    topology(const topology& other);

    future<> clear_gently() noexcept;

    /**
     * Stores current DC/rack assignment for ep
     */
    void add_endpoint(const inet_address& ep);

    /**
     * Removes current DC/rack assignment for ep
     */
    void remove_endpoint(inet_address ep);

    /**
     * Re-reads the DC/rack info for the given endpoint
     * @param ep endpoint in question
     */
    void update_endpoint(inet_address ep);

    /**
     * Returns true iff contains given endpoint
     */
    bool has_endpoint(inet_address) const;

    std::unordered_map<sstring,
                       std::unordered_set<inet_address>>&
    get_datacenter_endpoints() {
        return _dc_endpoints;
    }

    const std::unordered_map<sstring,
                           std::unordered_set<inet_address>>&
    get_datacenter_endpoints() const {
        return _dc_endpoints;
    }

    std::unordered_map<sstring,
                       std::unordered_map<sstring,
                                          std::unordered_set<inet_address>>>&
    get_datacenter_racks() {
        return _dc_racks;
    }

    const std::unordered_map<sstring,
                       std::unordered_map<sstring,
                                          std::unordered_set<inet_address>>>&
    get_datacenter_racks() const {
        return _dc_racks;
    }

    const endpoint_dc_rack& get_location(const inet_address& ep) const;
    sstring get_rack() const;
    sstring get_rack(inet_address ep) const;
    sstring get_datacenter() const;
    sstring get_datacenter(inet_address ep) const;

private:
    /** multi-map: DC -> endpoints in that DC */
    std::unordered_map<sstring,
                       std::unordered_set<inet_address>>
        _dc_endpoints;

    /** map: DC -> (multi-map: rack -> endpoints in that rack) */
    std::unordered_map<sstring,
                       std::unordered_map<sstring,
                                          std::unordered_set<inet_address>>>
        _dc_racks;

    /** reverse-lookup map: endpoint -> current known dc/rack assignment */
    std::unordered_map<inet_address, endpoint_dc_rack> _current_locations;
};

class token_metadata_impl;

class token_metadata final {
    std::unique_ptr<token_metadata_impl> _impl;
public:
    using UUID = utils::UUID;
    using inet_address = gms::inet_address;
private:
    class tokens_iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type = token;
        using difference_type = std::ptrdiff_t;
        using pointer = token*;
        using reference = token&;
    public:
        tokens_iterator() = default;
        tokens_iterator(const token& start, const token_metadata_impl* token_metadata);
        bool operator==(const tokens_iterator& it) const;
        const token& operator*() const;
        tokens_iterator& operator++();
    private:
        std::vector<token>::const_iterator _cur_it;
        size_t _remaining = 0;
        const token_metadata_impl* _token_metadata = nullptr;

        friend class token_metadata_impl;
    };
public:
    token_metadata();
    explicit token_metadata(std::unique_ptr<token_metadata_impl> impl);
    token_metadata(token_metadata&&) noexcept; // Can't use "= default;" - hits some static_assert in unique_ptr
    token_metadata& operator=(token_metadata&&) noexcept;
    ~token_metadata();
    const std::vector<token>& sorted_tokens() const;
    // Update token->endpoint mappings for a given \c endpoint.
    // \c tokens are all the tokens that are now owned by \c endpoint.
    //
    // Note: the function is not exception safe!
    // It must be called only on a temporary copy of the token_metadata
    future<> update_normal_tokens(std::unordered_set<token> tokens, inet_address endpoint);
    // Batch update token->endpoint mappings for the given endpoints.
    // The \c endpoint_tokens map contains the set of tokens currently owned by each respective endpoint.
    //
    // Note: the function is not exception safe!
    // It must be called only on a temporary copy of the token_metadata
    future<> update_normal_tokens(const std::unordered_map<inet_address, std::unordered_set<token>>& endpoint_tokens);
    const token& first_token(const token& start) const;
    size_t first_token_index(const token& start) const;
    std::optional<inet_address> get_endpoint(const token& token) const;
    std::vector<token> get_tokens(const inet_address& addr) const;
    const std::unordered_map<token, inet_address>& get_token_to_endpoint() const;
    const std::unordered_set<inet_address>& get_leaving_endpoints() const;
    const std::unordered_map<token, inet_address>& get_bootstrap_tokens() const;
    void update_topology(inet_address ep);
    /**
     * Creates an iterable range of the sorted tokens starting at the token next
     * after the given one.
     *
     * @param start A token that will define the beginning of the range
     *
     * @return The requested range (see the description above)
     */
    boost::iterator_range<tokens_iterator> ring_range(const token& start) const;
    boost::iterator_range<tokens_iterator> ring_range(
        const std::optional<dht::partition_range::bound>& start) const;

    topology& get_topology();
    const topology& get_topology() const;
    void debug_show() const;

    /**
     * Store an end-point to host ID mapping.  Each ID must be unique, and
     * cannot be changed after the fact.
     *
     * @param hostId
     * @param endpoint
     */
    void update_host_id(const UUID& host_id, inet_address endpoint);

    /** Return the unique host ID for an end-point. */
    UUID get_host_id(inet_address endpoint) const;

    /// Return the unique host ID for an end-point or nullopt if not found.
    std::optional<UUID> get_host_id_if_known(inet_address endpoint) const;

    /** Return the end-point for a unique host ID */
    std::optional<inet_address> get_endpoint_for_host_id(UUID host_id) const;

    /** @return a copy of the endpoint-to-id map for read-only operations */
    const std::unordered_map<inet_address, utils::UUID>& get_endpoint_to_host_id_map_for_reading() const;

    void add_bootstrap_token(token t, inet_address endpoint);

    void add_bootstrap_tokens(std::unordered_set<token> tokens, inet_address endpoint);

    void remove_bootstrap_tokens(std::unordered_set<token> tokens);

    void add_leaving_endpoint(inet_address endpoint);
    void del_leaving_endpoint(inet_address endpoint);

    void remove_endpoint(inet_address endpoint);

    bool is_member(inet_address endpoint) const;

    bool is_leaving(inet_address endpoint) const;

    // Is this node being replaced by another node
    bool is_being_replaced(inet_address endpoint) const;

    // Is any node being replaced by another node
    bool is_any_node_being_replaced() const;

    void add_replacing_endpoint(inet_address existing_node, inet_address replacing_node);

    void del_replacing_endpoint(inet_address existing_node);

    /**
     * Create a full copy of token_metadata using asynchronous continuations.
     * The caller must ensure that the cloned object will not change if
     * the function yields.
     */
    future<token_metadata> clone_async() const noexcept;

    /**
     * Create a copy of TokenMetadata with only tokenToEndpointMap. That is, pending ranges,
     * bootstrap tokens and leaving endpoints are not included in the copy.
     * The caller must ensure that the cloned object will not change if
     * the function yields.
     */
    future<token_metadata> clone_only_token_map() const noexcept;
    /**
     * Create a copy of TokenMetadata with tokenToEndpointMap reflecting situation after all
     * current leave operations have finished.
     * The caller must ensure that the cloned object will not change if
     * the function yields.
     *
     * @return a future holding a new token metadata
     */
    future<token_metadata> clone_after_all_left() const noexcept;

    /**
     * Gently clear the token_metadata members.
     * Yield if needed to prevent reactor stalls.
     */
    future<> clear_gently() noexcept;

    /*
     * Number of returned ranges = O(tokens.size())
     */
    dht::token_range_vector get_primary_ranges_for(std::unordered_set<token> tokens) const;

    /*
     * Number of returned ranges = O(1)
     */
    dht::token_range_vector get_primary_ranges_for(token right) const;
    static boost::icl::interval<token>::interval_type range_to_interval(range<dht::token> r);
    static range<dht::token> interval_to_range(boost::icl::interval<token>::interval_type i);

    bool has_pending_ranges(sstring keyspace_name, inet_address endpoint) const;
     /**
     * Calculate pending ranges according to bootsrapping and leaving nodes. Reasoning is:
     *
     * (1) When in doubt, it is better to write too much to a node than too little. That is, if
     * there are multiple nodes moving, calculate the biggest ranges a node could have. Cleaning
     * up unneeded data afterwards is better than missing writes during movement.
     * (2) When a node leaves, ranges for other nodes can only grow (a node might get additional
     * ranges, but it will not lose any of its current ranges as a result of a leave). Therefore
     * we will first remove _all_ leaving tokens for the sake of calculation and then check what
     * ranges would go where if all nodes are to leave. This way we get the biggest possible
     * ranges with regard current leave operations, covering all subsets of possible final range
     * values.
     * (3) When a node bootstraps, ranges of other nodes can only get smaller. Without doing
     * complex calculations to see if multiple bootstraps overlap, we simply base calculations
     * on the same token ring used before (reflecting situation after all leave operations have
     * completed). Bootstrapping nodes will be added and removed one by one to that metadata and
     * checked what their ranges would be. This will give us the biggest possible ranges the
     * node could have. It might be that other bootstraps make our actual final ranges smaller,
     * but it does not matter as we can clean up the data afterwards.
     *
     * NOTE: This is heavy and ineffective operation. This will be done only once when a node
     * changes state in the cluster, so it should be manageable.
     */
    future<> update_pending_ranges(const abstract_replication_strategy& strategy, const sstring& keyspace_name);

    token get_predecessor(token t) const;

    const std::unordered_set<inet_address>& get_all_endpoints() const;

    /* Returns the number of different endpoints that own tokens in the ring.
     * Bootstrapping tokens are not taken into account. */
    size_t count_normal_token_owners() const;

    // returns empty vector if keyspace_name not found.
    inet_address_vector_topology_change pending_endpoints_for(const token& token, const sstring& keyspace_name) const;

    /** @return an endpoint to token multimap representation of tokenToEndpointMap (a copy) */
    std::multimap<inet_address, token> get_endpoint_to_token_map_for_reading() const;
    /**
     * @return a (stable copy, won't be modified) Token to Endpoint map for all the normal and bootstrapping nodes
     *         in the cluster.
     */
    std::map<token, inet_address> get_normal_and_bootstrapping_token_to_endpoint_map() const;

    long get_ring_version() const;
    void invalidate_cached_rings();

    friend class token_metadata_impl;
};

using token_metadata_ptr = lw_shared_ptr<const token_metadata>;
using mutable_token_metadata_ptr = lw_shared_ptr<token_metadata>;
using token_metadata_lock = semaphore_units<>;
using token_metadata_lock_func = noncopyable_function<future<token_metadata_lock>() noexcept>;

template <typename... Args>
mutable_token_metadata_ptr make_token_metadata_ptr(Args... args) {
    return make_lw_shared<token_metadata>(std::forward<Args>(args)...);
}

class shared_token_metadata {
    mutable_token_metadata_ptr _shared;
    token_metadata_lock_func _lock_func;

public:
    // used to construct the shared object as a sharded<> instance
    // lock_func returns semaphore_units<>
    explicit shared_token_metadata(token_metadata_lock_func lock_func)
        : _shared(make_token_metadata_ptr())
        , _lock_func(std::move(lock_func))
    { }

    shared_token_metadata(const shared_token_metadata& x) = delete;
    shared_token_metadata(shared_token_metadata&& x) = default;

    token_metadata_ptr get() const noexcept {
        return _shared;
    }

    void set(mutable_token_metadata_ptr tmptr) noexcept;

    // Token metadata changes are serialized
    // using the schema_tables merge_lock.
    //
    // Must be called on shard 0.
    future<token_metadata_lock> get_lock() noexcept {
        return _lock_func();
    }

    // mutate_token_metadata acquires the shared_token_metadata lock,
    // clones the token_metadata (using clone_async)
    // and calls an asynchronous functor on
    // the cloned copy of the token_metadata to mutate it.
    //
    // If the functor is successful, the mutated clone
    // is set back to to the shared_token_metadata,
    // otherwise, the clone is destroyed.
    future<> mutate_token_metadata(seastar::noncopyable_function<future<> (token_metadata&)> func);
};

}
