@0xb87a2e0b5c4f3d21;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("raftpp::capnp");

enum EntryType {
    entryNormal @0;
    entryConfChange @1;
    entryConfChangeV2 @2;
}

# The entry is a type of change that needs to be applied. It contains two data fields.
# While the fields are built into the model; their usage is determined by the entryType.
#
# For normal entries, the data field should contain the data change that should be applied.
# The context field can be used for any contextual data that might be relevant to the
# application of the data.
#
# For configuration changes, the data will contain the ConfChange message and the
# context will provide anything needed to assist the configuration change. The context
# is for the user to set and use in this case.
struct Entry {
    entryType @0 :EntryType;
    term @1 :UInt64;
    index @2 :UInt64;
    data @3 :Data;
    context @4 :Data;
}

struct SnapshotMetadata {
    # The current ConfState.
    confState @0 :ConfState;
    # The applied index.
    index @1 :UInt64;
    # The term of the applied index.
    term @2 :UInt64;
}

struct Snapshot {
    data @0 :Data;
    metadata @1 :SnapshotMetadata;
}

enum MessageType {
    msgHup @0;
    msgBeat @1;
    msgPropose @2;
    msgAppend @3;
    msgAppendResponse @4;
    msgRequestVote @5;
    msgRequestVoteResponse @6;
    msgSnapshot @7;
    msgHeartbeat @8;
    msgHeartbeatResponse @9;
    msgUnreachable @10;
    msgSnapStatus @11;
    msgCheckQuorum @12;
    msgTransferLeader @13;
    msgTimeoutNow @14;
    msgReadIndex @15;
    msgReadIndexResp @16;
    msgRequestPreVote @17;
    msgRequestPreVoteResponse @18;
}

struct Message {
    msgType @0 :MessageType;
    to @1 :UInt64;
    from @2 :UInt64;
    term @3 :UInt64;
    # logTerm is generally used for appending Raft logs to followers. For example,
    # (type=MsgAppend,index=100,logTerm=5) means leader appends entries starting at
    # index=101, and the term of entry at index 100 is 5.
    # (type=MsgAppendResponse,reject=true,index=100,logTerm=5) means follower rejects some
    # entries from its leader as it already has an entry with term 5 at index 100.
    logTerm @4 :UInt64;
    index @5 :UInt64;
    entries @6 :List(Entry);
    commit @7 :UInt64;
    commitTerm @8 :UInt64;
    snapshot @9 :Snapshot;
    requestSnapshot @10 :UInt64;
    reject @11 :Bool;
    rejectHint @12 :UInt64;
    context @13 :Data;
    # If this new field is not set, then use the above old field; otherwise
    # use the new field. When broadcasting request vote, both fields are
    # set if the priority is larger than 0. This change is not a fully
    # compatible change, but it makes minimal impact that only new priority
    # is not recognized by the old nodes during rolling update.
    priority @14 :Int64;
}

struct HardState {
    term @0 :UInt64;
    vote @1 :UInt64;
    commit @2 :UInt64;
}

enum ConfChangeTransition {
    # Automatically use the simple protocol if possible, otherwise fall back
    # to ConfChangeType::Implicit. Most applications will want to use this.
    auto @0;
    # Use joint consensus unconditionally, and transition out of them
    # automatically (by proposing a zero configuration change).
    #
    # This option is suitable for applications that want to minimize the time
    # spent in the joint configuration and do not store the joint configuration
    # in the state machine (outside of InitialState).
    implicit @1;
    # Use joint consensus and remain in the joint configuration until the
    # application proposes a no-op configuration change. This is suitable for
    # applications that want to explicitly control the transitions, for example
    # to use a custom payload (via the Context field).
    explicit @2;
}

struct ConfState {
    voters @0 :List(UInt64);
    learners @1 :List(UInt64);
    # The voters in the outgoing config. If not empty the node is in joint consensus.
    votersOutgoing @2 :List(UInt64);
    # The nodes that will become learners when the outgoing config is removed.
    # These nodes are necessarily currently in nodes_joint (or they would have
    # been added to the incoming config right away).
    learnersNext @3 :List(UInt64);
    # If set, the config is joint and Raft will automatically transition into
    # the final config (i.e. remove the outgoing config) when this is safe.
    autoLeave @4 :Bool;
}

enum ConfChangeType {
    addNode @0;
    removeNode @1;
    addLearnerNode @2;
}

struct ConfChange {
    changeType @0 :ConfChangeType;
    nodeId @1 :UInt64;
    context @2 :Data;
    id @3 :UInt64;
}

# ConfChangeSingle is an individual configuration change operation. Multiple
# such operations can be carried out atomically via a ConfChangeV2.
struct ConfChangeSingle {
    changeType @0 :ConfChangeType;
    nodeId @1 :UInt64;
}

# ConfChangeV2 messages initiate configuration changes. They support both the
# simple "one at a time" membership change protocol and full Joint Consensus
# allowing for arbitrary changes in membership.
#
# The supplied context is treated as an opaque payload and can be used to
# attach an action on the state machine to the application of the config change
# proposal. Note that contrary to Joint Consensus as outlined in the Raft
# paper[1], configuration changes become active when they are *applied* to the
# state machine (not when they are appended to the log).
#
# The simple protocol can be used whenever only a single change is made.
#
# Non-simple changes require the use of Joint Consensus, for which two
# configuration changes are run. The first configuration change specifies the
# desired changes and transitions the Raft group into the joint configuration,
# in which quorum requires a majority of both the pre-changes and post-changes
# configuration. Joint Consensus avoids entering fragile intermediate
# configurations that could compromise survivability. For example, without the
# use of Joint Consensus and running across three availability zones with a
# replication factor of three, it is not possible to replace a voter without
# entering an intermediate configuration that does not survive the outage of
# one availability zone.
#
# The provided ConfChangeTransition specifies how (and whether) Joint Consensus
# is used, and assigns the task of leaving the joint configuration either to
# Raft or the application. Leaving the joint configuration is accomplished by
# proposing a ConfChangeV2 with only and optionally the Context field
# populated.
#
# For details on Raft membership changes, see:
#
# [1]: https://github.com/ongardie/dissertation/blob/master/online-trim.pdf
struct ConfChangeV2 {
    transition @0 :ConfChangeTransition;
    changes @1 :List(ConfChangeSingle);
    context @2 :Data;
}

enum CompressionType {
    compressionNone @0;
    compressionLz4 @1;
    compressionZstd @2;
}

# RPC header for message framing
struct RpcHeader {
    # Protocol version for compatibility checking
    version @0 :UInt32;
    # Source node ID
    fromNode @1 :UInt64;
    # Destination node ID (0 for broadcast or unknown)
    toNode @2 :UInt64;
    # Request ID for request/response correlation
    requestId @3 :UInt64;
    # Compression type for payload
    compression @4 :CompressionType;
    # Size of the payload in bytes
    payloadSize @5 :UInt32;
    # Message type hint (allows routing without parsing payload)
    msgType @6 :MessageType;
}

# Handshake message for connection establishment
struct RpcHandshake {
    # Protocol version
    version @0 :UInt32;
    # Node ID of the sender
    nodeId @1 :UInt64;
    # Cluster ID for isolation (optional)
    clusterId @2 :UInt64;
}

# Cap'n Proto RPC interface
interface RaftTransport {
    # Send batch of messages to a peer
    sendMessages @0 (messages :List(Message)) -> ();

    # Receive snapshot data (streaming)
    sendSnapshot @1 (snapshot :Snapshot) -> ();
}
