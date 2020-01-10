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

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arrow/dataset/filter.h"
#include "arrow/dataset/type_fwd.h"
#include "arrow/dataset/visibility.h"
#include "arrow/util/optional.h"

namespace arrow {

namespace fs {
struct FileStats;
struct FileSelector;
}  // namespace fs

namespace dataset {

// ----------------------------------------------------------------------
// Partition schemes

/// \brief Interface for parsing partition expressions from string partition
/// identifiers.
///
/// For example, the identifier "foo=5" might be parsed to an equality expression
/// between the "foo" field and the value 5.
///
/// Some partition schemes may store the field names in a metadata
/// store instead of in file paths, for example
/// dataset_root/2009/11/... could be used when the partition fields
/// are "year" and "month"
///
/// Paths are consumed from left to right. Paths must be relative to
/// the root of a partition; path prefixes must be removed before passing
/// the path to a scheme for parsing.
class ARROW_DS_EXPORT PartitionScheme {
 public:
  virtual ~PartitionScheme() = default;

  /// \brief The name identifying the kind of partition scheme
  virtual std::string type_name() const = 0;

  /// \brief Parse a path segment into a partition expression
  ///
  /// \param[in] segment the path segment to parse
  /// \param[in] i the index of segment within a path
  /// \return the parsed expression
  virtual Result<std::shared_ptr<Expression>> Parse(const std::string& segment,
                                                    int i) const = 0;

  /// \brief Parse a path into a partition expression
  Result<std::shared_ptr<Expression>> Parse(const std::string& path) const;

  /// \brief A default PartitionScheme which always yields scalar(true)
  static std::shared_ptr<PartitionScheme> Default();

  const std::shared_ptr<Schema>& schema() { return schema_; }

 protected:
  explicit PartitionScheme(std::shared_ptr<Schema> schema) : schema_(std::move(schema)) {}

  std::shared_ptr<Schema> schema_;
};

/// \brief PartitionSchemeDiscovery provides creation of a partition scheme when the
/// specific schema must be inferred from available paths (no explicit schema is known).
class ARROW_DS_EXPORT PartitionSchemeDiscovery {
 public:
  virtual ~PartitionSchemeDiscovery() = default;

  /// Get the schema for the resulting PartitionScheme.
  virtual Result<std::shared_ptr<Schema>> Inspect(
      const std::vector<util::string_view>& paths) const = 0;

  /// Create a partition scheme using the provided schema
  /// (fields may be dropped).
  virtual Result<std::shared_ptr<PartitionScheme>> Finish(
      const std::shared_ptr<Schema>& schema) const = 0;
};

/// \brief Subclass for representing the default, always true scheme.
class DefaultPartitionScheme : public PartitionScheme {
 public:
  DefaultPartitionScheme() : PartitionScheme(::arrow::schema({})) {}

  std::string type_name() const override { return "default"; }

  Result<std::shared_ptr<Expression>> Parse(const std::string& segment,
                                            int i) const override {
    return scalar(true);
  }
};

/// \brief Subclass for looking up partition information from a dictionary
/// mapping segments to expressions provided on construction.
class ARROW_DS_EXPORT SegmentDictionaryPartitionScheme : public PartitionScheme {
 public:
  SegmentDictionaryPartitionScheme(
      std::shared_ptr<Schema> schema,
      std::vector<std::unordered_map<std::string, std::shared_ptr<Expression>>>
          dictionaries)
      : PartitionScheme(std::move(schema)), dictionaries_(std::move(dictionaries)) {}

  std::string type_name() const override { return "segment_dictionary"; }

  /// Return dictionaries_[i][segment] or scalar(true)
  Result<std::shared_ptr<Expression>> Parse(const std::string& segment,
                                            int i) const override;

 protected:
  std::vector<std::unordered_map<std::string, std::shared_ptr<Expression>>> dictionaries_;
};

/// \brief Subclass for the common case of a partition scheme which yields an equality
/// expression for each segment
class ARROW_DS_EXPORT PartitionKeysScheme : public PartitionScheme {
 public:
  /// An unconverted equality expression consisting of a field name and the representation
  /// of a scalar value
  struct Key {
    std::string name, value;
  };

  /// Convert a Key to a full expression.
  /// If the field referenced in key is absent from the schema will be ignored.
  static Result<std::shared_ptr<Expression>> ConvertKey(const Key& key,
                                                        const Schema& schema);

  /// Extract a partition key from a path segment.
  virtual util::optional<Key> ParseKey(const std::string& segment, int i) const = 0;

  Result<std::shared_ptr<Expression>> Parse(const std::string& segment,
                                            int i) const override;

 protected:
  using PartitionScheme::PartitionScheme;
};

/// \brief SchemaPartitionScheme parses one segment of a path for each field in its
/// schema. All fields are required, so paths passed to SchemaPartitionScheme::Parse
/// must contain segments for each field.
///
/// For example given schema<year:int16, month:int8> the path "/2009/11" would be
/// parsed to ("year"_ == 2009 and "month"_ == 11)
class ARROW_DS_EXPORT SchemaPartitionScheme : public PartitionKeysScheme {
 public:
  explicit SchemaPartitionScheme(std::shared_ptr<Schema> schema)
      : PartitionKeysScheme(std::move(schema)) {}

  std::string type_name() const override { return "schema"; }

  util::optional<Key> ParseKey(const std::string& segment, int i) const override;

  static std::shared_ptr<PartitionSchemeDiscovery> MakeDiscovery(
      std::vector<std::string> field_names);
};

/// \brief Multi-level, directory based partitioning scheme
/// originating from Apache Hive with all data files stored in the
/// leaf directories. Data is partitioned by static values of a
/// particular column in the schema. Partition keys are represented in
/// the form $key=$value in directory names.
/// Field order is ignored, as are missing or unrecognized field names.
///
/// For example given schema<year:int16, month:int8, day:int8> the path
/// "/day=321/ignored=3.4/year=2009" parses to ("year"_ == 2009 and "day"_ == 321)
class ARROW_DS_EXPORT HivePartitionScheme : public PartitionKeysScheme {
 public:
  explicit HivePartitionScheme(std::shared_ptr<Schema> schema)
      : PartitionKeysScheme(std::move(schema)) {}

  std::string type_name() const override { return "hive"; }

  util::optional<Key> ParseKey(const std::string& segment, int i) const override {
    return ParseKey(segment);
  }

  static util::optional<Key> ParseKey(const std::string& segment);

  static std::shared_ptr<PartitionSchemeDiscovery> MakeDiscovery();
};

/// \brief Implementation provided by lambda or other callable
class ARROW_DS_EXPORT FunctionPartitionScheme : public PartitionScheme {
 public:
  explicit FunctionPartitionScheme(
      std::shared_ptr<Schema> schema,
      std::function<Result<std::shared_ptr<Expression>>(const std::string&, int)> impl,
      std::string name = "function")
      : PartitionScheme(std::move(schema)),
        impl_(std::move(impl)),
        name_(std::move(name)) {}

  std::string type_name() const override { return name_; }

  Result<std::shared_ptr<Expression>> Parse(const std::string& segment,
                                            int i) const override {
    return impl_(segment, i);
  }

 private:
  std::function<Result<std::shared_ptr<Expression>>(const std::string&, int)> impl_;
  std::string name_;
};

// TODO(bkietz) use RE2 and named groups to provide RegexpPartitionScheme

/// \brief Either a PartitionScheme or a PartitionSchemeDiscovery
class ARROW_DS_EXPORT PartitionSchemeOrDiscovery {
 public:
  explicit PartitionSchemeOrDiscovery(std::shared_ptr<PartitionScheme> scheme)
      : variant_(std::move(scheme)) {}

  explicit PartitionSchemeOrDiscovery(std::shared_ptr<PartitionSchemeDiscovery> discovery)
      : variant_(std::move(discovery)) {}

  PartitionSchemeOrDiscovery& operator=(std::shared_ptr<PartitionScheme> scheme) {
    variant_ = std::move(scheme);
    return *this;
  }

  PartitionSchemeOrDiscovery& operator=(
      std::shared_ptr<PartitionSchemeDiscovery> discovery) {
    variant_ = std::move(discovery);
    return *this;
  }

  std::shared_ptr<PartitionScheme> scheme() const {
    if (util::holds_alternative<std::shared_ptr<PartitionScheme>>(variant_)) {
      return util::get<std::shared_ptr<PartitionScheme>>(variant_);
    }
    return NULLPTR;
  }

  std::shared_ptr<PartitionSchemeDiscovery> discovery() const {
    if (util::holds_alternative<std::shared_ptr<PartitionSchemeDiscovery>>(variant_)) {
      return util::get<std::shared_ptr<PartitionSchemeDiscovery>>(variant_);
    }
    return NULLPTR;
  }

 private:
  util::variant<std::shared_ptr<PartitionSchemeDiscovery>,
                std::shared_ptr<PartitionScheme>>
      variant_;
};

}  // namespace dataset
}  // namespace arrow
