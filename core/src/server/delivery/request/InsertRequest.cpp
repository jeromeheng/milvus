// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/delivery/request/InsertRequest.h"
#include "db/Utils.h"
#include "server/DBWrapper.h"
#include "utils/CommonUtil.h"
#include "utils/Log.h"
#include "utils/TimeRecorder.h"
#include "utils/ValidationUtil.h"

#include <fiu-local.h>
#include <memory>
#include <string>
#include <vector>

#ifdef MILVUS_ENABLE_PROFILING
#include <gperftools/profiler.h>
#endif

namespace milvus {
namespace server {

InsertRequest::InsertRequest(const std::shared_ptr<milvus::server::Context>& context,
                             const std::string& collection_name, engine::VectorsData& vectors,
                             const std::string& partition_tag)
    : BaseRequest(context, BaseRequest::kInsert),
      collection_name_(collection_name),
      vectors_data_(vectors),
      partition_tag_(partition_tag) {
}

BaseRequestPtr
InsertRequest::Create(const std::shared_ptr<milvus::server::Context>& context, const std::string& collection_name,
                      engine::VectorsData& vectors, const std::string& partition_tag) {
    return std::shared_ptr<BaseRequest>(new InsertRequest(context, collection_name, vectors, partition_tag));
}

Status
InsertRequest::OnExecute() {
    SERVER_LOG_INFO << LogOut("[%s][%ld] ", "insert", 0) << "Execute insert request.";
    try {
        int64_t vector_count = vectors_data_.vector_count_;
        fiu_do_on("InsertRequest.OnExecute.throw_std_exception", throw std::exception());
        std::string hdr = "InsertRequest(collection=" + collection_name_ + ", n=" + std::to_string(vector_count) +
                          ", partition_tag=" + partition_tag_ + ")";
        TimeRecorder rc(LogOut("[%s][%ld] %s", "insert", 0, hdr.c_str()));

        // step 1: check arguments
        auto status = ValidationUtil::ValidateCollectionName(collection_name_);
        if (!status.ok()) {
            SERVER_LOG_ERROR << LogOut("[%s][%ld] Invalid collection name: %s", "insert", 0, status.message().c_str());
            return status;
        }
        if (vectors_data_.float_data_.empty() && vectors_data_.binary_data_.empty()) {
            std::string msg = "The vector array is empty. Make sure you have entered vector records.";
            SERVER_LOG_ERROR << LogOut("[%s][%ld] Invalid records: %s", "insert", 0, msg.c_str());
            return Status(SERVER_INVALID_ROWRECORD_ARRAY, msg);
        }

        fiu_do_on("InsertRequest.OnExecute.id_array_error", vectors_data_.id_array_.resize(vector_count + 1));
        if (!vectors_data_.id_array_.empty()) {
            if (vectors_data_.id_array_.size() != vector_count) {
                std::string msg = "The size of vector ID array must be equal to the size of the vector.";
                SERVER_LOG_ERROR << LogOut("[%s][%ld] Invalid id array: %s", "insert", 0, msg.c_str());
                return Status(SERVER_ILLEGAL_VECTOR_ID, msg);
            }
        }

        // step 2: check collection existence
        // only process root collection, ignore partition collection
        engine::meta::CollectionSchema table_schema;
        table_schema.collection_id_ = collection_name_;
        status = DBWrapper::DB()->DescribeCollection(table_schema);
        fiu_do_on("InsertRequest.OnExecute.db_not_found", status = Status(milvus::DB_NOT_FOUND, ""));
        fiu_do_on("InsertRequest.OnExecute.describe_table_fail", status = Status(milvus::SERVER_UNEXPECTED_ERROR, ""));
        if (!status.ok()) {
            if (status.code() == DB_NOT_FOUND) {
                SERVER_LOG_ERROR << LogOut("[%s][%ld] Collection %s not found", "insert", 0, collection_name_.c_str());
                return Status(SERVER_TABLE_NOT_EXIST, TableNotExistMsg(collection_name_));
            } else {
                SERVER_LOG_ERROR << LogOut("[%s][%ld] Describe collection %s fail: %s", "insert", 0,
                                           collection_name_.c_str(), status.message().c_str());
                return status;
            }
        } else {
            if (!table_schema.owner_collection_.empty()) {
                SERVER_LOG_ERROR << LogOut("[%s][%ld] owner collection of %s is empty", "insert", 0,
                                           collection_name_.c_str());
                return Status(SERVER_INVALID_TABLE_NAME, TableNotExistMsg(collection_name_));
            }
        }

        // step 3: check collection flag
        // all user provide id, or all internal id
        bool user_provide_ids = !vectors_data_.id_array_.empty();
        fiu_do_on("InsertRequest.OnExecute.illegal_vector_id", user_provide_ids = false;
                  table_schema.flag_ = engine::meta::FLAG_MASK_HAS_USERID);
        // user already provided id before, all insert action require user id
        if ((table_schema.flag_ & engine::meta::FLAG_MASK_HAS_USERID) != 0 && !user_provide_ids) {
            std::string msg = "Entities IDs are user-defined. Please provide IDs for all entities of the collection.";
            SERVER_LOG_ERROR << LogOut("[%s][%ld] %s", "insert", 0, msg.c_str());
            return Status(SERVER_ILLEGAL_VECTOR_ID, msg);
        }

        fiu_do_on("InsertRequest.OnExecute.illegal_vector_id2", user_provide_ids = true;
                  table_schema.flag_ = engine::meta::FLAG_MASK_NO_USERID);
        // user didn't provided id before, no need to provide user id
        if ((table_schema.flag_ & engine::meta::FLAG_MASK_NO_USERID) != 0 && user_provide_ids) {
            std::string msg =
                "Entities IDs are auto-generated. All vectors of this collection must use auto-generated IDs.";
            return Status(SERVER_ILLEGAL_VECTOR_ID, msg);
        }

        rc.RecordSection("check validation");

#ifdef MILVUS_ENABLE_PROFILING
        std::string fname = "/tmp/insert_" + CommonUtil::GetCurrentTimeStr() + ".profiling";
        ProfilerStart(fname.c_str());
#endif
        // step 4: some metric type doesn't support float vectors
        if (!vectors_data_.float_data_.empty()) {  // insert float vectors
            if (engine::utils::IsBinaryMetricType(table_schema.metric_type_)) {
                std::string msg = "Collection metric type doesn't support float vectors.";
                SERVER_LOG_ERROR << LogOut("[%s][%ld] %s", "insert", 0, msg.c_str());
                return Status(SERVER_INVALID_ROWRECORD_ARRAY, msg);
            }

            // check prepared float data
            if (vectors_data_.float_data_.size() % vector_count != 0) {
                std::string msg = "The vector dimension must be equal to the collection dimension.";
                SERVER_LOG_ERROR << LogOut("[%s][%ld] %s", "insert", 0, msg.c_str());
                return Status(SERVER_INVALID_ROWRECORD_ARRAY, msg);
            }

            fiu_do_on("InsertRequest.OnExecute.invalid_dim", table_schema.dimension_ = -1);
            if (vectors_data_.float_data_.size() / vector_count != table_schema.dimension_) {
                std::string msg = "The vector dimension must be equal to the collection dimension.";
                SERVER_LOG_ERROR << LogOut("[%s][%ld] %s", "insert", 0, msg.c_str());
                return Status(SERVER_INVALID_VECTOR_DIMENSION, msg);
            }
        } else if (!vectors_data_.binary_data_.empty()) {  // insert binary vectors
            if (!engine::utils::IsBinaryMetricType(table_schema.metric_type_)) {
                std::string msg = "Collection metric type doesn't support binary vectors.";
                SERVER_LOG_ERROR << LogOut("[%s][%ld] %s", "insert", 0, msg.c_str());
                return Status(SERVER_INVALID_ROWRECORD_ARRAY, msg);
            }

            // check prepared binary data
            if (vectors_data_.binary_data_.size() % vector_count != 0) {
                std::string msg = "The vector dimension must be equal to the collection dimension.";
                SERVER_LOG_ERROR << LogOut("[%s][%ld] %s", "insert", 0, msg.c_str());
                return Status(SERVER_INVALID_ROWRECORD_ARRAY, msg);
            }

            if (vectors_data_.binary_data_.size() * 8 / vector_count != table_schema.dimension_) {
                std::string msg = "The vector dimension must be equal to the collection dimension.";
                SERVER_LOG_ERROR << LogOut("[%s][%ld] %s", "insert", 0, msg.c_str());
                return Status(SERVER_INVALID_VECTOR_DIMENSION, msg);
            }
        }

        // step 5: insert vectors
        auto vec_count = static_cast<uint64_t>(vector_count);

        rc.RecordSection("prepare vectors data");
        status = DBWrapper::DB()->InsertVectors(collection_name_, partition_tag_, vectors_data_);
        fiu_do_on("InsertRequest.OnExecute.insert_fail", status = Status(milvus::SERVER_UNEXPECTED_ERROR, ""));
        if (!status.ok()) {
            SERVER_LOG_ERROR << LogOut("[%s][%ld] Insert fail: %s", "insert", 0, status.message().c_str());
            return status;
        }

        auto ids_size = vectors_data_.id_array_.size();
        fiu_do_on("InsertRequest.OnExecute.invalid_ids_size", ids_size = vec_count - 1);
        if (ids_size != vec_count) {
            std::string msg =
                "Add " + std::to_string(vec_count) + " vectors but only return " + std::to_string(ids_size) + " id";
            SERVER_LOG_ERROR << LogOut("[%s][%ld] Insert fail: %s", "insert", 0, msg.c_str());
            return Status(SERVER_ILLEGAL_VECTOR_ID, msg);
        }

        // step 6: update collection flag
        user_provide_ids ? table_schema.flag_ |= engine::meta::FLAG_MASK_HAS_USERID
                         : table_schema.flag_ |= engine::meta::FLAG_MASK_NO_USERID;
        status = DBWrapper::DB()->UpdateCollectionFlag(collection_name_, table_schema.flag_);

#ifdef MILVUS_ENABLE_PROFILING
        ProfilerStop();
#endif

        rc.RecordSection("add vectors to engine");
        rc.ElapseFromBegin("total cost");
    } catch (std::exception& ex) {
        SERVER_LOG_ERROR << LogOut("[%s][%ld] Encounter exception: %s", "insert", 0, ex.what());
        return Status(SERVER_UNEXPECTED_ERROR, ex.what());
    }

    return Status::OK();
}

}  // namespace server
}  // namespace milvus
