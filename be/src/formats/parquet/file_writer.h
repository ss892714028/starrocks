
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

#pragma once

#include <arrow/api.h>
#include <arrow/buffer.h>
#include <arrow/io/api.h>
#include <arrow/io/file.h>
#include <arrow/io/interfaces.h>
#include <gen_cpp/DataSinks_types.h>
#include <parquet/api/reader.h>
#include <parquet/api/writer.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/exception.h>

#include "column/chunk.h"
#include "fs/fs.h"
#include "runtime/runtime_state.h"
#include "util/priority_thread_pool.hpp"

namespace starrocks::parquet {

class ParquetOutputStream : public arrow::io::OutputStream {
public:
    ParquetOutputStream(std::unique_ptr<starrocks::WritableFile> wfile);
    ~ParquetOutputStream() override;

    arrow::Status Write(const void* data, int64_t nbytes) override;
    arrow::Status Write(const std::shared_ptr<arrow::Buffer>& data) override;
    arrow::Status Close() override;
    arrow::Result<int64_t> Tell() const override;

    bool closed() const override { return _is_closed; };

private:
    std::unique_ptr<starrocks::WritableFile> _wfile;
    bool _is_closed = false;

    enum HEADER_STATE {
        INITED = 1,
        CACHED = 2,
        WRITEN = 3,
    };
    HEADER_STATE _header_state = INITED;
};

class ParquetBuildHelper {
public:
    static void build_file_data_type(::parquet::Type::type& parquet_data_type, const LogicalType& column_data_type);

    static void build_parquet_repetition_type(::parquet::Repetition::type& parquet_repetition_type,
                                              const bool is_nullable);

    static void build_compression_type(::parquet::WriterProperties::Builder& builder,
                                       const TCompressionType::type& compression_type);
};

class FileWriterBase {
public:
    FileWriterBase(std::unique_ptr<WritableFile> writable_file, std::shared_ptr<::parquet::WriterProperties> properties,
                   std::shared_ptr<::parquet::schema::GroupNode> schema,
                   const std::vector<ExprContext*>& output_expr_ctxs);
    virtual ~FileWriterBase() = default;

    Status init();
    Status write(Chunk* chunk);
    std::size_t file_size() const;
    void set_max_row_group_size(int64_t rg_size) { _max_row_group_size = rg_size; }
    std::shared_ptr<::parquet::FileMetaData> metadata() const { return _file_metadata; }
    Status split_offsets(std::vector<int64_t>& splitOffsets) const;
    virtual bool closed() const = 0;

protected:
    virtual void _flush_row_group() = 0;

private:
    ::parquet::RowGroupWriter* _get_rg_writer();
    std::size_t _get_current_rg_written_bytes() const;

protected:
    std::shared_ptr<ParquetOutputStream> _outstream;
    std::shared_ptr<::parquet::WriterProperties> _properties;
    std::shared_ptr<::parquet::schema::GroupNode> _schema;
    std::unique_ptr<::parquet::ParquetFileWriter> _writer;
    ::parquet::RowGroupWriter* _rg_writer = nullptr;
    std::vector<ExprContext*> _output_expr_ctxs;
    std::shared_ptr<::parquet::FileMetaData> _file_metadata;

    const static int64_t kDefaultMaxRowGroupSize = 128 * 1024 * 1024; // 128MB
    int64_t _max_row_group_size = kDefaultMaxRowGroupSize;
    std::vector<int64_t> _buffered_values_estimate;
};

class SyncFileWriter : public FileWriterBase {
public:
    SyncFileWriter(std::unique_ptr<WritableFile> writable_file, std::shared_ptr<::parquet::WriterProperties> properties,
                   std::shared_ptr<::parquet::schema::GroupNode> schema,
                   const std::vector<ExprContext*>& output_expr_ctxs)
            : FileWriterBase(std::move(writable_file), std::move(properties), std::move(schema), output_expr_ctxs) {}
    ~SyncFileWriter() override = default;

    Status close();
    bool closed() const override { return _closed; }

private:
    void _flush_row_group() override;
    bool _closed = false;
};

class AsyncFileWriter : public FileWriterBase {
public:
    AsyncFileWriter(std::unique_ptr<WritableFile> writable_file, std::string file_name, std::string& file_dir,
                    std::shared_ptr<::parquet::WriterProperties> properties,
                    std::shared_ptr<::parquet::schema::GroupNode> schema,
                    const std::vector<ExprContext*>& output_expr_ctxs, PriorityThreadPool* executor_pool,
                    RuntimeProfile* parent_profile);

    ~AsyncFileWriter() override = default;

    Status close(RuntimeState* state,
                 std::function<void(starrocks::parquet::AsyncFileWriter*, RuntimeState*)> cb = nullptr);

    bool writable() {
        auto lock = std::unique_lock(_m);
        return !_rg_writer_closing;
    }
    bool closed() const override { return _closed.load(); }

    std::string file_name() const { return _file_name; }

    std::string file_dir() const { return _file_dir; }

private:
    void _flush_row_group() override;

    std::string _file_name;
    std::string _file_dir;

    std::atomic<bool> _closed = false;

    PriorityThreadPool* _executor_pool;

    RuntimeProfile* _parent_profile = nullptr;
    RuntimeProfile::Counter* _io_timer = nullptr;

    std::condition_variable _cv;
    bool _rg_writer_closing = false;
    std::mutex _m;
};

} // namespace starrocks::parquet
