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

// Functions for pandas conversion via NumPy

#include "arrow/python/numpy_interop.h"  // IWYU pragma: expand

#include "arrow/python/arrow_to_pandas.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/status.h"
#include "arrow/table.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/hashing.h"
#include "arrow/util/logging.h"
#include "arrow/util/macros.h"
#include "arrow/util/parallel.h"
#include "arrow/util/string_view.h"
#include "arrow/visitor_inline.h"

#include "arrow/compute/api.h"

#include "arrow/python/common.h"
#include "arrow/python/config.h"
#include "arrow/python/datetime.h"
#include "arrow/python/decimal.h"
#include "arrow/python/helpers.h"
#include "arrow/python/numpy_convert.h"
#include "arrow/python/numpy_internal.h"
#include "arrow/python/pyarrow.h"
#include "arrow/python/python_to_arrow.h"
#include "arrow/python/type_traits.h"

namespace arrow {

class MemoryPool;

using internal::checked_cast;
using internal::ParallelFor;

namespace py {

using internal::kNanosecondsInDay;
using internal::kPandasTimestampNull;

using compute::Datum;
using compute::FunctionContext;

// ----------------------------------------------------------------------
// Utility code

template <typename T>
struct WrapBytes {};

template <>
struct WrapBytes<StringType> {
  static inline PyObject* Wrap(const char* data, int64_t length) {
    return PyUnicode_FromStringAndSize(data, length);
  }
};

template <>
struct WrapBytes<LargeStringType> {
  static inline PyObject* Wrap(const char* data, int64_t length) {
    return PyUnicode_FromStringAndSize(data, length);
  }
};

template <>
struct WrapBytes<BinaryType> {
  static inline PyObject* Wrap(const char* data, int64_t length) {
    return PyBytes_FromStringAndSize(data, length);
  }
};

template <>
struct WrapBytes<LargeBinaryType> {
  static inline PyObject* Wrap(const char* data, int64_t length) {
    return PyBytes_FromStringAndSize(data, length);
  }
};

template <>
struct WrapBytes<FixedSizeBinaryType> {
  static inline PyObject* Wrap(const char* data, int64_t length) {
    return PyBytes_FromStringAndSize(data, length);
  }
};

static inline bool ListTypeSupported(const DataType& type) {
  switch (type.id()) {
    case Type::BOOL:
    case Type::UINT8:
    case Type::INT8:
    case Type::UINT16:
    case Type::INT16:
    case Type::UINT32:
    case Type::INT32:
    case Type::INT64:
    case Type::UINT64:
    case Type::FLOAT:
    case Type::DOUBLE:
    case Type::DECIMAL:
    case Type::BINARY:
    case Type::STRING:
    case Type::DATE32:
    case Type::DATE64:
    case Type::TIME32:
    case Type::TIME64:
    case Type::TIMESTAMP:
    case Type::DURATION:
    case Type::NA:  // empty list
      // The above types are all supported.
      return true;
    case Type::LIST: {
      const ListType& list_type = checked_cast<const ListType&>(type);
      return ListTypeSupported(*list_type.value_type());
    }
    default:
      break;
  }
  return false;
}
// ----------------------------------------------------------------------
// PyCapsule code for setting ndarray base to reference C++ object

struct ArrayCapsule {
  std::shared_ptr<Array> array;
};

struct BufferCapsule {
  std::shared_ptr<Buffer> buffer;
};

namespace {

void ArrayCapsule_Destructor(PyObject* capsule) {
  delete reinterpret_cast<ArrayCapsule*>(PyCapsule_GetPointer(capsule, "arrow::Array"));
}

void BufferCapsule_Destructor(PyObject* capsule) {
  delete reinterpret_cast<BufferCapsule*>(PyCapsule_GetPointer(capsule, "arrow::Buffer"));
}

}  // namespace

Status CapsulizeArray(const std::shared_ptr<Array>& arr, PyObject** out) {
  auto capsule = new ArrayCapsule{{arr}};
  *out = PyCapsule_New(reinterpret_cast<void*>(capsule), "arrow::Array",
                       &ArrayCapsule_Destructor);
  if (*out == nullptr) {
    delete capsule;
    RETURN_IF_PYERROR();
  }
  return Status::OK();
}

Status CapsulizeBuffer(const std::shared_ptr<Buffer>& buffer, PyObject** out) {
  auto capsule = new BufferCapsule{{buffer}};
  *out = PyCapsule_New(reinterpret_cast<void*>(capsule), "arrow::Buffer",
                       &BufferCapsule_Destructor);
  if (*out == nullptr) {
    delete capsule;
    RETURN_IF_PYERROR();
  }
  return Status::OK();
}

Status SetNdarrayBase(PyArrayObject* arr, PyObject* base) {
  if (PyArray_SetBaseObject(arr, base) == -1) {
    // Error occurred, trust that SetBaseObject sets the error state
    Py_XDECREF(base);
    RETURN_IF_PYERROR();
  }
  return Status::OK();
}

Status SetBufferBase(PyArrayObject* arr, const std::shared_ptr<Buffer>& buffer) {
  PyObject* base;
  RETURN_NOT_OK(CapsulizeBuffer(buffer, &base));
  return SetNdarrayBase(arr, base);
}

// ----------------------------------------------------------------------
// pandas 0.x DataFrame conversion internals

inline void set_numpy_metadata(int type, const DataType* datatype, PyArray_Descr* out) {
  if (type == NPY_DATETIME) {
    auto date_dtype = reinterpret_cast<PyArray_DatetimeDTypeMetaData*>(out->c_metadata);
    if (datatype->id() == Type::TIMESTAMP) {
      const auto& timestamp_type = checked_cast<const TimestampType&>(*datatype);

      switch (timestamp_type.unit()) {
        case TimestampType::Unit::SECOND:
          date_dtype->meta.base = NPY_FR_s;
          break;
        case TimestampType::Unit::MILLI:
          date_dtype->meta.base = NPY_FR_ms;
          break;
        case TimestampType::Unit::MICRO:
          date_dtype->meta.base = NPY_FR_us;
          break;
        case TimestampType::Unit::NANO:
          date_dtype->meta.base = NPY_FR_ns;
          break;
      }
    } else {
      // datatype->type == Type::DATE64
      date_dtype->meta.base = NPY_FR_D;
    }
  } else if (type == NPY_TIMEDELTA) {
    DCHECK_EQ(datatype->id(), Type::DURATION);
    auto timedelta_dtype =
        reinterpret_cast<PyArray_DatetimeDTypeMetaData*>(out->c_metadata);
    const auto& duration_type = checked_cast<const DurationType&>(*datatype);

    switch (duration_type.unit()) {
      case DurationType::Unit::SECOND:
        timedelta_dtype->meta.base = NPY_FR_s;
        break;
      case DurationType::Unit::MILLI:
        timedelta_dtype->meta.base = NPY_FR_ms;
        break;
      case DurationType::Unit::MICRO:
        timedelta_dtype->meta.base = NPY_FR_us;
        break;
      case DurationType::Unit::NANO:
        timedelta_dtype->meta.base = NPY_FR_ns;
        break;
    }
  }
}

namespace {

Status PyArray_NewFromPool(int nd, npy_intp* dims, PyArray_Descr* descr,
                           const DataType* arrow_type, MemoryPool* pool, PyObject** out) {
  // ARROW-6570: Allocate memory from MemoryPool for a couple reasons
  //
  // * Track allocations
  // * Get better performance through custom allocators
  if (arrow_type != nullptr) {
    set_numpy_metadata(descr->type_num, arrow_type, descr);
  }

  int64_t total_size = descr->elsize;
  for (int i = 0; i < nd; ++i) {
    total_size *= dims[i];
  }

  std::shared_ptr<Buffer> buffer;
  RETURN_NOT_OK(AllocateBuffer(pool, total_size, &buffer));
  *out = PyArray_NewFromDescr(&PyArray_Type, descr, nd, dims,
                              /*strides=*/nullptr,
                              /*data=*/buffer->mutable_data(),
                              /*flags=*/NPY_ARRAY_CARRAY | NPY_ARRAY_WRITEABLE,
                              /*obj=*/nullptr);
  if (*out == nullptr) {
    RETURN_IF_PYERROR();
    // Trust that error set if NULL returned
  }
  return SetBufferBase(reinterpret_cast<PyArrayObject*>(*out), buffer);
}

}  // namespace

class PandasBlock {
 public:
  enum type {
    OBJECT,
    UINT8,
    INT8,
    UINT16,
    INT16,
    UINT32,
    INT32,
    UINT64,
    INT64,
    HALF_FLOAT,
    FLOAT,
    DOUBLE,
    BOOL,
    DATETIME,
    DATETIME_WITH_TZ,
    TIMEDELTA,
    CATEGORICAL,
    EXTENSION
  };

  PandasBlock(const PandasOptions& options, int64_t num_rows, int num_columns)
      : num_rows_(num_rows), num_columns_(num_columns), options_(options) {}
  virtual ~PandasBlock() {}

  virtual Status Allocate() = 0;
  virtual Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
                       int64_t rel_placement) = 0;

  PyObject* block_arr() const { return block_arr_.obj(); }

  virtual Status GetPyResult(PyObject** output) {
    PyObject* result = PyDict_New();
    RETURN_IF_PYERROR();

    PyDict_SetItemString(result, "block", block_arr_.obj());
    PyDict_SetItemString(result, "placement", placement_arr_.obj());

    *output = result;

    return Status::OK();
  }

 protected:
  Status AllocateNDArray(int npy_type, int ndim = 2) {
    PyAcquireGIL lock;

    PyObject* block_arr;
    npy_intp block_dims[2] = {0, 0};

    if (ndim == 2) {
      block_dims[0] = num_columns_;
      block_dims[1] = num_rows_;
    } else {
      block_dims[0] = num_rows_;
    }
    PyArray_Descr* descr = internal::GetSafeNumPyDtype(npy_type);
    if (PyDataType_REFCHK(descr)) {
      // ARROW-6876: if the array has refcounted items, let Numpy
      // own the array memory so as to decref elements on array destruction
      block_arr = PyArray_SimpleNewFromDescr(ndim, block_dims, descr);
      RETURN_IF_PYERROR();
    } else {
      RETURN_NOT_OK(PyArray_NewFromPool(ndim, block_dims, descr,
                                        /*arrow_type=*/nullptr, options_.pool,
                                        &block_arr));
    }

    npy_intp placement_dims[1] = {num_columns_};
    PyObject* placement_arr = PyArray_SimpleNew(1, placement_dims, NPY_INT64);

    RETURN_IF_PYERROR();

    block_arr_.reset(block_arr);
    placement_arr_.reset(placement_arr);

    block_data_ = reinterpret_cast<uint8_t*>(
        PyArray_DATA(reinterpret_cast<PyArrayObject*>(block_arr)));

    placement_data_ = reinterpret_cast<int64_t*>(
        PyArray_DATA(reinterpret_cast<PyArrayObject*>(placement_arr)));

    return Status::OK();
  }

  int64_t num_rows_;
  int num_columns_;

  OwnedRefNoGIL block_arr_;
  uint8_t* block_data_;

  PandasOptions options_;

  // ndarray<int32>
  OwnedRefNoGIL placement_arr_;
  int64_t* placement_data_;

 private:
  ARROW_DISALLOW_COPY_AND_ASSIGN(PandasBlock);
};

template <typename T>
inline const T* GetPrimitiveValues(const Array& arr) {
  if (arr.length() == 0) {
    return nullptr;
  }
  const auto& prim_arr = checked_cast<const PrimitiveArray&>(arr);
  const T* raw_values = reinterpret_cast<const T*>(prim_arr.values()->data());
  return raw_values + arr.offset();
}

template <typename T>
inline void ConvertIntegerWithNulls(const PandasOptions& options,
                                    const ChunkedArray& data, double* out_values) {
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = *data.chunk(c);
    const T* in_values = GetPrimitiveValues<T>(arr);
    // Upcast to double, set NaN as appropriate

    for (int i = 0; i < arr.length(); ++i) {
      *out_values++ = arr.IsNull(i) ? NAN : static_cast<double>(in_values[i]);
    }
  }
}

template <typename T>
inline void ConvertIntegerNoNullsSameType(const PandasOptions& options,
                                          const ChunkedArray& data, T* out_values) {
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = *data.chunk(c);
    if (arr.length() > 0) {
      const T* in_values = GetPrimitiveValues<T>(arr);
      memcpy(out_values, in_values, sizeof(T) * arr.length());
      out_values += arr.length();
    }
  }
}

template <typename InType, typename OutType>
inline void ConvertIntegerNoNullsCast(const PandasOptions& options,
                                      const ChunkedArray& data, OutType* out_values) {
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = *data.chunk(c);
    const InType* in_values = GetPrimitiveValues<InType>(arr);
    for (int64_t i = 0; i < arr.length(); ++i) {
      *out_values = in_values[i];
    }
  }
}

static Status ConvertBooleanWithNulls(const PandasOptions& options,
                                      const ChunkedArray& data, PyObject** out_values) {
  PyAcquireGIL lock;
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = checked_cast<const BooleanArray&>(*data.chunk(c));

    for (int64_t i = 0; i < arr.length(); ++i) {
      if (arr.IsNull(i)) {
        Py_INCREF(Py_None);
        *out_values++ = Py_None;
      } else if (arr.Value(i)) {
        // True
        Py_INCREF(Py_True);
        *out_values++ = Py_True;
      } else {
        // False
        Py_INCREF(Py_False);
        *out_values++ = Py_False;
      }
    }
  }
  return Status::OK();
}

static void ConvertBooleanNoNulls(const PandasOptions& options, const ChunkedArray& data,
                                  uint8_t* out_values) {
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = checked_cast<const BooleanArray&>(*data.chunk(c));
    for (int64_t i = 0; i < arr.length(); ++i) {
      *out_values++ = static_cast<uint8_t>(arr.Value(i));
    }
  }
}

// Generic Array -> PyObject** converter that handles object deduplication, if
// requested
template <typename ArrayType, typename WriteValue>
inline Status WriteArrayObjects(const ArrayType& arr, WriteValue&& write_func,
                                PyObject** out_values) {
  const bool has_nulls = arr.null_count() > 0;
  for (int64_t i = 0; i < arr.length(); ++i) {
    if (has_nulls && arr.IsNull(i)) {
      Py_INCREF(Py_None);
      *out_values = Py_None;
    } else {
      RETURN_NOT_OK(write_func(arr.GetView(i), out_values));
    }
    ++out_values;
  }
  return Status::OK();
}

template <typename T, typename Enable = void>
struct MemoizationTraits {
  using Scalar = typename T::c_type;
};

template <typename T>
struct MemoizationTraits<T, enable_if_has_string_view<T>> {
  // For binary, we memoize string_view as a scalar value to avoid having to
  // unnecessarily copy the memory into the memo table data structure
  using Scalar = util::string_view;
};

template <typename Type, typename WrapFunction>
inline Status ConvertAsPyObjects(const PandasOptions& options, const ChunkedArray& data,
                                 WrapFunction&& wrap_func, PyObject** out_values) {
  using ArrayType = typename TypeTraits<Type>::ArrayType;
  using Scalar = typename MemoizationTraits<Type>::Scalar;

  PyAcquireGIL lock;
  // TODO(fsaintjacques): propagate memory pool.
  ::arrow::internal::ScalarMemoTable<Scalar> memo_table(default_memory_pool());
  std::vector<PyObject*> unique_values;
  int32_t memo_size = 0;

  auto WrapMemoized = [&](const Scalar& value, PyObject** out_values) {
    int32_t memo_index = memo_table.GetOrInsert(value);
    if (memo_index == memo_size) {
      // New entry
      RETURN_NOT_OK(wrap_func(value, out_values));
      unique_values.push_back(*out_values);
      ++memo_size;
    } else {
      // Duplicate entry
      Py_INCREF(unique_values[memo_index]);
      *out_values = unique_values[memo_index];
    }
    return Status::OK();
  };

  auto WrapUnmemoized = [&](const Scalar& value, PyObject** out_values) {
    return wrap_func(value, out_values);
  };

  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = checked_cast<const ArrayType&>(*data.chunk(c));
    if (options.deduplicate_objects) {
      RETURN_NOT_OK(WriteArrayObjects(arr, WrapMemoized, out_values));
    } else {
      RETURN_NOT_OK(WriteArrayObjects(arr, WrapUnmemoized, out_values));
    }
    out_values += arr.length();
  }
  return Status::OK();
}

template <typename Type>
static Status ConvertIntegerObjects(const PandasOptions& options,
                                    const ChunkedArray& data, PyObject** out_values) {
  using T = typename Type::c_type;
  auto WrapValue = [](T value, PyObject** out) {
    *out = std::is_signed<T>::value ? PyLong_FromLongLong(value)
                                    : PyLong_FromUnsignedLongLong(value);
    RETURN_IF_PYERROR();
    return Status::OK();
  };
  return ConvertAsPyObjects<Type>(options, data, WrapValue, out_values);
}

template <typename Type>
inline Status ConvertBinaryLike(const PandasOptions& options, const ChunkedArray& data,
                                PyObject** out_values) {
  auto WrapValue = [](const util::string_view& view, PyObject** out) {
    *out = WrapBytes<Type>::Wrap(view.data(), view.length());
    if (*out == nullptr) {
      PyErr_Clear();
      return Status::UnknownError("Wrapping ", view, " failed");
    }
    return Status::OK();
  };
  return ConvertAsPyObjects<Type>(options, data, WrapValue, out_values);
}

inline Status ConvertNulls(const PandasOptions& options, const ChunkedArray& data,
                           PyObject** out_values) {
  PyAcquireGIL lock;
  for (int c = 0; c < data.num_chunks(); c++) {
    std::shared_ptr<Array> arr = data.chunk(c);

    for (int64_t i = 0; i < arr->length(); ++i) {
      // All values are null
      Py_INCREF(Py_None);
      *out_values = Py_None;
      ++out_values;
    }
  }
  return Status::OK();
}

inline Status ConvertStruct(const PandasOptions& options, const ChunkedArray& data,
                            PyObject** out_values) {
  PyAcquireGIL lock;
  if (data.num_chunks() == 0) {
    return Status::OK();
  }
  // ChunkedArray has at least one chunk
  auto arr = checked_cast<const StructArray*>(data.chunk(0).get());
  // Use it to cache the struct type and number of fields for all chunks
  int32_t num_fields = arr->num_fields();
  auto array_type = arr->type();
  std::vector<OwnedRef> fields_data(num_fields);
  OwnedRef dict_item;
  for (int c = 0; c < data.num_chunks(); c++) {
    auto arr = checked_cast<const StructArray*>(data.chunk(c).get());
    // Convert the struct arrays first
    for (int32_t i = 0; i < num_fields; i++) {
      PyObject* numpy_array;
      RETURN_NOT_OK(ConvertArrayToPandas(options, arr->field(static_cast<int>(i)),
                                         nullptr, &numpy_array));
      fields_data[i].reset(numpy_array);
    }

    // Construct a dictionary for each row
    const bool has_nulls = data.null_count() > 0;
    for (int64_t i = 0; i < arr->length(); ++i) {
      if (has_nulls && arr->IsNull(i)) {
        Py_INCREF(Py_None);
        *out_values = Py_None;
      } else {
        // Build the new dict object for the row
        dict_item.reset(PyDict_New());
        RETURN_IF_PYERROR();
        for (int32_t field_idx = 0; field_idx < num_fields; ++field_idx) {
          OwnedRef field_value;
          auto name = array_type->child(static_cast<int>(field_idx))->name();
          if (!arr->field(static_cast<int>(field_idx))->IsNull(i)) {
            // Value exists in child array, obtain it
            auto array = reinterpret_cast<PyArrayObject*>(fields_data[field_idx].obj());
            auto ptr = reinterpret_cast<const char*>(PyArray_GETPTR1(array, i));
            field_value.reset(PyArray_GETITEM(array, ptr));
            RETURN_IF_PYERROR();
          } else {
            // Translate the Null to a None
            Py_INCREF(Py_None);
            field_value.reset(Py_None);
          }
          // PyDict_SetItemString increments reference count
          auto setitem_result =
              PyDict_SetItemString(dict_item.obj(), name.c_str(), field_value.obj());
          RETURN_IF_PYERROR();
          DCHECK_EQ(setitem_result, 0);
        }
        *out_values = dict_item.obj();
        // Grant ownership to the resulting array
        Py_INCREF(*out_values);
      }
      ++out_values;
    }
  }
  return Status::OK();
}

template <typename ArrowType>
inline Status ConvertListsLike(const PandasOptions& options, const ChunkedArray& data,
                               PyObject** out_values) {
  // Get column of underlying value arrays
  std::vector<std::shared_ptr<Array>> value_arrays;
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = checked_cast<const ListArray&>(*data.chunk(c));
    value_arrays.emplace_back(arr.values());
  }
  auto value_type = checked_cast<const ListType&>(*data.type()).value_type();
  auto flat_column = std::make_shared<ChunkedArray>(value_arrays, value_type);
  // TODO(ARROW-489): Currently we don't have a Python reference for single columns.
  //    Storing a reference to the whole Array would be to expensive.

  OwnedRefNoGIL owned_numpy_array;
  RETURN_NOT_OK(ConvertChunkedArrayToPandas(options, flat_column, nullptr,
                                            owned_numpy_array.ref()));

  PyObject* numpy_array = owned_numpy_array.obj();

  PyAcquireGIL lock;

  int64_t chunk_offset = 0;
  for (int c = 0; c < data.num_chunks(); c++) {
    auto arr = std::static_pointer_cast<ListArray>(data.chunk(c));

    const bool has_nulls = data.null_count() > 0;
    for (int64_t i = 0; i < arr->length(); ++i) {
      if (has_nulls && arr->IsNull(i)) {
        Py_INCREF(Py_None);
        *out_values = Py_None;
      } else {
        OwnedRef start(PyLong_FromLongLong(arr->value_offset(i) + chunk_offset));
        OwnedRef end(PyLong_FromLongLong(arr->value_offset(i + 1) + chunk_offset));
        OwnedRef slice(PySlice_New(start.obj(), end.obj(), nullptr));

        if (ARROW_PREDICT_FALSE(slice.obj() == nullptr)) {
          // Fall out of loop, will return from RETURN_IF_PYERROR
          break;
        }
        *out_values = PyObject_GetItem(numpy_array, slice.obj());

        if (*out_values == nullptr) {
          // Fall out of loop, will return from RETURN_IF_PYERROR
          break;
        }
      }
      ++out_values;
    }
    RETURN_IF_PYERROR();

    chunk_offset += arr->values()->length();
  }

  return Status::OK();
}

template <typename T>
inline void ConvertNumericNullable(const ChunkedArray& data, T na_value, T* out_values) {
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = *data.chunk(c);
    const T* in_values = GetPrimitiveValues<T>(arr);

    if (arr.null_count() > 0) {
      for (int64_t i = 0; i < arr.length(); ++i) {
        *out_values++ = arr.IsNull(i) ? na_value : in_values[i];
      }
    } else {
      memcpy(out_values, in_values, sizeof(T) * arr.length());
      out_values += arr.length();
    }
  }
}

template <typename InType, typename OutType>
inline void ConvertNumericNullableCast(const ChunkedArray& data, OutType na_value,
                                       OutType* out_values) {
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = *data.chunk(c);
    const InType* in_values = GetPrimitiveValues<InType>(arr);

    for (int64_t i = 0; i < arr.length(); ++i) {
      *out_values++ = arr.IsNull(i) ? na_value : static_cast<OutType>(in_values[i]);
    }
  }
}

template <typename T, int64_t SHIFT>
inline void ConvertDatetimeLikeNanos(const ChunkedArray& data, int64_t* out_values) {
  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = *data.chunk(c);
    const T* in_values = GetPrimitiveValues<T>(arr);

    for (int64_t i = 0; i < arr.length(); ++i) {
      *out_values++ = arr.IsNull(i) ? kPandasTimestampNull
                                    : (static_cast<int64_t>(in_values[i]) * SHIFT);
    }
  }
}

template <typename Type>
static Status ConvertDates(const PandasOptions& options, const ChunkedArray& data,
                           PyObject** out_values) {
  auto WrapValue = [](typename Type::c_type value, PyObject** out) {
    RETURN_NOT_OK(internal::PyDate_from_int(value, Type::UNIT, out));
    RETURN_IF_PYERROR();
    return Status::OK();
  };
  return ConvertAsPyObjects<Type>(options, data, WrapValue, out_values);
}

template <typename Type>
static Status ConvertTimes(const PandasOptions& options, const ChunkedArray& data,
                           PyObject** out_values) {
  const TimeUnit::type unit = checked_cast<const Type&>(*data.type()).unit();

  auto WrapValue = [unit](typename Type::c_type value, PyObject** out) {
    RETURN_NOT_OK(internal::PyTime_from_int(value, unit, out));
    RETURN_IF_PYERROR();
    return Status::OK();
  };
  return ConvertAsPyObjects<Type>(options, data, WrapValue, out_values);
}

static Status ConvertDecimals(const PandasOptions& options, const ChunkedArray& data,
                              PyObject** out_values) {
  PyAcquireGIL lock;
  OwnedRef decimal;
  OwnedRef Decimal;
  RETURN_NOT_OK(internal::ImportModule("decimal", &decimal));
  RETURN_NOT_OK(internal::ImportFromModule(decimal.obj(), "Decimal", &Decimal));
  PyObject* decimal_constructor = Decimal.obj();

  for (int c = 0; c < data.num_chunks(); c++) {
    const auto& arr = checked_cast<const arrow::Decimal128Array&>(*data.chunk(c));

    for (int64_t i = 0; i < arr.length(); ++i) {
      if (arr.IsNull(i)) {
        Py_INCREF(Py_None);
        *out_values++ = Py_None;
      } else {
        *out_values++ =
            internal::DecimalFromString(decimal_constructor, arr.FormatValue(i));
        RETURN_IF_PYERROR();
      }
    }
  }

  return Status::OK();
}

#define CONVERTLISTSLIKE_CASE(ArrowType, ArrowEnum)                            \
  case Type::ArrowEnum:                                                        \
    RETURN_NOT_OK((ConvertListsLike<ArrowType>(options_, *data, out_buffer))); \
    break;

class ObjectBlock : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status Allocate() override { return AllocateNDArray(NPY_OBJECT); }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    Type::type type = data->type()->id();

    PyObject** out_buffer =
        reinterpret_cast<PyObject**>(block_data_) + rel_placement * num_rows_;

    if (type == Type::BOOL) {
      RETURN_NOT_OK(ConvertBooleanWithNulls(options_, *data, out_buffer));
    } else if (type == Type::UINT8) {
      RETURN_NOT_OK(ConvertIntegerObjects<UInt8Type>(options_, *data, out_buffer));
    } else if (type == Type::INT8) {
      RETURN_NOT_OK(ConvertIntegerObjects<Int8Type>(options_, *data, out_buffer));
    } else if (type == Type::UINT16) {
      RETURN_NOT_OK(ConvertIntegerObjects<UInt16Type>(options_, *data, out_buffer));
    } else if (type == Type::INT16) {
      RETURN_NOT_OK(ConvertIntegerObjects<Int16Type>(options_, *data, out_buffer));
    } else if (type == Type::UINT32) {
      RETURN_NOT_OK(ConvertIntegerObjects<UInt32Type>(options_, *data, out_buffer));
    } else if (type == Type::INT32) {
      RETURN_NOT_OK(ConvertIntegerObjects<Int32Type>(options_, *data, out_buffer));
    } else if (type == Type::UINT64) {
      RETURN_NOT_OK(ConvertIntegerObjects<UInt64Type>(options_, *data, out_buffer));
    } else if (type == Type::INT64) {
      RETURN_NOT_OK(ConvertIntegerObjects<Int64Type>(options_, *data, out_buffer));
    } else if (type == Type::BINARY) {
      RETURN_NOT_OK(ConvertBinaryLike<BinaryType>(options_, *data, out_buffer));
    } else if (type == Type::LARGE_BINARY) {
      RETURN_NOT_OK(ConvertBinaryLike<LargeBinaryType>(options_, *data, out_buffer));
    } else if (type == Type::STRING) {
      RETURN_NOT_OK(ConvertBinaryLike<StringType>(options_, *data, out_buffer));
    } else if (type == Type::LARGE_STRING) {
      RETURN_NOT_OK(ConvertBinaryLike<LargeStringType>(options_, *data, out_buffer));
    } else if (type == Type::FIXED_SIZE_BINARY) {
      RETURN_NOT_OK(ConvertBinaryLike<FixedSizeBinaryType>(options_, *data, out_buffer));
    } else if (type == Type::DATE32) {
      RETURN_NOT_OK(ConvertDates<Date32Type>(options_, *data, out_buffer));
    } else if (type == Type::DATE64) {
      RETURN_NOT_OK(ConvertDates<Date64Type>(options_, *data, out_buffer));
    } else if (type == Type::TIME32) {
      RETURN_NOT_OK(ConvertTimes<Time32Type>(options_, *data, out_buffer));
    } else if (type == Type::TIME64) {
      RETURN_NOT_OK(ConvertTimes<Time64Type>(options_, *data, out_buffer));
    } else if (type == Type::DECIMAL) {
      RETURN_NOT_OK(ConvertDecimals(options_, *data, out_buffer));
    } else if (type == Type::NA) {
      RETURN_NOT_OK(ConvertNulls(options_, *data, out_buffer));
    } else if (type == Type::LIST) {
      auto list_type = std::static_pointer_cast<ListType>(data->type());
      switch (list_type->value_type()->id()) {
        CONVERTLISTSLIKE_CASE(BooleanType, BOOL)
        CONVERTLISTSLIKE_CASE(UInt8Type, UINT8)
        CONVERTLISTSLIKE_CASE(Int8Type, INT8)
        CONVERTLISTSLIKE_CASE(UInt16Type, UINT16)
        CONVERTLISTSLIKE_CASE(Int16Type, INT16)
        CONVERTLISTSLIKE_CASE(UInt32Type, UINT32)
        CONVERTLISTSLIKE_CASE(Int32Type, INT32)
        CONVERTLISTSLIKE_CASE(UInt64Type, UINT64)
        CONVERTLISTSLIKE_CASE(Int64Type, INT64)
        CONVERTLISTSLIKE_CASE(Date32Type, DATE32)
        CONVERTLISTSLIKE_CASE(Date64Type, DATE64)
        CONVERTLISTSLIKE_CASE(Time32Type, TIME32)
        CONVERTLISTSLIKE_CASE(Time64Type, TIME64)
        CONVERTLISTSLIKE_CASE(TimestampType, TIMESTAMP)
        CONVERTLISTSLIKE_CASE(DurationType, DURATION)
        CONVERTLISTSLIKE_CASE(FloatType, FLOAT)
        CONVERTLISTSLIKE_CASE(DoubleType, DOUBLE)
        CONVERTLISTSLIKE_CASE(DecimalType, DECIMAL)
        CONVERTLISTSLIKE_CASE(BinaryType, BINARY)
        CONVERTLISTSLIKE_CASE(StringType, STRING)
        CONVERTLISTSLIKE_CASE(ListType, LIST)
        CONVERTLISTSLIKE_CASE(NullType, NA)
        default: {
          return Status::NotImplemented(
              "Not implemented type for conversion from List to Pandas ObjectBlock: ",
              list_type->value_type()->ToString());
        }
      }
    } else if (type == Type::STRUCT) {
      RETURN_NOT_OK(ConvertStruct(options_, *data, out_buffer));
    } else {
      return Status::NotImplemented("Unsupported type for object array output: ",
                                    data->type()->ToString());
    }

    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

template <int ARROW_TYPE, typename C_TYPE>
class IntBlock : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status Allocate() override {
    return AllocateNDArray(internal::arrow_traits<ARROW_TYPE>::npy_type);
  }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    Type::type type = data->type()->id();

    C_TYPE* out_buffer =
        reinterpret_cast<C_TYPE*>(block_data_) + rel_placement * num_rows_;

    if (type != ARROW_TYPE) {
      return Status::NotImplemented("Cannot write Arrow data of type ",
                                    data->type()->ToString(), " to a Pandas int",
                                    sizeof(C_TYPE), " block");
    }

    ConvertIntegerNoNullsSameType<C_TYPE>(options_, *data, out_buffer);
    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

using UInt8Block = IntBlock<Type::UINT8, uint8_t>;
using Int8Block = IntBlock<Type::INT8, int8_t>;
using UInt16Block = IntBlock<Type::UINT16, uint16_t>;
using Int16Block = IntBlock<Type::INT16, int16_t>;
using UInt32Block = IntBlock<Type::UINT32, uint32_t>;
using Int32Block = IntBlock<Type::INT32, int32_t>;
using UInt64Block = IntBlock<Type::UINT64, uint64_t>;
using Int64Block = IntBlock<Type::INT64, int64_t>;

class Float16Block : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status Allocate() override { return AllocateNDArray(NPY_FLOAT16); }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    Type::type type = data->type()->id();

    if (type != Type::HALF_FLOAT) {
      return Status::NotImplemented("Cannot write Arrow data of type ",
                                    data->type()->ToString(),
                                    " to a Pandas float16 block");
    }

    npy_half* out_buffer =
        reinterpret_cast<npy_half*>(block_data_) + rel_placement * num_rows_;

    ConvertNumericNullable<npy_half>(*data, NPY_HALF_NAN, out_buffer);
    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

class Float32Block : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status Allocate() override { return AllocateNDArray(NPY_FLOAT32); }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    Type::type type = data->type()->id();

    if (type != Type::FLOAT) {
      return Status::NotImplemented("Cannot write Arrow data of type ",
                                    data->type()->ToString(),
                                    " to a Pandas float32 block");
    }

    float* out_buffer = reinterpret_cast<float*>(block_data_) + rel_placement * num_rows_;

    ConvertNumericNullable<float>(*data, NAN, out_buffer);
    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

class Float64Block : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status Allocate() override { return AllocateNDArray(NPY_FLOAT64); }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    Type::type type = data->type()->id();

    double* out_buffer =
        reinterpret_cast<double*>(block_data_) + rel_placement * num_rows_;

#define INTEGER_CASE(IN_TYPE)                                    \
  ConvertIntegerWithNulls<IN_TYPE>(options_, *data, out_buffer); \
  break;

    switch (type) {
      case Type::UINT8:
        INTEGER_CASE(uint8_t);
      case Type::INT8:
        INTEGER_CASE(int8_t);
      case Type::UINT16:
        INTEGER_CASE(uint16_t);
      case Type::INT16:
        INTEGER_CASE(int16_t);
      case Type::UINT32:
        INTEGER_CASE(uint32_t);
      case Type::INT32:
        INTEGER_CASE(int32_t);
      case Type::UINT64:
        INTEGER_CASE(uint64_t);
      case Type::INT64:
        INTEGER_CASE(int64_t);
      case Type::FLOAT:
        ConvertNumericNullableCast<float, double>(*data, NAN, out_buffer);
        break;
      case Type::DOUBLE:
        ConvertNumericNullable<double>(*data, NAN, out_buffer);
        break;
      default:
        return Status::NotImplemented("Cannot write Arrow data of type ",
                                      data->type()->ToString(),
                                      " to a Pandas float64 block");
    }

#undef INTEGER_CASE

    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

class BoolBlock : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status Allocate() override { return AllocateNDArray(NPY_BOOL); }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    if (data->type()->id() != Type::BOOL) {
      return Status::NotImplemented("Cannot write Arrow data of type ",
                                    data->type()->ToString(),
                                    " to a Pandas boolean block");
    }

    uint8_t* out_buffer =
        reinterpret_cast<uint8_t*>(block_data_) + rel_placement * num_rows_;

    ConvertBooleanNoNulls(options_, *data, out_buffer);
    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

class DatetimeBlock : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status AllocateDatetime(int ndim) {
    RETURN_NOT_OK(AllocateNDArray(NPY_DATETIME, ndim));

    PyAcquireGIL lock;
    auto date_dtype = reinterpret_cast<PyArray_DatetimeDTypeMetaData*>(
        PyArray_DESCR(reinterpret_cast<PyArrayObject*>(block_arr_.obj()))->c_metadata);
    date_dtype->meta.base = NPY_FR_ns;
    return Status::OK();
  }

  Status Allocate() override { return AllocateDatetime(2); }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    Type::type type = data->type()->id();

    int64_t* out_buffer =
        reinterpret_cast<int64_t*>(block_data_) + rel_placement * num_rows_;

    if (type == Type::DATE32) {
      // Convert from days since epoch to datetime64[ns]
      ConvertDatetimeLikeNanos<int32_t, kNanosecondsInDay>(*data, out_buffer);
    } else if (type == Type::DATE64) {
      // Date64Type is millisecond timestamp stored as int64_t
      // TODO(wesm): Do we want to make sure to zero out the milliseconds?
      ConvertDatetimeLikeNanos<int64_t, 1000000L>(*data, out_buffer);
    } else if (type == Type::TIMESTAMP) {
      const auto& ts_type = checked_cast<const TimestampType&>(*data->type());

      if (ts_type.unit() == TimeUnit::NANO) {
        ConvertNumericNullable<int64_t>(*data, kPandasTimestampNull, out_buffer);
      } else if (ts_type.unit() == TimeUnit::MICRO) {
        ConvertDatetimeLikeNanos<int64_t, 1000L>(*data, out_buffer);
      } else if (ts_type.unit() == TimeUnit::MILLI) {
        ConvertDatetimeLikeNanos<int64_t, 1000000L>(*data, out_buffer);
      } else if (ts_type.unit() == TimeUnit::SECOND) {
        ConvertDatetimeLikeNanos<int64_t, 1000000000L>(*data, out_buffer);
      } else {
        return Status::NotImplemented("Unsupported time unit");
      }
    } else {
      return Status::NotImplemented("Cannot write Arrow data of type ",
                                    data->type()->ToString(),
                                    " to a Pandas datetime block.");
    }

    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

class TimedeltaBlock : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;
  Status AllocateDatetime(int ndim) {
    RETURN_NOT_OK(AllocateNDArray(NPY_TIMEDELTA, ndim));

    PyAcquireGIL lock;
    auto date_dtype = reinterpret_cast<PyArray_DatetimeDTypeMetaData*>(
        PyArray_DESCR(reinterpret_cast<PyArrayObject*>(block_arr_.obj()))->c_metadata);
    date_dtype->meta.base = NPY_FR_ns;
    return Status::OK();
  }

  Status Allocate() override { return AllocateDatetime(2); }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    Type::type type = data->type()->id();

    int64_t* out_buffer =
        reinterpret_cast<int64_t*>(block_data_) + rel_placement * num_rows_;

    if (type == Type::DURATION) {
      const auto& ts_type = checked_cast<const DurationType&>(*data->type());

      if (ts_type.unit() == TimeUnit::NANO) {
        ConvertNumericNullable<int64_t>(*data, kPandasTimestampNull, out_buffer);
      } else if (ts_type.unit() == TimeUnit::MICRO) {
        ConvertDatetimeLikeNanos<int64_t, 1000L>(*data, out_buffer);
      } else if (ts_type.unit() == TimeUnit::MILLI) {
        ConvertDatetimeLikeNanos<int64_t, 1000000L>(*data, out_buffer);
      } else if (ts_type.unit() == TimeUnit::SECOND) {
        ConvertDatetimeLikeNanos<int64_t, 1000000000L>(*data, out_buffer);
      } else {
        return Status::NotImplemented("Unsupported time unit");
      }
    } else {
      return Status::NotImplemented("Cannot write Arrow data of type ",
                                    data->type()->ToString(),
                                    " to a Pandas timedelta block.");
    }

    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }
};

class DatetimeTZBlock : public DatetimeBlock {
 public:
  DatetimeTZBlock(const PandasOptions& options, const std::string& timezone,
                  int64_t num_rows)
      : DatetimeBlock(options, num_rows, 1), timezone_(timezone) {}

  // Like Categorical, the internal ndarray is 1-dimensional
  Status Allocate() override { return AllocateDatetime(1); }

  Status GetPyResult(PyObject** output) override {
    PyObject* result = PyDict_New();
    RETURN_IF_PYERROR();

    PyObject* py_tz = PyUnicode_FromStringAndSize(
        timezone_.c_str(), static_cast<Py_ssize_t>(timezone_.size()));
    RETURN_IF_PYERROR();

    PyDict_SetItemString(result, "block", block_arr_.obj());
    PyDict_SetItemString(result, "timezone", py_tz);
    PyDict_SetItemString(result, "placement", placement_arr_.obj());

    *output = result;

    return Status::OK();
  }

 private:
  std::string timezone_;
};

Status MakeZeroLengthArray(const std::shared_ptr<DataType>& type,
                           std::shared_ptr<Array>* out) {
  std::unique_ptr<ArrayBuilder> builder;
  RETURN_NOT_OK(MakeBuilder(default_memory_pool(), type, &builder));
  RETURN_NOT_OK(builder->Resize(0));
  return builder->Finish(out);
}

bool NeedDictionaryUnification(const ChunkedArray& data) {
  if (data.num_chunks() < 2) {
    return false;
  }
  const auto& arr_first = checked_cast<const DictionaryArray&>(*data.chunk(0));
  for (int c = 1; c < data.num_chunks(); c++) {
    const auto& arr = checked_cast<const DictionaryArray&>(*data.chunk(c));
    if (!(arr_first.dictionary()->Equals(arr.dictionary()))) {
      return true;
    }
  }
  return false;
}

template <typename IndexType>
Status CheckDictionaryIndices(const Array& arr, int64_t dict_length) {
  const auto& typed_arr =
      checked_cast<const typename TypeTraits<IndexType>::ArrayType&>(arr);
  const typename IndexType::c_type* values = typed_arr.raw_values();
  for (int64_t i = 0; i < arr.length(); ++i) {
    if (arr.IsValid(i) && (values[i] < 0 || values[i] >= dict_length)) {
      return Status::Invalid("Out of bounds dictionary index: ",
                             static_cast<int64_t>(values[i]));
    }
  }
  return Status::OK();
}

class CategoricalBlock : public PandasBlock {
 public:
  explicit CategoricalBlock(const PandasOptions& options, int64_t num_rows)
      : PandasBlock(options, num_rows, 1), ordered_(false), needs_copy_(false) {}

  Status Allocate() override {
    return Status::NotImplemented(
        "CategoricalBlock allocation happens when calling Write");
  }

  template <typename IndexType>
  Status WriteIndicesUniform(const ChunkedArray& data) {
    using ArrayType = typename TypeTraits<IndexType>::ArrayType;
    using TRAITS = internal::arrow_traits<IndexType::type_id>;
    using T = typename TRAITS::T;

    RETURN_NOT_OK(AllocateNDArray(TRAITS::npy_type, 1));
    T* out_values = reinterpret_cast<T*>(block_data_);

    for (int c = 0; c < data.num_chunks(); c++) {
      const auto& arr = checked_cast<const DictionaryArray&>(*data.chunk(c));
      const auto& indices = checked_cast<const ArrayType&>(*arr.indices());
      auto values = reinterpret_cast<const T*>(indices.raw_values());

      int64_t dict_length = arr.dictionary()->length();
      // Null is -1 in CategoricalBlock
      for (int i = 0; i < arr.length(); ++i) {
        if (indices.IsValid(i)) {
          if (ARROW_PREDICT_FALSE(values[i] < 0 || values[i] >= dict_length)) {
            return Status::Invalid("Out of bounds dictionary index: ",
                                   static_cast<int64_t>(values[i]));
          }
          *out_values++ = values[i];
        } else {
          *out_values++ = -1;
        }
      }
    }
    return Status::OK();
  }

  template <typename IndexType>
  Status WriteIndicesVarying(const ChunkedArray& data, std::shared_ptr<Array>* out_dict) {
    using ArrayType = typename TypeTraits<IndexType>::ArrayType;
    using TRAITS = internal::arrow_traits<IndexType::type_id>;
    using T = typename TRAITS::T;

    // Yield int32 indices to allow for dictionary outgrowing the current index
    // type
    RETURN_NOT_OK(AllocateNDArray(NPY_INT32, 1));
    auto out_values = reinterpret_cast<int32_t*>(block_data_);

    const auto& dict_type = checked_cast<const DictionaryType&>(*data.type());

    std::unique_ptr<DictionaryUnifier> unifier;
    RETURN_NOT_OK(
        DictionaryUnifier::Make(options_.pool, dict_type.value_type(), &unifier));
    for (int c = 0; c < data.num_chunks(); c++) {
      const auto& arr = checked_cast<const DictionaryArray&>(*data.chunk(c));
      const auto& indices = checked_cast<const ArrayType&>(*arr.indices());
      auto values = reinterpret_cast<const T*>(indices.raw_values());

      std::shared_ptr<Buffer> transpose_buffer;
      RETURN_NOT_OK(unifier->Unify(*arr.dictionary(), &transpose_buffer));

      auto transpose = reinterpret_cast<const int32_t*>(transpose_buffer->data());
      int64_t dict_length = arr.dictionary()->length();

      // Null is -1 in CategoricalBlock
      for (int i = 0; i < arr.length(); ++i) {
        if (indices.IsValid(i)) {
          if (ARROW_PREDICT_FALSE(values[i] < 0 || values[i] >= dict_length)) {
            return Status::Invalid("Out of bounds dictionary index: ",
                                   static_cast<int64_t>(values[i]));
          }
          *out_values++ = transpose[values[i]];
        } else {
          *out_values++ = -1;
        }
      }
    }

    std::shared_ptr<DataType> unused_type;
    return unifier->GetResult(&unused_type, out_dict);
  }

  template <typename IndexType>
  Status WriteIndices(const ChunkedArray& data, std::shared_ptr<Array>* out_dict) {
    using ArrayType = typename TypeTraits<IndexType>::ArrayType;
    using TRAITS = internal::arrow_traits<IndexType::type_id>;
    using T = typename TRAITS::T;

    DCHECK_GT(data.num_chunks(), 0);

    // Sniff the first chunk
    const auto& arr_first = checked_cast<const DictionaryArray&>(*data.chunk(0));
    const auto indices_first = std::static_pointer_cast<ArrayType>(arr_first.indices());

    if (!needs_copy_ && data.num_chunks() == 1 && indices_first->null_count() == 0) {
      RETURN_NOT_OK(CheckDictionaryIndices<IndexType>(*indices_first,
                                                      arr_first.dictionary()->length()));
      RETURN_NOT_OK(WrapIndicesZeroCopy<T>(TRAITS::npy_type, indices_first));
      *out_dict = arr_first.dictionary();
    } else {
      if (options_.zero_copy_only) {
        if (needs_copy_) {
          return Status::Invalid("Need to allocate categorical memory, but ",
                                 "only zero-copy conversions "
                                 "allowed");
        }

        return Status::Invalid("Needed to copy ", data.num_chunks(), " chunks with ",
                               indices_first->null_count(),
                               " indices nulls, but zero_copy_only was True");
      }

      if (NeedDictionaryUnification(data)) {
        RETURN_NOT_OK(WriteIndicesVarying<IndexType>(data, out_dict));
      } else {
        RETURN_NOT_OK(WriteIndicesUniform<IndexType>(data));
        *out_dict = arr_first.dictionary();
      }
    }
    return Status::OK();
  }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    if (options_.strings_to_categorical &&
        (data->type()->id() == Type::STRING || data->type()->id() == Type::BINARY)) {
      needs_copy_ = true;
      compute::FunctionContext ctx(options_.pool);

      Datum out;
      RETURN_NOT_OK(compute::DictionaryEncode(&ctx, data, &out));
      DCHECK_EQ(out.kind(), Datum::CHUNKED_ARRAY);
      data = out.chunked_array();
    }

    const auto& dict_type = checked_cast<const DictionaryType&>(*data->type());
    std::shared_ptr<Array> dict;
    if (data->num_chunks() == 0) {
      // no dictionary values => create empty array
      RETURN_NOT_OK(AllocateNDArray(/* any type */ NPY_INT32, 1));
      RETURN_NOT_OK(MakeZeroLengthArray(dict_type.value_type(), &dict));
    } else {
      switch (dict_type.index_type()->id()) {
        case Type::INT8:
          RETURN_NOT_OK(WriteIndices<Int8Type>(*data, &dict));
          break;
        case Type::INT16:
          RETURN_NOT_OK(WriteIndices<Int16Type>(*data, &dict));
          break;
        case Type::INT32:
          RETURN_NOT_OK(WriteIndices<Int32Type>(*data, &dict));
          break;
        case Type::INT64:
          RETURN_NOT_OK(WriteIndices<Int64Type>(*data, &dict));
          break;
        default: {
          return Status::NotImplemented("Categorical index type not supported: ",
                                        dict_type.index_type()->ToString());
        }
      }
    }

    placement_data_[rel_placement] = abs_placement;
    PyObject* pydict;
    RETURN_NOT_OK(ConvertArrayToPandas(options_, dict, nullptr, &pydict));
    dictionary_.reset(pydict);
    ordered_ = dict_type.ordered();

    return Status::OK();
  }

  Status GetPyResult(PyObject** output) override {
    PyObject* result = PyDict_New();
    RETURN_IF_PYERROR();

    PyDict_SetItemString(result, "block", block_arr_.obj());
    PyDict_SetItemString(result, "dictionary", dictionary_.obj());
    PyDict_SetItemString(result, "placement", placement_arr_.obj());

    PyObject* py_ordered = ordered_ ? Py_True : Py_False;
    Py_INCREF(py_ordered);
    PyDict_SetItemString(result, "ordered", py_ordered);

    *output = result;

    return Status::OK();
  }

  PyObject* dictionary() const { return dictionary_.obj(); }

 protected:
  template <typename T>
  Status WrapIndicesZeroCopy(int npy_type,
                             const std::shared_ptr<PrimitiveArray>& indices) {
    const T* in_values = GetPrimitiveValues<T>(*indices);
    void* data = const_cast<T*>(in_values);

    PyAcquireGIL lock;

    PyArray_Descr* descr = internal::GetSafeNumPyDtype(npy_type);
    if (descr == nullptr) {
      // Error occurred, trust error state is set
      return Status::OK();
    }

    npy_intp block_dims[1] = {num_rows_};
    PyObject* block_arr = PyArray_NewFromDescr(&PyArray_Type, descr, 1, block_dims,
                                               nullptr, data, NPY_ARRAY_CARRAY, nullptr);
    RETURN_IF_PYERROR();

    // Add a reference to the underlying Array. Otherwise the array may be
    // deleted once we leave the block conversion.
    PyObject* base;
    RETURN_NOT_OK(CapsulizeArray(indices, &base));
    RETURN_NOT_OK(SetNdarrayBase(reinterpret_cast<PyArrayObject*>(block_arr), base));

    npy_intp placement_dims[1] = {num_columns_};
    PyObject* placement_arr = PyArray_SimpleNew(1, placement_dims, NPY_INT64);
    RETURN_IF_PYERROR();

    block_arr_.reset(block_arr);
    placement_arr_.reset(placement_arr);

    block_data_ = reinterpret_cast<uint8_t*>(
        PyArray_DATA(reinterpret_cast<PyArrayObject*>(block_arr)));

    placement_data_ = reinterpret_cast<int64_t*>(
        PyArray_DATA(reinterpret_cast<PyArrayObject*>(placement_arr)));

    return Status::OK();
  }

  OwnedRefNoGIL dictionary_;
  bool ordered_;
  bool needs_copy_;
};

class ExtensionBlock : public PandasBlock {
 public:
  using PandasBlock::PandasBlock;

  // Don't create a block array here, only the placement array
  Status Allocate() override {
    PyAcquireGIL lock;

    npy_intp placement_dims[1] = {num_columns_};
    PyObject* placement_arr = PyArray_SimpleNew(1, placement_dims, NPY_INT64);

    RETURN_IF_PYERROR();

    placement_arr_.reset(placement_arr);

    placement_data_ = reinterpret_cast<int64_t*>(
        PyArray_DATA(reinterpret_cast<PyArrayObject*>(placement_arr)));

    return Status::OK();
  }

  Status Write(std::shared_ptr<ChunkedArray> data, int64_t abs_placement,
               int64_t rel_placement) override {
    PyAcquireGIL lock;

    PyObject* py_array;
    py_array = wrap_chunked_array(data);
    py_array_.reset(py_array);

    placement_data_[rel_placement] = abs_placement;
    return Status::OK();
  }

  Status GetPyResult(PyObject** output) override {
    PyObject* result = PyDict_New();
    RETURN_IF_PYERROR();

    PyDict_SetItemString(result, "py_array", py_array_.obj());
    PyDict_SetItemString(result, "placement", placement_arr_.obj());

    *output = result;

    return Status::OK();
  }

 protected:
  OwnedRefNoGIL py_array_;
};

Status MakeBlock(const PandasOptions& options, PandasBlock::type type, int64_t num_rows,
                 int num_columns, std::shared_ptr<PandasBlock>* block) {
#define BLOCK_CASE(NAME, TYPE)                                       \
  case PandasBlock::NAME:                                            \
    *block = std::make_shared<TYPE>(options, num_rows, num_columns); \
    break;

  switch (type) {
    BLOCK_CASE(OBJECT, ObjectBlock);
    BLOCK_CASE(UINT8, UInt8Block);
    BLOCK_CASE(INT8, Int8Block);
    BLOCK_CASE(UINT16, UInt16Block);
    BLOCK_CASE(INT16, Int16Block);
    BLOCK_CASE(UINT32, UInt32Block);
    BLOCK_CASE(INT32, Int32Block);
    BLOCK_CASE(UINT64, UInt64Block);
    BLOCK_CASE(INT64, Int64Block);
    BLOCK_CASE(HALF_FLOAT, Float16Block);
    BLOCK_CASE(FLOAT, Float32Block);
    BLOCK_CASE(DOUBLE, Float64Block);
    BLOCK_CASE(BOOL, BoolBlock);
    BLOCK_CASE(DATETIME, DatetimeBlock);
    BLOCK_CASE(TIMEDELTA, TimedeltaBlock);
    default:
      return Status::NotImplemented("Unsupported block type");
  }

#undef BLOCK_CASE

  return (*block)->Allocate();
}

using BlockMap = std::unordered_map<int, std::shared_ptr<PandasBlock>>;

static Status GetPandasBlockType(const ChunkedArray& data, const PandasOptions& options,
                                 PandasBlock::type* output_type) {
#define INTEGER_CASE(NAME)                                                           \
  *output_type =                                                                     \
      data.null_count() > 0                                                          \
          ? options.integer_object_nulls ? PandasBlock::OBJECT : PandasBlock::DOUBLE \
          : PandasBlock::NAME;                                                       \
  break;

  switch (data.type()->id()) {
    case Type::BOOL:
      *output_type = data.null_count() > 0 ? PandasBlock::OBJECT : PandasBlock::BOOL;
      break;
    case Type::UINT8:
      INTEGER_CASE(UINT8);
    case Type::INT8:
      INTEGER_CASE(INT8);
    case Type::UINT16:
      INTEGER_CASE(UINT16);
    case Type::INT16:
      INTEGER_CASE(INT16);
    case Type::UINT32:
      INTEGER_CASE(UINT32);
    case Type::INT32:
      INTEGER_CASE(INT32);
    case Type::UINT64:
      INTEGER_CASE(UINT64);
    case Type::INT64:
      INTEGER_CASE(INT64);
    case Type::HALF_FLOAT:
      *output_type = PandasBlock::HALF_FLOAT;
      break;
    case Type::FLOAT:
      *output_type = PandasBlock::FLOAT;
      break;
    case Type::DOUBLE:
      *output_type = PandasBlock::DOUBLE;
      break;
    case Type::STRING:        // fall through
    case Type::LARGE_STRING:  // fall through
    case Type::BINARY:        // fall through
    case Type::LARGE_BINARY:
      if (options.strings_to_categorical) {
        *output_type = PandasBlock::CATEGORICAL;
        break;
      }                            // fall through
    case Type::NA:                 // fall through
    case Type::FIXED_SIZE_BINARY:  // fall through
    case Type::STRUCT:             // fall through
    case Type::TIME32:             // fall through
    case Type::TIME64:             // fall through
    case Type::DECIMAL:            // fall through
      *output_type = PandasBlock::OBJECT;
      break;
    case Type::DATE32:  // fall through
    case Type::DATE64:
      *output_type = options.date_as_object ? PandasBlock::OBJECT : PandasBlock::DATETIME;
      break;
    case Type::TIMESTAMP: {
      const auto& ts_type = checked_cast<const TimestampType&>(*data.type());
      if (ts_type.timezone() != "") {
        *output_type = PandasBlock::DATETIME_WITH_TZ;
      } else {
        *output_type = PandasBlock::DATETIME;
      }
    } break;
    case Type::DURATION:
      *output_type = PandasBlock::TIMEDELTA;
      break;
    case Type::LIST: {
      auto list_type = std::static_pointer_cast<ListType>(data.type());
      if (!ListTypeSupported(*list_type->value_type())) {
        return Status::NotImplemented("Not implemented type for list in DataFrameBlock: ",
                                      list_type->value_type()->ToString());
      }
      *output_type = PandasBlock::OBJECT;
    } break;
    case Type::DICTIONARY:
      *output_type = PandasBlock::CATEGORICAL;
      break;
    default:
      return Status::NotImplemented(
          "No known equivalent Pandas block for Arrow data of type ",
          data.type()->ToString(), " is known.");
  }
  return Status::OK();
}

// Construct the exact pandas 0.x "BlockManager" memory layout
//
// * For each column determine the correct output pandas type
// * Allocate 2D blocks (ncols x nrows) for each distinct data type in output
// * Allocate  block placement arrays
// * Write Arrow columns out into each slice of memory; populate block
// * placement arrays as we go
class DataFrameBlockCreator {
 public:
  explicit DataFrameBlockCreator(const PandasOptions& options,
                                 const std::unordered_set<std::string>& extension_columns,
                                 const std::shared_ptr<Table>& table)
      : table_(table), options_(options), extension_columns_(extension_columns) {}

  Status Convert(PyObject** output) {
    column_types_.resize(table_->num_columns());
    column_block_placement_.resize(table_->num_columns());
    type_counts_.clear();
    blocks_.clear();

    RETURN_NOT_OK(CreateBlocks());
    RETURN_NOT_OK(WriteTableToBlocks());

    return GetResultList(output);
  }

  Status CreateBlocks() {
    for (int i = 0; i < table_->num_columns(); ++i) {
      std::shared_ptr<ChunkedArray> col = table_->column(i);
      PandasBlock::type output_type = PandasBlock::OBJECT;
      if (extension_columns_.count(table_->field(i)->name())) {
        output_type = PandasBlock::EXTENSION;
      } else {
        RETURN_NOT_OK(GetPandasBlockType(*col, options_, &output_type));
      }

      int block_placement = 0;
      std::shared_ptr<PandasBlock> block;
      if (output_type == PandasBlock::CATEGORICAL) {
        block = std::make_shared<CategoricalBlock>(options_, table_->num_rows());
        categorical_blocks_[i] = block;
      } else if (output_type == PandasBlock::DATETIME_WITH_TZ) {
        const auto& ts_type = checked_cast<const TimestampType&>(*col->type());
        block = std::make_shared<DatetimeTZBlock>(options_, ts_type.timezone(),
                                                  table_->num_rows());
        RETURN_NOT_OK(block->Allocate());
        datetimetz_blocks_[i] = block;
      } else if (output_type == PandasBlock::EXTENSION) {
        block = std::make_shared<ExtensionBlock>(options_, table_->num_rows(), 1);
        RETURN_NOT_OK(block->Allocate());
        extension_blocks_[i] = block;
      } else {
        auto it = type_counts_.find(output_type);
        if (it != type_counts_.end()) {
          block_placement = it->second;
          // Increment count
          it->second += 1;
        } else {
          // Add key to map
          type_counts_[output_type] = 1;
        }
      }
      column_types_[i] = output_type;
      column_block_placement_[i] = block_placement;
    }

    // Create normal non-categorical blocks
    for (const auto& it : this->type_counts_) {
      PandasBlock::type type = static_cast<PandasBlock::type>(it.first);
      std::shared_ptr<PandasBlock> block;
      RETURN_NOT_OK(
          MakeBlock(this->options_, type, this->table_->num_rows(), it.second, &block));
      this->blocks_[type] = block;
    }
    return Status::OK();
  }

  Status GetBlock(int i, std::shared_ptr<PandasBlock>* block) {
    PandasBlock::type output_type = this->column_types_[i];

    if (output_type == PandasBlock::CATEGORICAL) {
      auto it = this->categorical_blocks_.find(i);
      if (it == this->blocks_.end()) {
        return Status::KeyError("No categorical block allocated");
      }
      *block = it->second;
    } else if (output_type == PandasBlock::DATETIME_WITH_TZ) {
      auto it = this->datetimetz_blocks_.find(i);
      if (it == this->datetimetz_blocks_.end()) {
        return Status::KeyError("No datetimetz block allocated");
      }
      *block = it->second;
    } else if (output_type == PandasBlock::EXTENSION) {
      auto it = this->extension_blocks_.find(i);
      if (it == this->extension_blocks_.end()) {
        return Status::KeyError("No extension block allocated");
      }
      *block = it->second;
    } else {
      auto it = this->blocks_.find(output_type);
      if (it == this->blocks_.end()) {
        return Status::KeyError("No block allocated");
      }
      *block = it->second;
    }
    return Status::OK();
  }

  Status WriteTableToBlocks() {
    auto WriteColumn = [this](int i) {
      std::shared_ptr<PandasBlock> block;
      RETURN_NOT_OK(this->GetBlock(i, &block));
      return block->Write(this->table_->column(i), i, this->column_block_placement_[i]);
    };

    if (options_.use_threads) {
      return ParallelFor(table_->num_columns(), WriteColumn);
    } else {
      for (int i = 0; i < table_->num_columns(); ++i) {
        RETURN_NOT_OK(WriteColumn(i));
      }
      return Status::OK();
    }
  }

  Status AppendBlocks(const BlockMap& blocks, PyObject* list) {
    for (const auto& it : blocks) {
      PyObject* item;
      RETURN_NOT_OK(it.second->GetPyResult(&item));
      if (PyList_Append(list, item) < 0) {
        RETURN_IF_PYERROR();
      }

      // ARROW-1017; PyList_Append increments object refcount
      Py_DECREF(item);
    }
    return Status::OK();
  }

  Status GetResultList(PyObject** out) {
    PyAcquireGIL lock;

    PyObject* result = PyList_New(0);
    RETURN_IF_PYERROR();

    RETURN_NOT_OK(AppendBlocks(blocks_, result));
    RETURN_NOT_OK(AppendBlocks(categorical_blocks_, result));
    RETURN_NOT_OK(AppendBlocks(datetimetz_blocks_, result));
    RETURN_NOT_OK(AppendBlocks(extension_blocks_, result));

    *out = result;
    return Status::OK();
  }

 private:
  std::shared_ptr<Table> table_;

  // column num -> block type id
  std::vector<PandasBlock::type> column_types_;

  // column num -> relative placement within internal block
  std::vector<int> column_block_placement_;

  // block type -> type count
  std::unordered_map<int, int> type_counts_;

  PandasOptions options_;
  std::unordered_set<std::string> extension_columns_;

  // block type -> block
  BlockMap blocks_;

  // column number -> categorical block
  BlockMap categorical_blocks_;

  // column number -> datetimetz block
  BlockMap datetimetz_blocks_;

  // column number -> extension block
  BlockMap extension_blocks_;
};

class ArrowDeserializer {
 public:
  ArrowDeserializer(const PandasOptions& options,
                    const std::shared_ptr<ChunkedArray>& data, PyObject* py_ref)
      : data_(data), options_(options), py_ref_(py_ref) {}

  Status AllocateOutput(int type) {
    PyAcquireGIL lock;

    npy_intp dims[1] = {data_->length()};
    PyArray_Descr* descr = internal::GetSafeNumPyDtype(type);
    if (descr == nullptr) {
      RETURN_IF_PYERROR();
    }
    if (PyDataType_REFCHK(descr)) {
      // ARROW-6876: if the array has refcounted items, let Numpy
      // own the array memory so as to decref elements on array destruction
      set_numpy_metadata(type, data_->type().get(), descr);
      result_ = PyArray_SimpleNewFromDescr(1, dims, descr);
      RETURN_IF_PYERROR();
    } else {
      RETURN_NOT_OK(PyArray_NewFromPool(1, dims, descr, data_->type().get(),
                                        options_.pool, &result_));
    }
    arr_ = reinterpret_cast<PyArrayObject*>(result_);
    return Status::OK();
  }

  template <int TYPE>
  Status ConvertValuesZeroCopy(const PandasOptions& options, int npy_type,
                               const std::shared_ptr<Array>& arr) {
    typedef typename internal::arrow_traits<TYPE>::T T;

    const T* in_values = GetPrimitiveValues<T>(*arr);

    // Zero-Copy. We can pass the data pointer directly to NumPy.
    PyAcquireGIL lock;

    PyArray_Descr* descr = internal::GetSafeNumPyDtype(npy_type);
    npy_intp dims[1] = {arr->length()};
    set_numpy_metadata(npy_type, arr->type().get(), descr);
    result_ = PyArray_NewFromDescr(&PyArray_Type, descr, 1, dims,
                                   /*strides=*/nullptr, const_cast<T*>(in_values),
                                   /*flags=*/0, nullptr);
    arr_ = reinterpret_cast<PyArrayObject*>(result_);

    if (arr_ == nullptr) {
      // Error occurred, trust that error set
      return Status::OK();
    }

    // See ARROW-1973 for the original memory leak report.
    //
    // There are two scenarios: py_ref_ is nullptr or py_ref_ is not nullptr
    //
    //   1. py_ref_ is nullptr (it **was not** passed in to ArrowDeserializer's
    //      constructor)
    //
    //      In this case, the stolen reference must not be incremented since nothing
    //      outside of the PyArrayObject* (the arr_ member) is holding a reference to
    //      it. If we increment this, then we have a memory leak.
    //
    //
    //      Here's an example of how memory can be leaked when converting an arrow Array
    //      of List<Float64>.to a numpy array
    //
    //      1. Create a 1D numpy that is the flattened arrow array.
    //
    //         There's nothing outside of the serializer that owns this new numpy array.
    //
    //      2. Make a capsule for the base array.
    //
    //         The reference count of base is 1.
    //
    //      3. Call PyArray_SetBaseObject(arr_, base)
    //
    //         The reference count is still 1, because the reference is stolen.
    //
    //      4. Increment the reference count of base (unconditionally)
    //
    //         The reference count is now 2. This is okay if there's an object holding
    //         another reference. The PyArrayObject that stole the reference will
    //         eventually decrement the reference count, which will leaves us with a
    //         refcount of 1, with nothing owning that 1 reference. Memory leakage
    //         ensues.
    //
    //   2. py_ref_ is not nullptr (it **was** passed in to ArrowDeserializer's
    //      constructor)
    //
    //      This case is simpler. We assume that the reference accounting is correct
    //      coming in. We need to preserve that accounting knowing that the
    //      PyArrayObject that stole the reference will eventually decref it, thus we
    //      increment the reference count.

    PyObject* base;
    if (py_ref_ == nullptr) {
      RETURN_NOT_OK(CapsulizeArray(arr, &base));
    } else {
      base = py_ref_;
      Py_INCREF(base);
    }

    RETURN_NOT_OK(SetNdarrayBase(arr_, base));

    // Arrow data is immutable.
    PyArray_CLEARFLAGS(arr_, NPY_ARRAY_WRITEABLE);

    return Status::OK();
  }

  // ----------------------------------------------------------------------
  // Allocate new array and deserialize. Can do a zero copy conversion for some
  // types

  template <typename Type>
  enable_if_t<is_floating_type<Type>::value, Status> Visit(const Type& type) {
    constexpr int TYPE = Type::type_id;
    using traits = internal::arrow_traits<TYPE>;

    typedef typename traits::T T;
    int npy_type = traits::npy_type;

    if (data_->num_chunks() == 1 && data_->null_count() == 0) {
      return ConvertValuesZeroCopy<TYPE>(options_, npy_type, data_->chunk(0));
    } else if (options_.zero_copy_only) {
      return Status::Invalid("Needed to copy ", data_->num_chunks(), " chunks with ",
                             data_->null_count(), " nulls, but zero_copy_only was True");
    }

    RETURN_NOT_OK(AllocateOutput(npy_type));
    auto out_values = reinterpret_cast<T*>(PyArray_DATA(arr_));
    ConvertNumericNullable<T>(*data_, traits::na_value, out_values);

    return Status::OK();
  }

  template <typename Type>
  enable_if_t<is_timestamp_type<Type>::value || is_duration_type<Type>::value, Status>
  Visit(const Type& type) {
    constexpr int TYPE = Type::type_id;
    using traits = internal::arrow_traits<TYPE>;
    using c_type = typename Type::c_type;

    typedef typename traits::T T;

    if (data_->num_chunks() == 1 && data_->null_count() == 0) {
      return ConvertValuesZeroCopy<TYPE>(options_, traits::npy_type, data_->chunk(0));
    } else if (options_.zero_copy_only) {
      return Status::Invalid("Copy Needed, but zero_copy_only was True");
    }

    RETURN_NOT_OK(AllocateOutput(traits::npy_type));
    auto out_values = reinterpret_cast<T*>(PyArray_DATA(arr_));

    constexpr T na_value = traits::na_value;
    constexpr int64_t kShift = traits::npy_shift;

    for (int c = 0; c < data_->num_chunks(); c++) {
      const auto& arr = *data_->chunk(c);
      const c_type* in_values = GetPrimitiveValues<c_type>(arr);

      for (int64_t i = 0; i < arr.length(); ++i) {
        *out_values++ = arr.IsNull(i) ? na_value : static_cast<T>(in_values[i]) / kShift;
      }
    }
    return Status::OK();
  }

  template <typename Type>
  enable_if_date<Type, Status> Visit(const Type& type) {
    if (options_.zero_copy_only) {
      return Status::Invalid("Copy Needed, but zero_copy_only was True");
    }
    if (options_.date_as_object) {
      return VisitObjects(ConvertDates<Type>);
    }

    constexpr int TYPE = Type::type_id;
    using traits = internal::arrow_traits<TYPE>;
    using c_type = typename Type::c_type;

    typedef typename traits::T T;

    RETURN_NOT_OK(AllocateOutput(traits::npy_type));
    auto out_values = reinterpret_cast<T*>(PyArray_DATA(arr_));

    constexpr T na_value = traits::na_value;
    constexpr int64_t kShift = traits::npy_shift;

    for (int c = 0; c < data_->num_chunks(); c++) {
      const auto& arr = *data_->chunk(c);
      const c_type* in_values = GetPrimitiveValues<c_type>(arr);

      for (int64_t i = 0; i < arr.length(); ++i) {
        *out_values++ = arr.IsNull(i) ? na_value : static_cast<T>(in_values[i]) / kShift;
      }
    }
    return Status::OK();
  }

  template <typename Type>
  enable_if_time<Type, Status> Visit(const Type& type) {
    return Status::NotImplemented("Don't know how to serialize Arrow time type to NumPy");
  }

  // Integer specialization
  template <typename Type>
  enable_if_integer<Type, Status> Visit(const Type& type) {
    constexpr int TYPE = Type::type_id;
    using traits = internal::arrow_traits<TYPE>;

    typedef typename traits::T T;

    if (data_->num_chunks() == 1 && data_->null_count() == 0) {
      return ConvertValuesZeroCopy<TYPE>(options_, traits::npy_type, data_->chunk(0));
    } else if (options_.zero_copy_only) {
      return Status::Invalid("Needed to copy ", data_->num_chunks(), " chunks with ",
                             data_->null_count(), " nulls, but zero_copy_only was True");
    }

    if (data_->null_count() > 0) {
      if (options_.integer_object_nulls) {
        return VisitObjects(ConvertIntegerObjects<Type>);
      } else {
        RETURN_NOT_OK(AllocateOutput(NPY_FLOAT64));
        auto out_values = reinterpret_cast<double*>(PyArray_DATA(arr_));
        ConvertIntegerWithNulls<T>(options_, *data_, out_values);
      }
    } else {
      RETURN_NOT_OK(AllocateOutput(traits::npy_type));
      auto out_values = reinterpret_cast<T*>(PyArray_DATA(arr_));
      ConvertIntegerNoNullsSameType<T>(options_, *data_, out_values);
    }

    return Status::OK();
  }

  template <typename FUNCTOR>
  inline Status VisitObjects(FUNCTOR func) {
    if (options_.zero_copy_only) {
      return Status::Invalid("Object types need copies, but zero_copy_only was True");
    }
    RETURN_NOT_OK(AllocateOutput(NPY_OBJECT));
    auto out_values = reinterpret_cast<PyObject**>(PyArray_DATA(arr_));
    return func(options_, *data_, out_values);
  }

  // Strings and binary
  template <typename Type>
  enable_if_base_binary<Type, Status> Visit(const Type& type) {
    return VisitObjects(ConvertBinaryLike<Type>);
  }

  // Fixed length binary strings
  Status Visit(const FixedSizeBinaryType& type) {
    return VisitObjects(ConvertBinaryLike<FixedSizeBinaryType>);
  }

  Status Visit(const NullType& type) { return VisitObjects(ConvertNulls); }

  Status Visit(const Decimal128Type& type) { return VisitObjects(ConvertDecimals); }

  Status Visit(const Time32Type& type) { return VisitObjects(ConvertTimes<Time32Type>); }

  Status Visit(const Time64Type& type) { return VisitObjects(ConvertTimes<Time64Type>); }

  Status Visit(const StructType& type) { return VisitObjects(ConvertStruct); }

  // Boolean specialization
  Status Visit(const BooleanType& type) {
    if (options_.zero_copy_only) {
      return Status::Invalid("BooleanType needs copies, but zero_copy_only was True");
    } else if (data_->null_count() > 0) {
      return VisitObjects(ConvertBooleanWithNulls);
    } else {
      RETURN_NOT_OK(AllocateOutput(internal::arrow_traits<Type::BOOL>::npy_type));
      auto out_values = reinterpret_cast<uint8_t*>(PyArray_DATA(arr_));
      ConvertBooleanNoNulls(options_, *data_, out_values);
    }
    return Status::OK();
  }

  Status Visit(const ListType& type) {
    if (options_.zero_copy_only) {
      return Status::Invalid("ListType needs copies, but zero_copy_only was True");
    }
#define CONVERTVALUES_LISTSLIKE_CASE(ArrowType, ArrowEnum) \
  case Type::ArrowEnum:                                    \
    return ConvertListsLike<ArrowType>(options_, *data_, out_values);

    RETURN_NOT_OK(AllocateOutput(NPY_OBJECT));
    auto out_values = reinterpret_cast<PyObject**>(PyArray_DATA(arr_));
    auto list_type = std::static_pointer_cast<ListType>(data_->type());
    switch (list_type->value_type()->id()) {
      CONVERTVALUES_LISTSLIKE_CASE(BooleanType, BOOL)
      CONVERTVALUES_LISTSLIKE_CASE(UInt8Type, UINT8)
      CONVERTVALUES_LISTSLIKE_CASE(Int8Type, INT8)
      CONVERTVALUES_LISTSLIKE_CASE(UInt16Type, UINT16)
      CONVERTVALUES_LISTSLIKE_CASE(Int16Type, INT16)
      CONVERTVALUES_LISTSLIKE_CASE(UInt32Type, UINT32)
      CONVERTVALUES_LISTSLIKE_CASE(Int32Type, INT32)
      CONVERTVALUES_LISTSLIKE_CASE(UInt64Type, UINT64)
      CONVERTVALUES_LISTSLIKE_CASE(Int64Type, INT64)
      CONVERTVALUES_LISTSLIKE_CASE(Date32Type, DATE32)
      CONVERTVALUES_LISTSLIKE_CASE(Date64Type, DATE64)
      CONVERTVALUES_LISTSLIKE_CASE(Time32Type, TIME32)
      CONVERTVALUES_LISTSLIKE_CASE(Time64Type, TIME64)
      CONVERTVALUES_LISTSLIKE_CASE(TimestampType, TIMESTAMP)
      CONVERTVALUES_LISTSLIKE_CASE(DurationType, DURATION)
      CONVERTVALUES_LISTSLIKE_CASE(FloatType, FLOAT)
      CONVERTVALUES_LISTSLIKE_CASE(DoubleType, DOUBLE)
      CONVERTVALUES_LISTSLIKE_CASE(BinaryType, BINARY)
      CONVERTVALUES_LISTSLIKE_CASE(StringType, STRING)
      CONVERTVALUES_LISTSLIKE_CASE(Decimal128Type, DECIMAL)
      CONVERTVALUES_LISTSLIKE_CASE(ListType, LIST)
      default: {
        return Status::NotImplemented("Not implemented type for lists: ",
                                      list_type->value_type()->ToString());
      }
    }
#undef CONVERTVALUES_LISTSLIKE_CASE
  }

  Status Visit(const DictionaryType& type) {
    auto block = std::make_shared<CategoricalBlock>(options_, data_->length());
    RETURN_NOT_OK(block->Write(data_, 0, 0));

    PyAcquireGIL lock;
    result_ = PyDict_New();
    RETURN_IF_PYERROR();

    PyDict_SetItemString(result_, "indices", block->block_arr());
    RETURN_IF_PYERROR();
    PyDict_SetItemString(result_, "dictionary", block->dictionary());
    RETURN_IF_PYERROR();

    PyObject* py_ordered = type.ordered() ? Py_True : Py_False;
    Py_INCREF(py_ordered);
    PyDict_SetItemString(result_, "ordered", py_ordered);
    RETURN_IF_PYERROR();

    return Status::OK();
  }

  Status Visit(const ExtensionType& type) {
    auto storage_type = type.storage_type();

    ArrayVector out_chunks(data_->num_chunks());
    for (int i = 0; i < data_->num_chunks(); i++) {
      auto chunk = data_->chunk(i);
      auto storage_data = checked_cast<const ExtensionArray&>(*chunk).storage();
      out_chunks[i] = storage_data;
    }

    data_ = std::make_shared<ChunkedArray>(out_chunks);

    RETURN_NOT_OK(VisitTypeInline(*data_->type(), this));
    return Status::OK();
  }

  Status Visit(const FixedSizeListType& type) { return NotImplemented(type); }
  Status Visit(const LargeListType& type) { return NotImplemented(type); }
  Status Visit(const UnionType& type) { return NotImplemented(type); }
  Status Visit(const DayTimeIntervalType& type) { return NotImplemented(type); }
  Status Visit(const MonthIntervalType& type) { return NotImplemented(type); }

  Status NotImplemented(const DataType& type) {
    return Status::NotImplemented(
        "Conversion from arrow to pandas is not implemented for type ", type.name());
  }

  Status Convert(PyObject** out) {
    RETURN_NOT_OK(VisitTypeInline(*data_->type(), this));
    *out = result_;
    return Status::OK();
  }

 private:
  std::shared_ptr<ChunkedArray> data_;
  PandasOptions options_;
  PyObject* py_ref_;
  PyArrayObject* arr_;
  PyObject* result_;
};

Status ConvertArrayToPandas(const PandasOptions& options,
                            const std::shared_ptr<Array>& arr, PyObject* py_ref,
                            PyObject** out) {
  auto carr = std::make_shared<ChunkedArray>(arr);
  return ConvertChunkedArrayToPandas(options, carr, py_ref, out);
}

Status ConvertChunkedArrayToPandas(const PandasOptions& options,
                                   const std::shared_ptr<ChunkedArray>& ca,
                                   PyObject* py_ref, PyObject** out) {
  ArrowDeserializer converter(options, ca, py_ref);
  return converter.Convert(out);
}

Status ConvertTableToPandas(const PandasOptions& options,
                            const std::shared_ptr<Table>& table, PyObject** out) {
  return ConvertTableToPandas(options, std::unordered_set<std::string>(), table, out);
}

Status ConvertTableToPandas(const PandasOptions& options,
                            const std::unordered_set<std::string>& categorical_columns,
                            const std::shared_ptr<Table>& table, PyObject** out) {
  return ConvertTableToPandas(options, categorical_columns,
                              std::unordered_set<std::string>(), table, out);
}

Status ConvertTableToPandas(const PandasOptions& options,
                            const std::unordered_set<std::string>& categorical_columns,
                            const std::unordered_set<std::string>& extension_columns,
                            const std::shared_ptr<Table>& table, PyObject** out) {
  std::shared_ptr<Table> current_table = table;
  if (!categorical_columns.empty()) {
    FunctionContext ctx;
    for (int i = 0; i < table->num_columns(); i++) {
      std::shared_ptr<ChunkedArray> col = table->column(i);
      if (col->type()->id() == Type::DICTIONARY) {
        // No need to dictionary encode again. Came up in ARROW-6434,
        // ARROW-6435
        continue;
      }
      if (categorical_columns.count(table->field(i)->name())) {
        Datum out;
        RETURN_NOT_OK(DictionaryEncode(&ctx, Datum(col), &out));
        std::shared_ptr<ChunkedArray> array = out.chunked_array();
        auto field = table->field(i)->WithType(array->type());
        RETURN_NOT_OK(current_table->RemoveColumn(i, &current_table));
        RETURN_NOT_OK(current_table->AddColumn(i, field, array, &current_table));
      }
    }
  }

  DataFrameBlockCreator helper(options, extension_columns, current_table);
  return helper.Convert(out);
}

}  // namespace py
}  // namespace arrow
