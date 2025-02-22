// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "script/script.h"

#include <google/protobuf/util/json_util.h>

#include "common/greplog.h"
#include "common/logging.h"
#include "exec/schema_scanner/schema_be_tablets_scanner.h"
#include "gen_cpp/olap_file.pb.h"
#include "gutil/strings/substitute.h"
#include "http/action/compaction_action.h"
#include "runtime/exec_env.h"
#include "runtime/mem_tracker.h"
#include "storage/storage_engine.h"
#include "storage/tablet.h"
#include "storage/tablet_manager.h"
#include "storage/tablet_updates.h"
#include "util/stack_util.h"
#include "wrenbind17/wrenbind17.hpp"

using namespace wrenbind17;
using std::string;

namespace starrocks {

#define REG_VAR(TYPE, NAME) cls.var<&TYPE::NAME>(#NAME)
#define REG_METHOD(TYPE, NAME) cls.func<&TYPE::NAME>(#NAME)
#define REG_STATIC_METHOD(TYPE, NAME) cls.funcStatic<&TYPE::NAME>(#NAME)

template <class T>
std::string proto_to_json(T& proto) {
    std::string json;
    google::protobuf::util::MessageToJsonString(proto, &json);
    return json;
}

static std::shared_ptr<TabletUpdatesPB> tablet_updates_to_pb(TabletUpdates& self) {
    std::shared_ptr<TabletUpdatesPB> pb = std::make_shared<TabletUpdatesPB>();
    self.to_updates_pb(pb.get());
    return pb;
}

static EditVersionMetaPB* tablet_updates_pb_version(TabletUpdatesPB& self, int idx) {
    if (idx < 0 || idx >= self.versions_size()) return nullptr;
    return self.mutable_versions(idx);
}

static uint64_t kv_store_get_live_sst_files_size(KVStore& store) {
    uint64_t ret = 0;
    store.get_live_sst_files_size(&ret);
    return ret;
}

static int tablet_keys_type_int(Tablet& tablet) {
    return static_cast<int>(tablet.keys_type());
}

static int tablet_tablet_state(Tablet& tablet) {
    return static_cast<int>(tablet.tablet_state());
}

static const TabletSchema& tablet_tablet_schema(Tablet& tablet) {
    return tablet.tablet_schema();
}

static uint64_t tablet_tablet_id(Tablet& tablet) {
    return tablet.tablet_id();
}

static std::string tablet_path(Tablet& tablet) {
    return tablet.schema_hash_path();
}

static DataDir* tablet_data_dir(Tablet& tablet) {
    return tablet.data_dir();
}

static uint64_t get_major(EditVersion& self) {
    return self.major();
}

static uint64_t get_minor(EditVersion& self) {
    return self.minor();
}

static void bind_common(ForeignModule& m) {
    {
        auto& cls = m.klass<Status>("Status");
        cls.func<&Status::to_string>("toString");
        REG_METHOD(Status, ok);
    }
}

std::string memtracker_debug_string(MemTracker& self) {
    return self.debug_string();
}

void bind_exec_env(ForeignModule& m) {
    {
        auto& cls = m.klass<MemTracker>("MemTracker");
        REG_METHOD(MemTracker, label);
        REG_METHOD(MemTracker, limit);
        REG_METHOD(MemTracker, consumption);
        REG_METHOD(MemTracker, peak_consumption);
        REG_METHOD(MemTracker, parent);
        cls.funcExt<&memtracker_debug_string>("toString");
    }
    {
        auto& cls = m.klass<ExecEnv>("ExecEnv");
        REG_STATIC_METHOD(ExecEnv, GetInstance);
        cls.funcStaticExt<&get_thread_id_list>("get_thread_id_list");
        cls.funcStaticExt<&get_stack_trace_for_thread>("get_stack_trace_for_thread");
        cls.funcStaticExt<&get_stack_trace_for_threads>("get_stack_trace_for_threads");
        cls.funcStaticExt<&get_stack_trace_for_all_threads>("get_stack_trace_for_all_threads");
        cls.funcStaticExt<&grep_log_as_string>("grep_log_as_string");
        REG_METHOD(ExecEnv, process_mem_tracker);
        REG_METHOD(ExecEnv, query_pool_mem_tracker);
        REG_METHOD(ExecEnv, load_mem_tracker);
        REG_METHOD(ExecEnv, metadata_mem_tracker);
        REG_METHOD(ExecEnv, tablet_metadata_mem_tracker);
        REG_METHOD(ExecEnv, rowset_metadata_mem_tracker);
        REG_METHOD(ExecEnv, segment_metadata_mem_tracker);
        REG_METHOD(ExecEnv, compaction_mem_tracker);
        REG_METHOD(ExecEnv, update_mem_tracker);
        REG_METHOD(ExecEnv, clone_mem_tracker);
    }
}

class StorageEngineRef {
public:
    static string drop_tablet(int64_t tablet_id) {
        auto manager = StorageEngine::instance()->tablet_manager();
        string err;
        auto ptr = manager->get_tablet(tablet_id, true, &err);
        if (ptr == nullptr) {
            return strings::Substitute("get tablet $0 failed: $1", tablet_id, err);
        }
        auto st = manager->drop_tablet(tablet_id, TabletDropFlag::kKeepMetaAndFiles);
        return st.to_string();
    }

    static std::shared_ptr<Tablet> get_tablet(int64_t tablet_id) {
        string err;
        auto ptr = StorageEngine::instance()->tablet_manager()->get_tablet(tablet_id, true, &err);
        if (ptr == nullptr) {
            LOG(WARNING) << "get_tablet " << tablet_id << " failed: " << err;
            return nullptr;
        }
        return ptr;
    }

    static std::shared_ptr<TabletBasicInfo> get_tablet_info(int64_t tablet_id) {
        std::vector<TabletBasicInfo> tablet_infos;
        auto manager = StorageEngine::instance()->tablet_manager();
        manager->get_tablets_basic_infos(-1, -1, tablet_id, tablet_infos);
        if (tablet_infos.empty()) {
            return nullptr;
        } else {
            return std::make_shared<TabletBasicInfo>(tablet_infos[0]);
        }
    }

    static std::vector<TabletBasicInfo> get_tablet_infos(int64_t table_id, int64_t partition_id) {
        std::vector<TabletBasicInfo> tablet_infos;
        auto manager = StorageEngine::instance()->tablet_manager();
        manager->get_tablets_basic_infos(table_id, partition_id, -1, tablet_infos);
        return tablet_infos;
    }

    static std::vector<DataDir*> get_data_dirs() { return StorageEngine::instance()->get_stores(); }

    /**
     * @param tablet_id
     * @param type base|cumulative|update
     * @return
     */
    static Status do_compaction(int64_t tablet_id, const string& type) {
        return CompactionAction::do_compaction(tablet_id, type, "");
    }

    static void bind(ForeignModule& m) {
        {
            auto& cls = m.klass<TabletBasicInfo>("TabletBasicInfo");
            REG_VAR(TabletBasicInfo, table_id);
            REG_VAR(TabletBasicInfo, partition_id);
            REG_VAR(TabletBasicInfo, tablet_id);
            REG_VAR(TabletBasicInfo, num_version);
            REG_VAR(TabletBasicInfo, max_version);
            REG_VAR(TabletBasicInfo, min_version);
            REG_VAR(TabletBasicInfo, num_rowset);
            REG_VAR(TabletBasicInfo, num_row);
            REG_VAR(TabletBasicInfo, data_size);
            REG_VAR(TabletBasicInfo, index_mem);
            REG_VAR(TabletBasicInfo, create_time);
            REG_VAR(TabletBasicInfo, state);
            REG_VAR(TabletBasicInfo, type);
        }
        {
            auto& cls = m.klass<TabletSchema>("TabletSchema");
            REG_METHOD(TabletSchema, num_columns);
            REG_METHOD(TabletSchema, num_key_columns);
            REG_METHOD(TabletSchema, keys_type);
            REG_METHOD(TabletSchema, mem_usage);
            cls.func<&TabletSchema::debug_string>("toString");
        }
        {
            auto& cls = m.klass<Tablet>("Tablet");
            cls.funcExt<tablet_tablet_id>("tablet_id");
            cls.funcExt<tablet_tablet_schema>("schema");
            cls.funcExt<tablet_path>("path");
            cls.funcExt<tablet_data_dir>("data_dir");
            cls.funcExt<tablet_keys_type_int>("keys_type_as_int");
            cls.funcExt<tablet_tablet_state>("tablet_state_as_int");
            REG_METHOD(Tablet, tablet_footprint);
            REG_METHOD(Tablet, num_rows);
            REG_METHOD(Tablet, version_count);
            REG_METHOD(Tablet, max_version);
            REG_METHOD(Tablet, max_continuous_version);
            REG_METHOD(Tablet, compaction_score);
            REG_METHOD(Tablet, schema_debug_string);
            REG_METHOD(Tablet, debug_string);
            REG_METHOD(Tablet, support_binlog);
            REG_METHOD(Tablet, updates);
        }
        {
            auto& cls = m.klass<EditVersionPB>("EditVersionPB");
            cls.funcExt<&proto_to_json<EditVersionPB>>("toString");
        }
        {
            auto& cls = m.klass<EditVersionMetaPB>("EditVersionMetaPB");
            REG_METHOD(EditVersionMetaPB, version);
            REG_METHOD(EditVersionMetaPB, creation_time);
            cls.funcExt<&proto_to_json<EditVersionMetaPB>>("toString");
        }
        {
            auto& cls = m.klass<TabletUpdatesPB>("TabletUpdatesPB");
            REG_METHOD(TabletUpdatesPB, versions_size);
            cls.funcExt<&tablet_updates_pb_version>("versions");
            REG_METHOD(TabletUpdatesPB, apply_version);
            REG_METHOD(TabletUpdatesPB, next_rowset_id);
            REG_METHOD(TabletUpdatesPB, next_log_id);
            cls.funcExt<&proto_to_json<TabletUpdatesPB>>("toString");
        }
        {
            auto& cls = m.klass<EditVersion>("EditVersion");
            cls.funcExt<&get_major>("major");
            cls.funcExt<&get_minor>("minor");
            cls.func<&EditVersion::to_string>("toString");
        }
        {
            auto& cls = m.klass<CompactionInfo>("CompactionInfo");
            REG_VAR(CompactionInfo, start_version);
            REG_VAR(CompactionInfo, inputs);
            REG_VAR(CompactionInfo, output);
        }
        {
            auto& cls = m.klass<EditVersionInfo>("EditVersionInfo");
            REG_VAR(EditVersionInfo, version);
            REG_VAR(EditVersionInfo, creation_time);
            REG_VAR(EditVersionInfo, rowsets);
            REG_VAR(EditVersionInfo, deltas);
            REG_METHOD(EditVersionInfo, get_compaction);
        }
        {
            auto& cls = m.klass<Rowset>("Rowset");
            REG_METHOD(Rowset, rowset_id_str);
            REG_METHOD(Rowset, schema);
            REG_METHOD(Rowset, start_version);
            REG_METHOD(Rowset, end_version);
            REG_METHOD(Rowset, creation_time);
            REG_METHOD(Rowset, data_disk_size);
            REG_METHOD(Rowset, empty);
            REG_METHOD(Rowset, num_rows);
            REG_METHOD(Rowset, total_row_size);
            REG_METHOD(Rowset, txn_id);
            REG_METHOD(Rowset, partition_id);
            REG_METHOD(Rowset, num_segments);
            REG_METHOD(Rowset, num_delete_files);
            REG_METHOD(Rowset, rowset_path);
        }
        {
            auto& cls = m.klass<TabletUpdates>("TabletUpdates");
            REG_METHOD(TabletUpdates, get_error_msg);
            REG_METHOD(TabletUpdates, num_rows);
            REG_METHOD(TabletUpdates, data_size);
            REG_METHOD(TabletUpdates, num_rowsets);
            REG_METHOD(TabletUpdates, max_version);
            REG_METHOD(TabletUpdates, version_count);
            REG_METHOD(TabletUpdates, num_pending);
            REG_METHOD(TabletUpdates, get_compaction_score);
            REG_METHOD(TabletUpdates, version_history_count);
            REG_METHOD(TabletUpdates, get_average_row_size);
            REG_METHOD(TabletUpdates, debug_string);
            REG_METHOD(TabletUpdates, get_version_list);
            REG_METHOD(TabletUpdates, get_edit_version);
            REG_METHOD(TabletUpdates, get_rowset_map);
            cls.funcExt<&tablet_updates_to_pb>("toPB");
        }
        {
            auto& cls = m.klass<DataDir>("DataDir");
            REG_METHOD(DataDir, path);
            REG_METHOD(DataDir, path_hash);
            REG_METHOD(DataDir, is_used);
            REG_METHOD(DataDir, get_meta);
            REG_METHOD(DataDir, is_used);
            REG_METHOD(DataDir, available_bytes);
            REG_METHOD(DataDir, disk_capacity_bytes);
        }
        {
            auto& cls = m.klass<KVStore>("KVStore");
            REG_METHOD(KVStore, compact);
            REG_METHOD(KVStore, flushMemTable);
            REG_METHOD(KVStore, get_stats);
            cls.funcExt<&kv_store_get_live_sst_files_size>("sst_file_size");
        }
        {
            auto& cls = m.klass<StorageEngineRef>("StorageEngine");
            REG_STATIC_METHOD(StorageEngineRef, get_tablet_info);
            REG_STATIC_METHOD(StorageEngineRef, get_tablet_infos);
            REG_STATIC_METHOD(StorageEngineRef, get_tablet);
            REG_STATIC_METHOD(StorageEngineRef, drop_tablet);
            REG_STATIC_METHOD(StorageEngineRef, get_data_dirs);
            REG_STATIC_METHOD(StorageEngineRef, do_compaction);
        }
    }
};

Status execute_script(const std::string& script, std::string& output) {
    wrenbind17::VM vm;
    vm.setPrintFunc([&](const char* text) { output.append(text); });
    auto& m = vm.module("starrocks");
    bind_common(m);
    bind_exec_env(m);
    StorageEngineRef::bind(m);
    vm.runFromSource("main", R"(import "starrocks" for ExecEnv, StorageEngine)");
    try {
        vm.runFromSource("main", script);
    } catch (const std::exception& e) {
        output.append(e.what());
    }
    return Status::OK();
}

} // namespace starrocks