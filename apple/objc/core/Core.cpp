/*
 * Tencent is pleased to support the open source community by making
 * WCDB available.
 *
 * Copyright (C) 2017 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <WCDB/BusyRetryConfig.hpp>
#include <WCDB/Core.h>
#include <WCDB/FileManager.hpp>
#include <WCDB/Global.hpp>
#include <WCDB/Notifier.hpp>
#include <WCDB/String.hpp>

namespace WCDB {

#pragma mark - Core
Core& Core::shared()
{
    static Core* s_shared = new Core;
    return *s_shared;
}

Core::Core()
// Database
: m_databasePool(this)
, m_modules(new TokenizerModules())
// Corruption
, m_observationQueue(new ObservationQueue(ObservationQueueName, this))
// Checkpoint
, m_checkpointQueue(new CheckpointQueue(CheckpointQueueName, this))
// Backup
, m_backupQueue(new BackupQueue(BackupQueueName, this))
, m_backupConfig(new BackupConfig(m_backupQueue))
// Migration
, m_migrationQueue(new MigrationQueue(MigrationQueueName, this))
// Trace
, m_globalSQLTraceConfig(new ShareableSQLTraceConfig)
, m_globalPerformanceTraceConfig(new ShareablePerformanceTraceConfig)
// Config
, m_configs(new Configs(OrderedUniqueList<String, std::shared_ptr<Config>>({
  { Configs::Priority::Highest, GlobalSQLTraceConfigName, m_globalSQLTraceConfig },
  { Configs::Priority::Highest, GlobalPerformanceTraceConfigName, m_globalPerformanceTraceConfig },
  { Configs::Priority::Highest, CheckpointConfigName, std::shared_ptr<Config>(new CheckpointConfig(m_checkpointQueue)) },
  { Configs::Priority::Higher, BasicConfigName, std::shared_ptr<Config>(new BasicConfig) },
  })))
{
    Global::shared().enableMultithread();
    Global::shared().enableMemoryStatus(false);
    //    Global::shared().setMemoryMapSize(0x7fff0000, 0x7fff0000);
    Global::shared().setNotificationForLog(
    NotifierLoggerName,
    std::bind(&Core::globalLog, this, std::placeholders::_1, std::placeholders::_2));

    Notifier::shared().setNotificationForPreprocessing(
    NotifierPreprocessorName,
    std::bind(&Core::preprocessError, this, std::placeholders::_1));
}

Core::~Core()
{
    Global::shared().setNotificationForLog(NotifierLoggerName, nullptr);
    Notifier::shared().setNotificationForPreprocessing(NotifierPreprocessorName, nullptr);
}

#pragma mark - Database
RecyclableDatabase Core::getOrCreateDatabase(const String& path)
{
    return m_databasePool.getOrCreate(path);
}

RecyclableDatabase Core::getAlivingDatabase(const String& path)
{
    return m_databasePool.get(path);
}

void Core::purgeDatabasePool()
{
    m_databasePool.purge();
}

void Core::databaseDidCreate(Database* database)
{
    WCTInnerAssert(database != nullptr);

    database->setConfigs(m_configs);

    database->setConfig(BusyRetryConfigName,
                        std::shared_ptr<Config>(new BusyRetryConfig),
                        Configs::Priority::Highest);
}

void Core::preprocessError(Error& error)
{
    const auto& strings = error.infos.getStrings();

    auto iter = strings.find(ErrorStringKeyPath);
    if (iter != strings.end()) {
        auto database = m_databasePool.get(iter->second);
        if (database != nullptr) {
            auto tag = database->getTag();
            if (tag.isValid()) {
                error.infos.set(ErrorIntKeyTag, tag);
            }
        }
    }
}

#pragma mark - Tokenizer
void Core::registerTokenizer(const String& name, const TokenizerModule& module)
{
    m_modules->add(name, module);
}

bool Core::tokenizerExists(const String& name) const
{
    return m_modules->get(name) != nullptr;
}

std::shared_ptr<Config> Core::tokenizerConfig(const String& tokenizeName)
{
    return std::shared_ptr<Config>(new TokenizerConfig(tokenizeName, m_modules));
}

#pragma mark - Observation
void Core::observatedThatNeedPurge()
{
    purgeDatabasePool();
}

void Core::observatedThatMayBeCorrupted(const String& path)
{
    RecyclableDatabase database = m_databasePool.get(path);
    if (database != nullptr) {
        database->checkIntegrityIfAlreadyInitialized();

        std::set<String> sourcePaths = database->getPathsOfSourceDatabases();
        for (const String& sourcePath : sourcePaths) {
            RecyclableDatabase sourceDatabase = m_databasePool.get(sourcePath);
            if (sourceDatabase != nullptr) {
                sourceDatabase->checkIntegrityIfAlreadyInitialized();
            }
        }
    }
}

bool Core::isFileObservedCorrupted(const String& path)
{
    return m_observationQueue->isFileObservedCorrupted(path);
}

void Core::setNotificationWhenDatabaseCorrupted(const String& path,
                                                const CorruptedNotification& notification)
{
    ObservationQueue::Notification underlyingNotification = nullptr;
    if (notification != nullptr) {
        underlyingNotification
        = [this, notification](const String& path, uint32_t corruptedIdentifier) -> bool {
            RecyclableDatabase database = m_databasePool.get(path);
            if (database == nullptr) {
                // delay it since the database is not referenced.
                return false;
            }
            database->blockade();
            bool succeed;
            bool exists;
            std::tie(succeed, exists) = FileManager::fileExists(path);
            if (!succeed) {
                // delay it due to the I/O error
                return false;
            }
            if (!exists) {
                // mark as resolved since it's alredy not existing
                return true;
            }
            uint32_t identifier;
            std::tie(succeed, identifier) = FileManager::getFileIdentifier(path);
            if (!succeed) {
                // delay it due to the I/O error
                return false;
            }
            if (identifier != corruptedIdentifier) {
                // mark as resolved since the file is changed.
                return true;
            }
            succeed = notification(database.get());
            database->unblockade();
            return succeed;
        };
    }
    m_observationQueue->setNotificationWhenCorrupted(path, underlyingNotification);
}

#pragma mark - Checkpoint
bool Core::databaseShouldCheckpoint(const String& path, int frames)
{
    RecyclableDatabase database = m_databasePool.get(path);
    bool succeed = true; // mark as no error if database is not referenced.
    if (database != nullptr) {
        succeed = database->checkpointIfAlreadyInitialized(
        frames >= CheckpointFramesThresholdForTruncating ?
        Database::CheckpointMode::Truncate :
        Database::CheckpointMode::Passive);
    }
    return succeed;
}

#pragma mark - Backup
void Core::setAutoBackup(Database* database, bool enable)
{
    WCTInnerAssert(database != nullptr);
    if (enable) {
        database->setConfig(
        WCDB::BackupConfigName, m_backupConfig, WCDB::Configs::Priority::Highest);
    } else {
        m_backupQueue->unregister(database->getPath());
    }
}

bool Core::databaseShouldBackup(const String& path)
{
    RecyclableDatabase database = m_databasePool.get(path);
    bool succeed = true; // mark as no error if database is not referenced.
    if (database != nullptr) {
        succeed = database->backupIfAlreadyInitialized();
    }
    return succeed;
}

#pragma mark - Migration
std::pair<bool, bool> Core::databaseShouldMigrate(const String& path)
{
    RecyclableDatabase database = m_databasePool.get(path);
    bool succeed = true; // mark as no error if database is not referenced.
    bool done = false;
    if (database != nullptr) {
        std::tie(succeed, done) = database->stepMigrationIfAlreadyInitialized();
    }
    return { succeed, done };
}

void Core::setAutoMigration(const String& path, bool flag)
{
    if (flag) {
        m_migrationQueue->put(path);
    } else {
        m_migrationQueue->remove(path);
    }
}

#pragma mark - Trace
void Core::globalLog(int rc, const char* message)
{
    Error error;
    error.setSQLiteCode(rc, message);
    switch (error.code()) {
    case Error::Code::Warning:
        error.level = Error::Level::Warning;
        break;
    case Error::Code::Notice:
        error.level = Error::Level::Ignore;
        break;
    default:
        error.level = Error::Level::Debug;
        break;
    }
    Notifier::shared().notify(error);
}

void Core::setNotificationForSQLGLobalTraced(const ShareableSQLTraceConfig::Notification& notification)
{
    static_cast<ShareableSQLTraceConfig*>(m_globalSQLTraceConfig.get())->setNotification(notification);
}

void Core::setNotificationWhenPerformanceGlobalTraced(const ShareablePerformanceTraceConfig::Notification& notification)
{
    static_cast<ShareablePerformanceTraceConfig*>(m_globalPerformanceTraceConfig.get())
    ->setNotification(notification);
}

#pragma mark - Config
const std::shared_ptr<Configs>& Core::configs()
{
    return m_configs;
}

} // namespace WCDB
