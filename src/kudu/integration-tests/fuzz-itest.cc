// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <boost/optional.hpp>
#include <boost/optional/optional_io.hpp>
#include <glog/logging.h>
#include <glog/stl_logging.h>
#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include <string>
#include <vector>

#include "kudu/client/client.h"
#include "kudu/client/client-test-util.h"
#include "kudu/client/row_result.h"
#include "kudu/client/schema.h"
#include "kudu/gutil/casts.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/mini_master.h"
#include "kudu/integration-tests/mini_cluster.h"
#include "kudu/tablet/key_value_test_schema.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet_peer.h"
#include "kudu/tserver/mini_tablet_server.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/tserver/ts_tablet_manager.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_util.h"

DEFINE_int32(keyspace_size, 2,  "number of distinct primary keys to test with");
DECLARE_bool(enable_maintenance_manager);
DECLARE_bool(use_hybrid_clock);

using boost::optional;
using std::string;
using std::vector;
using std::unique_ptr;

// The type of operation in a sequence of operations generated by
// the fuzz test.
enum TestOpType {
  TEST_INSERT,
  TEST_UPSERT,
  TEST_UPSERT_PK_ONLY,
  TEST_UPDATE,
  TEST_DELETE,
  TEST_FLUSH_OPS,
  TEST_FLUSH_TABLET,
  TEST_FLUSH_DELTAS,
  TEST_MINOR_COMPACT_DELTAS,
  TEST_MAJOR_COMPACT_DELTAS,
  TEST_COMPACT_TABLET,
  TEST_RESTART_TS,
  TEST_NUM_OP_TYPES // max value for enum
};
MAKE_ENUM_LIMITS(TestOpType, TEST_INSERT, TEST_NUM_OP_TYPES);

const char* kTableName = "table";

namespace kudu {

using client::KuduClient;
using client::KuduClientBuilder;
using client::KuduColumnSchema;
using client::KuduDelete;
using client::KuduInsert;
using client::KuduPredicate;
using client::KuduScanBatch;
using client::KuduScanner;
using client::KuduSchema;
using client::KuduSchemaBuilder;
using client::KuduSession;
using client::KuduTable;
using client::KuduTableCreator;
using client::KuduUpdate;
using client::KuduUpsert;
using client::KuduValue;
using client::KuduWriteOperation;
using client::sp::shared_ptr;

namespace tablet {

const char* TestOpType_names[] = {
  "TEST_INSERT",
  "TEST_UPSERT",
  "TEST_UPSERT_PK_ONLY",
  "TEST_UPDATE",
  "TEST_DELETE",
  "TEST_FLUSH_OPS",
  "TEST_FLUSH_TABLET",
  "TEST_FLUSH_DELTAS",
  "TEST_MINOR_COMPACT_DELTAS",
  "TEST_MAJOR_COMPACT_DELTAS",
  "TEST_COMPACT_TABLET",
  "TEST_RESTART_TS"
};

// An operation in a fuzz-test sequence.
struct TestOp {
  // The op to run.
  TestOpType type;

  // For INSERT/UPSERT/UPDATE/DELETE, the key of the row to be modified. Otherwise, unused.
  int row_key;

  string ToString() const {
    return strings::Substitute("{$0, $1}", TestOpType_names[type], row_key);
  }
};

// Test which does only random operations against a tablet, including update and random
// get (ie scans with equal lower and upper bounds).
//
// The test maintains an in-memory copy of the expected state of the tablet, and uses only
// a single thread, so that it's easy to verify that the tablet always matches the expected
// state.
class FuzzTest : public KuduTest {
 public:
  FuzzTest() {
    FLAGS_enable_maintenance_manager = false;
    FLAGS_use_hybrid_clock = false;

    schema_ = client::KuduSchemaFromSchema(CreateKeyValueTestSchema());
  }

  void SetUp() override {
    KuduTest::SetUp();

    MiniClusterOptions opts;
    cluster_.reset(new MiniCluster(env_, opts));
    ASSERT_OK(cluster_->Start());
    CHECK_OK(KuduClientBuilder()
             .add_master_server_addr(cluster_->mini_master()->bound_rpc_addr_str())
             .default_admin_operation_timeout(MonoDelta::FromSeconds(60))
             .Build(&client_));
    // Add a table, make sure it reports itself.
    gscoped_ptr<KuduTableCreator> table_creator(client_->NewTableCreator());
    CHECK_OK(table_creator->table_name(kTableName)
             .schema(&schema_)
             .set_range_partition_columns({ "key" })
             .num_replicas(1)
             .Create());

    // Find the peer.
    tablet_peer_ = LookupTabletPeer();

    // Setup session and table.
    session_ = client_->NewSession();
    CHECK_OK(session_->SetFlushMode(KuduSession::MANUAL_FLUSH));
    session_->SetTimeoutMillis(60 * 1000);
    CHECK_OK(client_->OpenTable(kTableName, &table_));
  }

  void TearDown() override {
    tablet_peer_.reset();
    cluster_->Shutdown();
  }

  scoped_refptr<TabletPeer> LookupTabletPeer() {
    vector<scoped_refptr<TabletPeer> > peers;
    cluster_->mini_tablet_server(0)->server()->tablet_manager()->GetTabletPeers(&peers);
    CHECK_EQ(1, peers.size());
    return peers[0];
  }

  void RestartTabletServer() {
    tablet_peer_.reset();
    auto ts = cluster_->mini_tablet_server(0);
    if (ts->server()) {
      ASSERT_OK(ts->Restart());
    } else {
      ASSERT_OK(ts->Start());
    }
    ASSERT_OK(ts->server()->WaitInited());

    tablet_peer_ = LookupTabletPeer();
  }

  Tablet* tablet() const {
    return tablet_peer_->tablet();
  }

  // Adds an insert for the given key/value pair to 'ops', returning the new contents
  // of the row.
  ExpectedKeyValueRow InsertOrUpsertRow(int key, int val,
                                        optional<ExpectedKeyValueRow> old_row,
                                        TestOpType type) {
    ExpectedKeyValueRow ret;
    unique_ptr<KuduWriteOperation> op;
    if (type == TEST_INSERT) {
      op.reset(table_->NewInsert());
    } else {
      op.reset(table_->NewUpsert());
    }
    KuduPartialRow* row = op->mutable_row();
    CHECK_OK(row->SetInt32(0, key));
    ret.key = key;
    if (type != TEST_UPSERT_PK_ONLY) {
      if (val & 1) {
        CHECK_OK(row->SetNull(1));
      } else {
        CHECK_OK(row->SetInt32(1, val));
        ret.val = val;
      }
    } else {
      // For "upsert PK only", we expect the row to keep its old value
      // the row existed, or NULL if there was no old row.
      ret.val = old_row ? old_row->val : boost::none;
    }
    CHECK_OK(session_->Apply(op.release()));
    return ret;
  }

  // Adds an update of the given key/value pair to 'ops', returning the new contents
  // of the row.
  ExpectedKeyValueRow MutateRow(int key, uint32_t new_val) {
    ExpectedKeyValueRow ret;
    unique_ptr<KuduUpdate> update(table_->NewUpdate());
    KuduPartialRow* row = update->mutable_row();
    CHECK_OK(row->SetInt32(0, key));
    ret.key = key;
    if (new_val & 1) {
      CHECK_OK(row->SetNull(1));
    } else {
      CHECK_OK(row->SetInt32(1, new_val));
      ret.val = new_val;
    }
    CHECK_OK(session_->Apply(update.release()));
    return ret;
  }

  // Adds a delete of the given row to 'ops', returning boost::none (indicating that
  // the row no longer exists).
  optional<ExpectedKeyValueRow> DeleteRow(int key) {
    unique_ptr<KuduDelete> del(table_->NewDelete());
    KuduPartialRow* row = del->mutable_row();
    CHECK_OK(row->SetInt32(0, key));
    CHECK_OK(session_->Apply(del.release()));
    return boost::none;
  }

  // Random-read the given row, returning its current value.
  // If the row doesn't exist, returns boost::none.
  optional<ExpectedKeyValueRow> GetRow(int key) {
    KuduScanner s(table_.get());
    CHECK_OK(s.AddConjunctPredicate(table_->NewComparisonPredicate(
        "key", KuduPredicate::EQUAL, KuduValue::FromInt(key))));
    CHECK_OK(s.Open());
    while (s.HasMoreRows()) {
      KuduScanBatch batch;
      CHECK_OK(s.NextBatch(&batch));
      for (KuduScanBatch::RowPtr row : batch) {
        ExpectedKeyValueRow ret;
        CHECK_OK(row.GetInt32(0, &ret.key));
        if (!row.IsNull(1)) {
          ret.val = 0;
          CHECK_OK(row.GetInt32(1, ret.val.get_ptr()));
        }
        return ret;
      }
    }
    return boost::none;
  }

 protected:
  void RunFuzzCase(const vector<TestOp>& ops,
                   int update_multiplier);

  KuduSchema schema_;
  gscoped_ptr<MiniCluster> cluster_;
  shared_ptr<KuduClient> client_;
  shared_ptr<KuduSession> session_;
  shared_ptr<KuduTable> table_;

  scoped_refptr<TabletPeer> tablet_peer_;
};

// Generate a random valid sequence of operations for use as a
// fuzz test.
void GenerateTestCase(vector<TestOp>* ops, int len) {
  vector<bool> exists(FLAGS_keyspace_size);
  bool ops_pending = false;
  bool data_in_mrs = false;
  bool worth_compacting = false;
  bool data_in_dms = false;
  ops->clear();
  while (ops->size() < len) {
    TestOpType r = tight_enum_cast<TestOpType>(rand() % enum_limits<TestOpType>::max_enumerator);
    int row_key = rand() % FLAGS_keyspace_size;
    switch (r) {
      case TEST_INSERT:
        if (exists[row_key]) continue;
        ops->push_back({TEST_INSERT, row_key});
        exists[row_key] = true;
        ops_pending = true;
        data_in_mrs = true;
        break;
      case TEST_UPSERT:
      case TEST_UPSERT_PK_ONLY:
        ops->push_back({r, row_key});
        exists[row_key] = true;
        ops_pending = true;
        // If the row doesn't currently exist, this will act like an insert
        // and put it into MRS.
        if (!exists[row_key]) {
          data_in_mrs = true;
        } else if (!data_in_mrs) {
          // If it does exist, but not in MRS, then this will put data into
          // a DMS.
          data_in_dms = true;
        }
        break;
      case TEST_UPDATE:
        if (!exists[row_key]) continue;
        ops->push_back({TEST_UPDATE, row_key});
        ops_pending = true;
        if (!data_in_mrs) {
          data_in_dms = true;
        }
        break;
      case TEST_DELETE:
        if (!exists[row_key]) continue;
        ops->push_back({TEST_DELETE, row_key});
        ops_pending = true;
        exists[row_key] = false;
        if (!data_in_mrs) {
          data_in_dms = true;
        }
        break;
      case TEST_FLUSH_OPS:
        if (ops_pending) {
          ops->push_back({TEST_FLUSH_OPS, 0});
          ops_pending = false;
        }
        break;
      case TEST_FLUSH_TABLET:
        if (data_in_mrs) {
          if (ops_pending) {
            ops->push_back({TEST_FLUSH_OPS, 0});
            ops_pending = false;
          }
          ops->push_back({TEST_FLUSH_TABLET, 0});
          data_in_mrs = false;
          worth_compacting = true;
        }
        break;
      case TEST_COMPACT_TABLET:
        if (worth_compacting) {
          if (ops_pending) {
            ops->push_back({TEST_FLUSH_OPS, 0});
            ops_pending = false;
          }
          ops->push_back({TEST_COMPACT_TABLET, 0});
          worth_compacting = false;
        }
        break;
      case TEST_FLUSH_DELTAS:
        if (data_in_dms) {
          if (ops_pending) {
            ops->push_back({TEST_FLUSH_OPS, 0});
            ops_pending = false;
          }
          ops->push_back({TEST_FLUSH_DELTAS, 0});
          data_in_dms = false;
        }
        break;
      case TEST_MAJOR_COMPACT_DELTAS:
        ops->push_back({TEST_MAJOR_COMPACT_DELTAS, 0});
        break;
      case TEST_MINOR_COMPACT_DELTAS:
        ops->push_back({TEST_MINOR_COMPACT_DELTAS, 0});
        break;
      case TEST_RESTART_TS:
        ops->push_back({TEST_RESTART_TS, 0});
        break;
      default:
        LOG(FATAL);
    }
  }
}

string DumpTestCase(const vector<TestOp>& ops) {
  vector<string> strs;
  for (TestOp test_op : ops) {
    strs.push_back(test_op.ToString());
  }
  return JoinStrings(strs, ",\n");
}

void FuzzTest::RunFuzzCase(const vector<TestOp>& test_ops,
                           int update_multiplier = 1) {
  // Dump the test case, since we usually run a random one.
  // This dump format is easy for a developer to copy-paste back
  // into a test method in order to reproduce a failure.
  LOG(INFO) << "test case:\n" << DumpTestCase(test_ops);

  vector<optional<ExpectedKeyValueRow>> cur_val(FLAGS_keyspace_size);
  vector<optional<ExpectedKeyValueRow>> pending_val(FLAGS_keyspace_size);

  int i = 0;
  for (const TestOp& test_op : test_ops) {
    optional<ExpectedKeyValueRow> val_in_table = GetRow(test_op.row_key);
    EXPECT_EQ(cur_val[test_op.row_key], val_in_table);

    LOG(INFO) << test_op.ToString();
    switch (test_op.type) {
      case TEST_INSERT:
      case TEST_UPSERT:
      case TEST_UPSERT_PK_ONLY: {
        pending_val[test_op.row_key] = InsertOrUpsertRow(
            test_op.row_key, i++, pending_val[test_op.row_key], test_op.type);
        break;
      }
      case TEST_UPDATE:
        for (int j = 0; j < update_multiplier; j++) {
          pending_val[test_op.row_key] = MutateRow(test_op.row_key, i++);
        }
        break;
      case TEST_DELETE:
        pending_val[test_op.row_key] = DeleteRow(test_op.row_key);
        break;
      case TEST_FLUSH_OPS:
        FlushSessionOrDie(session_);
        cur_val = pending_val;
        break;
      case TEST_FLUSH_TABLET:
        ASSERT_OK(tablet()->Flush());
        break;
      case TEST_FLUSH_DELTAS:
        ASSERT_OK(tablet()->FlushBiggestDMS());
        break;
      case TEST_MAJOR_COMPACT_DELTAS:
        ASSERT_OK(tablet()->CompactWorstDeltas(RowSet::MAJOR_DELTA_COMPACTION));
        break;
      case TEST_MINOR_COMPACT_DELTAS:
        ASSERT_OK(tablet()->CompactWorstDeltas(RowSet::MINOR_DELTA_COMPACTION));
        break;
      case TEST_COMPACT_TABLET:
        ASSERT_OK(tablet()->Compact(Tablet::FORCE_COMPACT_ALL));
        break;
      case TEST_RESTART_TS:
        NO_FATALS(RestartTabletServer());
        break;
      default:
        LOG(FATAL) << test_op.type;
    }
  }
}


// Generates a random test sequence and runs it.
// The logs of this test are designed to easily be copy-pasted and create
// more specific test cases like TestFuzz<N> below.
TEST_F(FuzzTest, TestFuzz) {
  SeedRandom();
  vector<TestOp> test_ops;
  GenerateTestCase(&test_ops, AllowSlowTests() ? 1000 : 50);
  RunFuzzCase(test_ops);
}

// Generates a random test case, but the UPDATEs are all repeated many times.
// This results in very large batches which are likely to span multiple delta blocks
// when flushed.
TEST_F(FuzzTest, TestFuzzHugeBatches) {
  SeedRandom();
  vector<TestOp> test_ops;
  GenerateTestCase(&test_ops, AllowSlowTests() ? 500 : 50);
  int update_multiplier;
#ifdef THREAD_SANITIZER
  // TSAN builds run more slowly, so 500 can cause timeouts.
  update_multiplier = 100;
#else
  update_multiplier = 500;
#endif
  RunFuzzCase(test_ops, update_multiplier);
}

// A particular test case which previously failed TestFuzz.
TEST_F(FuzzTest, TestFuzz1) {
  vector<TestOp> test_ops = {
    // Get an inserted row in a DRS.
    {TEST_INSERT, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},

    // DELETE in DMS, INSERT in MRS and flush again.
    {TEST_DELETE, 0},
    {TEST_INSERT, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},

    // State:
    // RowSet RowSet(0):
    //   (int32 key=1, int32 val=NULL) Undos: [@1(DELETE)] Redos (in DMS): [@2 DELETE]
    // RowSet RowSet(1):
    //   (int32 key=1, int32 val=NULL) Undos: [@2(DELETE)] Redos: []

    {TEST_COMPACT_TABLET, 0},
  };
  RunFuzzCase(test_ops);
}

// A particular test case which previously failed TestFuzz.
TEST_F(FuzzTest, TestFuzz2) {
  vector<TestOp> test_ops = {
    {TEST_INSERT, 0},
    {TEST_DELETE, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    // (int32 key=1, int32 val=NULL)
    // Undo Mutations: [@1(DELETE)]
    // Redo Mutations: [@1(DELETE)]

    {TEST_INSERT, 0},
    {TEST_DELETE, 0},
    {TEST_INSERT, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    // (int32 key=1, int32 val=NULL)
    // Undo Mutations: [@2(DELETE)]
    // Redo Mutations: []

    {TEST_COMPACT_TABLET, 0},
    // Output Row: (int32 key=1, int32 val=NULL)
    // Undo Mutations: [@1(DELETE)]
    // Redo Mutations: [@1(DELETE)]

    {TEST_DELETE, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_COMPACT_TABLET, 0},
  };
  RunFuzzCase(test_ops);
}

// A particular test case which previously failed TestFuzz.
TEST_F(FuzzTest, TestFuzz3) {
  vector<TestOp> test_ops = {
    {TEST_INSERT, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    // Output Row: (int32 key=1, int32 val=NULL)
    // Undo Mutations: [@1(DELETE)]
    // Redo Mutations: []

    {TEST_DELETE, 0},
    // Adds a @2 DELETE to DMS for above row.

    {TEST_INSERT, 0},
    {TEST_DELETE, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    // (int32 key=1, int32 val=NULL)
    // Undo Mutations: [@2(DELETE)]
    // Redo Mutations: [@2(DELETE)]

    // Compaction input:
    // Row 1: (int32 key=1, int32 val=NULL)
    //   Undo Mutations: [@2(DELETE)]
    //   Redo Mutations: [@2(DELETE)]
    // Row 2: (int32 key=1, int32 val=NULL)
    //  Undo Mutations: [@1(DELETE)]
    //  Redo Mutations: [@2(DELETE)]

    {TEST_COMPACT_TABLET, 0},
  };
  RunFuzzCase(test_ops);
}

// A particular test case which previously failed TestFuzz.
TEST_F(FuzzTest, TestFuzz4) {
  vector<TestOp> test_ops = {
    {TEST_INSERT, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_COMPACT_TABLET, 0},
    {TEST_DELETE, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_COMPACT_TABLET, 0},
    {TEST_INSERT, 0},
    {TEST_UPDATE, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    {TEST_DELETE, 0},
    {TEST_INSERT, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    {TEST_UPDATE, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    {TEST_UPDATE, 0},
    {TEST_DELETE, 0},
    {TEST_INSERT, 0},
    {TEST_DELETE, 0},
    {TEST_FLUSH_OPS, 0},
    {TEST_FLUSH_TABLET, 0},
    {TEST_COMPACT_TABLET, 0},
  };
  RunFuzzCase(test_ops);
}

// Previously caused incorrect data being read after restart.
// Failure:
//  Value of: val_in_table
//  Actual: "()"
//  Expected: "(" + cur_val + ")"
TEST_F(FuzzTest, TestFuzzWithRestarts1) {
  RunFuzzCase({
      {TEST_INSERT, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_UPDATE, 1},
      {TEST_RESTART_TS, 0},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_DELTAS, 0},
      {TEST_INSERT, 0},
      {TEST_DELETE, 1},
      {TEST_INSERT, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_RESTART_TS, 0},
      {TEST_MINOR_COMPACT_DELTAS, 0},
      {TEST_COMPACT_TABLET, 0},
      {TEST_UPDATE, 1},
      {TEST_FLUSH_OPS, 0}
    });
}

// Previously caused KUDU-1341:
// deltafile.cc:134] Check failed: last_key_.CompareTo<UNDO>(key) <= 0 must
// insert undo deltas in sorted order (ascending key, then descending ts):
// got key (row 1@tx5965182714017464320) after (row 1@tx5965182713875046400)
TEST_F(FuzzTest, TestFuzzWithRestarts2) {
  RunFuzzCase({
      {TEST_INSERT, 0},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_DELETE, 0},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_DELTAS, 0},
      {TEST_RESTART_TS, 0},
      {TEST_INSERT, 1},
      {TEST_INSERT, 0},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_DELETE, 0},
      {TEST_INSERT, 0},
      {TEST_UPDATE, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_FLUSH_DELTAS, 0},
      {TEST_RESTART_TS, 0},
      {TEST_UPDATE, 1},
      {TEST_DELETE, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_RESTART_TS, 0},
      {TEST_INSERT, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_RESTART_TS, 0},
      {TEST_COMPACT_TABLET, 0}
    });
}

// Regression test for KUDU-1467: a sequence involving
// UPSERT which failed to replay properly upon bootstrap.
TEST_F(FuzzTest, TestUpsertSeq) {
  RunFuzzCase({
      {TEST_INSERT, 1},
      {TEST_UPSERT, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_UPSERT, 1},
      {TEST_DELETE, 1},
      {TEST_UPSERT, 1},
      {TEST_INSERT, 0},
      {TEST_FLUSH_OPS, 0},
      {TEST_FLUSH_TABLET, 0},
      {TEST_RESTART_TS, 0},
      {TEST_UPDATE, 1},
    });
}

// Regression test for KUDU-1623: updates without primary key
// columns specified can cause crashes and issues at restart.
TEST_F(FuzzTest, TestUpsert_PKOnly) {
  RunFuzzCase({
      {TEST_INSERT, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_UPSERT_PK_ONLY, 1},
      {TEST_FLUSH_OPS, 0},
      {TEST_RESTART_TS, 0}
    });
}

} // namespace tablet
} // namespace kudu
