// Copyright 2017 The etcd Authors
// Copyright 2017 Wu Tao
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "raft.h"
#include "memory_storage.h"
#include "test_utils.h"

#include <gtest/gtest.h>

namespace yaraft {

class RaftTest {
 public:
  // Ensure that the Step function ignores the message from old term and does not pass it to the
  // actual stepX function.
  static void TestStepIgnoreOldTermMsg() {
    RaftUPtr raft(newTestRaft(1, {1}, 10, 1, new MemoryStorage()));

    bool called = false;
    raft->step_ = [&](const pb::Message& m) { called = true; };

    raft->currentTerm_ = 2;

    pb::Message m;
    m.set_term(raft->Term() - 1);
    m.set_type(pb::MsgApp);
    raft->Step(m);
    ASSERT_FALSE(called);
  }

  // TestHandleMsgApp ensures:
  // 1. Reply false if log doesn’t contain an entry at prevLogIndex whose term matches prevLogTerm.
  // 2. If an existing entry conflicts with a new one (same index but different terms),
  //    delete the existing entry and all that follow it; append any new entries not already in the
  //    log.
  // 3. If leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new entry).
  static void TestHandleMsgApp() {
    struct TestData {
      uint64_t prevLogIndex, prevLogTerm;
      uint64_t commit;
      EntryVec ents;

      uint64_t wIndex;
      uint64_t wCommit;
      bool wReject;
    } tests[] = {
        // Ensure 1
        // previous log mismatch
        {3, 2, 3, {}, 2, 0, true},
        // previous log non-exist
        {3, 3, 3, {}, 2, 0, true},

        // Ensure 2
        {1, 1, 1, {}, 2, 1, false},
        {0, 0, 1, {pbEntry(1, 2)}, 1, 1, false},
        {2, 2, 3, {pbEntry(3, 2), pbEntry(4, 2)}, 4, 3, false},
        {2, 2, 4, {pbEntry(3, 2)}, 3, 3, false},
        {1, 1, 4, {pbEntry(2, 2)}, 2, 2, false},

        // Ensure 3
        // match entry 1, commit up to last new entry 1
        {1, 1, 3, {}, 2, 1, false},
        // match entry 1, commit up to last new entry 2
        {1, 1, 3, {pbEntry(2, 2)}, 2, 2, false},
        // match entry 2, commit up to last new entry 2
        {2, 2, 3, {}, 2, 2, false},
        // commit up to log.last()
        {2, 2, 4, {}, 2, 2, false},
    };

    for (auto t : tests) {
      auto storage = new MemoryStorage();
      storage->Append(EntryVec({pbEntry(1, 1), pbEntry(2, 2)}));
      RaftUPtr raft(newTestRaft(1, {1}, 10, 1, storage));
      raft->becomeFollower(2, 0);

      raft->handleAppendEntries(PBMessage()
                                    .Type(pb::MsgApp)
                                    .Term(raft->currentTerm_)
                                    .LogTerm(t.prevLogTerm)
                                    .Index(t.prevLogIndex)
                                    .Commit(t.commit)
                                    .Entries(t.ents)
                                    .v);
      ASSERT_EQ(raft->log_->LastIndex(), t.wIndex);
      ASSERT_EQ(raft->log_->CommitIndex(), t.wCommit);
      ASSERT_EQ(raft->mails_.size(), 1);
      ASSERT_EQ(raft->mails_.begin()->reject(), t.wReject);
    }
  }

  static void TestStateTransition() {
    struct TestData {
      Raft::StateRole from;
      Raft::StateRole to;

      bool wallow;
      uint64_t wterm;
      uint64_t wlead;
    } tests[] = {
        {Raft::kFollower, Raft::kFollower, true, 1, 0},
        {Raft::kFollower, Raft::kCandidate, true, 1, 0},
        {Raft::kFollower, Raft::kLeader, false, 0, 0},

        {Raft::kCandidate, Raft::kFollower, true, 0, 0},
        {Raft::kCandidate, Raft::kCandidate, true, 1, 0},
        {Raft::kCandidate, Raft::kLeader, true, 0, 1},

        {Raft::kLeader, Raft::kFollower, true, 1, 0},
        {Raft::kLeader, Raft::kCandidate, false, 1, 0},

        // TODO: Is it really allowed to convert leader to leader?
        {Raft::kLeader, Raft::kLeader, true, 0, 1},
    };

    for (auto t : tests) {
      RaftUPtr raft(newTestRaft(1, {1}, 10, 1, new MemoryStorage()));
      raft->role_ = t.from;

      bool failed = false;
      try {
        switch (t.to) {
          case Raft::kFollower:
            raft->becomeFollower(t.wterm, t.wlead);
            break;
          case Raft::kCandidate:
            raft->becomeCandidate();
            break;
          case Raft::kLeader:
            raft->becomeLeader();
            break;
          default:
            break;
        }
      } catch (RaftError& e) {
        failed = true;
      }
      ASSERT_EQ(!t.wallow, failed);

      if (t.wallow) {
        ASSERT_EQ(raft->currentTerm_, t.wterm);
        ASSERT_EQ(raft->currentLeader_, t.wlead);
      }
    }
  }

  static void TestHandleHeartbeat() {
    uint64_t commit = 2;

    struct TestData {
      pb::Message m;

      uint64_t wCommit;
    } tests[] = {
        // do not decrease commit
        {PBMessage().From(2).To(1).Type(pb::MsgHeartbeat).Term(2).Commit(commit - 1).v, commit},

        {PBMessage().From(2).To(1).Type(pb::MsgHeartbeat).Term(2).Commit(commit + 1).v, commit + 1},
    };

    for (auto t : tests) {
      auto storage = new MemoryStorage();
      storage->Append({pbEntry(1, 1), pbEntry(2, 2), pbEntry(3, 3)});
      RaftUPtr raft(newTestRaft(1, {1, 2}, 10, 1, storage));
      raft->becomeFollower(2, 0);
      raft->log_->CommitTo(commit);

      raft->handleHeartbeat(t.m);

      ASSERT_EQ(raft->log_->CommitIndex(), t.wCommit);
    }
  }

  // TestHandleHeartbeatResp ensures that we re-send log entries when we get a heartbeat response.
  static void TestHandleHeartbeatResp() {
    RaftUPtr raft(newTestRaft(1, {1, 2}, 10, 1,
                              new MemoryStorage(pbEntry(1, 1) + pbEntry(2, 2) + pbEntry(3, 3))));
    raft->becomeCandidate();
    raft->becomeLeader();

    ASSERT_EQ(raft->prs_[2].NextIndex(), 4);

    // A heartbeat response from a node that is behind; re-send MsgApp
    raft->Step(PBMessage().From(2).Type(pb::MsgHeartbeatResp).Term(1).v);
    ASSERT_EQ(raft->mails_.size(), 1);
    ASSERT_EQ(raft->mails_.begin()->type(), pb::MsgApp);

    // A second heartbeat response generates another MsgApp re-send
    raft->mails_.clear();
    raft->Step(PBMessage().From(2).Type(pb::MsgHeartbeatResp).Term(1).v);
    ASSERT_EQ(raft->mails_.size(), 1);
    ASSERT_EQ(raft->mails_.begin()->type(), pb::MsgApp);

    // Once we have an MsgAppResp that pushes MatchIndex forward, heartbeats no longer send MsgApp.
    auto msg = *raft->mails_.begin();
    raft->Step(
        PBMessage().From(2).Type(pb::MsgAppResp).Index(msg.index() + msg.entries_size()).Term(1).v);
    raft->mails_.clear();

    raft->Step(PBMessage().From(2).Type(pb::MsgHeartbeatResp).Term(1).v);
    ASSERT_EQ(raft->mails_.size(), 0);
  }

  static void TestLeaderElection() {
    struct TestData {
      Network* network;
      Raft::StateRole role;

      uint64_t wterm;
    } tests[] = {
        // three nodes, all healthy
        {Network::New(3), Raft::kLeader, 1},

        // three nodes, one sick
        {Network::New(3)->Down(2), Raft::kLeader, 1},

        // three nodes, two sick
        {Network::New(3)->Down(2)->Down(3), Raft::kCandidate, 1},

        // four nodes, two sick
        {Network::New(4)->Down(2)->Down(3), Raft::kCandidate, 1},

        // five nodes, two sick
        {Network::New(5)->Down(2)->Down(3), Raft::kLeader, 1},
    };

    for (auto t : tests) {
      t.network->RaiseElection(1);

      auto node = t.network->Peer(1);
      ASSERT_EQ(node->role_, t.role);
      ASSERT_EQ(node->currentTerm_, t.wterm);
      delete t.network;
    }
  }

  // TestLeaderCycle verifies that each node in a cluster can campaign
  // and be elected in turn. This ensures that elections (including
  // pre-vote) work when not starting from a clean slate (as they do in
  // TestLeaderElection)
  static void TestLeaderCycle(bool preVote) {
    for (uint64_t cand = 1; cand <= 1; cand++) {
      std::unique_ptr<Network> n(Network::New(3));

      if (preVote) {
        n->MutablePeerConfig(cand)->preVote = true;
      }

      n->RaiseElection(cand);

      for (uint64_t id = 1; id <= 3; id++) {
        ASSERT_EQ(n->Peer(id)->role_, cand == id ? Raft::kLeader : Raft::kFollower);
      }
    }
  }

  static void TestCommit() {
    struct TestData {
      std::vector<uint64_t> matches;
      EntryVec logs;
      uint64_t smTerm;

      uint64_t wcommit;
    } tests[] = {

        /// single
        {{1}, {pbEntry(1, 1)}, 1, 1},
        {{1}, {pbEntry(1, 1)}, 2, 0},  // not commit in newer term
        {{2}, {pbEntry(1, 1), pbEntry(2, 2)}, 2, 2},
        {{1}, {pbEntry(1, 2)}, 2, 1},

        // odd
        {{2, 1, 1}, {pbEntry(1, 1), pbEntry(2, 1)}, 1, 1},
        {{2, 1, 1}, {pbEntry(1, 1), pbEntry(2, 1)}, 2, 0},
        {{2, 1, 2}, {pbEntry(1, 1), pbEntry(2, 2)}, 2, 2},
        {{2, 1, 2}, {pbEntry(1, 1), pbEntry(2, 1)}, 2, 0},

        // odd
        {{2, 1, 1, 1}, {pbEntry(1, 1), pbEntry(2, 2)}, 1, 1},
        {{2, 1, 1, 1}, {pbEntry(1, 1), pbEntry(2, 1)}, 2, 0},
        {{2, 1, 1, 2}, {pbEntry(1, 1), pbEntry(2, 2)}, 1, 1},
        {{2, 1, 1, 2}, {pbEntry(1, 1), pbEntry(2, 1)}, 2, 0},
        {{2, 1, 2, 2}, {pbEntry(1, 1), pbEntry(2, 2)}, 2, 2},
        {{2, 1, 2, 2}, {pbEntry(1, 1), pbEntry(2, 1)}, 2, 0},
    };

    for (auto t : tests) {
      RaftUPtr r(newTestRaft(1, {1}, 5, 1, new MemoryStorage(t.logs)));
      r->loadState(PBHardState().Term(t.smTerm).v);
      r->role_ = Raft::kLeader;

      for (int i = 0; i < t.matches.size(); i++) {
        uint64_t id = static_cast<uint64_t>(i + 1);
        r->prs_[id] = Progress().MatchIndex(t.matches[i]).NextIndex(t.matches[i] + 1);
      }
      r->advanceCommitIndex();
      ASSERT_EQ(r->log_->CommitIndex(), t.wcommit);
    }
  }

  // TestCampaignWhileLeader ensures that a leader node won't step down
  // when it elects itself.
  static void TestCampaignWhileLeader() {
    RaftUPtr r(newTestRaft(1, {1}, 5, 1, new MemoryStorage()));
    ASSERT_EQ(r->role_, Raft::kFollower);

    r->Step(PBMessage().From(1).To(1).Type(pb::MsgHup).Term(1).v);
    ASSERT_EQ(r->role_, Raft::kLeader);

    r->Step(PBMessage().From(1).To(1).Type(pb::MsgHup).Term(1).v);
    ASSERT_EQ(r->role_, Raft::kLeader);
  }

  static void TestDuelingCandidates() {
    std::unique_ptr<Network> n(Network::New(3));
    n->Cut(1, 3);
    n->RaiseElection(1);
    ASSERT_EQ(n->Peer(1)->role_, Raft::kLeader);
    ASSERT_EQ(n->Peer(1)->log_->CommitIndex(), 1);
    ASSERT_EQ(n->Peer(2)->log_->LastIndex(), 1);
    ASSERT_EQ(n->Peer(3)->log_->LastIndex(), 0);

    // 3 stays as candidate since it receives a vote from 3 and a rejection from 2
    n->RaiseElection(3);
    ASSERT_EQ(n->Peer(3)->role_, Raft::kCandidate);
    ASSERT_EQ(n->Peer(1)->role_, Raft::kLeader);
    ASSERT_EQ(n->Peer(2)->Term(), 1);

    n->Restore(1, 3);

    // candidate 3 now increases its term and tries to vote again
    // we expect it to disrupt the leader 1 since it has a higher term
    // 3 will be follower again since both 1 and 2 rejects its vote request
    // since 3 does not have a long enough log
    n->RaiseElection(3);
    ASSERT_EQ(n->Peer(1)->role_, Raft::kFollower);
    ASSERT_EQ(n->Peer(1)->role_, Raft::kFollower);
    ASSERT_EQ(n->Peer(3)->role_, Raft::kFollower);
  }

  // TestVoteFromAnyState ensures that no matter what state a node is from,
  // it will always step down and vote for a legal candidate.
  static void TestVoteFromAnyState() {
    for (int i = 0; i < Raft::kStateNum; i++) {
      auto role = Raft::StateRole(i);
      RaftUPtr r(newTestRaft(1, {1, 2, 3}, 10, 1, new MemoryStorage()));

      if (role == Raft::kFollower) {
        r->becomeFollower(1, 3);
      } else if (role == Raft::kCandidate) {
        r->becomeCandidate();
      } else if (role == Raft::kLeader) {
        r->becomeCandidate();
        r->becomeLeader();
      } else if (role == Raft::kPreCandidate) {
        r->becomeFollower(1, 3);
        r->becomePreCandidate();
      }
      ASSERT_EQ(r->Term(), 1);

      uint64_t newTerm = 2;
      uint64_t from = 2;
      r->Step(
          PBMessage().From(from).To(1).Type(pb::MsgVote).Term(newTerm).LogTerm(newTerm).Index(4).v);

      ASSERT_EQ(r->mails_.size(), 1);
      ASSERT_EQ(r->mails_[0].type(), pb::MsgVoteResp);
      ASSERT_FALSE(r->mails_[0].reject());
      ASSERT_EQ(r->votedFor_, from);
      ASSERT_EQ(r->currentTerm_, newTerm);
      ASSERT_EQ(r->role_, Raft::kFollower);
    }
  }
};

}  // namespace yaraft

TEST(Raft, StepIgnoreOldTermMsg) {
  yaraft::RaftTest::TestStepIgnoreOldTermMsg();
}

TEST(Raft, HandleAppendEntries) {
  yaraft::RaftTest::TestHandleMsgApp();
}

TEST(Raft, StateTransition) {
  yaraft::RaftTest::TestStateTransition();
}

TEST(Raft, HandleHeartbeat) {
  yaraft::RaftTest::TestHandleHeartbeat();
}

TEST(Raft, HandleHeartbeatResp) {
  yaraft::RaftTest::TestHandleHeartbeatResp();
}

TEST(Raft, LogReplication) {}

TEST(Raft, LeaderElection) {
  yaraft::RaftTest::TestLeaderElection();
}

TEST(Raft, LeaderCycle) {
  yaraft::RaftTest::TestLeaderCycle(false);
}

TEST(Raft, LeaderCyclePreVote) {
  yaraft::RaftTest::TestLeaderCycle(true);
}

TEST(Raft, Commit) {
  yaraft::RaftTest::TestCommit();
}

TEST(Raft, CampaignWhileLeader) {
  yaraft::RaftTest::TestCampaignWhileLeader();
}

TEST(Raft, DuelingCandidates) {
  yaraft::RaftTest::TestDuelingCandidates();
}

TEST(Raft, VoteFromAnyState) {
  yaraft::RaftTest::TestVoteFromAnyState();
}