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

#include "vec/functions/function_java_udf.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gutil/strings/substitute.h"
#include "jni.h"
#include "jni_md.h"
#include "runtime/user_function_cache.h"
#include "util/jni-util.h"
#include "vec/columns/column.h"
#include "vec/columns/column_array.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_string.h"
#include "vec/columns/column_vector.h"
#include "vec/columns/columns_number.h"
#include "vec/common/assert_cast.h"
#include "vec/common/string_ref.h"
#include "vec/core/block.h"
#include "vec/data_types/data_type_nullable.h"

const char* EXECUTOR_CLASS = "org/apache/doris/udf/UdfExecutor";
const char* EXECUTOR_CTOR_SIGNATURE = "([B)V";
const char* EXECUTOR_EVALUATE_SIGNATURE = "()V";
const char* EXECUTOR_CLOSE_SIGNATURE = "()V";

namespace doris::vectorized {
JavaFunctionCall::JavaFunctionCall(const TFunction& fn, const DataTypes& argument_types,
                                   const DataTypePtr& return_type)
        : fn_(fn), _argument_types(argument_types), _return_type(return_type) {}

Status JavaFunctionCall::open(FunctionContext* context, FunctionContext::FunctionStateScope scope) {
    JNIEnv* env = nullptr;
    RETURN_IF_ERROR(JniUtil::GetJNIEnv(&env));
    if (env == nullptr) {
        return Status::InternalError("Failed to get/create JVM");
    }
    if (scope == FunctionContext::FunctionStateScope::FRAGMENT_LOCAL) {
        std::shared_ptr<JniEnv> jni_env = std::make_shared<JniEnv>();
        RETURN_IF_ERROR(JniUtil::GetGlobalClassRef(env, EXECUTOR_CLASS, &jni_env->executor_cl));
        jni_env->executor_ctor_id =
                env->GetMethodID(jni_env->executor_cl, "<init>", EXECUTOR_CTOR_SIGNATURE);
        RETURN_ERROR_IF_EXC(env);
        jni_env->executor_evaluate_id =
                env->GetMethodID(jni_env->executor_cl, "evaluate", EXECUTOR_EVALUATE_SIGNATURE);
        RETURN_ERROR_IF_EXC(env);
        jni_env->executor_close_id =
                env->GetMethodID(jni_env->executor_cl, "close", EXECUTOR_CLOSE_SIGNATURE);
        RETURN_ERROR_IF_EXC(env);
        context->set_function_state(FunctionContext::FRAGMENT_LOCAL, jni_env);
    }
    JniEnv* jni_env =
            reinterpret_cast<JniEnv*>(context->get_function_state(FunctionContext::FRAGMENT_LOCAL));
    std::shared_ptr<JniContext> jni_ctx = std::make_shared<JniContext>(
            _argument_types.size(), jni_env->executor_cl, jni_env->executor_close_id);
    context->set_function_state(FunctionContext::THREAD_LOCAL, jni_ctx);

    // Add a scoped cleanup jni reference object. This cleans up local refs made below.
    JniLocalFrame jni_frame;
    {
        std::string local_location;
        auto function_cache = UserFunctionCache::instance();
        RETURN_IF_ERROR(function_cache->get_jarpath(fn_.id, fn_.hdfs_location, fn_.checksum,
                                                    &local_location));
        TJavaUdfExecutorCtorParams ctor_params;
        ctor_params.__set_fn(fn_);
        ctor_params.__set_location(local_location);
        ctor_params.__set_input_offsets_ptrs((int64_t)jni_ctx->input_offsets_ptrs.get());
        ctor_params.__set_input_buffer_ptrs((int64_t)jni_ctx->input_values_buffer_ptr.get());
        ctor_params.__set_input_nulls_ptrs((int64_t)jni_ctx->input_nulls_buffer_ptr.get());
        ctor_params.__set_input_array_nulls_buffer_ptr(
                (int64_t)jni_ctx->input_array_nulls_buffer_ptr.get());
        ctor_params.__set_input_array_string_offsets_ptrs(
                (int64_t)jni_ctx->input_array_string_offsets_ptrs.get());
        ctor_params.__set_output_buffer_ptr((int64_t)jni_ctx->output_value_buffer.get());
        ctor_params.__set_output_null_ptr((int64_t)jni_ctx->output_null_value.get());
        ctor_params.__set_output_offsets_ptr((int64_t)jni_ctx->output_offsets_ptr.get());
        ctor_params.__set_output_array_null_ptr((int64_t)jni_ctx->output_array_null_ptr.get());
        ctor_params.__set_output_array_string_offsets_ptr(
                (int64_t)jni_ctx->output_array_string_offsets_ptr.get());
        ctor_params.__set_output_intermediate_state_ptr(
                (int64_t)jni_ctx->output_intermediate_state_ptr.get());
        ctor_params.__set_batch_size_ptr((int64_t)jni_ctx->batch_size_ptr.get());

        jbyteArray ctor_params_bytes;

        // Pushed frame will be popped when jni_frame goes out-of-scope.
        RETURN_IF_ERROR(jni_frame.push(env));

        RETURN_IF_ERROR(SerializeThriftMsg(env, &ctor_params, &ctor_params_bytes));

        jni_ctx->executor =
                env->NewObject(jni_env->executor_cl, jni_env->executor_ctor_id, ctor_params_bytes);

        jbyte* pBytes = env->GetByteArrayElements(ctor_params_bytes, nullptr);
        env->ReleaseByteArrayElements(ctor_params_bytes, pBytes, JNI_ABORT);
        env->DeleteLocalRef(ctor_params_bytes);
    }
    RETURN_ERROR_IF_EXC(env);
    RETURN_IF_ERROR(JniUtil::LocalToGlobalRef(env, jni_ctx->executor, &jni_ctx->executor));

    return Status::OK();
}

Status JavaFunctionCall::execute(FunctionContext* context, Block& block,
                                 const ColumnNumbers& arguments, size_t result, size_t num_rows,
                                 bool dry_run) {
    JNIEnv* env = nullptr;
    RETURN_IF_ERROR(JniUtil::GetJNIEnv(&env));
    JniContext* jni_ctx = reinterpret_cast<JniContext*>(
            context->get_function_state(FunctionContext::THREAD_LOCAL));
    JniEnv* jni_env =
            reinterpret_cast<JniEnv*>(context->get_function_state(FunctionContext::FRAGMENT_LOCAL));
    int arg_idx = 0;
    ColumnPtr data_cols[arguments.size()];
    ColumnPtr null_cols[arguments.size()];
    for (size_t col_idx : arguments) {
        ColumnWithTypeAndName& column = block.get_by_position(col_idx);
        data_cols[arg_idx] = column.column->convert_to_full_column_if_const();
        if (!_argument_types[arg_idx]->equals(*column.type)) {
            return Status::InvalidArgument(strings::Substitute(
                    "$0-th input column's type $1 does not equal to required type $2", arg_idx,
                    column.type->get_name(), _argument_types[arg_idx]->get_name()));
        }
        if (auto* nullable = check_and_get_column<const ColumnNullable>(*data_cols[arg_idx])) {
            null_cols[arg_idx] = nullable->get_null_map_column_ptr();
            jni_ctx->input_nulls_buffer_ptr.get()[arg_idx] = reinterpret_cast<int64_t>(
                    check_and_get_column<ColumnVector<UInt8>>(null_cols[arg_idx])
                            ->get_data()
                            .data());
            data_cols[arg_idx] = nullable->get_nested_column_ptr();
        } else {
            jni_ctx->input_nulls_buffer_ptr.get()[arg_idx] = -1;
        }

        if (data_cols[arg_idx]->is_column_string()) {
            const ColumnString* str_col =
                    assert_cast<const ColumnString*>(data_cols[arg_idx].get());
            jni_ctx->input_values_buffer_ptr.get()[arg_idx] =
                    reinterpret_cast<int64_t>(str_col->get_chars().data());
            jni_ctx->input_offsets_ptrs.get()[arg_idx] =
                    reinterpret_cast<int64_t>(str_col->get_offsets().data());
        } else if (data_cols[arg_idx]->is_numeric() || data_cols[arg_idx]->is_column_decimal()) {
            jni_ctx->input_values_buffer_ptr.get()[arg_idx] =
                    reinterpret_cast<int64_t>(data_cols[arg_idx]->get_raw_data().data);
        } else if (data_cols[arg_idx]->is_column_array()) {
            const ColumnArray* array_col =
                    assert_cast<const ColumnArray*>(data_cols[arg_idx].get());
            jni_ctx->input_offsets_ptrs.get()[arg_idx] =
                    reinterpret_cast<int64_t>(array_col->get_offsets_column().get_raw_data().data);
            const ColumnNullable& array_nested_nullable =
                    assert_cast<const ColumnNullable&>(array_col->get_data());
            auto data_column_null_map = array_nested_nullable.get_null_map_column_ptr();
            auto data_column = array_nested_nullable.get_nested_column_ptr();
            jni_ctx->input_array_nulls_buffer_ptr.get()[arg_idx] = reinterpret_cast<int64_t>(
                    check_and_get_column<ColumnVector<UInt8>>(data_column_null_map)
                            ->get_data()
                            .data());

            //need pass FE, nullamp and offset, chars
            if (data_column->is_column_string()) {
                const ColumnString* col = assert_cast<const ColumnString*>(data_column.get());
                jni_ctx->input_values_buffer_ptr.get()[arg_idx] =
                        reinterpret_cast<int64_t>(col->get_chars().data());
                jni_ctx->input_array_string_offsets_ptrs.get()[arg_idx] =
                        reinterpret_cast<int64_t>(col->get_offsets().data());
            } else {
                jni_ctx->input_values_buffer_ptr.get()[arg_idx] =
                        reinterpret_cast<int64_t>(data_column->get_raw_data().data);
            }
        } else {
            return Status::InvalidArgument(
                    strings::Substitute("Java UDF doesn't support type $0 now !",
                                        _argument_types[arg_idx]->get_name()));
        }
        arg_idx++;
    }
    *(jni_ctx->batch_size_ptr) = num_rows;
    auto return_type = block.get_data_type(result);
    if (return_type->is_nullable()) {
        auto null_type = std::reinterpret_pointer_cast<const DataTypeNullable>(return_type);
        auto data_col = null_type->get_nested_type()->create_column();
        auto null_col = ColumnUInt8::create(data_col->size(), 0);
        null_col->resize(num_rows);

        *(jni_ctx->output_null_value) = reinterpret_cast<int64_t>(null_col->get_data().data());
#ifndef EVALUATE_JAVA_UDF
#define EVALUATE_JAVA_UDF                                                                         \
    if (data_col->is_column_string()) {                                                           \
        const ColumnString* str_col = assert_cast<const ColumnString*>(data_col.get());           \
        ColumnString::Chars& chars = const_cast<ColumnString::Chars&>(str_col->get_chars());      \
        ColumnString::Offsets& offsets =                                                          \
                const_cast<ColumnString::Offsets&>(str_col->get_offsets());                       \
        int increase_buffer_size = 0;                                                             \
        int64_t buffer_size = JniUtil::IncreaseReservedBufferSize(increase_buffer_size);          \
        chars.resize(buffer_size);                                                                \
        offsets.resize(num_rows);                                                                 \
        *(jni_ctx->output_value_buffer) = reinterpret_cast<int64_t>(chars.data());                \
        *(jni_ctx->output_offsets_ptr) = reinterpret_cast<int64_t>(offsets.data());               \
        jni_ctx->output_intermediate_state_ptr->row_idx = 0;                                      \
        jni_ctx->output_intermediate_state_ptr->buffer_size = buffer_size;                        \
        env->CallNonvirtualVoidMethodA(jni_ctx->executor, jni_env->executor_cl,                   \
                                       jni_env->executor_evaluate_id, nullptr);                   \
        while (jni_ctx->output_intermediate_state_ptr->row_idx < num_rows) {                      \
            increase_buffer_size++;                                                               \
            buffer_size = JniUtil::IncreaseReservedBufferSize(increase_buffer_size);              \
            chars.resize(buffer_size);                                                            \
            *(jni_ctx->output_value_buffer) = reinterpret_cast<int64_t>(chars.data());            \
            jni_ctx->output_intermediate_state_ptr->buffer_size = buffer_size;                    \
            env->CallNonvirtualVoidMethodA(jni_ctx->executor, jni_env->executor_cl,               \
                                           jni_env->executor_evaluate_id, nullptr);               \
        }                                                                                         \
    } else if (data_col->is_numeric() || data_col->is_column_decimal()) {                         \
        data_col->resize(num_rows);                                                               \
        *(jni_ctx->output_value_buffer) =                                                         \
                reinterpret_cast<int64_t>(data_col->get_raw_data().data);                         \
        env->CallNonvirtualVoidMethodA(jni_ctx->executor, jni_env->executor_cl,                   \
                                       jni_env->executor_evaluate_id, nullptr);                   \
    } else if (data_col->is_column_array()) {                                                     \
        ColumnArray* array_col = assert_cast<ColumnArray*>(data_col.get());                       \
        ColumnNullable& array_nested_nullable =                                                   \
                assert_cast<ColumnNullable&>(array_col->get_data());                              \
        auto data_column_null_map = array_nested_nullable.get_null_map_column_ptr();              \
        auto data_column = array_nested_nullable.get_nested_column_ptr();                         \
        auto& offset_column = array_col->get_offsets_column();                                    \
        int increase_buffer_size = 0;                                                             \
        int64_t buffer_size = JniUtil::IncreaseReservedBufferSize(increase_buffer_size);          \
        offset_column.resize(num_rows);                                                           \
        *(jni_ctx->output_offsets_ptr) =                                                          \
                reinterpret_cast<int64_t>(offset_column.get_raw_data().data);                     \
        data_column_null_map->resize(buffer_size);                                                \
        auto& null_map_data =                                                                     \
                assert_cast<ColumnVector<UInt8>*>(data_column_null_map.get())->get_data();        \
        *(jni_ctx->output_array_null_ptr) = reinterpret_cast<int64_t>(null_map_data.data());      \
        jni_ctx->output_intermediate_state_ptr->row_idx = 0;                                      \
        jni_ctx->output_intermediate_state_ptr->buffer_size = buffer_size;                        \
        if (data_column->is_column_string()) {                                                    \
            ColumnString* str_col = assert_cast<ColumnString*>(data_column.get());                \
            ColumnString::Chars& chars = assert_cast<ColumnString::Chars&>(str_col->get_chars()); \
            ColumnString::Offsets& offsets =                                                      \
                    assert_cast<ColumnString::Offsets&>(str_col->get_offsets());                  \
            chars.resize(buffer_size);                                                            \
            offsets.resize(buffer_size);                                                          \
            *(jni_ctx->output_value_buffer) = reinterpret_cast<int64_t>(chars.data());            \
            *(jni_ctx->output_array_string_offsets_ptr) =                                         \
                    reinterpret_cast<int64_t>(offsets.data());                                    \
            env->CallNonvirtualVoidMethodA(jni_ctx->executor, jni_env->executor_cl,               \
                                           jni_env->executor_evaluate_id, nullptr);               \
            while (jni_ctx->output_intermediate_state_ptr->row_idx < num_rows) {                  \
                increase_buffer_size++;                                                           \
                buffer_size = JniUtil::IncreaseReservedBufferSize(increase_buffer_size);          \
                null_map_data.resize(buffer_size);                                                \
                chars.resize(buffer_size);                                                        \
                offsets.resize(buffer_size);                                                      \
                *(jni_ctx->output_array_null_ptr) =                                               \
                        reinterpret_cast<int64_t>(null_map_data.data());                          \
                *(jni_ctx->output_value_buffer) = reinterpret_cast<int64_t>(chars.data());        \
                *(jni_ctx->output_array_string_offsets_ptr) =                                     \
                        reinterpret_cast<int64_t>(offsets.data());                                \
                jni_ctx->output_intermediate_state_ptr->buffer_size = buffer_size;                \
                env->CallNonvirtualVoidMethodA(jni_ctx->executor, jni_env->executor_cl,           \
                                               jni_env->executor_evaluate_id, nullptr);           \
            }                                                                                     \
        } else {                                                                                  \
            data_column->resize(buffer_size);                                                     \
            *(jni_ctx->output_value_buffer) =                                                     \
                    reinterpret_cast<int64_t>(data_column->get_raw_data().data);                  \
            env->CallNonvirtualVoidMethodA(jni_ctx->executor, jni_env->executor_cl,               \
                                           jni_env->executor_evaluate_id, nullptr);               \
            while (jni_ctx->output_intermediate_state_ptr->row_idx < num_rows) {                  \
                increase_buffer_size++;                                                           \
                buffer_size = JniUtil::IncreaseReservedBufferSize(increase_buffer_size);          \
                null_map_data.resize(buffer_size);                                                \
                data_column->resize(buffer_size);                                                 \
                *(jni_ctx->output_array_null_ptr) =                                               \
                        reinterpret_cast<int64_t>(null_map_data.data());                          \
                *(jni_ctx->output_value_buffer) =                                                 \
                        reinterpret_cast<int64_t>(data_column->get_raw_data().data);              \
                jni_ctx->output_intermediate_state_ptr->buffer_size = buffer_size;                \
                env->CallNonvirtualVoidMethodA(jni_ctx->executor, jni_env->executor_cl,           \
                                               jni_env->executor_evaluate_id, nullptr);           \
            }                                                                                     \
        }                                                                                         \
    } else {                                                                                      \
        return Status::InvalidArgument(strings::Substitute(                                       \
                "Java UDF doesn't support return type $0 now !", return_type->get_name()));       \
    }
#endif
        EVALUATE_JAVA_UDF;
        block.replace_by_position(result,
                                  ColumnNullable::create(std::move(data_col), std::move(null_col)));
    } else {
        *(jni_ctx->output_null_value) = -1;
        auto data_col = return_type->create_column();
        EVALUATE_JAVA_UDF;
        block.replace_by_position(result, std::move(data_col));
    }
    return JniUtil::GetJniExceptionMsg(env);
}

Status JavaFunctionCall::close(FunctionContext* context,
                               FunctionContext::FunctionStateScope scope) {
    JniContext* jni_ctx = reinterpret_cast<JniContext*>(
            context->get_function_state(FunctionContext::THREAD_LOCAL));
    // JNIContext own some resource and its release method depend on JavaFunctionCall
    // has to release the resource before JavaFunctionCall is deconstructed.
    if (jni_ctx) {
        jni_ctx->close();
    }
    return Status::OK();
}
} // namespace doris::vectorized
