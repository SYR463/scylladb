/*
 * Copyright 2015-present ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * SPDX-License-Identifier: (AGPL-3.0-or-later and Apache-2.0)
 */

#include <seastar/core/coroutine.hh>
#include "cql3/statements/create_keyspace_statement.hh"
#include "cql3/statements/ks_prop_defs.hh"
#include "prepared_statement.hh"
#include "data_dictionary/data_dictionary.hh"
#include "data_dictionary/keyspace_metadata.hh"
#include "mutation.hh"
#include "service/migration_manager.hh"
#include "service/storage_proxy.hh"
#include "transport/messages/result_message.hh"
#include "cql3/query_processor.hh"
#include "db/config.hh"
#include "gms/feature_service.hh"

#include <regex>

bool is_system_keyspace(std::string_view keyspace);

namespace cql3 {

namespace statements {

static logging::logger mylogger("create_keyspace");

create_keyspace_statement::create_keyspace_statement(const sstring& name, shared_ptr<ks_prop_defs> attrs, bool if_not_exists)
    : _name{name}
    , _attrs{attrs}
    , _if_not_exists{if_not_exists}
{
}

const sstring& create_keyspace_statement::keyspace() const
{
    return _name;
}

future<> create_keyspace_statement::check_access(query_processor& qp, const service::client_state& state) const
{
    return state.has_all_keyspaces_access(auth::permission::CREATE);
}

void create_keyspace_statement::validate(query_processor& qp, const service::client_state& state) const
{
    std::string name;
    name.resize(_name.length());
    std::transform(_name.begin(), _name.end(), name.begin(), ::tolower);
    if (is_system_keyspace(name)) {
        throw exceptions::invalid_request_exception("system keyspace is not user-modifiable");
    }
    // keyspace name
    std::regex name_regex("\\w+");
    if (!std::regex_match(name, name_regex)) {
        throw exceptions::invalid_request_exception(format("\"{}\" is not a valid keyspace name", _name.c_str()));
    }
    if (name.length() > schema::NAME_LENGTH) {
        throw exceptions::invalid_request_exception(format("Keyspace names shouldn't be more than {:d} characters long (got \"{}\")", schema::NAME_LENGTH, _name.c_str()));
    }

    _attrs->validate();

    if (!bool(_attrs->get_replication_strategy_class())) {
        throw exceptions::configuration_exception("Missing mandatory replication strategy class");
    }
    try {
        _attrs->get_storage_options();
    } catch (const std::runtime_error& e) {
        throw exceptions::invalid_request_exception(e.what());
    }
    if (!qp.proxy().features().keyspace_storage_options
            && _attrs->get_storage_options().type_string() != "LOCAL") {
        throw exceptions::invalid_request_exception("Keyspace storage options not supported in the cluster");
    }
#if 0
    // The strategy is validated through KSMetaData.validate() in announceNewKeyspace below.
    // However, for backward compatibility with thrift, this doesn't validate unexpected options yet,
    // so doing proper validation here.
    AbstractReplicationStrategy.validateReplicationStrategy(name,
                                                            AbstractReplicationStrategy.getClass(attrs.getReplicationStrategyClass()),
                                                            StorageService.instance.getTokenMetadata(),
                                                            DatabaseDescriptor.getEndpointSnitch(),
                                                            attrs.getReplicationOptions());
#endif
}

future<std::pair<::shared_ptr<cql_transport::event::schema_change>, std::vector<mutation>>> create_keyspace_statement::prepare_schema_mutations(query_processor& qp, api::timestamp_type ts) const {
    using namespace cql_transport;
    const auto& tm = *qp.proxy().get_token_metadata_ptr();
    ::shared_ptr<event::schema_change> ret;
    std::vector<mutation> m;

    try {
        m = qp.get_migration_manager().prepare_new_keyspace_announcement(_attrs->as_ks_metadata(_name, tm), ts);

        ret = ::make_shared<event::schema_change>(
                event::schema_change::change_type::CREATED,
                event::schema_change::target_type::KEYSPACE,
                keyspace());
    } catch (const exceptions::already_exists_exception& e) {
        if (!_if_not_exists) {
          co_return coroutine::exception(std::current_exception());
        }
    }

    co_return std::make_pair(std::move(ret), std::move(m));
}

std::unique_ptr<cql3::statements::prepared_statement>
cql3::statements::create_keyspace_statement::prepare(data_dictionary::database db, cql_stats& stats) {
    return std::make_unique<prepared_statement>(make_shared<create_keyspace_statement>(*this));
}

future<> cql3::statements::create_keyspace_statement::grant_permissions_to_creator(const service::client_state& cs) const {
    return do_with(auth::make_data_resource(keyspace()), [&cs](const auth::resource& r) {
        return auth::grant_applicable_permissions(
                *cs.get_auth_service(),
                *cs.user(),
                r).handle_exception_type([](const auth::unsupported_authorization_operation&) {
            // Nothing.
        });
    });
}

// Check for replication strategy choices which are restricted by the
// configuration. This check can throw a configuration_exception immediately
// if the strategy is forbidden by the configuration, or return a warning
// string if the restriction was set to "warn".
// This function is only supposed to check for replication strategies
// restricted by the configuration. Checks for other types of strategy
// errors (such as unknown replication strategy name or unknown options
// to a known replication strategy) are done elsewhere.
std::optional<sstring> check_restricted_replication_strategy(
    query_processor& qp,
    const sstring& keyspace,
    const ks_prop_defs& attrs)
{
    if (!attrs.get_replication_strategy_class()) {
        return std::nullopt;
    }
    sstring replication_strategy = locator::abstract_replication_strategy::to_qualified_class_name(
        *attrs.get_replication_strategy_class());
    // SimpleStrategy is not recommended in any setup which already has - or
    // may have in the future - multiple racks or DCs. So depending on how
    // protective we are configured, let's prevent it or allow with a warning:
    if (replication_strategy == "org.apache.cassandra.locator.SimpleStrategy") {
        switch(qp.db().get_config().restrict_replication_simplestrategy()) {
        case db::tri_mode_restriction_t::mode::TRUE:
            throw exceptions::configuration_exception(
                "SimpleStrategy replication class is not recommended, and "
                "forbidden by the current configuration. Please use "
                "NetworkToplogyStrategy instead. You may also override this "
                "restriction with the restrict_replication_simplestrategy=false "
                "configuration option.");
        case db::tri_mode_restriction_t::mode::WARN:
            return format("SimpleStrategy replication class is not "
                "recommended, but was used for keyspace {}. The "
                "restrict_replication_simplestrategy configuration option "
                "can be changed to silence this warning or make it into an error.",
                keyspace);
        case db::tri_mode_restriction_t::mode::FALSE:
            // Scylla was configured to allow SimpleStrategy, but let's warn
            // if it's used on a cluster which *already* has multiple DCs:
            if (qp.proxy().get_token_metadata_ptr()->get_topology().get_datacenter_endpoints().size() > 1) {
                return "Using SimpleStrategy in a multi-datacenter environment is not recommended.";
            }
            break;
        }
    }
    return std::nullopt;
}

future<::shared_ptr<messages::result_message>>
create_keyspace_statement::execute(query_processor& qp, service::query_state& state, const query_options& options) const {
    std::optional<sstring> warning = check_restricted_replication_strategy(qp, keyspace(), *_attrs);
    return schema_altering_statement::execute(qp, state, options).then([this, warning = std::move(warning)] (::shared_ptr<messages::result_message> msg) {
        if (warning) {
            msg->add_warning(*warning);
            mylogger.warn("{}", *warning);
        }
        return msg;
    });
}

lw_shared_ptr<data_dictionary::keyspace_metadata> create_keyspace_statement::get_keyspace_metadata(const locator::token_metadata& tm) {
    _attrs->validate();
    return _attrs->as_ks_metadata(_name, tm);
}

}

}
