/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

/**
 * Internal sharding command run on config servers to enable sharding on a database.
 */
class ConfigSvrEnableShardingCommand : public BasicCommand {
public:
    ConfigSvrEnableShardingCommand() : BasicCommand("_configsvrEnableSharding") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    static constexpr StringData kShardNameField = "primaryShard"_sd;

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Enable sharding on a database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(parseNs(dbname, cmdObj)), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return cmdObj.firstElement().str();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname_unused,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        uassert(ErrorCodes::IllegalOperation,
                "_configsvrEnableSharding can only be run on config servers",
                serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
        uassert(ErrorCodes::InvalidOptions,
                str::stream()
                    << "_configsvrEnableSharding must be called with majority writeConcern, got "
                    << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        // Set the operation context read concern level to local for reads into the config database.
        repl::ReadConcernArgs::get(opCtx) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);

        const std::string dbname = parseNs("", cmdObj);

        auto shardElem = cmdObj[kShardNameField];
        ShardId shardId = shardElem.ok() ? ShardId(shardElem.String()) : ShardId();

        // If assigned, check that the shardId is valid
        uassert(ErrorCodes::BadValue,
                str::stream() << "invalid shard name: " << shardId,
                !shardElem.ok() || shardId.isValid());

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        if (dbname == NamespaceString::kAdminDb || dbname == NamespaceString::kLocalDb) {
            uasserted(ErrorCodes::InvalidOptions,
                      str::stream() << "can't shard " + dbname + " database");
        }

        // Make sure to force update of any stale metadata
        ON_BLOCK_EXIT([opCtx, dbname] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbname); });

        // For an existing database, the enableSharding operation is just adding the {sharded: true}
        // field to config.database. First do an optimistic attempt to add it and if the write
        // succeeds do not go through the createDatabase flow.
        DBDirectClient client(opCtx);
        auto response = UpdateOp::parseResponse([&] {
            write_ops::Update updateOp(DatabaseType::ConfigNS);
            updateOp.setUpdates({[&] {
                BSONObjBuilder queryFilterBuilder;
                queryFilterBuilder.append(DatabaseType::name.name(), dbname);
                if (shardId.isValid())
                    queryFilterBuilder.append(DatabaseType::primary.name(), shardId.toString());
                auto updateModification = write_ops::UpdateModification(
                    write_ops::UpdateModification::parseFromClassicUpdate(
                        BSON("$set" << BSON(DatabaseType::sharded(true)))));
                write_ops::UpdateOpEntry updateEntry(queryFilterBuilder.obj(), updateModification);
                updateEntry.setMulti(false);
                updateEntry.setUpsert(false);
                return updateEntry;
            }()});

            auto response = client.runCommand(updateOp.serialize({}));
            return response->getCommandReply();
        }());

        // If an entry for the database was found it can be assumed that it was either updated or
        // already had 'sharded' enabled, so we can assume success
        if (response.getN() != 1) {
            auto dbDistLock = uassertStatusOK(DistLockManager::get(opCtx)->lock(
                opCtx, dbname, "enableSharding", DistLockManager::kDefaultLockTimeout));

            ShardingCatalogManager::get(opCtx)->enableSharding(opCtx, dbname, shardId);
        }

        audit::logEnableSharding(Client::getCurrent(), dbname);

        return true;
    }

} configsvrEnableShardingCmd;

}  // namespace
}  // namespace mongo
