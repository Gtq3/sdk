/**
 * @file sync.cpp
 * @brief Class for synchronizing local and remote trees
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include <type_traits>
#include <unordered_set>

#include "mega.h"

#ifdef ENABLE_SYNC
#include "mega/sync.h"
#include "mega/megaapp.h"
#include "mega/transfer.h"
#include "mega/megaclient.h"
#include "mega/base64.h"

namespace mega {

const int Sync::SCANNING_DELAY_DS = 5;
const int Sync::EXTRA_SCANNING_DELAY_DS = 150;
const int Sync::FILE_UPDATE_DELAY_DS = 30;
const int Sync::FILE_UPDATE_MAX_DELAY_SECS = 60;
const dstime Sync::RECENT_VERSION_INTERVAL_SECS = 10800;

bool gLogsync = false;
#define SYNC_verbose if (gLogsync) LOG_verbose

std::atomic<size_t> ScanService::mNumServices(0);
std::unique_ptr<ScanService::Worker> ScanService::mWorker;
std::mutex ScanService::mWorkerLock;

ScanService::ScanService(Waiter& waiter)
  : mCookie(std::make_shared<Cookie>(waiter))
{
    // Locking here, rather than in the if statement, ensures that the
    // worker is fully constructed when control leaves the constructor.
    std::lock_guard<std::mutex> lock(mWorkerLock);

    if (++mNumServices == 1)
    {
        mWorker.reset(new Worker());
    }
}

ScanService::~ScanService()
{
    if (--mNumServices == 0)
    {
        std::lock_guard<std::mutex> lock(mWorkerLock);
        mWorker.reset();
    }
}

auto ScanService::scan(const LocalNode& target, LocalPath targetPath) -> RequestPtr
{
    // For convenience.
    const auto& debris = target.sync->localdebris;
    const auto& separator = target.sync->client->fsaccess->localseparator;

    // Create a request to represent the scan.
    auto request = std::make_shared<ScanRequest>(mCookie, target, targetPath);

    // Have we been asked to scan the debris?
    request->mComplete = debris.isContainingPathOf(targetPath, separator);

    // Don't bother scanning the debris.
    if (!request->mComplete)
    {
        LOG_debug << "Queuing scan for: "
                  << targetPath.toPath(*target.sync->client->fsaccess);

        // Queue request for processing.
        mWorker->queue(request);
    }

    return request;
}

auto ScanService::scan(const LocalNode& target) -> RequestPtr
{
    return scan(target, target.getLocalPath(true));
}

ScanService::ScanRequest::ScanRequest(const std::shared_ptr<Cookie>& cookie,
                                      const LocalNode& target,
                                      LocalPath targetPath)
  : mCookie(cookie)
  , mComplete(false)
  , mDebrisPath(target.sync->localdebris)
  , mFollowSymLinks(target.sync->client->followsymlinks)
  , mKnown()
  , mResults()
  , mTarget(target)
  , mTargetPath(std::move(targetPath))
{
    // Track details about mTarget's current children.
    for (auto& childIt : mTarget.children)
    {
        LocalNode& child = *childIt.second;

        if (child.fsid != UNDEF)
        {
            mKnown.emplace(child.localname, child.getKnownFSDetails());
        }
    }
}

ScanService::Worker::Worker(size_t numThreads)
  : mFsAccess(new FSACCESS_CLASS())
  , mPending()
  , mPendingLock()
  , mPendingNotifier()
  , mThreads()
{
    // Always at least one thread.
    assert(numThreads > 0);

    LOG_debug << "Starting ScanService worker...";

    // Start the threads.
    while (numThreads--)
    {
        try
        {
            mThreads.emplace_back([this]() { loop(); });
        }
        catch (std::system_error& e)
        {
            LOG_err << "Failed to start worker thread: " << e.what();
        }
    }

    LOG_debug << mThreads.size() << " worker thread(s) started.";
    LOG_debug << "ScanService worker started.";
}

ScanService::Worker::~Worker()
{
    LOG_debug << "Stopping ScanService worker...";

    // Queue the 'terminate' sentinel.
    {
        std::unique_lock<std::mutex> lock(mPendingLock);
        mPending.emplace_back();
    }

    // Wake any sleeping threads.
    mPendingNotifier.notify_all();

    LOG_debug << "Waiting for worker thread(s) to terminate...";

    // Wait for the threads to terminate.
    for (auto& thread : mThreads)
    {
        thread.join();
    }

    LOG_debug << "ScanService worker stopped.";
}

void ScanService::Worker::queue(ScanRequestPtr request)
{
    // Queue the request.
    {
        std::unique_lock<std::mutex> lock(mPendingLock);
        mPending.emplace_back(std::move(request));
    }

    // Tell the lucky thread it has something to do.
    mPendingNotifier.notify_one();
}

void ScanService::Worker::loop()
{
    // We're ready when we have some work to do.
    auto ready = [this]() { return mPending.size(); };

    for ( ; ; )
    {
        ScanRequestPtr request;

        {
            // Wait for something to do.
            std::unique_lock<std::mutex> lock(mPendingLock);
            mPendingNotifier.wait(lock, ready);

            // Are we being told to terminate?
            if (!mPending.front())
            {
                // Bail, don't deque the sentinel.
                return;
            }

            request = std::move(mPending.front());
            mPending.pop_front();
        }

        const auto targetPath =
          request->mTargetPath.toPath(*mFsAccess);

        LOG_debug << "Scanning directory: " << targetPath;

        // Process the request.
        scan(request);

        // Mark the request as complete.
        request->mComplete = true;

        LOG_debug << "Scan complete for: " << targetPath;

        // Do we still have someone to notify?
        auto cookie = request->mCookie.lock();

        if (cookie)
        {
            LOG_debug << "Letting the waiter know it has "
                      << request->mResults.size()
                      << " scan result(s).";

            // Yep, let them know the request is complete.
            cookie->completed();
        }
        else
        {
            LOG_debug << "No waiter, discarding "
                      << request->mResults.size()
                      << " scan result(s).";
        }
    }
}

FSNode ScanService::Worker::interrogate(DirAccess& iterator,
                                        const LocalPath& name,
                                        LocalPath& path,
                                        ScanRequest& request)
{
    auto reuseFingerprint =
      [](const FSNode& lhs, const FSNode& rhs)
      {
          return lhs.type == rhs.type
                 && lhs.fsid == rhs.fsid
                 && lhs.mtime == rhs.mtime
                 && rhs.size == rhs.size;
      };

    FSNode result;
    auto& known = request.mKnown;

    // Always record the name.
    result.localname = name;
    result.name = name.toName(*mFsAccess);

    // Can we open the file?
    auto fileAccess = mFsAccess->newfileaccess(false);

    if (fileAccess->fopen(path, true, false, &iterator))
    {
        // Populate result.
        result.fsid = fileAccess->fsidvalid ? fileAccess->fsid : UNDEF;
        result.isSymlink = fileAccess->mIsSymLink;
        result.mtime = fileAccess->mtime;
        result.size = fileAccess->size;
        result.shortname = mFsAccess->fsShortname(path);
        result.type = fileAccess->type;

        // Warn about symlinks.
        if (result.isSymlink)
        {
            LOG_debug << "Interrogated path is a symlink: "
                      << path.toPath(*mFsAccess);
        }

        // No need to fingerprint directories.
        if (result.type == FOLDERNODE)
        {
            return result;
        }

        // Do we already know about this child?
        auto it = known.find(name);

        // Can we reuse an existing fingerprint?
        if (it != known.end() && reuseFingerprint(it->second, result))
        {
            // Yep as fsid/mtime/size/type match.
            result.fingerprint = std::move(it->second.fingerprint);
        }
        else
        {
            // Child has changed, need a new fingerprint.
            result.fingerprint.genfingerprint(fileAccess.get());
        }

        return result;
    }

    // Couldn't open the file.
    LOG_warn << "Error opening file: " << path.toPath(*mFsAccess);

    // File's blocked if the error is transient.
    result.isBlocked = fileAccess->retry;

    // Warn about the blocked file.
    if (result.isBlocked)
    {
        LOG_warn << "File blocked: " << path.toPath(*mFsAccess);
    }

    return result;
}

void ScanService::Worker::scan(ScanRequestPtr request)
{
    // For convenience.
    const auto& debris = request->mDebrisPath;
    const auto& separator = mFsAccess->localseparator;

    // Don't bother processing the debris directory.
    if (debris.isContainingPathOf(request->mTargetPath, separator))
    {
        LOG_debug << "Skipping scan of debris directory.";
        return;
    }

    // Have we been passed a valid target path?
    auto fileAccess = mFsAccess->newfileaccess();
    auto path = request->mTargetPath;

    if (!fileAccess->fopen(path, true, false))
    {
        LOG_debug << "Scan target does not exist: "
                  << path.toPath(*mFsAccess);
        return;
    }

    // Does the path denote a directory?
    if (fileAccess->type != FOLDERNODE)
    {
        LOG_debug << "Scan target is not a directory: "
                  << path.toPath(*mFsAccess);
        return;
    }

    std::unique_ptr<DirAccess> dirAccess(mFsAccess->newdiraccess());
    LocalPath name;

    // Can we open the directory?
    if (!dirAccess->dopen(&path, fileAccess.get(), false))
    {
        LOG_debug << "Unable to iterate scan target: "
                  << path.toPath(*mFsAccess);
        return;
    }

    // Process each file in the target.
    std::vector<FSNode> results;

    while (dirAccess->dnext(path, name, request->mFollowSymLinks))
    {
        ScopedLengthRestore restorer(path);
        path.appendWithSeparator(name, false, separator);

        // Except the debris...
        if (debris.isContainingPathOf(path, separator))
        {
            continue;
        }

        // Learn everything we can about the file.
        auto info = interrogate(*dirAccess, name, path, *request);
        results.emplace_back(std::move(info));
    }

    // No need to keep this data around anymore.
    request->mKnown.clear();

    // Publish the results.
    request->mResults = std::move(results);
}

SyncConfigBag::SyncConfigBag(DbAccess& dbaccess, FileSystemAccess& fsaccess, PrnGen& rng, const std::string& id)
{
    std::string dbname = "syncconfigsv2_" + id;
    mTable.reset(dbaccess.open(rng, &fsaccess, &dbname, false, false));
    if (!mTable)
    {
        LOG_err << "Unable to open DB table: " << dbname;
        assert(false);
        return;
    }

    mTable->rewind();

    uint32_t tableId;
    std::string data;
    while (mTable->next(&tableId, &data))
    {
        auto syncConfig = SyncConfig::unserialize(data);
        if (!syncConfig)
        {
            LOG_err << "Unable to unserialize sync config at id: " << tableId;
            assert(false);
            continue;
        }
        syncConfig->dbid = tableId;

        mSyncConfigs.insert(std::make_pair(syncConfig->getTag(), *syncConfig));
        if (tableId > mTable->nextid)
        {
            mTable->nextid = tableId;
        }
    }
    ++mTable->nextid;
}

void SyncConfigBag::insert(const SyncConfig& syncConfig)
{
    auto insertOrUpdate = [this](const uint32_t id, const SyncConfig& syncConfig)
    {
        std::string data;
        const_cast<SyncConfig&>(syncConfig).serialize(&data);
        DBTableTransactionCommitter committer{mTable.get()};
        if (!mTable->put(id, &data)) // put either inserts or updates
        {
            LOG_err << "Incomplete database put at id: " << mTable->nextid;
            assert(false);
            mTable->abort();
            return false;
        }
        return true;
    };

    map<int, SyncConfig>::iterator syncConfigIt = mSyncConfigs.find(syncConfig.getTag());
    if (syncConfigIt == mSyncConfigs.end()) // syncConfig is new
    {
        if (mTable)
        {
            if (!insertOrUpdate(mTable->nextid, syncConfig))
            {
                return;
            }
        }
        auto insertPair = mSyncConfigs.insert(std::make_pair(syncConfig.getTag(), syncConfig));
        if (mTable)
        {
            insertPair.first->second.dbid = mTable->nextid;
            ++mTable->nextid;
        }
    }
    else // syncConfig exists already
    {
        const uint32_t tableId = syncConfigIt->second.dbid;
        if (mTable)
        {
            if (!insertOrUpdate(tableId, syncConfig))
            {
                return;
            }
        }
        syncConfigIt->second = syncConfig;
        syncConfigIt->second.dbid = tableId;
    }
}

bool SyncConfigBag::removeByTag(const int tag)
{
    auto syncConfigPair = mSyncConfigs.find(tag);
    if (syncConfigPair != mSyncConfigs.end())
    {
        if (mTable)
        {
            DBTableTransactionCommitter committer{mTable.get()};
            if (!mTable->del(syncConfigPair->second.dbid))
            {
                LOG_err << "Incomplete database del at id: " << syncConfigPair->second.dbid;
                assert(false);
                mTable->abort();
            }
        }
        mSyncConfigs.erase(syncConfigPair);
        return true;
    }
    return false;
}

const SyncConfig* SyncConfigBag::get(const int tag) const
{
    auto syncConfigPair = mSyncConfigs.find(tag);
    if (syncConfigPair != mSyncConfigs.end())
    {
        return &syncConfigPair->second;
    }
    return nullptr;
}


const SyncConfig* SyncConfigBag::getByNodeHandle(handle nodeHandle) const
{
    for (const auto& syncConfigPair : mSyncConfigs)
    {
        if (syncConfigPair.second.getRemoteNode() == nodeHandle)
            return &syncConfigPair.second;
    }
    return nullptr;
}

void SyncConfigBag::clear()
{
    if (mTable)
    {
        mTable->truncate();
        mTable->nextid = 0;
    }
    mSyncConfigs.clear();
}

std::vector<SyncConfig> SyncConfigBag::all() const
{
    std::vector<SyncConfig> syncConfigs;
    for (const auto& syncConfigPair : mSyncConfigs)
    {
        syncConfigs.push_back(syncConfigPair.second);
    }
    return syncConfigs;
}

// new Syncs are automatically inserted into the session's syncs list
// and a full read of the subtree is initiated
Sync::Sync(MegaClient* cclient, SyncConfig &config, const char* cdebris,
           LocalPath* clocaldebris, Node* remotenode, bool cinshare, int ctag, void *cappdata)
: localroot(new LocalNode)
{
    isnetwork = false;
    client = cclient;
    tag = ctag;
    inshare = cinshare;
    appData = cappdata;
    errorCode = NO_SYNC_ERROR;
    tmpfa = NULL;
    //initializing = true;

    localnodes[FILENODE] = 0;
    localnodes[FOLDERNODE] = 0;

    state = SYNC_INITIALSCAN;
    statecachetable = NULL;

    fullscan = true;
    scanseqno = 0;

    mLocalPath = config.getLocalPath();
    LocalPath crootpath = LocalPath::fromPath(mLocalPath, *client->fsaccess);

    if (cdebris)
    {
        debris = cdebris;
        localdebris = LocalPath::fromPath(debris, *client->fsaccess);

        dirnotify.reset(client->fsaccess->newdirnotify(crootpath, localdebris, client->waiter));

        localdebris.prependWithSeparator(crootpath, client->fsaccess->localseparator);
    }
    else
    {
        localdebris = *clocaldebris;

        // FIXME: pass last segment of localdebris
        dirnotify.reset(client->fsaccess->newdirnotify(crootpath, localdebris, client->waiter));
    }
    dirnotify->sync = this;

    // set specified fsfp or get from fs if none
    const auto cfsfp = config.getLocalFingerprint();
    if (cfsfp)
    {
        fsfp = cfsfp;
    }
    else
    {
        fsfp = dirnotify->fsfingerprint();
        config.setLocalFingerprint(fsfp);
    }

    fsstableids = dirnotify->fsstableids();
    LOG_info << "Filesystem IDs are stable: " << fsstableids;

    mFilesystemType = client->fsaccess->getlocalfstype(crootpath);

    localroot->init(this, FOLDERNODE, NULL, crootpath, nullptr);  // the root node must have the absolute path.  We don't store shortname, to avoid accidentally using relative paths.
    localroot->syncedCloudNodeHandle.set6byte(remotenode->nodehandle);
    cloudRootHandle = localroot->syncedCloudNodeHandle;

#ifdef __APPLE__
    if (macOSmajorVersion() >= 19) //macOS catalina+
    {
        LOG_debug << "macOS 10.15+ filesystem detected. Checking fseventspath.";
        string supercrootpath = "/System/Volumes/Data" + crootpath.platformEncoded();

        int fd = open(supercrootpath.c_str(), O_RDONLY);
        if (fd == -1)
        {
            LOG_debug << "Unable to open path using fseventspath.";
            mFsEventsPath = crootpath.platformEncoded();
        }
        else
        {
            char buf[MAXPATHLEN];
            if (fcntl(fd, F_GETPATH, buf) < 0)
            {
                LOG_debug << "Using standard paths to detect filesystem notifications.";
                mFsEventsPath = crootpath.platformEncoded();
            }
            else
            {
                LOG_debug << "Using fsevents paths to detect filesystem notifications.";
                mFsEventsPath = supercrootpath;
            }
            close(fd);
        }
    }
#endif

    sync_it = client->syncs.insert(client->syncs.end(), this);

    if (client->dbaccess)
    {
        // open state cache table
        handle tableid[3];
        string dbname;

        auto fas = client->fsaccess->newfileaccess(false);

        if (fas->fopen(crootpath, true, false))
        {
            tableid[0] = fas->fsid;
            tableid[1] = remotenode->nodehandle;
            tableid[2] = client->me;

            dbname.resize(sizeof tableid * 4 / 3 + 3);
            dbname.resize(Base64::btoa((byte*)tableid, sizeof tableid, (char*)dbname.c_str()));

            statecachetable = client->dbaccess->open(client->rng, client->fsaccess, &dbname, false, false);

            readstatecache();
        }
    }
}

Sync::~Sync()
{
    // must be set to prevent remote mass deletion while rootlocal destructor runs
    assert(state == SYNC_CANCELED || state == SYNC_FAILED || state == SYNC_DISABLED);
    mDestructorRunning = true;

    // unlock tmp lock
    tmpfa.reset();

    // stop all active and pending downloads
    if (Node* cr = cloudRoot())
    {
        TreeProcDelSyncGet tdsg;
        // Create a committer to ensure we update the transfer database in an efficient single commit,
        // if there are transactions in progress.
        DBTableTransactionCommitter committer(client->tctable);
        client->proctree(cr, &tdsg);
    }

    delete statecachetable;

    client->syncs.erase(sync_it);
    client->syncactivity = true;

    {
        // Create a committer and recursively delete all the associated LocalNodes, and their associated transfer and file objects.
        // If any have transactions in progress, the committer will ensure we update the transfer database in an efficient single commit.
        DBTableTransactionCommitter committer(client->tctable);
        localroot.reset();
    }
}

Node* Sync::cloudRoot()
{
    return client->nodeByHandle(cloudRootHandle);
}

void Sync::addstatecachechildren(uint32_t parent_dbid, idlocalnode_map* tmap, LocalPath& localpath, LocalNode *p, int maxdepth)
{
    auto range = tmap->equal_range(parent_dbid);

    for (auto it = range.first; it != range.second; it++)
    {
        ScopedLengthRestore restoreLen(localpath);

        localpath.appendWithSeparator(it->second->localname, true, client->fsaccess->localseparator);

        LocalNode* l = it->second;
        handle fsid = fsstableids ? l->fsid : UNDEF;
        m_off_t size = l->size;

        // clear localname to force newnode = true in setnameparent
        l->localname.clear();

        // if we already have the shortname from database, use that, otherwise (db is from old code) look it up
        std::unique_ptr<LocalPath> shortname;
        if (l->slocalname_in_db)
        {
            // null if there is no shortname, or the shortname matches the localname.
            shortname.reset(l->slocalname.release());
        }
        else
        {
            shortname = client->fsaccess->fsShortname(localpath);
        }

        l->init(this, l->type, p, localpath, std::move(shortname));

#ifdef DEBUG
        if (fsid != UNDEF)
        {
            auto fa = client->fsaccess->newfileaccess(false);
            if (fa->fopen(localpath))  // exists, is file
            {
                auto sn = client->fsaccess->fsShortname(localpath);
                if (!(!l->localname.empty() &&
                    ((!l->slocalname && (!sn || l->localname == *sn)) ||
                    (l->slocalname && sn && !l->slocalname->empty() && *l->slocalname != l->localname && *l->slocalname == *sn))))
                {
                    // This can happen if a file was moved elsewhere and moved back before the sync restarts.
                    // We'll refresh slocalname while scanning.
                    LOG_warn << "Shortname mismatch on LocalNode load!" <<
                        " Was: " << (l->slocalname ? l->slocalname->toPath(*client->fsaccess) : "(null") <<
                        " Now: " << (sn ? sn->toPath(*client->fsaccess) : "(null") <<
                        " at " << localpath.toPath(*client->fsaccess);
                }
            }
        }
#endif

        l->parent_dbid = parent_dbid;
        l->size = size;
        l->setfsid(fsid, client->localnodeByFsid);
        l->setSyncedNodeHandle(l->syncedCloudNodeHandle);

        p->assigned &= fsid != UNDEF;

        if (!l->slocalname_in_db)
        {
            statecacheadd(l);
            if (insertq.size() > 50000)
            {
                cachenodes();  // periodically output updated nodes with shortname updates, so people who restart megasync still make progress towards a fast startup
            }
        }

        if (maxdepth)
        {
            addstatecachechildren(l->dbid, tmap, localpath, l, maxdepth - 1);
        }
    }
}

bool Sync::readstatecache()
{
    if (statecachetable && state == SYNC_INITIALSCAN)
    {
        string cachedata;
        idlocalnode_map tmap;
        uint32_t cid;
        LocalNode* l;

        statecachetable->rewind();

        // bulk-load cached nodes into tmap
        while (statecachetable->next(&cid, &cachedata, &client->key))
        {
            if ((l = LocalNode::unserialize(this, &cachedata)))
            {
                l->dbid = cid;
                tmap.insert(pair<int32_t,LocalNode*>(l->parent_dbid,l));
            }
        }

        // recursively build LocalNode tree, set scanseqnos to sync's current scanseqno
        addstatecachechildren(0, &tmap, localroot->localname, localroot.get(), 100);
        cachenodes();

        // trigger a single-pass full scan to identify deleted nodes
        fullscan = true;
        scanseqno++;

        return true;
    }

    return false;
}

const SyncConfig& Sync::getConfig() const
{
    assert(client->syncConfigs && "Calling getConfig() requires sync configs");
    const auto config = client->syncConfigs->get(tag);
    assert(config);
    return *config;
}

// remove LocalNode from DB cache
void Sync::statecachedel(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    insertq.erase(l);

    if (l->dbid)
    {
        deleteq.insert(l->dbid);
    }
}

// insert LocalNode into DB cache
void Sync::statecacheadd(LocalNode* l)
{
    if (state == SYNC_CANCELED)
    {
        return;
    }

    if (l->dbid)
    {
        deleteq.erase(l->dbid);
    }

    insertq.insert(l);
}

void Sync::cachenodes()
{
    if (statecachetable && (state == SYNC_ACTIVE || (state == SYNC_INITIALSCAN /*&& insertq.size() > 100*/)) && (deleteq.size() || insertq.size()))
    {
        LOG_debug << "Saving LocalNode database with " << insertq.size() << " additions and " << deleteq.size() << " deletions";
        statecachetable->begin();

        // deletions
        for (set<uint32_t>::iterator it = deleteq.begin(); it != deleteq.end(); it++)
        {
            statecachetable->del(*it);
        }

        deleteq.clear();

        // additions - we iterate until completion or until we get stuck
        bool added;

        do {
            added = false;

            for (set<LocalNode*>::iterator it = insertq.begin(); it != insertq.end(); )
            {
                if ((*it)->type == TYPE_UNKNOWN)
                {
                    insertq.erase(it++);
                }
                else if ((*it)->parent->dbid || (*it)->parent == localroot.get())
                {
                    statecachetable->put(MegaClient::CACHEDLOCALNODE, *it, &client->key);
                    insertq.erase(it++);
                    added = true;
                }
                else it++;
            }
        } while (added);

        statecachetable->commit();

        if (insertq.size())
        {
            LOG_err << "LocalNode caching did not complete";
        }
    }
}

void Sync::changestate(syncstate_t newstate, SyncError newSyncError)
{
    if (newstate != state || newSyncError != errorCode)
    {
        LOG_debug << "Sync state/error changing. from " << state << "/" << errorCode << " to "  << newstate << "/" << newSyncError;
        if (newstate != SYNC_CANCELED)
        {
            client->changeSyncState(tag, newstate, newSyncError);
        }

        state = newstate;
        errorCode = newSyncError;
        fullscan = false;
    }
}

// walk path and return corresponding LocalNode and its parent
// path must be relative to l or start with the root prefix if l == NULL
// path must be a full sync path, i.e. start with localroot->localname
// NULL: no match, optionally returns residual path
LocalNode* Sync::localnodebypath(LocalNode* l, const LocalPath& localpath, LocalNode** parent, LocalPath* outpath)
{
    if (outpath)
    {
        assert(outpath->empty());
    }

    size_t subpathIndex = 0;

    if (!l)
    {
        // verify matching localroot prefix - this should always succeed for
        // internal use
        if (!localroot->localname.isContainingPathOf(localpath, client->fsaccess->localseparator, &subpathIndex))
        {
            if (parent)
            {
                *parent = NULL;
            }

            return NULL;
        }

        l = localroot.get();
    }


    LocalPath component;

    while (localpath.nextPathComponent(subpathIndex, component, client->fsaccess->localseparator))
    {
        if (parent)
        {
            *parent = l;
        }

        localnode_map::iterator it;
        if ((it = l->children.find(&component)) == l->children.end()
            && (it = l->schildren.find(&component)) == l->schildren.end())
        {
            // no full match: store residual path, return NULL with the
            // matching component LocalNode in parent
            if (outpath)
            {
                *outpath = std::move(component);
                auto remainder = localpath.subpathFrom(subpathIndex);
                if (!remainder.empty())
                {
                    outpath->appendWithSeparator(remainder, false, client->fsaccess->localseparator);
                }
            }

            return NULL;
        }

        l = it->second;
    }

    // full match: no residual path, return corresponding LocalNode
    if (outpath)
    {
        outpath->clear();
    }
    return l;
}



/// todo:   things to figure out where to put them in new system:
///
// no fsid change detected or overwrite with unknown file:
//if (fa->mtime != l->mtime || fa->size != l->size)
//{
//    if (fa->fsidvalid && l->fsid != fa->fsid)
//    {
//        l->setfsid(fa->fsid, client->fsidnode);
//    }
//
//    m_off_t dsize = l->size > 0 ? l->size : 0;
//
//    l->genfingerprint(fa.get());
//
//    client->app->syncupdate_local_file_change(this, l, path.c_str());
//
//    DBTableTransactionCommitter committer(client->tctable);
//    client->stopxfer(l, &committer); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
//    l->bumpnagleds();
//    l->deleted = false;
//
//    client->syncactivity = true;
//
//    statecacheadd(l);
//
//    fa.reset();
//
//    if (isnetwork && l->type == FILENODE)
//    {
//        LOG_debug << "Queueing extra fs notification for modified file";
//        dirnotify->notify(DirNotify::EXTRA, NULL, LocalPath(*localpathNew));
//    }
//    return l;
//}
//    }
//    else
//    {
//    // (we tolerate overwritten folders, because we do a
//    // content scan anyway)
//    if (fa->fsidvalid && fa->fsid != l->fsid)
//    {
//        l->setfsid(fa->fsid, client->fsidnode);
//        newnode = true;
//    }
//    }

//client->app->syncupdate_local_folder_addition(this, l, path.c_str());
//                else
//                {
//                if (fa->fsidvalid && l->fsid != fa->fsid)
//                {
//                    l->setfsid(fa->fsid, client->fsidnode);
//                }
//
//                if (l->genfingerprint(fa.get()))
//                {
//                    changed = true;
//                    l->bumpnagleds();
//                    l->deleted = false;
//                }
//
//                if (newnode)
//                {
//                    client->app->syncupdate_local_file_addition(this, l, path.c_str());
//                }
//                else if (changed)
//                {
//                    client->app->syncupdate_local_file_change(this, l, path.c_str());
//                    DBTableTransactionCommitter committer(client->tctable); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
//                    client->stopxfer(l, &committer);
//                }
//
//                if (newnode || changed)
//                {
//                    statecacheadd(l);
//                }
//                }
//            }
//        }
//
//        if (changed || newnode)
//        {
//            if (isnetwork && l->type == FILENODE)
//            {
//                LOG_debug << "Queueing extra fs notification for new file";
//                dirnotify->notify(DirNotify::EXTRA, NULL, LocalPath(*localpathNew));
//            }
//
//            client->syncactivity = true;
//        }
//    }
//    else
//    {
//    LOG_warn << "Error opening file";
//    if (fa->retry)
//    {
//        // fopen() signals that the failure is potentially transient - do
//        // nothing and request a recheck
//        LOG_warn << "File blocked. Adding notification to the retry queue: " << path;
//        dirnotify->notify(DirNotify::RETRY, ll, LocalPath(*localpathNew));
//        client->syncfslockretry = true;
//        client->syncfslockretrybt.backoff(SCANNING_DELAY_DS);
//        client->blockedfile = *localpathNew;
//    }
//    else if (l)
//    {
//        // immediately stop outgoing transfer, if any
//        if (l->transfer)
//        {
//            DBTableTransactionCommitter committer(client->tctable); // TODO:  can we use one committer for all the files in the folder?  Or for the whole recursion?
//            client->stopxfer(l, &committer);
//        }
//
//        client->syncactivity = true;
//
//        // in fullscan mode, missing files are handled in bulk in deletemissing()
//        // rather than through setnotseen()
//        if (!fullscan)
//        {
//            l->setnotseen(1);
//        }
//    }


bool Sync::checkLocalPathForMovesRenames(syncRow& row, syncRow& parentRow, LocalPath& fullPath, bool& rowResult)
{
    // rename or move of existing node?
    if (row.fsNode->isSymlink)
    {
        LOG_debug << "checked path is a symlink, blocked: " << fullPath.toPath(*client->fsaccess);
        row.syncNode->setUseBlocked();    // todo:   move earlier?  no syncnode here
        rowResult = false;
        return true;
    }
    else if (row.syncNode && row.syncNode->type != row.fsNode->type)
    {
        LOG_debug << "checked path does not have the same type, blocked: " << fullPath.toPath(*client->fsaccess);
        row.syncNode->setUseBlocked();
        rowResult = false;
        return true;
    }
    else
    {
        //// mark as present
        //row.syncNode->setnotseen(0); // todo: do we need this - prob not always right now

        // we already checked fsid differs before calling

        // was the file overwritten by moving an existing file over it?
        if (LocalNode* sourceLocalNode = client->findLocalNodeByFsid(*row.fsNode, *this))
        {
            // logic to detect files being updated in the local computer moving the original file
            // to another location as a temporary backup
            if (sourceLocalNode->type == FILENODE &&
                client->checkIfFileIsChanging(*row.fsNode, sourceLocalNode->getLocalPath(true)))
            {
                // if we revist here and the file is still the same after enough time, we'll move it
                rowResult = false;
                return true;
            }

            LOG_debug << client->clientname << "Move detected by fsid. Type: " << sourceLocalNode->type << " new path: " << fullPath.toPath(*client->fsaccess) << " old localnode: " << sourceLocalNode->localnodedisplaypath(*client->fsaccess);

            // catch the not so unlikely case of a false fsid match due to
            // e.g. a file deletion/creation cycle that reuses the same inode
            if (sourceLocalNode->type == FILENODE &&
                (sourceLocalNode->mtime != row.fsNode->mtime || sourceLocalNode->size != row.fsNode->size))
            {
                // This location's file can't be using that fsid then.
                // Clear our fsid, and let normal comparison run
                LOG_verbose << "Detaching fsid at: " << fullPath.toPath(*client->fsaccess);
                row.fsNode->fsid = UNDEF;

                return false;
            }
            else
            {
                Node* sourceCloudNode = client->nodeByHandle(sourceLocalNode->syncedCloudNodeHandle);
                Node* targetCloudNode = client->nodeByHandle(parentRow.syncNode->syncedCloudNodeHandle);

                if (sourceCloudNode && !sourceCloudNode->mPendingChanges.empty())
                {
                    // come back again later when there isn't already a command in progress
                    LOG_debug << client->clientname << "Actions are already in progress for " << sourceCloudNode->displaypath();
                    client->mSyncFlags.actionedMovesRenames = true;
                    rowResult = false;
                    return true;
                }

                if (sourceCloudNode && targetCloudNode)
                {

                    string newName = row.fsNode->localname.toName(*client->fsaccess);
                    if (newName == sourceCloudNode->displayname())
                    {
                        newName.clear();
                    }

                    if (sourceCloudNode->parent == targetCloudNode && newName.empty())
                    {
                        LOG_debug << client->clientname << "Move/rename has completed: " << sourceCloudNode->displaypath();
                        return false;
                    }

                    if (row.cloudNode && row.cloudNode != sourceCloudNode)
                    {
                        LOG_debug << "Moving node to debris for replacement: " << row.cloudNode->displaypath();
                        client->movetosyncdebris(row.cloudNode, false);
                        client->execsyncdeletions();
                    }

                    if (sourceCloudNode->parent == targetCloudNode && !newName.empty())
                    {
                        LOG_debug << "Renaming node: " << sourceCloudNode->displaypath() << " to " << newName;
                        client->setattr(sourceCloudNode, attr_map('n', newName), 0);
                        client->app->syncupdate_local_move(this, sourceLocalNode, fullPath.toPath(*client->fsaccess).c_str());
                        client->mSyncFlags.actionedMovesRenames = true;
                        rowResult = false;
                        return true;
                    }
                    else
                    {
                        LOG_debug << "Moving node: " << sourceCloudNode->displaypath() << " to " << targetCloudNode->displaypath() << (newName.empty() ? "" : (" as " + newName).c_str());
                        auto err = client->rename(sourceCloudNode, targetCloudNode,
                                                SYNCDEL_NONE,
                                                sourceCloudNode->parent ? sourceCloudNode->parent->nodehandle : UNDEF,
                                                newName.empty() ? nullptr : newName.c_str());
                        if (err == API_EACCESS)
                        {
                            LOG_debug << "Rename not permitted: " << err;
                        }
                        else
                        {
                            // command sent, now we wait for the actinpacket updates, later we will recognise
                            // the row as synced from fsNode, cloudNode and update the syncNode from those
                            client->mSyncFlags.actionedMovesRenames = true;
                            client->app->syncupdate_local_move(this, sourceLocalNode, fullPath.toPath(*client->fsaccess).c_str());
                            rowResult = false;
                            return true;
                        }
                    }
                }
                else
                {
                    LOG_debug << "Source/Target unavaliable for move";
                }


                // todo: adjust source sourceLocalNode so that it is treated as a deletion


                //LOG_debug << "File move/overwrite detected";

                //// delete existing LocalNode...
                //delete row.syncNode;      // todo:  CAUTION:  this will queue commands to remove the cloud node
                //row.syncNode = nullptr;

                //// ...move remote node out of the way...
                //client->execsyncdeletions();   // todo:  CAUTION:  this will send commands to remove the cloud node

                //// ...and atomically replace with moved one
                //client->app->syncupdate_local_move(this, sourceLocalNode, fullPath.toPath(*client->fsaccess).c_str());

                //// (in case of a move, this synchronously updates l->parent and l->node->parent)
                //sourceLocalNode->setnameparent(parentRow.syncNode, &fullPath, client->fsaccess->fsShortname(fullPath), true);

                //// mark as seen / undo possible deletion
                //sourceLocalNode->setnotseen(0);  // todo: do we still need this?

                //statecacheadd(sourceLocalNode);

                //rowResult = false;
                //return true;
            }
        }
    }
    //else
    //{
    //    // !row.syncNode

    //    // rename or move of existing node?
    //    if (row.fsNode->isSymlink)
    //    {
    //        LOG_debug << "checked path is a symlink, blocked: " << fullPath.toPath(*client->fsaccess);
    //        row.syncNode->setUseBlocked();    // todo:   move earlier?  no syncnode here
    //        rowResult = false;
    //        return true;
    //    }
    //    else if (LocalNode* sourceLocalNode = client->findLocalNodeByFsid(*row.fsNode, *this))
    //    {
    //        LOG_debug << client->clientname << "Move detected by fsid. Type: " << sourceLocalNode->type << " new path: " << fullPath.toPath(*client->fsaccess) << " old localnode: " << sourceLocalNode->localnodedisplaypath(*client->fsaccess);

    //        // logic to detect files being updated in the local computer moving the original file
    //        // to another location as a temporary backup
    //        if (sourceLocalNode->type == FILENODE &&
    //            client->checkIfFileIsChanging(*row.fsNode, sourceLocalNode->getLocalPath(true)))
    //        {
    //            // if we revist here and the file is still the same after enough time, we'll move it
    //            rowResult = false;
    //            return true;
    //        }

    //        client->app->syncupdate_local_move(this, sourceLocalNode, fullPath.toPath(*client->fsaccess).c_str());

    //        // (in case of a move, this synchronously updates l->parent
    //        // and l->node->parent)
    //        sourceLocalNode->setnameparent(parentRow.syncNode, &fullPath, client->fsaccess->fsShortname(fullPath), false);

    //        // make sure that active PUTs receive their updated filenames
    //        client->updateputs();

    //        statecacheadd(sourceLocalNode);

    //        // unmark possible deletion
    //        sourceLocalNode->setnotseen(0);    // todo: do we still need this?

    //        // immediately scan folder to detect deviations from cached state
    //        if (fullscan && sourceLocalNode->type == FOLDERNODE)
    //        {
    //            sourceLocalNode->setFutureScan(true, true);
    //        }
    //    }
    //}
    return false;
 }

bool Sync::checkCloudPathForMovesRenames(syncRow& row, syncRow& parentRow, LocalPath& fullPath, bool& rowResult)
{
    if (row.syncNode && row.syncNode->type != row.cloudNode->type)
    {
        LOG_debug << "checked node does not have the same type, blocked: " << fullPath.toPath(*client->fsaccess);
        row.syncNode->setUseBlocked();
        rowResult = false;
        return true;
    }
    else if (LocalNode* sourceLocalNode = client->findLocalNodeByNodeHandle(NodeHandle().set6byte(row.cloudNode->nodehandle)))
    {
        if (sourceLocalNode == row.syncNode) return false;

        // It's a move or rename

        sourceLocalNode->treestate(TREESTATE_SYNCING);
        if (row.syncNode) row.syncNode->treestate(TREESTATE_SYNCING);

        LocalPath sourcePath = sourceLocalNode->getLocalPath(true);
        LOG_verbose << "Renaming/moving from the previous location: " << sourcePath.toPath(*client->fsaccess) << logTriplet(row, fullPath);

        if (client->fsaccess->renamelocal(sourcePath, fullPath))
        {
            // todo: move anything at this path to sync debris first?  Old algo didn't though

            client->mSyncFlags.actionedMovesRenames = true;

            client->app->syncupdate_local_move(this, sourceLocalNode, fullPath.toPath(*client->fsaccess).c_str());

            // let the Localnodes be created at the new location, and removed at the old

            // update filenames so that PUT transfers can continue seamlessly
            // todo: client->updateputs();
            // client->syncactivity = true;  // todo: prob don't need this?

            // make sure we don't come back to this folder again until we've rescanned it
            if (sourceLocalNode->parent) sourceLocalNode->parent->setFutureScan(true, false);
            if (parentRow.syncNode->parent) parentRow.syncNode->parent->setFutureScan(true, true);

            rowResult = false;
            return true;
        }
        else if (client->fsaccess->transient_error)
        {
            row.syncNode->setUseBlocked();
            rowResult = false;
            return true;
        }
    }
    return false;
}



//bool Sync::checkValidNotification(int q, Notification& notification)
//{
//    // This code moved from filtering before going on notifyq, to filtering after when it's thread-safe to do so
//
//    if (q == DirNotify::DIREVENTS || q == DirNotify::EXTRA)
//    {
//        Notification next;
//        while (dirnotify->notifyq[q].peekFront(next)
//            && next.localnode == notification.localnode && next.path == notification.path)
//        {
//            dirnotify->notifyq[q].popFront(next);  // this is the only thread removing from the queue so it will be the same item
//            if (!notification.timestamp || !next.timestamp)
//            {
//                notification.timestamp = 0;  // immediate
//            }
//            else
//            {
//                notification.timestamp = std::max(notification.timestamp, next.timestamp);
//            }
//            LOG_debug << "Next notification repeats, skipping duplicate";
//        }
//    }
//
//    if (notification.timestamp && /*!initializing &&*/ q == DirNotify::DIREVENTS)
//    {
//        LocalPath tmppath;
//        if (notification.localnode)
//        {
//            tmppath = notification.localnode->getLocalPath(true);
//        }
//
//        if (!notification.path.empty())
//        {
//            tmppath.appendWithSeparator(notification.path, false, client->fsaccess->localseparator);
//        }
//
//        attr_map::iterator ait;
//        auto fa = client->fsaccess->newfileaccess(false);
//        bool success = fa->fopen(tmppath, false, false);
//        LocalNode *ll = localnodebypath(notification.localnode, notification.path);
//        if ((!ll && !success && !fa->retry) // deleted file
//            || (ll && success && ll->node && ll->node->localnode == ll
//                && (ll->type != FILENODE || (*(FileFingerprint *)ll) == (*(FileFingerprint *)ll->node))
//                && (ait = ll->node->attrs.map.find('n')) != ll->node->attrs.map.end()
//                && ait->second == ll->name
//                && fa->fsidvalid && fa->fsid == ll->fsid && fa->type == ll->type
//                && (ll->type != FILENODE || (ll->mtime == fa->mtime && ll->size == fa->size))))
//        {
//            LOG_debug << "Self filesystem notification skipped";
//            return false;
//        }
//    }
//    return true;
//}


//  Just mark the relative LocalNodes as needing to be rescanned.
void Sync::procscanq(int q)
{
    if (dirnotify->notifyq[q].empty()) return;

    LOG_verbose << "Marking sync tree with filesystem notifications: " << dirnotify->notifyq[q].size();

    Notification notification;
    while (dirnotify->notifyq[q].popFront(notification))
    {
        LocalNode* l;
        if ((l = notification.localnode) != (LocalNode*)~0)
        {
            LocalPath remainder;
            LocalNode *deepestParent;
            LocalNode *matching = localnodebypath(l, notification.path, &deepestParent, &remainder);

            if (LocalNode* deepest = matching && matching->parent ? matching->parent : deepestParent)
            {
                deepest->setFutureScan(true, !remainder.empty());

                // todo:  for Sync::EXTRA_SCANNING_DELAY_DS, we should scan now but also scan again in 15 seconds

                client->filesystemNotificationsQuietTime = Waiter::ds + (isnetwork && l->type == FILENODE ? Sync::EXTRA_SCANNING_DELAY_DS : SCANNING_DELAY_DS);
            }
        }
        else
        {
            string utf8path = notification.path.toPath(*client->fsaccess);
            LOG_debug << "Notification skipped: " << utf8path;
        }
    }
}

// todo: do we still need this?
// delete all child LocalNodes that have been missing for two consecutive scans (*l must still exist)
void Sync::deletemissing(LocalNode* l)
{
    LocalPath path;
    std::unique_ptr<FileAccess> fa;
    for (localnode_map::iterator it = l->children.begin(); it != l->children.end(); )
    {
        if (scanseqno-it->second->scanseqno > 1)
        {
            if (!fa)
            {
                fa = client->fsaccess->newfileaccess();
            }
            client->unlinkifexists(it->second, fa.get(), path);
            delete it++->second;
        }
        else
        {
            deletemissing(it->second);
            it++;
        }
    }
}

bool Sync::movetolocaldebris(LocalPath& localpath)
{
    char buf[32];
    struct tm tms;
    string day, localday;
    bool havedir = false;
    struct tm* ptm = m_localtime(m_time(), &tms);

    for (int i = -3; i < 100; i++)
    {
        ScopedLengthRestore restoreLen(localdebris);

        if (i == -2 || i > 95)
        {
            LOG_verbose << "Creating local debris folder";
            client->fsaccess->mkdirlocal(localdebris, true);
        }

        sprintf(buf, "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);

        if (i >= 0)
        {
            sprintf(strchr(buf, 0), " %02d.%02d.%02d.%02d", ptm->tm_hour,  ptm->tm_min, ptm->tm_sec, i);
        }

        day = buf;
        localdebris.appendWithSeparator(LocalPath::fromPath(day, *client->fsaccess), true, client->fsaccess->localseparator);

        if (i > -3)
        {
            LOG_verbose << "Creating daily local debris folder";
            havedir = client->fsaccess->mkdirlocal(localdebris, false) || client->fsaccess->target_exists;
        }

        localdebris.appendWithSeparator(localpath.subpathFrom(localpath.getLeafnameByteIndex(*client->fsaccess)), true, client->fsaccess->localseparator);

        client->fsaccess->skip_errorreport = i == -3;  // we expect a problem on the first one when the debris folders or debris day folders don't exist yet
        if (client->fsaccess->renamelocal(localpath, localdebris, false))
        {
            client->fsaccess->skip_errorreport = false;
            return true;
        }
        client->fsaccess->skip_errorreport = false;

        if (client->fsaccess->transient_error)
        {
            return false;
        }

        if (havedir && !client->fsaccess->target_exists)
        {
            return false;
        }
    }

    return false;
}

auto Sync::computeSyncTriplets(Node* cloudParent, const LocalNode& syncParent, vector<FSNode>& fsNodes) const -> vector<syncRow>
{
    // One comparator to sort them all.
    class Comparator
    {
    public:
        explicit Comparator(const Sync& sync)
          : mFsAccess(*sync.client->fsaccess)
          , mFsType(sync.mFilesystemType)
        {
        }

        int compare(const FSNode& lhs, const LocalNode& rhs) const
        {
            // Cloud name, case sensitive.
            return lhs.localname.compare(rhs.name);
        }

        int compare(const Node& lhs, const syncRow& rhs) const
        {
            // Local name, filesystem-dependent sensitivity.
            auto a = LocalPath::fromName(lhs.displayname(), mFsAccess, mFsType);

            return a.fsCompare(name(rhs), mFsType);
        }

        bool operator()(const FSNode& lhs, const FSNode& rhs) const
        {
            // Cloud name, case sensitive.
            return lhs.localname.compare(rhs.localname) < 0;
        }

        bool operator()(const LocalNode* lhs, const LocalNode* rhs) const
        {
            assert(lhs && rhs);

            // Cloud name, case sensitive.
            return lhs->name < rhs->name;
        }

        bool operator()(const Node* lhs, const Node* rhs) const
        {
            assert(lhs && rhs);

            // Local name, filesystem-dependent sensitivity.
            auto a = LocalPath::fromName(lhs->displayname(), mFsAccess, mFsType);

            return a.fsCompare(rhs->displayname(), mFsType) < 0;
        }

        bool operator()(const syncRow& lhs, const syncRow& rhs) const
        {
            // Local name, filesystem-dependent sensitivity.
            return name(lhs).fsCompare(name(rhs), mFsType) < 0;
        }

    private:
        const LocalPath& name(const syncRow& row) const
        {
            assert(row.fsNode || row.syncNode);

            if (row.syncNode)
            {
                return row.syncNode->localname;
            }

            return row.fsNode->localname;
        }

        FileSystemAccess& mFsAccess;
        FileSystemType mFsType;
    }; // Comparator

    Comparator comparator(*this);
    vector<LocalNode*> localNodes;
    vector<Node*> remoteNodes;
    vector<syncRow> triplets;

    localNodes.reserve(syncParent.children.size());
    remoteNodes.reserve(cloudParent ? cloudParent->children.size() : 0);

    for (auto& child : syncParent.children)
    {
        localNodes.emplace_back(child.second);
    }

    if (cloudParent)
    {
        for (auto* child : cloudParent->children)
        {
            remoteNodes.emplace_back(child);
        }
    }

    std::sort(fsNodes.begin(), fsNodes.end(), comparator);
    std::sort(localNodes.begin(), localNodes.end(), comparator);

    // Pair filesystem nodes with local nodes.
    {
        auto fCurr = fsNodes.begin();
        auto fEnd  = fsNodes.end();
        auto lCurr = localNodes.begin();
        auto lEnd  = localNodes.end();

        for ( ; ; )
        {
            // Determine the next filesystem node.
            auto fNext = std::upper_bound(fCurr, fEnd, *fCurr, comparator);

            // Determine the next local node.
            auto lNext = std::upper_bound(lCurr, lEnd, *lCurr, comparator);

            // By design, we should never have any conflicting local nodes.
            assert(std::distance(lCurr, lNext) < 2);

            auto *fsNode = fCurr != fEnd ? &*fCurr : nullptr;
            auto *localNode = lCurr != lEnd ? *lCurr : nullptr;

            // Bail, there's nothing left to pair.
            if (!(fsNode || localNode)) break;

            if (fsNode && localNode)
            {
                const auto relationship =
                  comparator.compare(*fsNode, *localNode);

                // Non-null entries are considered equivalent.
                if (relationship < 0)
                {
                    // Process the filesystem node first.
                    localNode = nullptr;
                }
                else if (relationship > 0)
                {
                    // Process the local node first.
                    fsNode = nullptr;
                }
            }

            // Add the pair.
            triplets.emplace_back(nullptr, localNode, fsNode);

            // Mark conflicts.
            if (fsNode && std::distance(fCurr, fNext) > 1)
            {
                triplets.back().fsNode = nullptr;

                for (auto i = fCurr; i != fNext; ++i)
                {
                    LOG_debug << "Conflicting filesystem name: "
                        << i->localname.toPath(*client->fsaccess);

                    triplets.back().fsClashingNames.push_back(&*i);

                    if (i->fsid != UNDEF && i->fsid == localNode->fsid)
                    {
                        // In case of a name clash, it might be new.
                        // Do sync the subtree we were already syncing.
                        // But also complain about the clash
                        triplets.back().fsNode = &*i;
                    }
                }
            }

            fCurr = fsNode ? fNext : fCurr;
            lCurr = localNode ? lNext : lCurr;
        }
    }

    std::sort(remoteNodes.begin(), remoteNodes.end(), comparator);
    std::sort(triplets.begin(), triplets.end(), comparator);

    // Link cloud nodes with triplets.
    {
        auto rCurr = remoteNodes.begin();
        auto rEnd = remoteNodes.end();
        size_t tCurr = 0;
        size_t tEnd = triplets.size();

        for ( ; ; )
        {
            auto rNext = std::upper_bound(rCurr, rEnd, *rCurr, comparator);
            auto tNext = tCurr;

            // Compute upper bound manually.
            for ( ; tNext != tEnd; ++tNext)
            {
                if (comparator(triplets[tCurr], triplets[tNext]))
                {
                    break;
                }
            }

            // There should never be any conflicting triplets.
            assert(tNext - tCurr < 2);

            auto* remoteNode = rCurr != rEnd ? *rCurr : nullptr;
            auto* triplet = tCurr != tEnd ? &triplets[tCurr] : nullptr;

            // Bail as there's nothing to pair.
            if (!(remoteNode || triplet)) break;

            if (remoteNode && triplet)
            {
                const auto relationship =
                  comparator.compare(*remoteNode, *triplet);

                // Non-null entries are considered equivalent.
                if (relationship < 0)
                {
                    // Process remote node first.
                    triplet = nullptr;
                }
                else if (relationship > 0)
                {
                    // Process triplet first.
                    remoteNode = nullptr;
                }
            }

            // Have we detected a remote name conflict?
            if (remoteNode && std::distance(rCurr, rNext) > 1)
            {
                for (auto i = rCurr; i != rNext; ++i)
                {
                    LOG_debug << "Conflicting cloud name: "
                        << (*i)->displaypath();

                    if (triplet)
                    {
                        triplet->cloudClashingNames.push_back(*i);

                        if ((*i)->nodehandle != UNDEF &&
                            triplet->syncNode->syncedCloudNodeHandle == (*i)->nodehandle)
                        {
                            // In case of a name clash, it might be new.
                            // Do sync the subtree we were already syncing.
                            // But also complain about the clash
                            triplet->cloudNode = *i;
                        }
                    }
                }
            }
            else if (triplet)
            {
                triplet->cloudNode = remoteNode;
            }
            else
            {
                triplets.emplace_back(remoteNode, nullptr, nullptr);
            }

            if (triplet)    tCurr = tNext;
            if (remoteNode) rCurr = rNext;
        }
    }

    return triplets;
}

bool Sync::recursiveSync(syncRow& row, LocalPath& localPath, DBTableTransactionCommitter& committer)
{
    assert(row.syncNode);
    SYNC_verbose << client->clientname << "Entering folder with syncagain=" << row.syncNode->syncAgain << " scanagain=" << row.syncNode->scanAgain << " at " << localPath.toPath(*client->fsaccess);

    // nothing to do for this subtree? Skip traversal
    if (!(row.syncNode->scanRequired() || row.syncNode->syncRequired()))
    {
        SYNC_verbose << "No syncing or scanning needed";
        return true;
    }

    // make sure any subtree flags are passed to child nodes, so we can clear the flag at this level
    for (auto& child : row.syncNode->children)
    {
        if (child.second->type != FILENODE)
        {
            child.second->scanAgain = propagateSubtreeFlag(row.syncNode->scanAgain, child.second->scanAgain);
            child.second->syncAgain = propagateSubtreeFlag(row.syncNode->syncAgain, child.second->syncAgain);
            // todo: similar for conflicts?
        }
    }

    // Whether we should perform sync actions at this level.
    bool wasSynced = row.syncNode->syncAgain < TREE_ACTION_HERE;
    bool syncHere = !wasSynced;

    vector<FSNode>* effectiveFsChildren;
    vector<FSNode> fsChildren;

    {
        // For convenience.
        LocalNode& node = *row.syncNode;

        // Do we need to scan this node?
        if (node.scanAgain >= TREE_ACTION_HERE)
        {
            client->mSyncFlags.performedScans = true;

            auto elapsed = Waiter::ds - node.lastScanTime;
            if (!mScanRequest && elapsed >= 20)
            {
                LOG_verbose << "Requesting scan for: " << localPath.toPath(*client->fsaccess);
                mScanRequest = client->mScanService->scan(node, localPath);
                syncHere = false;
            }
            else if (mScanRequest &&
                     mScanRequest->matches(node) &&
                     mScanRequest->completed())
            {
                LOG_verbose << "Received scan results for: " << localPath.toPath(*client->fsaccess);
                node.lastFolderScan.reset(
                    new vector<FSNode>(mScanRequest->results()));

                node.lastScanTime = Waiter::ds;
                mScanRequest.reset();
                row.syncNode->scanAgain = TREE_RESOLVED;
                row.syncNode->setFutureSync(true, false);
                syncHere = true;
            }
            else
            {
                syncHere = false;
            }
        }
        else
        {
            // this will be restored at the end of the function if any nodes below in the tree need it
            row.syncNode->scanAgain = TREE_RESOLVED;
        }

        // Effective children are from the last scan, if present.
        effectiveFsChildren = node.lastFolderScan.get();

        // Otherwise, we can reconstruct the filesystem entries from the LocalNodes
        if (!effectiveFsChildren)
        {
            fsChildren.reserve(node.children.size());

            for (auto &childIt : node.children)
            {
                if (childIt.second->fsid != UNDEF)
                {
                    fsChildren.emplace_back(childIt.second->getKnownFSDetails());
                }
            }

            effectiveFsChildren = &fsChildren;
        }
    }

    // Have we encountered the scan target?
    client->mSyncFlags.scanTargetReachable |= mScanRequest && mScanRequest->matches(*row.syncNode);

    // Get sync triplets.
    auto childRows = computeSyncTriplets(row.cloudNode, *row.syncNode, *effectiveFsChildren);

    bool folderSynced = syncHere;
    bool fsidsAssigned = false;
    bool subfoldersSynced = true;

    row.syncNode->conflicts = TREE_RESOLVED;

    syncHere &= !row.cloudNode || row.cloudNode->mPendingChanges.empty();

    for (unsigned firstPass = 2; firstPass--; )
    {
        for (auto& childRow : childRows)
        {
            // Skip rows that signal name conflicts.
            // Unless we were previously syncing it (ie, name clash is new)
            if (!childRow.cloudClashingNames.empty() ||
                !childRow.fsClashingNames.empty())
            {
                if (row.syncNode)
                {
                    row.syncNode->conflictDetected();
                }
                else
                {
                    continue;
                }
            }

            ScopedLengthRestore restoreLen(localPath);
            if (childRow.fsNode)
            {
                localPath.appendWithSeparator(childRow.fsNode->localname, true, client->fsaccess->localseparator);
            }
            else if (childRow.syncNode)
            {
                localPath.appendWithSeparator(childRow.syncNode->localname, true, client->fsaccess->localseparator);
            }
            else if (childRow.cloudNode)
            {
                localPath.appendWithSeparator(LocalPath::fromName(childRow.cloudNode->displayname(), *client->fsaccess, mFilesystemType), true, client->fsaccess->localseparator);
            }

            assert(!childRow.syncNode || childRow.syncNode->getLocalPath(true) == localPath);

            // Are we scanning the tree for the first time?
            if (state == SYNC_INITIALSCAN && !row.syncNode->assigned)
            {
                FSNode* fsnode = childRow.fsNode;
                LocalNode* localnode = childRow.syncNode;

                // Does this local node have a missing FSID?
                if (localnode && localnode->fsid == UNDEF)
                {
                    // Can we assign it from the paired fsnode?
                    if (fsnode && syncEqual(*fsnode, *localnode))
                    {
                        localnode->setfsid(fsnode->fsid, client->localnodeByFsid);
                        statecacheadd(localnode);

                        fsidsAssigned = true;
                    }
                }
            }

            if (firstPass)
            {
                if (syncHere)
                {
                    if (!syncItem(childRow, row, localPath, committer))
                    {
                        folderSynced = false;
                    }
                }
            }
            else
            {
                // recurse after dealing with all items, so any renames within the folder have been completed
                if (childRow.syncNode &&
                    childRow.syncNode->type == FOLDERNODE &&
                    !childRow.suppressRecursion)
                {
                    if (!recursiveSync(childRow, localPath, committer))
                    {
                        subfoldersSynced = false;
                    }
                }
            }
        }
    }

    // Record whether we performed any FSID assignments.
    row.syncNode->assigned |= fsidsAssigned;

    if (folderSynced)
    {
        // LocalNodes are now consistent with the last scan.
        row.syncNode->lastFolderScan.reset();
    }

    if (client->mSyncFlags.scansAndMovesComplete &&
        ((syncHere && folderSynced) ||
        (!syncHere && wasSynced)))
    {
        row.syncNode->syncAgain = TREE_RESOLVED;
    }

    // recompute our LocalNode flags from children
    for (auto& child : row.syncNode->children)
    {
        if (child.second->type != FILENODE)
        {
            if (row.syncNode->conflicts < TREE_ACTION_HERE)
            {
                row.syncNode->scanAgain = updateTreestateFromChild(row.syncNode->scanAgain, child.second->scanAgain);
                row.syncNode->syncAgain = updateTreestateFromChild(row.syncNode->syncAgain, child.second->syncAgain);
            }
            row.syncNode->conflicts = updateTreestateFromChild(row.syncNode->conflicts, child.second->conflicts);
        }
    }

    SYNC_verbose << client->clientname
                << "Exiting folder with synced="
                << folderSynced
                << " subsync= "
                << subfoldersSynced
                << " syncagain="
                << row.syncNode->syncAgain
                << " scanagain="
                << row.syncNode->scanAgain
                << " at "
                << localPath.toPath(*client->fsaccess);

    return folderSynced && subfoldersSynced;
}

string Sync::logTriplet(syncRow& row, LocalPath& fullPath)
{
    ostringstream s;
    s << " triplet:" <<
        " " << (row.cloudNode ? row.cloudNode->displaypath() : "(null)") <<
        " " << (row.syncNode ? row.syncNode->getLocalPath(true).toPath(*client->fsaccess) : "(null)") <<
        " " << (row.fsNode ? fullPath.toPath(*client->fsaccess) : "(null)");
    return s.str();
}

bool Sync::syncItem(syncRow& row, syncRow& parentRow, LocalPath& fullPath, DBTableTransactionCommitter& committer)
{

    // todo:  check             if (child->syncable(root))


//todo: this used to be in scan().  But now we create LocalNodes for all - shall we check it in this function
    //// check if this record is to be ignored
    //if (client->app->sync_syncable(this, name.c_str(), localPath))
    //{
    //}
    //else
    //{
    //    LOG_debug << "Excluded: " << name;
    //}

    // Under some circumstances on sync startup, our shortname records can be out of date.
    // If so, we adjust for that here, as the diretories are scanned
    if (row.syncNode && row.fsNode && row.fsNode->shortname)
    {
        if (!row.syncNode->slocalname || *row.syncNode->slocalname != *row.fsNode->shortname)
        {
            LOG_warn << "Updating slocalname: " << row.fsNode->shortname->toPath(*client->fsaccess)
                     << " at " << fullPath.toPath(*client->fsaccess)
                     << " was " << (row.syncNode->slocalname ? row.syncNode->slocalname->toPath(*client->fsaccess) : "(null)")
                     << logTriplet(row, fullPath);
            row.syncNode->setnameparent(row.syncNode->parent, nullptr, move(row.fsNode->shortname), false);
        }
    }

    if (row.syncNode)
    {
        if (row.syncNode->useBlocked >= TREE_ACTION_HERE)
        {
            if (!row.syncNode->rare().useBlockedTimer->armed())
            {
                LOG_verbose << "Waiting on use blocked timer, retry in ds: "
                            << row.syncNode->rare().useBlockedTimer->retryin()
                            << logTriplet(row, fullPath);
                return false;
            }
        }

        if (row.syncNode->scanBlocked >= TREE_ACTION_HERE)
        {
            if (row.syncNode->rare().scanBlockedTimer->armed())
            {
                LOG_verbose << "Scan blocked timer elapsed, trigger parent rescan."
                            << logTriplet(row, fullPath);
                parentRow.syncNode->setFutureScan(true, false);
            }
            else
            {
                LOG_verbose << "Waiting on scan blocked timer, retry in ds: "
                            << row.syncNode->rare().scanBlockedTimer->retryin()
                            << logTriplet(row, fullPath);
                return false;
            }
        }
    }

    // Was this sn representing a blocked file?
    if (row.syncNode && row.syncNode->type == TYPE_UNKNOWN)
    {
        // Have we been able to complete a scan for the sn?
        if (row.fsNode && row.fsNode->type != TYPE_UNKNOWN)
        {
            // Complete initialization of the sn.
            row.syncNode->init(*row.fsNode);
        }
    }

    // reset the flag for this node. Anything still blocked here or in the tree below will set it again.
    if (row.syncNode)
    {
        if (row.syncNode->useBlocked >= TREE_DESCENDANT_FLAGGED)
        {
            row.syncNode->useBlocked = TREE_RESOLVED;
            row.syncNode->rare().useBlockedTimer.reset();
        }

        if (row.syncNode->scanBlocked >= TREE_DESCENDANT_FLAGGED)
        {
            row.syncNode->scanBlocked = TREE_RESOLVED;
            row.syncNode->rare().scanBlockedTimer.reset();
        }
    }

    if (row.fsNode && (row.fsNode->type == TYPE_UNKNOWN || row.fsNode->isBlocked))
    {
        // We were not able to get details of the filesystem item when scanning the directory.
        // Consider it a blocked file, and we'll rescan the folder from time to time.
        LOG_verbose << "File/folder was blocked when reading directory, retry later: " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
        if (!row.syncNode) resolve_makeSyncNode_fromFS(row, parentRow, fullPath);
        row.syncNode->setScanBlocked();
        return false;
    }

    bool rowSynced = false;

    // First deal with detecting local moves/renames and propagating correspondingly
    // Independent of the 8 combos below so we don't have duplicate checks in those.

    if (row.fsNode && (!row.syncNode || (row.syncNode->fsid != UNDEF &&
                                         row.syncNode->fsid != row.fsNode->fsid)))
    {
        bool rowResult;
        if (checkLocalPathForMovesRenames(row, parentRow, fullPath, rowResult))
        {
            return rowResult;
        }
    }

    if (row.cloudNode && (!row.syncNode || (!row.syncNode->syncedCloudNodeHandle.isUndef() &&
        row.syncNode->syncedCloudNodeHandle.as8byte() != row.cloudNode->nodehandle)))
    {
        bool rowResult;
        if (checkCloudPathForMovesRenames(row, parentRow, fullPath, rowResult))
        {
            return rowResult;
        }
    }



    // each of the 8 possible cases of present/absent for this row
    if (row.syncNode)
    {
        if (row.fsNode)
        {
            if (row.cloudNode)
            {
                // all three exist; compare
                bool cloudEqual = syncEqual(*row.cloudNode, *row.syncNode);
                bool fsEqual = syncEqual(*row.fsNode, *row.syncNode);
                if (cloudEqual && fsEqual)
                {
                    // success! this row is synced
                    if (row.syncNode->fsid != row.fsNode->fsid ||
                        row.syncNode->syncedCloudNodeHandle != row.cloudNode->nodehandle)
                    {
                        LOG_verbose << "Row is synced, setting fsid and nodehandle" << logTriplet(row, fullPath);

                        row.syncNode->setfsid(row.fsNode->fsid, client->localnodeByFsid);
                        row.syncNode->setSyncedNodeHandle(NodeHandle().set6byte(row.cloudNode->nodehandle));

                        statecacheadd(row.syncNode);
                    }
                    else
                    {
                        SYNC_verbose << "Row was already synced" << logTriplet(row, fullPath);
                    }
                    rowSynced = true;
                }
                else if (cloudEqual)
                {
                    // filesystem changed, put the change
                    rowSynced = resolve_upsync(row, parentRow, fullPath, committer);
                }
                else if (fsEqual)
                {
                    // cloud has changed, get the change
                    rowSynced = resolve_downsync(row, parentRow, fullPath, committer, true);
                }
                else
                {
                    // both changed, so we can't decide without the user's help
                    rowSynced = resolve_userIntervention(row, parentRow, fullPath);
                }
            }
            else
            {
                // cloud item absent
                if (row.syncNode->syncedCloudNodeHandle.isUndef())
                {
                    // cloud item did not exist before; upsync
                    rowSynced = resolve_upsync(row, parentRow, fullPath, committer);
                }
                else
                {
                    // cloud item disappeared - remove locally (or figure out if it was a move, etc)
                    rowSynced = resolve_cloudNodeGone(row, parentRow, fullPath);
                }
            }
        }
        else
        {
            if (row.cloudNode)
            {
                // local item not present
                if (row.syncNode->fsid != UNDEF)
                {
                    // used to be synced - remove in the cloud (or detect move)
                    rowSynced = resolve_fsNodeGone(row, parentRow, fullPath);
                }
                else
                {
                    // fs item did not exist before; downsync
                    rowSynced = resolve_downsync(row, parentRow, fullPath, committer, false);
                }
            }
            else
            {
                // local and cloud disappeared; remove sync item also
                rowSynced = resolve_delSyncNode(row, parentRow, fullPath);
            }
        }
    }

    else
    {
        if (row.fsNode)
        {
            if (row.cloudNode)
            {
                // Item exists locally and remotely but we haven't synced them previously
                // If they are equal then join them with a Localnode. Othewise report or choose greater mtime.
                if (row.fsNode->type != row.cloudNode->type)
                {
                    rowSynced = resolve_userIntervention(row, parentRow, fullPath);
                }
                else if (row.fsNode->type != FILENODE ||
                         row.fsNode->fingerprint == *static_cast<FileFingerprint*>(row.cloudNode))
                {
                    rowSynced = resolve_makeSyncNode_fromFS(row, parentRow, fullPath);
                }
                else
                {
                    rowSynced = resolve_pickWinner(row, parentRow, fullPath);
                }
            }
            else
            {
                // Item exists locally only. Check if it was moved/renamed here, or Create
                // If creating, next run through will upload it
                rowSynced = resolve_makeSyncNode_fromFS(row, parentRow, fullPath);
            }
        }
        else
        {
            if (row.cloudNode)
            {
                // item exists remotely only
                rowSynced = resolve_makeSyncNode_fromCloud(row, parentRow, fullPath);
            }
            else
            {
                // no entries
                assert(false);
            }
        }
    }
    return rowSynced;
}


bool Sync::resolve_makeSyncNode_fromFS(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    // this really is a new node: add
    LOG_debug << "Creating LocalNode from FS at: " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
    auto l = new LocalNode;

    assert(row.syncNode == nullptr);
    row.syncNode = l;

    if (row.fsNode->type == FILENODE)
    {
        assert(row.fsNode->fingerprint.isvalid);
        *static_cast<FileFingerprint*>(l) = row.fsNode->fingerprint;
    }

    l->init(this, row.fsNode->type, parentRow.syncNode, fullPath, std::move(row.fsNode->shortname));
    l->setfsid(row.fsNode->fsid, client->localnodeByFsid);

    if (l->type != FILENODE)
    {
        l->setFutureScan(true, true);
    }

    l->treestate(TREESTATE_PENDING);
    statecacheadd(l);

    parentRow.syncNode->setFutureScan(true, false);

    return false;
}

bool Sync::resolve_makeSyncNode_fromCloud(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    LOG_debug << "Creating LocalNode from Cloud at: " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
    auto l = new LocalNode;

    if (row.cloudNode->type == FILENODE)
    {
        assert(row.cloudNode->fingerprint().isvalid);
        *static_cast<FileFingerprint*>(l) = row.cloudNode->fingerprint();
    }
    l->init(this, row.cloudNode->type, parentRow.syncNode, fullPath, nullptr);
    l->setSyncedNodeHandle(NodeHandle().set6byte(row.cloudNode->nodehandle));
    l->treestate(TREESTATE_PENDING);
    if (l->type != FILENODE)
    {
        l->setFutureScan(true, true);
    }
    parentRow.syncNode->setFutureScan(true, false);
    statecacheadd(l);
    return false;
}

bool Sync::resolve_delSyncNode(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    if (client->mSyncFlags.scansAndMovesComplete)
    {
        // local and cloud disappeared; remove sync item also
        LOG_verbose << "Marking Localnode for deletion" << logTriplet(row, fullPath);

        // deletes itself and subtree, queues db record removal
        delete row.syncNode;
        row.syncNode = nullptr;
    }
    return false;
}

bool Sync::resolve_upsync(syncRow& row, syncRow& parentRow, LocalPath& fullPath, DBTableTransactionCommitter& committer)
{
    if (row.fsNode->type == FILENODE)
    {
        // upload the file if we're not already uploading
        if (!row.syncNode->transfer)
        {
            if (parentRow.cloudNode)
            {
                LOG_debug << "Uploading file " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
                assert(row.syncNode->isvalid); // LocalNodes for files always have a valid fingerprint
                row.syncNode->h = parentRow.cloudNode->nodehandle;
                client->nextreqtag();
                client->startxfer(PUT, row.syncNode, committer);  // full path will be calculated in the prepare() callback
                client->app->syncupdate_put(this, row.syncNode, fullPath.toPath(*client->fsaccess).c_str());
            }
            else
            {
                LOG_verbose << "Parent cloud folder to upload to doesn't exist yet" << logTriplet(row, fullPath);
            }
        }
        else
        {
            LOG_verbose << "Upload already in progress" << logTriplet(row, fullPath);
        }
    }
    else
    {
        LOG_verbose << "Creating cloud node for: " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
        // while the operation is in progress sync() will skip over the parent folder
        vector<NewNode> nn(1);
        client->putnodes_prepareOneFolder(&nn[0], row.syncNode->name);
        client->putnodes(parentRow.cloudNode->nodehandle, move(nn), nullptr, 0);
    }
    return false;
}

bool Sync::resolve_downsync(syncRow& row, syncRow& parentRow, LocalPath& fullPath, DBTableTransactionCommitter& committer, bool alreadyExists)
{
    if (row.cloudNode->type == FILENODE)
    {
        // download the file if we're not already downloading
        // if (alreadyExists), we will move the target to the trash when/if download completes //todo: check
        if (!row.cloudNode->syncget)
        {
            // FIXME: to cover renames that occur during the
            // download, reconstruct localname in complete()
            LOG_debug << "Start fetching file node";
            client->app->syncupdate_get(this, row.cloudNode, fullPath.toPath(*client->fsaccess).c_str());

            row.cloudNode->syncget = new SyncFileGet(this, row.cloudNode, fullPath);
            client->nextreqtag();
            client->startxfer(GET, row.cloudNode->syncget, committer);

            if (row.syncNode) row.syncNode->treestate(TREESTATE_SYNCING);
            else if (parentRow.syncNode) parentRow.syncNode->treestate(TREESTATE_SYNCING);
        }
        else
        {
            LOG_verbose << "Download already in progress" << logTriplet(row, fullPath);
        }
    }
    else
    {
        assert(!alreadyExists); // if it did we would have matched it

        LOG_verbose << "Creating local folder at: " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);

        if (client->fsaccess->mkdirlocal(fullPath))
        {
            assert(row.syncNode);
            parentRow.syncNode->setFutureScan(true, false);
        }
        else if (client->fsaccess->transient_error)
        {
            LOG_debug << "Transient error creating folder, marking as blocked " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
            assert(row.syncNode);
            row.syncNode->setUseBlocked();
        }
        else // !transient_error
        {
            // let's consider this case as blocked too, alert the user
            LOG_debug << "Non transient error creating folder, marking as blocked " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
            assert(row.syncNode);
            row.syncNode->setUseBlocked();
        }
    }
    return false;
}


bool Sync::resolve_userIntervention(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    LOG_debug << "write me" << logTriplet(row, fullPath);
    return false;
}

bool Sync::resolve_pickWinner(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    LOG_debug << "write me" << logTriplet(row, fullPath);
    return false;
}

bool Sync::resolve_cloudNodeGone(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    if (client->mSyncFlags.scansAndMovesComplete)
    {
        // If the cloud node was moved to somewhere we can see it, we would have already made the corresponding fs move
        LOG_debug << client->clientname << "Moving local item to local sync debris: " << fullPath.toPath(*client->fsaccess) << logTriplet(row, fullPath);
        if (movetolocaldebris(fullPath))
        {
            row.suppressRecursion = true;
            parentRow.syncNode->setFutureScan(true, false);
        }
        else
        {
            LOG_err << "Failed to move to local debris:  " << fullPath.toPath(*client->fsaccess);
        }
    }
    return false;
}

LocalNode* MegaClient::findLocalNodeByFsid(FSNode& fsNode, Sync& filesystemSync)
{
    if (fsNode.fsid == UNDEF) return nullptr;

    auto range = localnodeByFsid.equal_range(fsNode.fsid);

    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second->type != fsNode.type) continue;

        // make sure we are in the same filesystem (fsid comparison is not valid in other filesystems)
        if (it->second->sync != &filesystemSync)
        {
            continue;
        }

        auto fp1 = it->second->sync->dirnotify->fsfingerprint();
        auto fp2 = filesystemSync.dirnotify->fsfingerprint();
        if (!fp1 || !fp2 || fp1 != fp2)
        {
            continue;
        }

#ifdef _WIN32
        // (from original sync code) Additionally for windows, check drive letter
        // only consider fsid matches between different syncs for local drives with the
        // same drive letter, to prevent problems with cloned Volume IDs
        if (it->second->sync->localroot->localname.driveLetter() !=
            filesystemSync.localroot->localname.driveLetter())
        {
            continue;
        }
#endif
        if (fsNode.type == FILENODE &&
            (fsNode.mtime != it->second->mtime ||
                fsNode.size != it->second->size))
        {
            // fsid match, but size or mtime mismatch
            // treat as different
            continue;
        }

        // If we got this far, it's a good enough match to use
        // todo: come back for other matches?
        return it->second;
    }
    return nullptr;
}

LocalNode* MegaClient::findLocalNodeByNodeHandle(NodeHandle h)
{
    if (h.isUndef()) return nullptr;

    auto range = localnodeByNodeHandle.equal_range(h);

    for (auto it = range.first; it != range.second; ++it)
    {
        // check the file/folder actually exists on disk for this LocalNode
        LocalPath lp = it->second->getLocalPath(true);

        auto prevfa = fsaccess->newfileaccess(false);
        bool exists = prevfa->fopen(lp);
        if (exists || prevfa->type == FOLDERNODE)
        {
            return it->second;
        }
    }
    return nullptr;
}



bool MegaClient::checkIfFileIsChanging(FSNode& fsNode, const LocalPath& fullPath)
{
    // logic to prevent moving files that may still be being updated

    // (original sync code comment:)
    // detect files being updated in the local computer moving the original file
    // to another location as a temporary backup

    assert(fsNode.type == FILENODE);

    bool waitforupdate = false;
    FileChangingState& state = mFileChangingCheckState[fullPath];

    m_time_t currentsecs = m_time();
    if (!state.updatedfileinitialts)
    {
        state.updatedfileinitialts = currentsecs;
    }

    if (currentsecs >= state.updatedfileinitialts)
    {
        if (currentsecs - state.updatedfileinitialts <= Sync::FILE_UPDATE_MAX_DELAY_SECS)
        {
            auto prevfa = fsaccess->newfileaccess(false);

            bool exists = prevfa->fopen(fullPath);
            if (exists)
            {
                LOG_debug << "File detected in the origin of a move";

                if (currentsecs >= state.updatedfilets)
                {
                    if ((currentsecs - state.updatedfilets) < (Sync::FILE_UPDATE_DELAY_DS / 10))
                    {
                        LOG_verbose << "currentsecs = " << currentsecs << "  lastcheck = " << state.updatedfilets
                            << "  currentsize = " << prevfa->size << "  lastsize = " << state.updatedfilesize;
                        LOG_debug << "The file was checked too recently. Waiting...";
                        waitforupdate = true;
                    }
                    else if (state.updatedfilesize != prevfa->size)
                    {
                        LOG_verbose << "currentsecs = " << currentsecs << "  lastcheck = " << state.updatedfilets
                            << "  currentsize = " << prevfa->size << "  lastsize = " << state.updatedfilesize;
                        LOG_debug << "The file size has changed since the last check. Waiting...";
                        state.updatedfilesize = prevfa->size;
                        state.updatedfilets = currentsecs;
                        waitforupdate = true;
                    }
                    else
                    {
                        LOG_debug << "The file size seems stable";
                    }
                }
                else
                {
                    LOG_warn << "File checked in the future";
                }

                if (!waitforupdate)
                {
                    if (currentsecs >= prevfa->mtime)
                    {
                        if (currentsecs - prevfa->mtime < (Sync::FILE_UPDATE_DELAY_DS / 10))
                        {
                            LOG_verbose << "currentsecs = " << currentsecs << "  mtime = " << prevfa->mtime;
                            LOG_debug << "File modified too recently. Waiting...";
                            waitforupdate = true;
                        }
                        else
                        {
                            LOG_debug << "The modification time seems stable.";
                        }
                    }
                    else
                    {
                        LOG_warn << "File modified in the future";
                    }
                }
            }
            else
            {
                if (prevfa->retry)
                {
                    LOG_debug << "The file in the origin is temporarily blocked. Waiting...";
                    waitforupdate = true;
                }
                else
                {
                    LOG_debug << "There isn't anything in the origin path";
                }
            }

            if (waitforupdate)
            {
                LOG_debug << "Possible file update detected.";
                return NULL;
            }
        }
        else
        {
            sendevent(99438, "Timeout waiting for file update", 0);
        }
    }
    else
    {
        LOG_warn << "File check started in the future";
    }

    if (!waitforupdate)
    {
        mFileChangingCheckState.erase(fullPath);
    }
    return waitforupdate;
}

bool Sync::resolve_fsNodeGone(syncRow& row, syncRow& parentRow, LocalPath& fullPath)
{
    if (client->mSyncFlags.scansAndMovesComplete)
    {
        if (!row.syncNode->deleting)
        {
            LOG_debug << "Moving cloud item to cloud sync debris: " << row.cloudNode->displaypath() << logTriplet(row, fullPath);
            client->movetosyncdebris(row.cloudNode, inshare);
            row.syncNode->deleting = true;
        }
    }

    if (row.syncNode->deleting)
    {
        row.suppressRecursion = true;
    }

    return false;
}

bool Sync::syncEqual(const Node& n, const LocalNode& ln)
{
    // Assuming names already match
    // Not comparing nodehandle here.  If they all match we set syncedCloudNodeHandle
    if (n.type != ln.type) return false;
    if (n.type != FILENODE) return true;
    assert(n.fingerprint().isvalid && ln.fingerprint().isvalid);
    return n.fingerprint() == ln.fingerprint();  // size, mtime, crc
}

bool Sync::syncEqual(const FSNode& fsn, const LocalNode& ln)
{
    // Assuming names already match
    // Not comparing fsid here. If they all match then we set LocalNode's fsid
    if (fsn.type != ln.type) return false;
    if (fsn.type != FILENODE) return true;
    assert(fsn.fingerprint.isvalid && ln.fingerprint().isvalid);
    return fsn.fingerprint == ln.fingerprint();  // size, mtime, crc
}


} // namespace
#endif
