/*
 * Copyright (C) 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#include "cql3/statements/select_statement.hh"
#include "cql3/expr/expression.hh"
#include "cql3/statements/raw/select_statement.hh"
#include "cql3/query_processor.hh"
#include "cql3/statements/prune_materialized_view_statement.hh"

#include "transport/messages/result_message.hh"
#include "cql3/functions/as_json_function.hh"
#include "cql3/selection/selection.hh"
#include "cql3/util.hh"
#include "cql3/restrictions/statement_restrictions.hh"
#include "cql3/selection/selector_factories.hh"
#include "validation.hh"
#include "exceptions/unrecognized_entity_exception.hh"
#include <optional>
#include <seastar/core/shared_ptr.hh>
#include "query-result-reader.hh"
#include "query_ranges_to_vnodes.hh"
#include "query_result_merger.hh"
#include "service/pager/query_pagers.hh"
#include "service/storage_proxy.hh"
#include <seastar/core/execution_stage.hh>
#include "view_info.hh"
#include "partition_slice_builder.hh"
#include "cql3/untyped_result_set.hh"
#include "db/timeout_clock.hh"
#include "db/consistency_level_validations.hh"
#include "data_dictionary/data_dictionary.hh"
#include "test/lib/select_statement_utils.hh"
#include <boost/algorithm/cxx11/any_of.hpp>
#include "gms/feature_service.hh"
#include "utils/result.hh"
#include "utils/result_combinators.hh"
#include "utils/result_loop.hh"

template<typename T = void>
using coordinator_result = cql3::statements::select_statement::coordinator_result<T>;

bool is_internal_keyspace(std::string_view name);

namespace cql3 {

namespace statements {

template<typename C>
struct result_to_error_message_wrapper {
    C c;

    template<typename T>
    auto operator()(coordinator_result<T>&& arg) {
        if constexpr (std::is_void_v<T>) {
            if (arg) {
                return futurize_invoke(c);
            } else {
                return make_ready_future<typename futurize_t<std::invoke_result_t<C>>::value_type>(
                    ::make_shared<cql_transport::messages::result_message::exception>(std::move(arg).assume_error())
                );
            }
        } else {
            if (arg) {
                return futurize_invoke(c, std::move(arg).value());
            } else {
                return make_ready_future<typename futurize_t<std::invoke_result_t<C, T>>::value_type>(
                    ::make_shared<cql_transport::messages::result_message::exception>(std::move(arg).assume_error())
                );
            }
        }
    }
};

template<typename C>
auto wrap_result_to_error_message(C&& c) {
    return result_to_error_message_wrapper<C>{std::move(c)};
}

static constexpr int DEFAULT_INTERNAL_PAGING_SIZE = select_statement::DEFAULT_COUNT_PAGE_SIZE;
thread_local int internal_paging_size = DEFAULT_INTERNAL_PAGING_SIZE;
thread_local const lw_shared_ptr<const select_statement::parameters> select_statement::_default_parameters = make_lw_shared<select_statement::parameters>();

select_statement::parameters::parameters()
    : _is_distinct{false}
    , _allow_filtering{false}
    , _statement_subtype{statement_subtype::REGULAR}
{ }

select_statement::parameters::parameters(orderings_type orderings,
                                         bool is_distinct,
                                         bool allow_filtering)
    : _orderings{std::move(orderings)}
    , _is_distinct{is_distinct}
    , _allow_filtering{allow_filtering}
    , _statement_subtype{statement_subtype::REGULAR}
{ }

select_statement::parameters::parameters(orderings_type orderings,
                                         bool is_distinct,
                                         bool allow_filtering,
                                         statement_subtype statement_subtype,
                                         bool bypass_cache)
    : _orderings{std::move(orderings)}
    , _is_distinct{is_distinct}
    , _allow_filtering{allow_filtering}
    , _statement_subtype{statement_subtype}
    , _bypass_cache{bypass_cache}
{ }

bool select_statement::parameters::is_distinct() const {
    return _is_distinct;
}

bool select_statement::parameters::is_json() const {
   return _statement_subtype == statement_subtype::JSON;
}

bool select_statement::parameters::allow_filtering() const {
    return _allow_filtering;
}

bool select_statement::parameters::bypass_cache() const {
    return _bypass_cache;
}

bool select_statement::parameters::is_prune_materialized_view() const {
    return _statement_subtype == statement_subtype::PRUNE_MATERIALIZED_VIEW;
}

select_statement::parameters::orderings_type const& select_statement::parameters::orderings() const {
    return _orderings;
}

timeout_config_selector
select_timeout(const restrictions::statement_restrictions& restrictions) {
    if (restrictions.is_key_range()) {
        return &timeout_config::range_read_timeout;
    } else {
        return &timeout_config::read_timeout;
    }
}

select_statement::select_statement(schema_ptr schema,
                                   uint32_t bound_terms,
                                   lw_shared_ptr<const parameters> parameters,
                                   ::shared_ptr<selection::selection> selection,
                                   ::shared_ptr<const restrictions::statement_restrictions> restrictions,
                                   ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
                                   bool is_reversed,
                                   ordering_comparator_type ordering_comparator,
                                   std::optional<expr::expression> limit,
                                   std::optional<expr::expression> per_partition_limit,
                                   cql_stats& stats,
                                   std::unique_ptr<attributes> attrs)
    : cql_statement(select_timeout(*restrictions))
    , _schema(schema)
    , _bound_terms(bound_terms)
    , _parameters(std::move(parameters))
    , _selection(std::move(selection))
    , _restrictions(std::move(restrictions))
    , _restrictions_need_filtering(_restrictions->need_filtering())
    , _group_by_cell_indices(group_by_cell_indices)
    , _is_reversed(is_reversed)
    , _limit(std::move(limit))
    , _per_partition_limit(std::move(per_partition_limit))
    , _ordering_comparator(std::move(ordering_comparator))
    , _stats(stats)
    , _ks_sel(::is_internal_keyspace(schema->ks_name()) ? ks_selector::SYSTEM : ks_selector::NONSYSTEM)
    , _attrs(std::move(attrs))
{
    _opts = _selection->get_query_options();
    _opts.set_if<query::partition_slice::option::bypass_cache>(_parameters->bypass_cache());
    _opts.set_if<query::partition_slice::option::distinct>(_parameters->is_distinct());
    _opts.set_if<query::partition_slice::option::reversed>(_is_reversed);
}

db::timeout_clock::duration select_statement::get_timeout(const service::client_state& state, const query_options& options) const {
    return _attrs->is_timeout_set() ? _attrs->get_timeout(options) : state.get_timeout_config().*get_timeout_config_selector();
}

::shared_ptr<const cql3::metadata> select_statement::get_result_metadata() const {
    // FIXME: COUNT needs special result metadata handling.
    return _selection->get_result_metadata();
}

uint32_t select_statement::get_bound_terms() const {
    return _bound_terms;
}

future<> select_statement::check_access(query_processor& qp, const service::client_state& state) const {
    try {
        const data_dictionary::database db = qp.db();
        auto&& s = db.find_schema(keyspace(), column_family());
        auto& cf_name = s->is_view() ? s->view_info()->base_name() : column_family();
        return state.has_column_family_access(db, keyspace(), cf_name, auth::permission::SELECT);
    } catch (const data_dictionary::no_such_column_family& e) {
        // Will be validated afterwards.
        return make_ready_future<>();
    }
}

void select_statement::validate(query_processor&, const service::client_state& state) const {
    // Nothing to do, all validation has been done by raw_statemet::prepare()
}

bool select_statement::depends_on(std::string_view ks_name, std::optional<std::string_view> cf_name) const {
    return keyspace() == ks_name && (!cf_name || column_family() == *cf_name);
}

const sstring& select_statement::keyspace() const {
    return _schema->ks_name();
}

const sstring& select_statement::column_family() const {
    return _schema->cf_name();
}

query::partition_slice
select_statement::make_partition_slice(const query_options& options) const
{
    query::column_id_vector static_columns;
    query::column_id_vector regular_columns;

    if (_selection->contains_static_columns()) {
        static_columns.reserve(_selection->get_column_count());
    }

    regular_columns.reserve(_selection->get_column_count());

    for (auto&& col : _selection->get_columns()) {
        if (col->is_static()) {
            static_columns.push_back(col->id);
        } else if (col->is_regular()) {
            regular_columns.push_back(col->id);
        }
    }

    if (_parameters->is_distinct()) {
        return query::partition_slice({ query::clustering_range::make_open_ended_both_sides() },
            std::move(static_columns), {}, _opts, nullptr, options.get_cql_serialization_format());
    }

    auto bounds =_restrictions->get_clustering_bounds(options);
    if (bounds.size() > 1) {
        auto comparer = position_in_partition::less_compare(*_schema);
        auto bounds_sorter = [&comparer] (const query::clustering_range& lhs, const query::clustering_range& rhs) {
            return comparer(position_in_partition_view::for_range_start(lhs), position_in_partition_view::for_range_start(rhs));
        };
        std::sort(bounds.begin(), bounds.end(), bounds_sorter);
    }
    if (_is_reversed) {
        std::reverse(bounds.begin(), bounds.end());
        ++_stats.reverse_queries;
    }
    return query::partition_slice(std::move(bounds),
        std::move(static_columns), std::move(regular_columns), _opts, nullptr, options.get_cql_serialization_format(), get_per_partition_limit(options));
}

uint64_t select_statement::do_get_limit(const query_options& options,
                                        const std::optional<expr::expression>& limit,
                                        uint64_t default_limit) const {
    if (!limit.has_value() || _selection->is_aggregate()) {
        return default_limit;
    }

    auto val = expr::evaluate(*limit, options);
    if (val.is_null()) {
        throw exceptions::invalid_request_exception("Invalid null value of limit");
    }
    if (val.is_unset_value()) {
        return default_limit;
    }
    try {
        auto l = val.view().validate_and_deserialize<int32_t>(*int32_type, cql_serialization_format::internal());
        if (l <= 0) {
            throw exceptions::invalid_request_exception("LIMIT must be strictly positive");
        }
        return l;
    } catch (const marshal_exception& e) {
        throw exceptions::invalid_request_exception("Invalid limit value");
    }
}

bool select_statement::needs_post_query_ordering() const {
    // We need post-query ordering only for queries with IN on the partition key and an ORDER BY.
    return _restrictions->key_is_in_relation() && !_parameters->orderings().empty();
}

struct select_statement_executor {
    static auto get() { return &select_statement::do_execute; }
};

static thread_local inheriting_concrete_execution_stage<
        future<shared_ptr<cql_transport::messages::result_message>>,
        const select_statement*,
        query_processor&,
        service::query_state&,
        const query_options&> select_stage{"cql3_select", select_statement_executor::get()};

future<shared_ptr<cql_transport::messages::result_message>>
select_statement::execute(query_processor& qp,
                             service::query_state& state,
                             const query_options& options) const
{
    return execute_without_checking_exception_message(qp, state, options)
            .then(cql_transport::messages::propagate_exception_as_future<shared_ptr<cql_transport::messages::result_message>>);
}

future<shared_ptr<cql_transport::messages::result_message>>
select_statement::execute_without_checking_exception_message(query_processor& qp,
                             service::query_state& state,
                             const query_options& options) const
{
    return select_stage(this, seastar::ref(qp), seastar::ref(state), seastar::cref(options));
}

future<shared_ptr<cql_transport::messages::result_message>>
select_statement::do_execute(query_processor& qp,
                          service::query_state& state,
                          const query_options& options) const
{
    tracing::add_table_name(state.get_trace_state(), keyspace(), column_family());

    auto cl = options.get_consistency();

    validate_for_read(cl);

    uint64_t limit = get_limit(options);
    auto now = gc_clock::now();

    _stats.filtered_reads += _restrictions_need_filtering;

    const source_selector src_sel = state.get_client_state().is_internal()
            ? source_selector::INTERNAL : source_selector::USER;
    ++_stats.query_cnt(src_sel, _ks_sel, cond_selector::NO_CONDITIONS, statement_type::SELECT);

    _stats.select_bypass_caches += _parameters->bypass_cache();
    _stats.select_allow_filtering += _parameters->allow_filtering();
    _stats.select_partition_range_scan += _range_scan;
    _stats.select_partition_range_scan_no_bypass_cache += _range_scan_no_bypass_cache;

    auto slice = make_partition_slice(options);
    auto max_result_size = qp.proxy().get_max_result_size(slice);
    auto command = ::make_lw_shared<query::read_command>(
            _schema->id(),
            _schema->version(),
            std::move(slice),
            max_result_size,
            query::row_limit(limit),
            query::partition_limit(query::max_partitions),
            now,
            tracing::make_trace_info(state.get_trace_state()),
            utils::UUID(),
            query::is_first_page::no,
            options.get_timestamp(state));
    command->allow_limit = db::allow_per_partition_rate_limit::yes;

    int32_t page_size = options.get_page_size();

    _stats.unpaged_select_queries(_ks_sel) += page_size <= 0;

    // An aggregation query will never be paged for the user, but we always page it internally to avoid OOM.
    // If we user provided a page_size we'll use that to page internally (because why not), otherwise we use our default
    // Note that if there are some nodes in the cluster with a version less than 2.0, we can't use paging (CASSANDRA-6707).
    // Also note: all GROUP BY queries are considered aggregation.
    const bool aggregate = _selection->is_aggregate() || has_group_by();
    const bool nonpaged_filtering = _restrictions_need_filtering && page_size <= 0;
    if (aggregate || nonpaged_filtering) {
        page_size = internal_paging_size;
    }

    auto key_ranges = _restrictions->get_partition_key_ranges(options);

    if (db::is_serial_consistency(options.get_consistency())) {
        if (key_ranges.size() != 1 || !query::is_single_partition(key_ranges.front())) {
             throw exceptions::invalid_request_exception(
                     "SERIAL/LOCAL_SERIAL consistency may only be requested for one partition at a time");
        }
        unsigned shard = dht::shard_of(*_schema, key_ranges[0].start()->value().as_decorated_key().token());
        if (this_shard_id() != shard) {
            return make_ready_future<shared_ptr<cql_transport::messages::result_message>>(
                    qp.bounce_to_shard(shard, std::move(const_cast<cql3::query_options&>(options).take_cached_pk_function_calls()))
                );
        }
    }

    if (!aggregate && !_restrictions_need_filtering && (page_size <= 0
            || !service::pager::query_pagers::may_need_paging(*_schema, page_size,
                    *command, key_ranges))) {
        return execute_without_checking_exception_message(qp, command, std::move(key_ranges), state, options, now);
    }

    command->slice.options.set<query::partition_slice::option::allow_short_read>();
    auto timeout_duration = get_timeout(state.get_client_state(), options);
    auto timeout = db::timeout_clock::now() + timeout_duration;
    auto p = service::pager::query_pagers::pager(qp.proxy(), _schema, _selection,
            state, options, command, std::move(key_ranges), _restrictions_need_filtering ? _restrictions : nullptr);

    if (aggregate || nonpaged_filtering) {
        return do_with(
                cql3::selection::result_set_builder(*_selection, now,
                        options.get_cql_serialization_format(), *_group_by_cell_indices), std::move(p),
                [this, page_size, now, timeout](auto& builder, std::unique_ptr<service::pager::query_pager>& p) {
                    return utils::result_do_until([&p] {return p->is_exhausted();},
                            [&p, &builder, page_size, now, timeout] {
                                return p->fetch_page_result(builder, page_size, now, timeout);
                            }
                    ).then(wrap_result_to_error_message([this, &p, &builder] {
                        return builder.with_thread_if_needed([this, &p, &builder] {
                            auto rs = builder.build();
                            if (_restrictions_need_filtering) {
                                _stats.filtered_rows_read_total += p->stats().rows_read_total;
                                _stats.filtered_rows_matched_total += rs->size();
                            }
                            update_stats_rows_read(rs->size());
                            auto msg = ::make_shared<cql_transport::messages::result_message::rows>(result(std::move(rs)));
                            return shared_ptr<cql_transport::messages::result_message>(std::move(msg));
                        });
                    }));
                });
    }

    if (needs_post_query_ordering()) {
        throw exceptions::invalid_request_exception(
                "Cannot page queries with both ORDER BY and a IN restriction on the partition key;"
                        " you must either remove the ORDER BY or the IN and sort client side, or disable paging for this query");
    }

    if (_selection->is_trivial() && !_restrictions_need_filtering && !_per_partition_limit) {
        return p->fetch_page_generator_result(page_size, now, timeout, _stats).then(wrap_result_to_error_message([this, p = std::move(p)] (result_generator&& generator) {
            auto meta = [&] () -> shared_ptr<const cql3::metadata> {
                if (!p->is_exhausted()) {
                    auto meta = make_shared<metadata>(*_selection->get_result_metadata());
                    meta->set_paging_state(p->state());
                    return meta;
                } else {
                    return _selection->get_result_metadata();
                }
            }();

            return shared_ptr<cql_transport::messages::result_message>(
                make_shared<cql_transport::messages::result_message::rows>(result(std::move(generator), std::move(meta)))
            );
        }));
    }

    return p->fetch_page_result(page_size, now, timeout).then(wrap_result_to_error_message(
            [this, p = std::move(p), &options, now](std::unique_ptr<cql3::result_set>&& rs) {
                if (!p->is_exhausted()) {
                    rs->get_metadata().set_paging_state(p->state());
                }

                if (_restrictions_need_filtering) {
                    _stats.filtered_rows_read_total += p->stats().rows_read_total;
                    _stats.filtered_rows_matched_total += rs->size();
                }
                update_stats_rows_read(rs->size());
                auto msg = ::make_shared<cql_transport::messages::result_message::rows>(result(std::move(rs)));
                return make_ready_future<shared_ptr<cql_transport::messages::result_message>>(std::move(msg));
            }));
}

template<typename KeyType>
requires (std::is_same_v<KeyType, partition_key> || std::is_same_v<KeyType, clustering_key_prefix>)
static KeyType
generate_base_key_from_index_pk(const partition_key& index_pk, const std::optional<clustering_key>& index_ck, const schema& base_schema, const schema& view_schema) {
    const auto& base_columns = std::is_same_v<KeyType, partition_key> ? base_schema.partition_key_columns() : base_schema.clustering_key_columns();

    // An empty key in the index paging state translates to an empty base key
    if (index_pk.is_empty() && !index_ck) {
        return KeyType::make_empty();
    }

    std::vector<managed_bytes_view> exploded_base_key;
    exploded_base_key.reserve(base_columns.size());

    for (const column_definition& base_col : base_columns) {
        const column_definition* view_col = view_schema.view_info()->view_column(base_col);
        if (!view_col) {
            throw std::runtime_error(format("Base key column not found in the view: {}", base_col.name_as_text()));
        }
        if (base_col.type->without_reversed() != view_col->type->without_reversed()) {
            throw std::runtime_error(format("Mismatched types for base and view columns {}: {} and {}",
                    base_col.name_as_text(), base_col.type->cql3_type_name(), view_col->type->cql3_type_name()));
        }
        if (view_col->is_partition_key()) {
            exploded_base_key.push_back(index_pk.get_component(view_schema, view_col->id));
        } else {
            if (!view_col->is_clustering_key()) {
                throw std::runtime_error(
                        format("Base primary key column {} is not a primary key column in the index (kind: {})",
                                view_col->name_as_text(), to_sstring(view_col->kind)));
            }
            if (!index_ck) {
                throw std::runtime_error(format("Column {} was expected to be provided "
                        "in the index clustering key, but the whole index clustering key is missing", view_col->name_as_text()));
            }
            exploded_base_key.push_back(index_ck->get_component(view_schema, view_col->id));
        }
    }
    return KeyType::from_range(exploded_base_key);
}

lw_shared_ptr<query::read_command>
indexed_table_select_statement::prepare_command_for_base_query(query_processor& qp, const query_options& options,
        service::query_state& state, gc_clock::time_point now, bool use_paging) const {
    auto slice = make_partition_slice(options);
    if (use_paging) {
        slice.options.set<query::partition_slice::option::allow_short_read>();
        slice.options.set<query::partition_slice::option::send_partition_key>();
        if (_schema->clustering_key_size() > 0) {
            slice.options.set<query::partition_slice::option::send_clustering_key>();
        }
    }
    lw_shared_ptr<query::read_command> cmd = ::make_lw_shared<query::read_command>(
            _schema->id(),
            _schema->version(),
            std::move(slice),
            qp.proxy().get_max_result_size(slice),
            query::row_limit(get_limit(options)),
            query::partition_limit(query::max_partitions),
            now,
            tracing::make_trace_info(state.get_trace_state()),
            utils::UUID(),
            query::is_first_page::no,
            options.get_timestamp(state));
    cmd->allow_limit = db::allow_per_partition_rate_limit::yes;
    return cmd;
}

future<coordinator_result<std::tuple<foreign_ptr<lw_shared_ptr<query::result>>, lw_shared_ptr<query::read_command>>>>
indexed_table_select_statement::do_execute_base_query(
        query_processor& qp,
        dht::partition_range_vector&& partition_ranges,
        service::query_state& state,
        const query_options& options,
        gc_clock::time_point now,
        lw_shared_ptr<const service::pager::paging_state> paging_state) const {
    using value_type = std::tuple<foreign_ptr<lw_shared_ptr<query::result>>, lw_shared_ptr<query::read_command>>;
    auto cmd = prepare_command_for_base_query(qp, options, state, now, bool(paging_state));
    auto timeout = db::timeout_clock::now() + get_timeout(state.get_client_state(), options);
    uint32_t queried_ranges_count = partition_ranges.size();
    query_ranges_to_vnodes_generator ranges_to_vnodes(qp.proxy().get_token_metadata_ptr(), _schema, std::move(partition_ranges));

    struct base_query_state {
        query::result_merger merger;
        query_ranges_to_vnodes_generator ranges_to_vnodes;
        size_t concurrency = 1;
        size_t previous_result_size = 0;
        base_query_state(uint64_t row_limit, query_ranges_to_vnodes_generator&& ranges_to_vnodes_)
                : merger(row_limit, query::max_partitions)
                , ranges_to_vnodes(std::move(ranges_to_vnodes_))
                {}
        base_query_state(base_query_state&&) = default;
        base_query_state(const base_query_state&) = delete;
    };

    const bool is_paged = bool(paging_state);
    base_query_state query_state{cmd->get_row_limit() * queried_ranges_count, std::move(ranges_to_vnodes)};
    return do_with(std::move(query_state), [this, is_paged, &qp, &state, &options, cmd, timeout] (auto&& query_state) {
        auto& merger = query_state.merger;
        auto& ranges_to_vnodes = query_state.ranges_to_vnodes;
        auto& concurrency = query_state.concurrency;
        auto& previous_result_size = query_state.previous_result_size;
        return utils::result_repeat([this, is_paged, &previous_result_size, &ranges_to_vnodes, &merger, &qp, &state, &options, &concurrency, cmd, timeout]() {
            // Starting with 1 range, we check if the result was a short read, and if not,
            // we continue exponentially, asking for 2x more ranges than before
            dht::partition_range_vector prange = ranges_to_vnodes(concurrency);
            auto command = ::make_lw_shared<query::read_command>(*cmd);
            auto old_paging_state = options.get_paging_state();
            if (old_paging_state && concurrency == 1) {
                auto base_pk = generate_base_key_from_index_pk<partition_key>(old_paging_state->get_partition_key(),
                        old_paging_state->get_clustering_key(), *_schema, *_view_schema);
                auto row_ranges = command->slice.default_row_ranges();
                if (old_paging_state->get_clustering_key() && _schema->clustering_key_size() > 0) {
                    auto base_ck = generate_base_key_from_index_pk<clustering_key>(old_paging_state->get_partition_key(),
                            old_paging_state->get_clustering_key(), *_schema, *_view_schema);

                    query::trim_clustering_row_ranges_to(*_schema, row_ranges, base_ck, false);
                    command->slice.set_range(*_schema, base_pk, row_ranges);
                } else {
                    // There is no clustering key in old_paging_state and/or no clustering key in 
                    // _schema, therefore read an entire partition (whole clustering range).
                    //
                    // The only exception to applying no restrictions on clustering key
                    // is a case when we have a secondary index on the first column
                    // of clustering key. In such a case we should not read the
                    // entire clustering range - only a range in which first column
                    // of clustering key has the correct value. 
                    //
                    // This means that we should not set a open_ended_both_sides
                    // clustering range on base_pk, instead intersect it with
                    // _row_ranges (which contains the restrictions neccessary for the
                    // case described above). The result of such intersection is just
                    // _row_ranges, which we explicity set on base_pk.
                    command->slice.set_range(*_schema, base_pk, row_ranges);
                }
            }
            if (previous_result_size < query::result_memory_limiter::maximum_result_size && concurrency < max_base_table_query_concurrency) {
                concurrency *= 2;
            }
            return qp.proxy().query_result(_schema, command, std::move(prange), options.get_consistency(), {timeout, state.get_permit(), state.get_client_state(), state.get_trace_state()})
            .then(utils::result_wrap([is_paged, &previous_result_size, &ranges_to_vnodes, &merger] (service::storage_proxy::coordinator_query_result qr) -> coordinator_result<stop_iteration> {
                auto is_short_read = qr.query_result->is_short_read();
                // Results larger than 1MB should be shipped to the client immediately
                const bool page_limit_reached = is_paged && qr.query_result->buf().size() >= query::result_memory_limiter::maximum_result_size;
                previous_result_size = qr.query_result->buf().size();
                merger(std::move(qr.query_result));
                return stop_iteration(is_short_read || ranges_to_vnodes.empty() || page_limit_reached);
            }));
        }).then(utils::result_wrap([&merger, cmd]() mutable {
            return make_ready_future<coordinator_result<value_type>>(value_type(merger.get(), std::move(cmd)));
        }));
    });
}

future<shared_ptr<cql_transport::messages::result_message>>
indexed_table_select_statement::execute_base_query(
        query_processor& qp,
        dht::partition_range_vector&& partition_ranges,
        service::query_state& state,
        const query_options& options,
        gc_clock::time_point now,
        lw_shared_ptr<const service::pager::paging_state> paging_state) const {
    return do_execute_base_query(qp, std::move(partition_ranges), state, options, now, paging_state).then(wrap_result_to_error_message(
            [this, &state, &options, now, paging_state = std::move(paging_state)] (std::tuple<foreign_ptr<lw_shared_ptr<query::result>>, lw_shared_ptr<query::read_command>> result_and_cmd) {
        auto&& [result, cmd] = result_and_cmd;
        return process_base_query_results(std::move(result), std::move(cmd), state, options, now, std::move(paging_state));
    }));
}

future<coordinator_result<std::tuple<foreign_ptr<lw_shared_ptr<query::result>>, lw_shared_ptr<query::read_command>>>>
indexed_table_select_statement::do_execute_base_query(
        query_processor& qp,
        std::vector<primary_key>&& primary_keys,
        service::query_state& state,
        const query_options& options,
        gc_clock::time_point now,
        lw_shared_ptr<const service::pager::paging_state> paging_state) const {
    using value_type = std::tuple<foreign_ptr<lw_shared_ptr<query::result>>, lw_shared_ptr<query::read_command>>;
    auto cmd = prepare_command_for_base_query(qp, options, state, now, bool(paging_state));
    auto timeout = db::timeout_clock::now() + get_timeout(state.get_client_state(), options);

    struct base_query_state {
        query::result_merger merger;
        std::vector<primary_key> primary_keys;
        std::vector<primary_key>::iterator current_primary_key;
        size_t previous_result_size = 0;
        size_t next_iteration_size = 0;
        base_query_state(uint64_t row_limit, std::vector<primary_key>&& keys)
                : merger(row_limit, query::max_partitions)
                , primary_keys(std::move(keys))
                , current_primary_key(primary_keys.begin())
                {}
        base_query_state(base_query_state&&) = default;
        base_query_state(const base_query_state&) = delete;
    };

    base_query_state query_state{cmd->get_row_limit(), std::move(primary_keys)};
    const bool is_paged = bool(paging_state);
    return do_with(std::move(query_state), [this, is_paged, &qp, &state, &options, cmd, timeout] (auto&& query_state) {
        auto &merger = query_state.merger;
        auto &keys = query_state.primary_keys;
        auto &key_it = query_state.current_primary_key;
        auto &previous_result_size = query_state.previous_result_size;
        auto &next_iteration_size = query_state.next_iteration_size;
        return utils::result_repeat([this, is_paged, &previous_result_size, &next_iteration_size, &keys, &key_it, &merger, &qp, &state, &options, cmd, timeout]() {
            // Starting with 1 key, we check if the result was a short read, and if not,
            // we continue exponentially, asking for 2x more key than before
            auto already_done = std::distance(keys.begin(), key_it);
            // If the previous result already provided 1MB worth of data,
            // stop increasing the number of fetched partitions
            if (previous_result_size < query::result_memory_limiter::maximum_result_size) {
                next_iteration_size = already_done + 1;
            }
            next_iteration_size = std::min<size_t>({next_iteration_size, keys.size() - already_done, max_base_table_query_concurrency});
            auto key_it_end = key_it + next_iteration_size;
            auto command = ::make_lw_shared<query::read_command>(*cmd);

            query::result_merger oneshot_merger(cmd->get_row_limit(), query::max_partitions);
            return utils::result_map_reduce(key_it, key_it_end, [this, &qp, &state, &options, cmd, timeout] (auto& key) {
                auto command = ::make_lw_shared<query::read_command>(*cmd);
                // for each partition, read just one clustering row (TODO: can
                // get all needed rows of one partition at once.)
                command->slice._row_ranges.clear();
                if (key.clustering) {
                    command->slice._row_ranges.push_back(query::clustering_range::make_singular(key.clustering));
                }
                return qp.proxy().query_result(_schema, command, {dht::partition_range::make_singular(key.partition)}, options.get_consistency(), {timeout, state.get_permit(), state.get_client_state(), state.get_trace_state()})
                .then(utils::result_wrap([] (service::storage_proxy::coordinator_query_result qr) -> coordinator_result<foreign_ptr<lw_shared_ptr<query::result>>> {
                    return std::move(qr.query_result);
                }));
            }, std::move(oneshot_merger)).then(utils::result_wrap([is_paged, &previous_result_size, &key_it, key_it_end = std::move(key_it_end), &keys, &merger] (foreign_ptr<lw_shared_ptr<query::result>> result) -> coordinator_result<stop_iteration> {
                auto is_short_read = result->is_short_read();
                // Results larger than 1MB should be shipped to the client immediately
                const bool page_limit_reached = is_paged && result->buf().size() >= query::result_memory_limiter::maximum_result_size;
                previous_result_size = result->buf().size();
                merger(std::move(result));
                key_it = key_it_end;
                return stop_iteration(is_short_read || key_it == keys.end() || page_limit_reached);
            }));
        }).then(utils::result_wrap([&merger, cmd] () mutable {
            return make_ready_future<coordinator_result<value_type>>(value_type(merger.get(), std::move(cmd)));
        }));
    });
}

future<shared_ptr<cql_transport::messages::result_message>>
indexed_table_select_statement::execute_base_query(
        query_processor& qp,
        std::vector<primary_key>&& primary_keys,
        service::query_state& state,
        const query_options& options,
        gc_clock::time_point now,
        lw_shared_ptr<const service::pager::paging_state> paging_state) const {
    return do_execute_base_query(qp, std::move(primary_keys), state, options, now, paging_state).then(wrap_result_to_error_message(
            [this, &state, &options, now, paging_state = std::move(paging_state)] (std::tuple<foreign_ptr<lw_shared_ptr<query::result>>, lw_shared_ptr<query::read_command>> result_and_cmd){
        auto&& [result, cmd] = result_and_cmd;
        return process_base_query_results(std::move(result), std::move(cmd), state, options, now, std::move(paging_state));
    }));
}

future<shared_ptr<cql_transport::messages::result_message>>
select_statement::execute(query_processor& qp,
                          lw_shared_ptr<query::read_command> cmd,
                          dht::partition_range_vector&& partition_ranges,
                          service::query_state& state,
                          const query_options& options,
                          gc_clock::time_point now) const
{
    return execute_without_checking_exception_message(qp, std::move(cmd), std::move(partition_ranges), state, options, now)
            .then(cql_transport::messages::propagate_exception_as_future<shared_ptr<cql_transport::messages::result_message>>);
}

future<shared_ptr<cql_transport::messages::result_message>>
select_statement::execute_without_checking_exception_message(query_processor& qp,
                          lw_shared_ptr<query::read_command> cmd,
                          dht::partition_range_vector&& partition_ranges,
                          service::query_state& state,
                          const query_options& options,
                          gc_clock::time_point now) const
{
    // If this is a query with IN on partition key, ORDER BY clause and LIMIT
    // is specified we need to get "limit" rows from each partition since there
    // is no way to tell which of these rows belong to the query result before
    // doing post-query ordering.
    auto timeout = db::timeout_clock::now() + get_timeout(state.get_client_state(), options);
    if (needs_post_query_ordering() && _limit) {
        return do_with(std::forward<dht::partition_range_vector>(partition_ranges), [this, &qp, &state, &options, cmd, timeout](auto& prs) {
            assert(cmd->partition_limit == query::max_partitions);
            query::result_merger merger(cmd->get_row_limit() * prs.size(), query::max_partitions);
            return utils::result_map_reduce(prs.begin(), prs.end(), [this, &qp, &state, &options, cmd, timeout] (auto& pr) {
                dht::partition_range_vector prange { pr };
                auto command = ::make_lw_shared<query::read_command>(*cmd);
                return qp.proxy().query_result(_schema,
                        command,
                        std::move(prange),
                        options.get_consistency(),
                        {timeout, state.get_permit(), state.get_client_state(), state.get_trace_state()}).then(utils::result_wrap([] (service::storage_proxy::coordinator_query_result qr) {
                    return make_ready_future<coordinator_result<foreign_ptr<lw_shared_ptr<query::result>>>>(std::move(qr.query_result));
                }));
            }, std::move(merger));
        }).then(wrap_result_to_error_message([this, &options, now, cmd] (auto result) {
            return this->process_results(std::move(result), cmd, options, now);
        }));
    } else {
        return qp.proxy().query_result(_schema, cmd, std::move(partition_ranges), options.get_consistency(), {timeout, state.get_permit(), state.get_client_state(), state.get_trace_state()})
            .then(wrap_result_to_error_message([this, &options, now, cmd] (service::storage_proxy::coordinator_query_result qr) {
                return this->process_results(std::move(qr.query_result), cmd, options, now);
            }));
    }
}

future<shared_ptr<cql_transport::messages::result_message>>
indexed_table_select_statement::process_base_query_results(
        foreign_ptr<lw_shared_ptr<query::result>> results,
        lw_shared_ptr<query::read_command> cmd,
        service::query_state& state,
        const query_options& options,
        gc_clock::time_point now,
        lw_shared_ptr<const service::pager::paging_state> paging_state) const
{
    if (paging_state) {
        paging_state = generate_view_paging_state_from_base_query_results(paging_state, results, state, options);
        _selection->get_result_metadata()->maybe_set_paging_state(std::move(paging_state));
    }
    return process_results(std::move(results), std::move(cmd), options, now);
}

future<shared_ptr<cql_transport::messages::result_message>>
select_statement::process_results(foreign_ptr<lw_shared_ptr<query::result>> results,
                                  lw_shared_ptr<query::read_command> cmd,
                                  const query_options& options,
                                  gc_clock::time_point now) const
{
    const bool fast_path = !needs_post_query_ordering() && _selection->is_trivial() && !_restrictions_need_filtering;
    if (fast_path) {
        return make_ready_future<shared_ptr<cql_transport::messages::result_message>>(make_shared<cql_transport::messages::result_message::rows>(result(
            result_generator(_schema, std::move(results), std::move(cmd), _selection, _stats),
            ::make_shared<metadata>(*_selection->get_result_metadata()))
        ));
    }

    cql3::selection::result_set_builder builder(*_selection, now,
            options.get_cql_serialization_format());
    return do_with(std::move(builder), [this, cmd, results = std::move(results), options] (cql3::selection::result_set_builder& builder) mutable {
        return builder.with_thread_if_needed([this, &builder, cmd, results = std::move(results), options] {
            if (_restrictions_need_filtering) {
                results->ensure_counts();
                _stats.filtered_rows_read_total += *results->row_count();
                query::result_view::consume(*results, cmd->slice,
                        cql3::selection::result_set_builder::visitor(builder, *_schema,
                                *_selection, cql3::selection::result_set_builder::restrictions_filter(_restrictions, options, cmd->get_row_limit(), _schema, cmd->slice.partition_row_limit())));
            } else {
                query::result_view::consume(*results, cmd->slice,
                        cql3::selection::result_set_builder::visitor(builder, *_schema,
                                *_selection));
            }
            auto rs = builder.build();

            if (needs_post_query_ordering()) {
                rs->sort(_ordering_comparator);
                if (_is_reversed) {
                    rs->reverse();
                }
                rs->trim(cmd->get_row_limit());
            }
            update_stats_rows_read(rs->size());
            _stats.filtered_rows_matched_total += _restrictions_need_filtering ? rs->size() : 0;
            return shared_ptr<cql_transport::messages::result_message>(::make_shared<cql_transport::messages::result_message::rows>(result(std::move(rs))));
        });
    });
}

const ::shared_ptr<const restrictions::statement_restrictions> select_statement::get_restrictions() const {
    return _restrictions;
}

primary_key_select_statement::primary_key_select_statement(schema_ptr schema, uint32_t bound_terms,
                                                           lw_shared_ptr<const parameters> parameters,
                                                           ::shared_ptr<selection::selection> selection,
                                                           ::shared_ptr<const restrictions::statement_restrictions> restrictions,
                                                           ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
                                                           bool is_reversed,
                                                           ordering_comparator_type ordering_comparator,
                                                           std::optional<expr::expression> limit,
                                                           std::optional<expr::expression> per_partition_limit,
                                                           cql_stats &stats,
                                                           std::unique_ptr<attributes> attrs)
    : select_statement{schema, bound_terms, parameters, selection, restrictions, group_by_cell_indices, is_reversed, ordering_comparator, std::move(limit), std::move(per_partition_limit), stats, std::move(attrs)}
{
    if (_ks_sel == ks_selector::NONSYSTEM) {
        if (_restrictions->need_filtering() ||
                _restrictions->partition_key_restrictions_is_empty() ||
                (has_token(_restrictions->get_partition_key_restrictions()) &&
                 !find(_restrictions->get_partition_key_restrictions(), expr::oper_t::EQ))) {
            _range_scan = true;
            if (!_parameters->bypass_cache())
                _range_scan_no_bypass_cache = true;
        }
    }
}

::shared_ptr<cql3::statements::select_statement>
indexed_table_select_statement::prepare(data_dictionary::database db,
                                        schema_ptr schema,
                                        uint32_t bound_terms,
                                        lw_shared_ptr<const parameters> parameters,
                                        ::shared_ptr<selection::selection> selection,
                                        ::shared_ptr<restrictions::statement_restrictions> restrictions,
                                        ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
                                        bool is_reversed,
                                        ordering_comparator_type ordering_comparator,
                                        std::optional<expr::expression> limit,
                                         std::optional<expr::expression> per_partition_limit,
                                         cql_stats &stats,
                                         std::unique_ptr<attributes> attrs)
{
    auto& sim = db.find_column_family(schema).get_index_manager();
    auto [index_opt, used_index_restrictions] = restrictions->find_idx(sim);
    if (!index_opt) {
        throw std::runtime_error("No index found.");
    }

    const auto& im = index_opt->metadata();
    sstring index_table_name = im.name() + "_index";
    schema_ptr view_schema = db.find_schema(schema->ks_name(), index_table_name);

    if (im.local()) {
        restrictions->prepare_indexed_local(*view_schema);
    } else {
        restrictions->prepare_indexed_global(*view_schema);
    }

    return ::make_shared<cql3::statements::indexed_table_select_statement>(
            schema,
            bound_terms,
            parameters,
            std::move(selection),
            std::move(restrictions),
            std::move(group_by_cell_indices),
            is_reversed,
            std::move(ordering_comparator),
            std::move(limit),
            std::move(per_partition_limit),
            stats,
            *index_opt,
            std::move(used_index_restrictions),
            view_schema,
            std::move(attrs));

}

indexed_table_select_statement::indexed_table_select_statement(schema_ptr schema, uint32_t bound_terms,
                                                           lw_shared_ptr<const parameters> parameters,
                                                           ::shared_ptr<selection::selection> selection,
                                                           ::shared_ptr<const restrictions::statement_restrictions> restrictions,
                                                           ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
                                                           bool is_reversed,
                                                           ordering_comparator_type ordering_comparator,
                                                           std::optional<expr::expression> limit,
                                                           std::optional<expr::expression> per_partition_limit,
                                                           cql_stats &stats,
                                                           const secondary_index::index& index,
                                                           expr::expression used_index_restrictions,
                                                           schema_ptr view_schema,
                                                           std::unique_ptr<attributes> attrs)
    : select_statement{schema, bound_terms, parameters, selection, restrictions, group_by_cell_indices, is_reversed, ordering_comparator, limit, per_partition_limit, stats, std::move(attrs)}
    , _index{index}
    , _used_index_restrictions(std::move(used_index_restrictions))
    , _view_schema(view_schema)
{
    if (_index.metadata().local()) {
        _get_partition_ranges_for_posting_list = [this] (const query_options& options) { return get_partition_ranges_for_local_index_posting_list(options); };
        _get_partition_slice_for_posting_list = [this] (const query_options& options) { return get_partition_slice_for_local_index_posting_list(options); };
    } else {
        _get_partition_ranges_for_posting_list = [this] (const query_options& options) { return get_partition_ranges_for_global_index_posting_list(options); };
        _get_partition_slice_for_posting_list = [this] (const query_options& options) { return get_partition_slice_for_global_index_posting_list(options); };
    }
}

template<typename KeyType>
requires (std::is_same_v<KeyType, partition_key> || std::is_same_v<KeyType, clustering_key_prefix>)
static void append_base_key_to_index_ck(std::vector<managed_bytes_view>& exploded_index_ck, const KeyType& base_key, const column_definition& index_cdef) {
    auto key_view = base_key.view();
    auto begin = key_view.begin();
    if ((std::is_same_v<KeyType, partition_key> && index_cdef.is_partition_key())
            || (std::is_same_v<KeyType, clustering_key_prefix> && index_cdef.is_clustering_key())) {
        auto key_position = std::next(begin, index_cdef.id);
        std::move(begin, key_position, std::back_inserter(exploded_index_ck));
        begin = std::next(key_position);
    }
    std::move(begin, key_view.end(), std::back_inserter(exploded_index_ck));
}

bytes indexed_table_select_statement::compute_idx_token(const partition_key& key) const {
    const column_definition& cdef = *_view_schema->clustering_key_columns().begin();
    clustering_row empty_row(clustering_key_prefix::make_empty());
    bytes_opt computed_value;
    if (!cdef.is_computed()) {
        // FIXME(pgrabowski): this legacy code is here for backward compatibility and should be removed
        // once "computed_columns feature" is supported by every node
        computed_value = legacy_token_column_computation().compute_value(*_schema, key, empty_row);
    } else {
        computed_value = cdef.get_computation().compute_value(*_schema, key, empty_row);
    }
    if (!computed_value) {
        throw std::logic_error(format("No value computed for idx_token column {}", cdef.name()));
    }
    return *computed_value;
}

lw_shared_ptr<const service::pager::paging_state> indexed_table_select_statement::generate_view_paging_state_from_base_query_results(lw_shared_ptr<const service::pager::paging_state> paging_state,
        const foreign_ptr<lw_shared_ptr<query::result>>& results, service::query_state& state, const query_options& options) const {
    const column_definition* cdef = _schema->get_column_definition(to_bytes(_index.target_column()));
    if (!cdef) {
        throw exceptions::invalid_request_exception("Indexed column not found in schema");
    }

    auto result_view = query::result_view(*results);
    if (!results->row_count() || *results->row_count() == 0) {
        return paging_state;
    }

    auto&& last_pos = results->get_or_calculate_last_position();
    auto& last_base_pk = last_pos.partition;
    auto* last_base_ck = last_pos.position.has_key() ? &last_pos.position.key() : nullptr;

    bytes_opt indexed_column_value = expr::value_for(*cdef, _used_index_restrictions, options);

    auto index_pk = [&]() {
        if (_index.metadata().local()) {
            return last_base_pk;
        } else {
            return partition_key::from_single_value(*_view_schema, *indexed_column_value);
        }
    }();

    std::vector<managed_bytes_view> exploded_index_ck;
    exploded_index_ck.reserve(_view_schema->clustering_key_size());

    bytes token_bytes;
    if (_index.metadata().local()) {
        exploded_index_ck.push_back(bytes_view(*indexed_column_value));
    } else {
        token_bytes = compute_idx_token(last_base_pk);
        exploded_index_ck.push_back(bytes_view(token_bytes));
        append_base_key_to_index_ck<partition_key>(exploded_index_ck, last_base_pk, *cdef);
    }
    if (last_base_ck) {
        append_base_key_to_index_ck<clustering_key>(exploded_index_ck, *last_base_ck, *cdef);
    }

    auto index_ck = clustering_key::from_range(std::move(exploded_index_ck));
    if (partition_key::tri_compare(*_view_schema)(paging_state->get_partition_key(), index_pk) == 0
            && (!paging_state->get_clustering_key() || clustering_key::prefix_equal_tri_compare(*_view_schema)(*paging_state->get_clustering_key(), index_ck) == 0)) {
        return paging_state;
    }

    auto paging_state_copy = make_lw_shared<service::pager::paging_state>(service::pager::paging_state(*paging_state));
    paging_state_copy->set_remaining(internal_paging_size);
    paging_state_copy->set_partition_key(std::move(index_pk));
    paging_state_copy->set_clustering_key(std::move(index_ck));
    return std::move(paging_state_copy);
}

future<shared_ptr<cql_transport::messages::result_message>>
indexed_table_select_statement::do_execute(query_processor& qp,
                             service::query_state& state,
                             const query_options& options) const
{
    tracing::add_table_name(state.get_trace_state(), _view_schema->ks_name(), _view_schema->cf_name());
    tracing::add_table_name(state.get_trace_state(), keyspace(), column_family());

    auto cl = options.get_consistency();

    validate_for_read(cl);

    auto now = gc_clock::now();

    ++_stats.secondary_index_reads;

    const source_selector src_sel = state.get_client_state().is_internal()
            ? source_selector::INTERNAL : source_selector::USER;
    ++_stats.query_cnt(src_sel, _ks_sel, cond_selector::NO_CONDITIONS, statement_type::SELECT);

    assert(_restrictions->uses_secondary_indexing());

    _stats.unpaged_select_queries(_ks_sel) += options.get_page_size() <= 0;

    // Secondary index search has two steps: 1. use the index table to find a
    // list of primary keys matching the query. 2. read the rows matching
    // these primary keys from the base table and return the selected columns.
    // In "whole_partitions" case, we can do the above in whole partition
    // granularity. "partition_slices" is similar, but we fetch the same
    // clustering prefix (make_partition_slice()) from a list of partitions.
    // In other cases we need to list, and retrieve, individual rows and
    // not entire partitions. See issue #3405 for more details.
    bool whole_partitions = false;
    bool partition_slices = false;
    if (_schema->clustering_key_size() == 0) {
        // Obviously, if there are no clustering columns, then we can work at
        // the granularity of whole partitions.
        whole_partitions = true;
    } else {
        if (_index.depends_on(*(_schema->clustering_key_columns().begin()))) {
            // Searching on the *first* clustering column means in each of
            // matching partition, we can take the same contiguous clustering
            // slice (clustering prefix).
            partition_slices = true;
        } else {
            // Search on any partition column means that either all rows
            // match or all don't, so we can work with whole partitions.
            for (auto& cdef : _schema->partition_key_columns()) {
                if (_index.depends_on(cdef)) {
                    whole_partitions = true;
                    break;
                }
            }
        }
    }

    // Aggregated and paged filtering needs to aggregate the results from all pages
    // in order to avoid returning partial per-page results (issue #4540).
    // It's a little bit more complicated than regular aggregation, because each paging state
    // needs to be translated between the base table and the underlying view.
    // The routine below keeps fetching pages from the underlying view, which are then
    // used to fetch base rows, which go straight to the result set builder.
    // A local, internal copy of query_options is kept in order to keep updating
    // the paging state between requesting data from replicas.
    const bool aggregate = _selection->is_aggregate() || has_group_by();
    if (aggregate) {
        return do_with(cql3::selection::result_set_builder(*_selection, now, options.get_cql_serialization_format(), *_group_by_cell_indices), std::make_unique<cql3::query_options>(cql3::query_options(options)),
                [this, &options, &qp, &state, now, whole_partitions, partition_slices] (cql3::selection::result_set_builder& builder, std::unique_ptr<cql3::query_options>& internal_options) {
            // page size is set to the internal count page size, regardless of the user-provided value
            internal_options.reset(new cql3::query_options(std::move(internal_options), options.get_paging_state(), internal_paging_size));
            return utils::result_repeat([this, &builder, &options, &internal_options, &qp, &state, now, whole_partitions, partition_slices] () {
                auto consume_results = [this, &builder, &options, &internal_options, &state] (foreign_ptr<lw_shared_ptr<query::result>> results, lw_shared_ptr<query::read_command> cmd, lw_shared_ptr<const service::pager::paging_state> paging_state) -> coordinator_result<stop_iteration> {
                    if (paging_state) {
                        paging_state = generate_view_paging_state_from_base_query_results(paging_state, results, state, options);
                    }
                    internal_options.reset(new cql3::query_options(std::move(internal_options), paging_state ? make_lw_shared<service::pager::paging_state>(*paging_state) : nullptr));
                    if (_restrictions_need_filtering) {
                        _stats.filtered_rows_read_total += *results->row_count();
                        query::result_view::consume(*results, cmd->slice, cql3::selection::result_set_builder::visitor(builder, *_schema, *_selection,
                                cql3::selection::result_set_builder::restrictions_filter(_restrictions, options, cmd->get_row_limit(), _schema, cmd->slice.partition_row_limit())));
                    } else {
                        query::result_view::consume(*results, cmd->slice, cql3::selection::result_set_builder::visitor(builder, *_schema, *_selection));
                    }
                    bool has_more_pages = paging_state && paging_state->get_remaining() > 0;
                    return stop_iteration(!has_more_pages);
                };

                if (whole_partitions || partition_slices) {
                    tracing::trace(state.get_trace_state(), "Consulting index {} for a single slice of keys, aggregation query", _index.metadata().name());
                    return find_index_partition_ranges(qp, state, *internal_options).then(utils::result_wrap_unpack(
                            [this, now, &state, &internal_options, &qp, consume_results = std::move(consume_results)] (dht::partition_range_vector partition_ranges, lw_shared_ptr<const service::pager::paging_state> paging_state) {
                        return do_execute_base_query(qp, std::move(partition_ranges), state, *internal_options, now, paging_state)
                        .then(utils::result_wrap_unpack([paging_state, consume_results = std::move(consume_results)](foreign_ptr<lw_shared_ptr<query::result>> results, lw_shared_ptr<query::read_command> cmd) {
                            return consume_results(std::move(results), std::move(cmd), std::move(paging_state));
                        }));
                    }));
                } else {
                    tracing::trace(state.get_trace_state(), "Consulting index {} for a list of rows containing keys, aggregation query", _index.metadata().name());
                    return find_index_clustering_rows(qp, state, *internal_options).then(utils::result_wrap_unpack(
                            [this, now, &state, &internal_options, &qp, consume_results = std::move(consume_results)] (std::vector<primary_key> primary_keys, lw_shared_ptr<const service::pager::paging_state> paging_state) {
                        return this->do_execute_base_query(qp, std::move(primary_keys), state, *internal_options, now, paging_state)
                        .then(utils::result_wrap_unpack([paging_state, consume_results = std::move(consume_results)](foreign_ptr<lw_shared_ptr<query::result>> results, lw_shared_ptr<query::read_command> cmd) {
                            return consume_results(std::move(results), std::move(cmd), std::move(paging_state));
                        }));
                    }));
                }
            }).then(wrap_result_to_error_message([this, &builder] () {
                auto rs = builder.build();
                update_stats_rows_read(rs->size());
                _stats.filtered_rows_matched_total += _restrictions_need_filtering ? rs->size() : 0;
                auto msg = ::make_shared<cql_transport::messages::result_message::rows>(result(std::move(rs)));
                return make_ready_future<shared_ptr<cql_transport::messages::result_message>>(std::move(msg));
            }));
        });
    }

    if (whole_partitions || partition_slices) {
        tracing::trace(state.get_trace_state(), "Consulting index {} for a single slice of keys", _index.metadata().name());
        // In this case, can use our normal query machinery, which retrieves
        // entire partitions or the same slice for many partitions.
        return find_index_partition_ranges(qp, state, options).then(wrap_result_to_error_message([now, &state, &options, &qp, this] (std::tuple<dht::partition_range_vector, lw_shared_ptr<const service::pager::paging_state>> result) {
            auto&& [partition_ranges, paging_state] = result;
            return this->execute_base_query(qp, std::move(partition_ranges), state, options, now, std::move(paging_state));
        }));
    } else {
        tracing::trace(state.get_trace_state(), "Consulting index {} for a list of rows containing keys", _index.metadata().name());
        // In this case, we need to retrieve a list of rows (not entire
        // partitions) and then retrieve those specific rows.
        return find_index_clustering_rows(qp, state, options).then(wrap_result_to_error_message([now, &state, &options, &qp, this] (std::tuple<std::vector<primary_key>, lw_shared_ptr<const service::pager::paging_state>> result) {
            auto&& [primary_keys, paging_state] = result;
            return this->execute_base_query(qp, std::move(primary_keys), state, options, now, std::move(paging_state));
        }));
    }
}

dht::partition_range_vector indexed_table_select_statement::get_partition_ranges_for_local_index_posting_list(const query_options& options) const {
    return _restrictions->get_partition_key_ranges(options);
}

dht::partition_range_vector indexed_table_select_statement::get_partition_ranges_for_global_index_posting_list(const query_options& options) const {
    dht::partition_range_vector partition_ranges;

    const column_definition* cdef = _schema->get_column_definition(to_bytes(_index.target_column()));
    if (!cdef) {
        throw exceptions::invalid_request_exception("Indexed column not found in schema");
    }

    bytes_opt value = expr::value_for(*cdef, _used_index_restrictions, options);
    if (value) {
        auto pk = partition_key::from_single_value(*_view_schema, *value);
        auto dk = dht::decorate_key(*_view_schema, pk);
        auto range = dht::partition_range::make_singular(dk);
        partition_ranges.emplace_back(range);
    }

    return partition_ranges;
}

query::partition_slice indexed_table_select_statement::get_partition_slice_for_global_index_posting_list(const query_options& options) const {
    partition_slice_builder partition_slice_builder{*_view_schema};

    if (!_restrictions->has_partition_key_unrestricted_components()) {
        bool pk_restrictions_is_single = !has_token(_restrictions->get_partition_key_restrictions());
        // Only EQ restrictions on base partition key can be used in an index view query
        if (pk_restrictions_is_single && _restrictions->partition_key_restrictions_is_all_eq()) {
            partition_slice_builder.with_ranges(
                    _restrictions->get_global_index_clustering_ranges(options, *_view_schema));
        } else if (_restrictions->has_token_restrictions()) {
            // Restrictions like token(p1, p2) < 0 have all partition key components restricted, but require special handling.
            partition_slice_builder.with_ranges(
                    _restrictions->get_global_index_token_clustering_ranges(options, *_view_schema));
        }
    }

    return partition_slice_builder.build();
}

query::partition_slice indexed_table_select_statement::get_partition_slice_for_local_index_posting_list(const query_options& options) const {
    partition_slice_builder partition_slice_builder{*_view_schema};

    partition_slice_builder.with_ranges(
        _restrictions->get_local_index_clustering_ranges(options, *_view_schema));

    return partition_slice_builder.build();
}

// Utility function for reading from the index view (get_index_view()))
// the posting-list for a particular value of the indexed column.
// Remember a secondary index can only be created on a single column.
future<coordinator_result<::shared_ptr<cql_transport::messages::result_message::rows>>>
indexed_table_select_statement::read_posting_list(query_processor& qp,
                  const query_options& options,
                  uint64_t limit,
                  service::query_state& state,
                  gc_clock::time_point now,
                  db::timeout_clock::time_point timeout,
                  bool include_base_clustering_key) const
{
    dht::partition_range_vector partition_ranges = _get_partition_ranges_for_posting_list(options);
    auto partition_slice = _get_partition_slice_for_posting_list(options);

    auto cmd = ::make_lw_shared<query::read_command>(
            _view_schema->id(),
            _view_schema->version(),
            partition_slice,
            qp.proxy().get_max_result_size(partition_slice),
            query::row_limit(limit),
            query::partition_limit(query::max_partitions),
            now,
            tracing::make_trace_info(state.get_trace_state()),
            utils::UUID(),
            query::is_first_page::no,
            options.get_timestamp(state));

    std::vector<const column_definition*> columns;
    for (const column_definition& cdef : _schema->partition_key_columns()) {
        columns.emplace_back(_view_schema->get_column_definition(cdef.name()));
    }
    if (include_base_clustering_key) {
        for (const column_definition& cdef : _schema->clustering_key_columns()) {
            columns.emplace_back(_view_schema->get_column_definition(cdef.name()));
        }
    }
    auto selection = selection::selection::for_columns(_view_schema, columns);

    int32_t page_size = options.get_page_size();
    if (page_size <= 0 || !service::pager::query_pagers::may_need_paging(*_view_schema, page_size, *cmd, partition_ranges)) {
        return qp.proxy().query_result(_view_schema, cmd, std::move(partition_ranges), options.get_consistency(), {timeout, state.get_permit(), state.get_client_state(), state.get_trace_state()})
        .then(utils::result_wrap([this, now, &options, selection = std::move(selection), partition_slice = std::move(partition_slice)] (service::storage_proxy::coordinator_query_result qr)
                -> coordinator_result<::shared_ptr<cql_transport::messages::result_message::rows>> {
            cql3::selection::result_set_builder builder(*selection, now, options.get_cql_serialization_format());
            query::result_view::consume(*qr.query_result,
                                        std::move(partition_slice),
                                        cql3::selection::result_set_builder::visitor(builder, *_view_schema, *selection));
            return ::make_shared<cql_transport::messages::result_message::rows>(result(builder.build()));
        }));
    }

    auto p = service::pager::query_pagers::pager(qp.proxy(), _view_schema, selection,
            state, options, cmd, std::move(partition_ranges), nullptr);
    return p->fetch_page_result(options.get_page_size(), now, timeout).then(utils::result_wrap([p = std::move(p), &options, limit, now] (std::unique_ptr<cql3::result_set> rs)
            -> coordinator_result<::shared_ptr<cql_transport::messages::result_message::rows>> {
        rs->get_metadata().set_paging_state(p->state());
        return ::make_shared<cql_transport::messages::result_message::rows>(result(std::move(rs)));
    }));
}

// Note: the partitions keys returned by this function are sorted
// in token order. See issue #3423.
future<coordinator_result<std::tuple<dht::partition_range_vector, lw_shared_ptr<const service::pager::paging_state>>>>
indexed_table_select_statement::find_index_partition_ranges(query_processor& qp,
                                             service::query_state& state,
                                             const query_options& options) const
{
    using value_type = std::tuple<dht::partition_range_vector, lw_shared_ptr<const service::pager::paging_state>>;
    auto now = gc_clock::now();
    auto timeout = db::timeout_clock::now() + get_timeout(state.get_client_state(), options);
    return read_posting_list(qp, options, get_limit(options), state, now, timeout, false).then(utils::result_wrap(
            [this, now, &options] (::shared_ptr<cql_transport::messages::result_message::rows> rows) {
        auto rs = cql3::untyped_result_set(rows);
        dht::partition_range_vector partition_ranges;
        partition_ranges.reserve(rs.size());
        // We are reading the list of primary keys as rows of a single
        // partition (in the index view), so they are sorted in
        // lexicographical order (N.B. this is NOT token order!). We need
        // to avoid outputting the same partition key twice, but luckily in
        // the sorted order, these will be adjacent.
        std::optional<dht::decorated_key> last_dk;
        for (size_t i = 0; i < rs.size(); i++) {
            const auto& row = rs.at(i);
            std::vector<bytes> pk_columns;
            for (const auto& column : row.get_columns()) {
                pk_columns.push_back(row.get_blob(column->name->to_string()));
            }
            auto pk = partition_key::from_exploded(*_schema, pk_columns);
            auto dk = dht::decorate_key(*_schema, pk);
            if (last_dk && last_dk->equal(*_schema, dk)) {
                // Another row of the same partition, no need to output the
                // same partition key again.
                continue;
            }
            last_dk = dk;
            auto range = dht::partition_range::make_singular(dk);
            partition_ranges.emplace_back(range);
        }
        auto paging_state = rows->rs().get_metadata().paging_state();
        return make_ready_future<coordinator_result<value_type>>(value_type(std::move(partition_ranges), std::move(paging_state)));
    }));
}

// Note: the partitions keys returned by this function are sorted
// in token order. See issue #3423.
future<coordinator_result<std::tuple<std::vector<indexed_table_select_statement::primary_key>, lw_shared_ptr<const service::pager::paging_state>>>>
indexed_table_select_statement::find_index_clustering_rows(query_processor& qp, service::query_state& state, const query_options& options) const
{
    using value_type = std::tuple<std::vector<indexed_table_select_statement::primary_key>, lw_shared_ptr<const service::pager::paging_state>>;
    auto now = gc_clock::now();
    auto timeout = db::timeout_clock::now() + get_timeout(state.get_client_state(), options);
    return read_posting_list(qp, options, get_limit(options), state, now, timeout, true).then(utils::result_wrap(
            [this, now, &options] (::shared_ptr<cql_transport::messages::result_message::rows> rows) {

        auto rs = cql3::untyped_result_set(rows);
        std::vector<primary_key> primary_keys;
        primary_keys.reserve(rs.size());
        for (size_t i = 0; i < rs.size(); i++) {
            const auto& row = rs.at(i);
            auto pk_columns = _schema->partition_key_columns() | boost::adaptors::transformed([&] (auto& cdef) {
                return row.get_blob(cdef.name_as_text());
            });
            auto pk = partition_key::from_range(pk_columns);
            auto dk = dht::decorate_key(*_schema, pk);
            auto ck_columns = _schema->clustering_key_columns() | boost::adaptors::transformed([&] (auto& cdef) {
                return row.get_blob(cdef.name_as_text());
            });
            auto ck = clustering_key::from_range(ck_columns);
            primary_keys.emplace_back(primary_key{std::move(dk), std::move(ck)});
        }
        auto paging_state = rows->rs().get_metadata().paging_state();
        return make_ready_future<coordinator_result<value_type>>(value_type(std::move(primary_keys), std::move(paging_state)));
    }));
}

class parallelized_select_statement : public select_statement {
public:
    static ::shared_ptr<cql3::statements::select_statement> prepare(
        schema_ptr schema,
        uint32_t bound_terms,
        lw_shared_ptr<const parameters> parameters,
        ::shared_ptr<selection::selection> selection,
        ::shared_ptr<restrictions::statement_restrictions> restrictions,
        ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
        bool is_reversed,
        ordering_comparator_type ordering_comparator,
        std::optional<expr::expression> limit,
        std::optional<expr::expression> per_partition_limit,
        cql_stats& stats,
        std::unique_ptr<cql3::attributes> attrs
    );

    parallelized_select_statement(
        schema_ptr schema,
        uint32_t bound_terms,
        lw_shared_ptr<const parameters> parameters,
        ::shared_ptr<selection::selection> selection,
        ::shared_ptr<const restrictions::statement_restrictions> restrictions,
        ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
        bool is_reversed,
        ordering_comparator_type ordering_comparator,
        std::optional<expr::expression> limit,
        std::optional<expr::expression> per_partition_limit,
        cql_stats& stats,
        std::unique_ptr<cql3::attributes> attrs
    );

private:
    virtual future<::shared_ptr<cql_transport::messages::result_message>> do_execute(
        query_processor& qp,
        service::query_state& state,
        const query_options& options
    ) const override;
};

::shared_ptr<cql3::statements::select_statement> parallelized_select_statement::prepare(
    schema_ptr schema,
    uint32_t bound_terms,
    lw_shared_ptr<const select_statement::parameters> parameters,
    ::shared_ptr<selection::selection> selection,
    ::shared_ptr<restrictions::statement_restrictions> restrictions,
    ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
    bool is_reversed,
    parallelized_select_statement::ordering_comparator_type ordering_comparator,
    std::optional<expr::expression> limit,
    std::optional<expr::expression> per_partition_limit,
    cql_stats& stats,
    std::unique_ptr<cql3::attributes> attrs
) {
    return ::make_shared<cql3::statements::parallelized_select_statement>(
        schema,
        bound_terms,
        std::move(parameters),
        std::move(selection),
        std::move(restrictions),
        std::move(group_by_cell_indices),
        is_reversed,
        std::move(ordering_comparator),
        std::move(limit),
        std::move(per_partition_limit),
        stats,
        std::move(attrs)
    );
}

parallelized_select_statement::parallelized_select_statement(
    schema_ptr schema,
    uint32_t bound_terms,
    lw_shared_ptr<const parallelized_select_statement::parameters> parameters,
    ::shared_ptr<selection::selection> selection,
    ::shared_ptr<const restrictions::statement_restrictions> restrictions,
    ::shared_ptr<std::vector<size_t>> group_by_cell_indices,
    bool is_reversed,
    parallelized_select_statement::ordering_comparator_type ordering_comparator,
    std::optional<expr::expression> limit,
    std::optional<expr::expression> per_partition_limit,
    cql_stats& stats,
    std::unique_ptr<cql3::attributes> attrs
) : select_statement(
    schema,
    bound_terms,
    std::move(parameters),
    std::move(selection),
    std::move(restrictions),
    std::move(group_by_cell_indices),
    is_reversed,
    std::move(ordering_comparator),
    std::move(limit),
    std::move(per_partition_limit),
    stats,
    std::move(attrs)
) {
}

future<::shared_ptr<cql_transport::messages::result_message>>
parallelized_select_statement::do_execute(
    query_processor& qp,
    service::query_state& state,
    const query_options& options
) const {
    tracing::add_table_name(state.get_trace_state(), keyspace(), column_family());

    auto cl = options.get_consistency();
    validate_for_read(cl);

    auto now = gc_clock::now();

    const source_selector src_sel = state.get_client_state().is_internal()
            ? source_selector::INTERNAL : source_selector::USER;
    ++_stats.query_cnt(src_sel, _ks_sel, cond_selector::NO_CONDITIONS, statement_type::SELECT);

    _stats.select_bypass_caches += _parameters->bypass_cache();
    _stats.select_allow_filtering += _parameters->allow_filtering();
    _stats.select_partition_range_scan += _range_scan;
    _stats.select_partition_range_scan_no_bypass_cache += _range_scan_no_bypass_cache;
    _stats.select_parallelized += 1;

    auto slice = make_partition_slice(options);
    auto command = ::make_lw_shared<query::read_command>(
        _schema->id(),
        _schema->version(),
        std::move(slice),
        qp.proxy().get_max_result_size(slice),
        query::row_limit(query::max_rows),
        query::partition_limit(query::max_partitions),
        now,
        tracing::make_trace_info(state.get_trace_state()),
        utils::UUID(),
        query::is_first_page::no,
        options.get_timestamp(state)
    );
    auto key_ranges = _restrictions->get_partition_key_ranges(options);

    if (db::is_serial_consistency(options.get_consistency())) {
        throw exceptions::invalid_request_exception(
            "SERIAL/LOCAL_SERIAL consistency may only be requested for one partition at a time"
        );
    }

    command->slice.options.set<query::partition_slice::option::allow_short_read>();
    auto timeout_duration = get_timeout(state.get_client_state(), options);
    auto timeout = db::timeout_clock::now() + timeout_duration;
    auto reductions = _selection->get_reductions();

    query::forward_request req = {
        .reduction_types = reductions.types,
        .cmd = *command,
        .pr = std::move(key_ranges),
        .cl = options.get_consistency(),
        .timeout = timeout,
        .aggregation_infos = reductions.infos,
    };

    // dispatch execution of this statement to other nodes
    return qp.forwarder().dispatch(req, state.get_trace_state()).then([this] (query::forward_result res) {
        auto meta = make_shared<metadata>(*_selection->get_result_metadata());
        auto rs = std::make_unique<result_set>(std::move(meta));
        rs->add_row(res.query_results);
        update_stats_rows_read(rs->size());
        return shared_ptr<cql_transport::messages::result_message>(
            make_shared<cql_transport::messages::result_message::rows>(result(std::move(rs)))
        );
    });
}

namespace raw {

static void validate_attrs(const cql3::attributes::raw& attrs) {
    if (attrs.timestamp) {
        throw exceptions::invalid_request_exception("Specifying TIMESTAMP is not legal for SELECT statement");
    }
    if (attrs.time_to_live) {
        throw exceptions::invalid_request_exception("Specifying TTL is not legal for SELECT statement");
    }
}

select_statement::select_statement(cf_name cf_name,
                                   lw_shared_ptr<const parameters> parameters,
                                   std::vector<::shared_ptr<selection::raw_selector>> select_clause,
                                   expr::expression where_clause,
                                   std::optional<expr::expression> limit,
                                   std::optional<expr::expression> per_partition_limit,
                                   std::vector<::shared_ptr<cql3::column_identifier::raw>> group_by_columns,
                                   std::unique_ptr<attributes::raw> attrs)
    : cf_statement(cf_name)
    , _parameters(std::move(parameters))
    , _select_clause(std::move(select_clause))
    , _where_clause(std::move(where_clause))
    , _limit(std::move(limit))
    , _per_partition_limit(std::move(per_partition_limit))
    , _group_by_columns(std::move(group_by_columns))
    , _attrs(std::move(attrs))
{
    validate_attrs(*_attrs);
}

void select_statement::maybe_jsonize_select_clause(data_dictionary::database db, schema_ptr schema) {
    // Fill wildcard clause with explicit column identifiers for as_json function
    if (_parameters->is_json()) {
        if (_select_clause.empty()) {
            _select_clause.reserve(schema->all_columns().size());
            for (const column_definition& column_def : schema->all_columns_in_select_order()) {
                _select_clause.push_back(make_shared<selection::raw_selector>(
                    expr::unresolved_identifier{::make_shared<column_identifier::raw>(column_def.name_as_text(), true)},
                    nullptr));
            }
        }

        // Prepare selector names + types for as_json function
        std::vector<sstring> selector_names;
        std::vector<data_type> selector_types;
        std::vector<const column_definition*> defs;
        selector_names.reserve(_select_clause.size());
        auto selectables = selection::raw_selector::to_selectables(_select_clause, *schema);
        selection::selector_factories factories(selection::raw_selector::to_selectables(_select_clause, *schema), db, schema, defs);
        auto selectors = factories.new_instances();
        for (size_t i = 0; i < selectors.size(); ++i) {
            selector_names.push_back(selectables[i]->to_string());
            selector_types.push_back(selectors[i]->get_type());
        }

        // Prepare args for as_json_function
        std::vector<expr::expression> raw_selectables;
        raw_selectables.reserve(_select_clause.size());
        for (const auto& raw_selector : _select_clause) {
            raw_selectables.push_back(raw_selector->selectable_);
        }
        auto as_json = ::make_shared<functions::as_json_function>(std::move(selector_names), std::move(selector_types));
        auto as_json_selector = ::make_shared<selection::raw_selector>(
                expr::function_call{as_json, std::move(raw_selectables)}, nullptr);
        _select_clause.clear();
        _select_clause.push_back(as_json_selector);
    }
}

std::unique_ptr<prepared_statement> select_statement::prepare(data_dictionary::database db, cql_stats& stats, bool for_view) {
    schema_ptr schema = validation::validate_column_family(db, keyspace(), column_family());
    prepare_context& ctx = get_prepare_context();

    maybe_jsonize_select_clause(db, schema);

    auto selection = _select_clause.empty()
                     ? selection::selection::wildcard(schema)
                     : selection::selection::from_selectors(db, schema, _select_clause);

    auto restrictions = prepare_restrictions(db, schema, ctx, selection, for_view, _parameters->allow_filtering());

    if (_parameters->is_distinct()) {
        validate_distinct_selection(*schema, *selection, *restrictions);
    }

    select_statement::ordering_comparator_type ordering_comparator;
    bool is_reversed_ = false;

    if (!_parameters->orderings().empty()) {
        assert(!for_view);
        verify_ordering_is_allowed(*restrictions);
        prepared_orderings_type prepared_orderings = prepare_orderings(*schema);
        verify_ordering_is_valid(prepared_orderings, *schema, *restrictions);

        ordering_comparator = get_ordering_comparator(prepared_orderings, *selection, *restrictions);
        is_reversed_ = is_ordering_reversed(prepared_orderings);
    }

    std::vector<sstring> warnings;
    check_needs_filtering(*restrictions, db.get_config().strict_allow_filtering(), warnings);
    ensure_filtering_columns_retrieval(db, *selection, *restrictions);
    auto group_by_cell_indices = ::make_shared<std::vector<size_t>>(prepare_group_by(*schema, *selection));

    ::shared_ptr<cql3::statements::select_statement> stmt;
    auto prepared_attrs = _attrs->prepare(db, keyspace(), column_family());
    prepared_attrs->fill_prepare_context(ctx);

    // Used to determine if an execution of this statement can be parallelized
    // using `forward_service`.
    auto can_be_forwarded = [&] {
        return selection->is_aggregate()        // Aggregation only
            && ( // SUPPORTED PARALLELIZATION
                 // All potential intermediate coordinators must support forwarding
                (db.features().parallelized_aggregation && selection->is_count())
                || (db.features().uda_native_parallelized_aggregation && selection->is_reducible())
            )
            && !restrictions->need_filtering()  // No filtering
            && group_by_cell_indices->empty()   // No GROUP BY
            && db.get_config().enable_parallelized_aggregation();
    };

    if (_parameters->is_prune_materialized_view()) {
        stmt = ::make_shared<cql3::statements::prune_materialized_view_statement>(
                schema,
                ctx.bound_variables_size(),
                _parameters,
                std::move(selection),
                std::move(restrictions),
                std::move(group_by_cell_indices),
                is_reversed_,
                std::move(ordering_comparator),
                prepare_limit(db, ctx, _limit),
                prepare_limit(db, ctx, _per_partition_limit),
                stats,
                std::move(prepared_attrs));
    } else if (restrictions->uses_secondary_indexing()) {
        stmt = indexed_table_select_statement::prepare(
                db,
                schema,
                ctx.bound_variables_size(),
                _parameters,
                std::move(selection),
                std::move(restrictions),
                std::move(group_by_cell_indices),
                is_reversed_,
                std::move(ordering_comparator),
                prepare_limit(db, ctx, _limit),
                prepare_limit(db, ctx, _per_partition_limit),
                stats,
                std::move(prepared_attrs));
    } else if (can_be_forwarded()) {
        stmt = parallelized_select_statement::prepare(
            schema,
            ctx.bound_variables_size(),
            _parameters,
            std::move(selection),
            std::move(restrictions),
            std::move(group_by_cell_indices),
            is_reversed_,
            std::move(ordering_comparator),
            prepare_limit(db, ctx, _limit),
            prepare_limit(db, ctx, _per_partition_limit),
            stats,
            std::move(prepared_attrs)
        );
    } else {
        stmt = ::make_shared<cql3::statements::primary_key_select_statement>(
                schema,
                ctx.bound_variables_size(),
                _parameters,
                std::move(selection),
                std::move(restrictions),
                std::move(group_by_cell_indices),
                is_reversed_,
                std::move(ordering_comparator),
                prepare_limit(db, ctx, _limit),
                prepare_limit(db, ctx, _per_partition_limit),
                stats,
                std::move(prepared_attrs));
    }

    auto partition_key_bind_indices = ctx.get_partition_key_bind_indexes(*schema);

    return make_unique<prepared_statement>(std::move(stmt), ctx, std::move(partition_key_bind_indices), std::move(warnings));
}

::shared_ptr<restrictions::statement_restrictions>
select_statement::prepare_restrictions(data_dictionary::database db,
                                       schema_ptr schema,
                                       prepare_context& ctx,
                                       ::shared_ptr<selection::selection> selection,
                                       bool for_view,
                                       bool allow_filtering)
{
    try {
        return ::make_shared<restrictions::statement_restrictions>(db, schema, statement_type::SELECT, _where_clause, ctx,
            selection->contains_only_static_columns(), for_view, allow_filtering);
    } catch (const exceptions::unrecognized_entity_exception& e) {
        if (contains_alias(e.entity)) {
            throw exceptions::invalid_request_exception(format("Aliases aren't allowed in the where clause ('{}')", e.relation_str));
        }
        throw;
    }
}

/** Returns a expr::expression for the limit or nullopt if no limit is set */
std::optional<expr::expression>
select_statement::prepare_limit(data_dictionary::database db, prepare_context& ctx, const std::optional<expr::expression>& limit)
{
    if (!limit.has_value()) {
        return std::nullopt;
    }

    expr::expression prep_limit = prepare_expression(*limit, db, keyspace(), nullptr, limit_receiver());
    expr::fill_prepare_context(prep_limit, ctx);
    return prep_limit;
}

void select_statement::verify_ordering_is_allowed(const restrictions::statement_restrictions& restrictions)
{
    if (restrictions.uses_secondary_indexing()) {
        throw exceptions::invalid_request_exception("ORDER BY with 2ndary indexes is not supported.");
    }
    if (restrictions.is_key_range()) {
        throw exceptions::invalid_request_exception("ORDER BY is only supported when the partition key is restricted by an EQ or an IN.");
    }
}

void select_statement::handle_unrecognized_ordering_column(const column_identifier& column) const
{
    if (contains_alias(column)) {
        throw exceptions::invalid_request_exception(format("Aliases are not allowed in order by clause ('{}')", column));
    }
    throw exceptions::invalid_request_exception(format("Order by on unknown column {}", column));
}

select_statement::prepared_orderings_type select_statement::prepare_orderings(const schema& schema) const {
    prepared_orderings_type prepared_orderings;
    prepared_orderings.reserve(_parameters->orderings().size());

    for (auto&& [column_id, column_ordering] : _parameters->orderings()) {
        ::shared_ptr<column_identifier> column = column_id->prepare_column_identifier(schema);

        const column_definition* def = schema.get_column_definition(column->name());
        if (def == nullptr) {
            handle_unrecognized_ordering_column(*column);
        }

        if (!def->is_clustering_key()) {
            throw exceptions::invalid_request_exception(format("Order by is currently only supported on the clustered columns of the PRIMARY KEY, got {}", *column));
        }

        prepared_orderings.emplace_back(def, column_ordering);
    }

    // Uncomment this to allow specifing ORDER BY columns in any order.
    // Right now specifying ORDER BY (c2 asc, c1 asc) is illegal, it can only be ORDER BY (c1 asc, c2 asc).
    //
    // std::sort(prepared_orderings.begin(), prepared_orderings.end(),
    //     [](const std::pair<const column_definition*, ordering>& a, const std::pair<const column_definition*, ordering>& b) {
    //         return a.first->component_index() < b.first->component_index();
    //     }
    // );

    return prepared_orderings;
}

// Checks whether this ordering causes select results on this column to be reversed.
// A clustering column can be ordered in the descending order in the table.
// Then specifying ascending order would cause the results of this column to be reverse in comparison to a standard select.
static bool are_column_select_results_reversed(const column_definition& column, select_statement::ordering column_ordering) {
    if (column_ordering == select_statement::ordering::ascending) {
        if (column.type->is_reversed()) {
            return true;
        } else {
            return false;
        }
    } else { // descending ordering
        if (column.type->is_reversed()) {
            return false;
        } else {
            return true;
        }
    }
}

bool select_statement::is_ordering_reversed(const prepared_orderings_type& orderings) const {
    if (orderings.empty()) {
        return false;
    }

    return are_column_select_results_reversed(*orderings[0].first, orderings[0].second);
}

void select_statement::verify_ordering_is_valid(const prepared_orderings_type& orderings,
                                                const schema& schema,
                                                const restrictions::statement_restrictions& restrictions) const {
    if (orderings.empty()) {
        return;
    }

    // Verify that columns are in the same order as in schema definition
    for (size_t i = 0; i + 1 < orderings.size(); i++) {
        if (orderings[i].first->component_index() >= orderings[i + 1].first->component_index()) {
            throw exceptions::invalid_request_exception("Order by currently only supports the ordering of columns following their declared order in the PRIMARY KEY");
        }
    }

    // We only allow leaving select results unchanged or reversing them. Find out if they should be reversed.
    bool is_reversed = is_ordering_reversed(orderings);

    // Now verify that
    // 1. Each column in the specified clustering prefix either has an ordering, or an EQ restriction.
    //    When there is an EQ restriction ordering is not needed because only a single value is possible.
    // 2. The ordering leaves the selected rows unchanged or reverses the normal result.
    //    For that to happen the ordering must reverse all columns or none at all.
    uint32_t max_ck_index = orderings.back().first->component_index();
    auto orderings_iterator = orderings.begin();
    for (uint32_t ck_column_index = 0; ck_column_index <= max_ck_index; ck_column_index++) {
         if (orderings_iterator->first->component_index() == ck_column_index) {
            // We have ordering for this column, let's see if its reversing is right.
            const column_definition* cur_ck_column = orderings_iterator->first;
            bool is_cur_column_reversed = are_column_select_results_reversed(*cur_ck_column, orderings_iterator->second);
            if (is_cur_column_reversed != is_reversed) {
                throw exceptions::invalid_request_exception(
                    format("Unsupported order by relation - only reversing all columns is supported, "
                           "but column {} has opposite ordering", cur_ck_column->name_as_text()));
            }
            orderings_iterator++;
        } else {
            // We don't have ordering for this column. Check if there is an EQ restriction or fail with an error.
            const column_definition& cur_ck_column = schema.clustering_column_at(ck_column_index);
            if (!restrictions.has_eq_restriction_on_column(cur_ck_column)) {
                throw exceptions::invalid_request_exception(format(
                    "Unsupported order by relation - column {} doesn't have an ordering or EQ relation.",
                    cur_ck_column.name_as_text()));
            }
        }
    }
}

select_statement::ordering_comparator_type select_statement::get_ordering_comparator(const prepared_orderings_type& orderings,
    selection::selection& selection,
    const restrictions::statement_restrictions& restrictions) {
    if (!restrictions.key_is_in_relation()) {
        return {};
    }

    // For the comparator we only need columns that have ordering defined.
    // Other columns are required to have an EQ restriction, so they will have no more than a single value.

    std::vector<std::pair<uint32_t, data_type>> sorters;
    sorters.reserve(orderings.size());

    // If we order post-query (see orderResults), the sorted column needs to be in the ResultSet for sorting,
    // even if we don't
    // ultimately ship them to the client (CASSANDRA-4911).
    for (auto&& [column_def, is_descending] : orderings) {
        auto index = selection.index_of(*column_def);
        if (index < 0) {
            index = selection.add_column_for_post_processing(*column_def);
        }

        sorters.emplace_back(index, column_def->type);
    }

    return [sorters = std::move(sorters)] (const result_row_type& r1, const result_row_type& r2) mutable {
        for (auto&& e : sorters) {
            auto& c1 = r1[e.first];
            auto& c2 = r2[e.first];
            auto type = e.second;

            if (bool(c1) != bool(c2)) {
                return bool(c2);
            }
            if (c1) {
                auto result = type->compare(*c1, *c2);
                if (result != 0) {
                    return result < 0;
                }
            }
        }
        return false;
    };
}

void select_statement::validate_distinct_selection(const schema& schema,
                                                   const selection::selection& selection,
                                                   const restrictions::statement_restrictions& restrictions)
{
    if (restrictions.has_non_primary_key_restriction() || restrictions.has_clustering_columns_restriction()) {
        throw exceptions::invalid_request_exception(
            "SELECT DISTINCT with WHERE clause only supports restriction by partition key.");
    }
    for (auto&& def : selection.get_columns()) {
        if (!def->is_partition_key() && !def->is_static()) {
            throw exceptions::invalid_request_exception(format("SELECT DISTINCT queries must only request partition key columns and/or static columns (not {})",
                def->name_as_text()));
        }
    }

    // If it's a key range, we require that all partition key columns are selected so we don't have to bother
    // with post-query grouping.
    if (!restrictions.is_key_range()) {
        return;
    }

    for (auto&& def : schema.partition_key_columns()) {
        if (!selection.has_column(def)) {
            throw exceptions::invalid_request_exception(format("SELECT DISTINCT queries must request all the partition key columns (missing {})", def.name_as_text()));
        }
    }
}

/// True iff restrictions require ALLOW FILTERING despite there being no coordinator-side filtering.
static bool needs_allow_filtering_anyway(
        const restrictions::statement_restrictions& restrictions,
        db::tri_mode_restriction_t::mode strict_allow_filtering,
        std::vector<sstring>& warnings) {
    using flag_t = db::tri_mode_restriction_t::mode;
    if (strict_allow_filtering == flag_t::FALSE) {
        return false;
    }
    const auto& ck_restrictions = restrictions.get_clustering_columns_restrictions();
    const auto& pk_restrictions = restrictions.get_partition_key_restrictions();
    // Even if no filtering happens on the coordinator, we still warn about poor performance when partition
    // slice is defined but in potentially unlimited number of partitions (see #7608).
    if ((expr::is_empty_restriction(pk_restrictions) || has_token(pk_restrictions)) // Potentially unlimited partitions.
        && !expr::is_empty_restriction(ck_restrictions) // Slice defined.
        && !restrictions.uses_secondary_indexing()) { // Base-table is used. (Index-table use always limits partitions.)
        if (strict_allow_filtering == flag_t::WARN) {
            warnings.emplace_back("This query should use ALLOW FILTERING and will be rejected in future versions.");
            return false;
        }
        return true;
    }
    return false;
}

/** If ALLOW FILTERING was not specified, this verifies that it is not needed */
void select_statement::check_needs_filtering(
        const restrictions::statement_restrictions& restrictions,
        db::tri_mode_restriction_t::mode strict_allow_filtering,
        std::vector<sstring>& warnings)
{
    // non-key-range non-indexed queries cannot involve filtering underneath
    if (!_parameters->allow_filtering() && (restrictions.is_key_range() || restrictions.uses_secondary_indexing())) {
        if (restrictions.need_filtering() || needs_allow_filtering_anyway(restrictions, strict_allow_filtering, warnings)) {
            throw exceptions::invalid_request_exception(
                "Cannot execute this query as it might involve data filtering and "
                    "thus may have unpredictable performance. If you want to execute "
                    "this query despite the performance unpredictability, use ALLOW FILTERING");
        }
    }
}

/**
 * Adds columns that are needed for the purpose of filtering to the selection.
 * The columns that are added to the selection are columns that
 * are needed for filtering on the coordinator but are not part of the selection.
 * The columns are added with a meta-data indicating they are not to be returned
 * to the user.
 */
void select_statement::ensure_filtering_columns_retrieval(data_dictionary::database db,
                                        selection::selection& selection,
                                        const restrictions::statement_restrictions& restrictions) {
    for (auto&& cdef : restrictions.get_column_defs_for_filtering(db)) {
        if (!selection.has_column(*cdef)) {
            selection.add_column_for_post_processing(*cdef);
        }
    }
}

bool select_statement::contains_alias(const column_identifier& name) const {
    return std::any_of(_select_clause.begin(), _select_clause.end(), [&name] (auto raw) {
        return raw->alias && name == *raw->alias;
    });
}

lw_shared_ptr<column_specification> select_statement::limit_receiver(bool per_partition) {
    sstring name = per_partition ? "[per_partition_limit]" : "[limit]";
    return make_lw_shared<column_specification>(keyspace(), column_family(), ::make_shared<column_identifier>(name, true),
        int32_type);
}

namespace {

/// True iff one of \p relations is a single-column EQ involving \p def.
bool equality_restricted(
        const column_definition& def, const schema& schema, const expr::expression& relations) {
    for (const auto& relation : boolean_factors(relations)) {
        if (auto binop = expr::as_if<expr::binary_operator>(&relation)) {
            if (binop->op != expr::oper_t::EQ) {
                continue;
            }

            if (auto lhs_col_ident = expr::as_if<expr::unresolved_identifier>(&binop->lhs)) {
                const ::shared_ptr<column_identifier> lhs_cdef = lhs_col_ident->ident->prepare_column_identifier(schema);
                if (lhs_cdef->name() == def.name()) {
                    return true;
                }
            }

            if (auto lhs_col = expr::as_if<expr::column_value>(&binop->lhs)) {
                if (lhs_col->col->name() == def.name()) {
                    return true;
                }
            }
        }
    }
    return false;
}

/// Returns an exception to throw when \p col is out of order in GROUP BY.
auto make_order_exception(const column_identifier::raw& col) {
    return exceptions::invalid_request_exception(format("Group by column {} is out of order", col));
}

} // anonymous namespace

std::vector<size_t> select_statement::prepare_group_by(const schema& schema, selection::selection& selection) const {
    if (_group_by_columns.empty()) {
        return {};
    }

    std::vector<size_t> indices;

    // We compare GROUP BY columns to the primary-key columns (in their primary-key order).  If a
    // primary-key column is equality-restricted by the WHERE clause, it can be skipped in GROUP BY.
    // It's OK if GROUP BY columns list ends before the primary key is exhausted.

    const auto key_size = schema.partition_key_size() + schema.clustering_key_size();
    const auto all_columns = schema.all_columns_in_select_order();
    uint32_t expected_index = 0; // Index of the next column we expect to encounter.

    using exceptions::invalid_request_exception;
    for (const auto& col : _group_by_columns) {
        auto def = schema.get_column_definition(col->prepare_column_identifier(schema)->name());
        if (!def) {
            throw invalid_request_exception(format("Group by unknown column {}", *col));
        }
        if (!def->is_primary_key()) {
            throw invalid_request_exception(format("Group by non-primary-key column {}", *col));
        }
        if (expected_index >= key_size) {
            throw make_order_exception(*col);
        }
        while (*def != all_columns[expected_index]
               && equality_restricted(all_columns[expected_index], schema, _where_clause)) {
            if (++expected_index >= key_size) {
                throw make_order_exception(*col);
            }
        }
        if (*def != all_columns[expected_index]) {
            throw make_order_exception(*col);
        }
        ++expected_index;
        const auto index = selection.index_of(*def);
        indices.push_back(index != -1 ? index : selection.add_column_for_post_processing(*def));
    }

    if (expected_index < schema.partition_key_size()) {
        throw invalid_request_exception(format("GROUP BY must include the entire partition key"));
    }

    return indices;
}

}

future<> set_internal_paging_size(int paging_size) {
    return seastar::smp::invoke_on_all([paging_size] {
        internal_paging_size = paging_size;
    });
}

future<> reset_internal_paging_size() {
    return set_internal_paging_size(DEFAULT_INTERNAL_PAGING_SIZE);
}

}

namespace util {

std::unique_ptr<cql3::statements::raw::select_statement> build_select_statement(
            const sstring_view& cf_name,
            const sstring_view& where_clause,
            bool select_all_columns,
            const std::vector<column_definition>& selected_columns) {
    std::ostringstream out;
    out << "SELECT ";
    if (select_all_columns) {
        out << "*";
    } else {
        // If the column name is not entirely lowercase (or digits or _),
        // when output to CQL it must be quoted to preserve case as well
        // as non alphanumeric characters.
        auto cols = boost::copy_range<std::vector<sstring>>(selected_columns
                | boost::adaptors::transformed(std::mem_fn(&column_definition::name_as_cql_string)));
        out << join(", ", cols);
    }
    // Note that cf_name may need to be quoted, just like column names above.
    out << " FROM " << util::maybe_quote(sstring(cf_name)) << " WHERE " << where_clause << " ALLOW FILTERING";
    return do_with_parser(out.str(), std::mem_fn(&cql3_parser::CqlParser::selectStatement));
}

}

}
