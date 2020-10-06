
/**
 * @file mega/node.h
 * @brief Classes for accessing local and remote nodes
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

#ifndef MEGA_NODE_H
#define MEGA_NODE_H 1

#include "filefingerprint.h"
#include "file.h"
#include "attrmap.h"

namespace mega {

struct LocalPathPtrCmp
{
    bool operator()(const LocalPath* a, const LocalPath* b) const
    {
        return *a < *b;
    }
};

typedef map<const LocalPath*, LocalNode*, LocalPathPtrCmp> localnode_map;
typedef map<const string*, Node*, StringCmp> remotenode_map;

using name_localnode_map = map<const string*, LocalNode*, NamePtrCmp>;
using name_remotenode_map = map<const string*, Node*, NamePtrCmp>;

struct MEGA_API NodeCore
{
    // node's own handle
    handle nodehandle = UNDEF;

    // parent node handle (in a Node context, temporary placeholder until parent is set)
    handle parenthandle = UNDEF;

    // node type
    nodetype_t type = TYPE_UNKNOWN;

    // node attributes
    std::unique_ptr<string> attrstring;
};

// new node for putnodes()
struct MEGA_API NewNode : public NodeCore
{
    static const int OLDUPLOADTOKENLEN = 27;
    static const int UPLOADTOKENLEN = 36;

    string nodekey;

    newnodesource_t source = NEW_NODE;

    handle ovhandle = UNDEF;
    handle uploadhandle = UNDEF;
    byte uploadtoken[UPLOADTOKENLEN]{};

    handle syncid = UNDEF;
#ifdef ENABLE_SYNC
    crossref_ptr<LocalNode, NewNode> localnode; // non-owning
#endif
    std::unique_ptr<string> fileattributes;

    bool added = false;           // set true when the actionpacket arrives
    handle mAddedHandle = UNDEF;  // updated as actionpacket arrives
    error mError = API_OK;        // per-node error (updated in cs response)
};

struct MEGA_API PublicLink
{
    handle ph;
    m_time_t cts;
    m_time_t ets;
    bool takendown;

    PublicLink(handle ph, m_time_t cts, m_time_t ets, bool takendown);
    PublicLink(PublicLink *plink);

    bool isExpired();
};

// Container storing FileFingerprint* (Node* in practice) ordered by fingerprint.
struct Fingerprints
{
    // maps FileFingerprints to node
    using fingerprint_set = std::multiset<FileFingerprint*, FileFingerprintCmp>;
    using iterator = fingerprint_set::iterator;

    void newnode(Node* n);
    void add(Node* n);
    void remove(Node* n);
    void clear();
    m_off_t getSumSizes();

    Node* nodebyfingerprint(FileFingerprint* fingerprint);
    node_vector *nodesbyfingerprint(FileFingerprint* fingerprint);

private:
    fingerprint_set mFingerprints;
    m_off_t mSumSizes = 0;
};

struct CommandChain
{
    // convenience functions, hides the unique_ptr aspect, removes it when empty
    bool empty()
    {
        return !chain || chain->empty();
    }

    void push_back(Command* c)
    {
        if (!chain)
        {
            chain.reset(new std::list<Command*>);
        }
        chain->push_back(c);
    }

    void erase(Command* c)
    {
        if (chain)
        {
            for (auto i = chain->begin(); i != chain->end(); ++i)
            {
                if (*i == c)
                {
                    chain->erase(i);
                    if (chain->empty())
                    {
                        chain.reset();
                    }
                    return;
                }
            }
        }
    }

private:
    friend class CommandSetAttr;

    // most nodes don't have commands in progress so keep representation super small
    std::unique_ptr<std::list<Command*>> chain;
};


// filesystem node
struct MEGA_API Node : public NodeCore, FileFingerprint
{
    MegaClient* client = nullptr;

    // supplies the nodekey (which is private to ensure we track changes to it)
    const string& nodekey() const;

    // Also returns the key but does not assert that the key has been applied.  Only use it where we don't need the node to be readable.
    const string& nodekeyUnchecked() const;

    // check if the key is present and is the correct size for this node
    bool keyApplied() const;

    // change parent node association
    bool setparent(Node*);

    // follow the parent links all the way to the top
    const Node* firstancestor() const;

    // try to resolve node key string
    bool applykey();

    // set up nodekey in a static SymmCipher
    SymmCipher* nodecipher();

    // decrypt attribute string and set fileattrs
    void setattr();

    // display name (UTF-8)
    const char* displayname() const;

    // check if the name matches (UTF-8)
    bool hasName(const string&) const;

    // display path from its root in the cloud (UTF-8)
    string displaypath() const;

    // node attributes
    AttrMap attrs;

    // track upcoming attribute changes for this node, so we can reason about current vs future state
    CommandChain mPendingChanges;

    // owner
    handle owner = mega::UNDEF;

    // actual time this node was created (cannot be set by user)
    m_time_t ctime = 0;

    // file attributes
    string fileattrstring;

    // check presence of file attribute
    int hasfileattribute(fatype) const;
    static int hasfileattribute(const string *fileattrstring, fatype);

    // decrypt node attribute string
    static byte* decryptattr(SymmCipher*, const char*, size_t);

    // parse node attributes from an incoming buffer, this function must be called after call decryptattr
    static void parseattr(byte*, AttrMap&, m_off_t, m_time_t&, string&, string&, FileFingerprint&);

    // inbound share
    Share* inshare = nullptr;

    // outbound shares by user
    share_map* outshares = nullptr;

    // outbound pending shares
    share_map* pendingshares = nullptr;

    // incoming/outgoing share key
    SymmCipher* sharekey = nullptr;

    // app-private pointer
    void* appdata = nullptr;

    bool foreignkey = false;

    struct
    {
        bool removed : 1;
        bool attrs : 1;
        bool owner : 1;
        bool ctime : 1;
        bool fileattrstring : 1;
        bool inshare : 1;
        bool outshares : 1;
        bool pendingshares : 1;
        bool parent : 1;
        bool publiclink : 1;
        bool newnode : 1;
    } changed;

    void setkey(const byte* = NULL);

    void setkeyfromjson(const char*);

    void setfingerprint();

    void faspec(string*);

    NodeCounter subnodeCounts() const;

    // parent
    Node* parent = nullptr;

    // children
    node_list children;

    // own position in parent's children
    node_list::iterator child_it;

    // own position in fingerprint set (only valid for file nodes)
    Fingerprints::iterator fingerprint_it;

#ifdef ENABLE_SYNC
    // related synced item or NULL
    //LocalNode* localnode = nullptr;

    // active sync get
    struct SyncFileGet* syncget = nullptr;

    // state of removal to //bin / SyncDebris
    syncdel_t syncdeleted = SYNCDEL_NONE;

    // location in the todebris node_set
    node_set::iterator todebris_it;

    // location in the tounlink node_set
    // FIXME: merge todebris / tounlink
    node_set::iterator tounlink_it;
#endif

    // source tag.  The tag of the request or transfer that last modified this node (available in MegaApi)
    int tag = 0;

    // check if node is below this node
    bool isbelow(Node*) const;

    // handle of public link for the node
    PublicLink* plink = nullptr;

    void setpubliclink(handle, m_time_t, m_time_t, bool);

    bool serialize(string*) override;
    static Node* unserialize(MegaClient*, const string*, node_vector*);

    Node(MegaClient*, vector<Node*>*, handle, handle, nodetype_t, m_off_t, handle, const char*, m_time_t);
    ~Node();

    // Retrieve the node's canonical name.
    string canonicalname() const;

    // Retrieve the node's name.
    string name() const;

#ifdef ENABLE_SYNC

    //// Detach this remote from it's local associate.
    //void detach(const bool recreate = false);

    // True if this node is syncable.
    //
    // That is, the node must:
    // - Be alive.
    // - Be decrypted.
    // - Be named.
    // - Not be the debris folder.
    bool syncable(const LocalNode& parent) const;

#endif /* ENABLE_SYNC */

private:
    // full folder/file key, symmetrically or asymmetrically encrypted
    // node crypto keys (raw or cooked -
    // cooked if size() == FOLDERNODEKEYLENGTH or FILEFOLDERNODEKEYLENGTH)
    string nodekeydata;
};

inline const string& Node::nodekey() const
{
    assert(keyApplied() || type == ROOTNODE || type == INCOMINGNODE || type == RUBBISHNODE);
    return nodekeydata;
}

inline const string& Node::nodekeyUnchecked() const
{
    return nodekeydata;
}

inline bool Node::keyApplied() const
{
    return nodekeydata.size() == size_t((type == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH);
}

#ifdef ENABLE_SYNC

struct MEGA_API FSNode
{
    // A structure convenient for containing just the attributes of one item from the filesystem
    LocalPath localname;
    string name;
    unique_ptr<LocalPath> shortname;
    nodetype_t type = TYPE_UNKNOWN;
    m_off_t size = 0;
    m_time_t mtime = 0;
    mega::handle fsid = mega::UNDEF;
    bool isSymlink = false;
    bool isBlocked = false;
    FileFingerprint fingerprint;
};

enum TREESTATE : unsigned
{
    TREE_RESOLVED = 0,
    TREE_DESCENDANT_FLAGGED = 1,
    TREE_ACTION_HERE = 2,       // but do check if any children have flags set
    TREE_ACTION_SUBTREE = 3   // overrides any children so the whole subtree is processed
};

inline unsigned updateTreestateFromChild(unsigned oldFlag, unsigned childFlag)
{
    return oldFlag == TREE_RESOLVED && childFlag != TREE_RESOLVED ? TREE_DESCENDANT_FLAGGED : oldFlag;
}

inline unsigned propagateSubtreeFlag(unsigned nodeFlag, unsigned childFlag)
{
    return nodeFlag == TREE_ACTION_SUBTREE ? TREE_ACTION_SUBTREE : childFlag;
}



struct MEGA_API LocalNode : public File
{
    class Sync* sync = nullptr;

    // parent linkage
    LocalNode* parent = nullptr;

    // stored to rebuild tree after serialization => this must not be a pointer to parent->dbid
    int32_t parent_dbid = 0;

    // whether this node can be synced to the remote tree
    bool mSyncable = true;

    // whether this node knew its shortname (otherwise it was loaded from an old db)
    bool slocalname_in_db = false;

    // children by name
    localnode_map children;

    // for botched filesystems with legacy secondary ("short") names
    // Filesystem notifications could arrive with long or short names, and we need to recognise which LocalNode corresponds.
    std::unique_ptr<LocalPath> slocalname;   // null means either the entry has no shortname or it's the same as the (normal) longname
    localnode_map schildren;

    // The last scan of the folder (for folders).
    // Removed again when the folder is fully synced.
    std::unique_ptr<vector<FSNode>> lastFolderScan;

    // local filesystem node ID (inode...) for rename/move detection
    handle fsid = mega::UNDEF;
    fsid_localnode_map::iterator fsid_it;

    // related cloud node, if any
    //Node* node = nullptr;
    NodeHandle syncedCloudNodeHandle;
    nodehandle_localnode_map::iterator syncedCloudNodeHandle_it;

    // related pending node creation or NULL
    crossref_ptr<NewNode, LocalNode> newnode;

    // FILENODE or FOLDERNODE
    nodetype_t type = TYPE_UNKNOWN;

    // detection of deleted filesystem records
    int scanseqno = 0;

    // number of iterations since last seen
    int notseen = 0;

    // global sync reference
    handle syncid = mega::UNDEF;

    struct
    {
        // fsids have been assigned in this node.
        bool assigned : 1;

        // disappeared from local FS; we are moving the cloud node to the trash.
        bool deleting : 1;

        // has been created remotely
        bool created : 1;

        // an issue has been reported
        bool reported : 1;

        // checked for missing attributes
        bool checked : 1;

        // whether any name conflicts have been detected.
        unsigned conflicts : 2;   // TREESTATE

        // needs another sync() at this level after pending changes
        unsigned syncAgain : 2;   // TREESTATE

        // needs another scan() (and sync() by implication) at this level after pending changes
        unsigned scanAgain : 2;    // TREESTATE

        // whether this file/folder is blocked - now we can have many at once
        unsigned useBlocked : 2;    // TREESTATE
        unsigned scanBlocked : 2;    // TREESTATE

    };

    // Fields which are hardly ever used.
    // We keep the average memory use by only alloating these when used.
    struct RareFields
    {
        unique_ptr<BackoffTimer> useBlockedTimer;
        unique_ptr<BackoffTimer> scanBlockedTimer;
    };

private:
    unique_ptr<RareFields> rareFields;
public:
    RareFields& rare();

    dstime lastScanTime = 0;

    // set the syncupTargetedAction for this, and parents
    void setFutureSync(bool doHere, bool doBelow);
    void setFutureScan(bool doHere, bool doBelow);
    void setUseBlocked();
    void setScanBlocked();

    // True if this subtree requires scanning.
    bool scanRequired() const;

    // True if this subtree requires syncing.
    bool syncRequired() const;

    // current subtree sync state: current and displayed
    treestate_t ts = TREESTATE_NONE;
    treestate_t dts = TREESTATE_NONE;

    // update sync state all the way to the root node
    void treestate(treestate_t = TREESTATE_NONE);

    // check the current state (only useful for folders)
    treestate_t checkstate();

    // timer to delay upload start
    dstime nagleds = 0;
    void bumpnagleds();

    // if delage > 0, own iterator inside MegaClient::localsyncnotseen
    localnode_set::iterator notseen_it{};

    // build full local path to this node
    void getlocalpath(LocalPath&, bool sdisable = false) const;
    LocalPath getLocalPath(bool sdisable = false) const;
    string localnodedisplaypath(FileSystemAccess& fsa) const;

    // return child node by name
    LocalNode* childbyname(LocalPath*);

    FSNode getKnownFSDetails();

#ifdef USE_INOTIFY
    // node-specific DirNotify tag
    handle dirnotifytag = mega::UNDEF;
#endif

    void prepare() override;
    void completed(Transfer*, LocalNode*) override;

    //void setnode(Node*);

    void setnotseen(int);

    void setfsid(handle newfsid, fsid_localnode_map& fsidnodes);

    void setSyncedNodeHandle(NodeHandle h);

    void setnameparent(LocalNode*, LocalPath* newlocalpath, std::unique_ptr<LocalPath>, bool applyToCloud);

    LocalNode();
    void init(Sync*, nodetype_t, LocalNode*, LocalPath&, std::unique_ptr<LocalPath>);

    // Reinitialize an UNKNOWN node.
    void init(const FSNode& fsNode);

    bool serialize(string*) override;
    static LocalNode* unserialize( Sync* sync, const string* sData );

    ~LocalNode();

    // Update this node's conflict state.
    void conflictDetected(const TREESTATE conflicts);

    // Signal that a name conflict has been detected in this node.
    void conflictDetected();

    // Propagate our conflict state to our parents.
    void conflictRefresh();

    // True if any name conflicts have been detected in this subtree.
    bool conflictsDetected() const;

    // True if any conflicts have been detected in any of our children.
    bool conflictsDetectedBelow() const;

    // True if any conflicts have been detected by this node.
    bool conflictsDetectedHere() const;

    // Clears this node's conflict detection state.
    void conflictsResolved();

    //// Detach this node from it's remote associate.
    //void detach(const bool recreate = false);

    // Are we above other?
    bool isAbove(const LocalNode& other) const;

    // Are we below other?
    bool isBelow(const LocalNode& other) const;
};

template <> inline NewNode*& crossref_other_ptr_ref<LocalNode, NewNode>(LocalNode* p) { return p->newnode.ptr; }
template <> inline LocalNode*& crossref_other_ptr_ref<NewNode, LocalNode>(NewNode* p) { return p->localnode.ptr; }

#endif

} // namespace



#endif
