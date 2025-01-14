/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Implementation of S3 filesystem and file interface.
// We provide a registration method for read and write files so the appropriate
// type of file can be constructed based on a filename. See the
// (register|generate)ReadFile and (register|generate)WriteFile functions.

#pragma once

#include <aws/s3/S3Errors.h>
#include <aws/s3/model/HeadObjectResult.h>

#include "velox/common/base/Exceptions.h"

namespace facebook::velox {

namespace {
constexpr std::string_view kSep{"/"};
constexpr std::string_view kS3Scheme{"s3://"};
// From AWS documentation
constexpr int kS3MaxKeySize{1024};
} // namespace

inline bool isS3File(const std::string_view filename) {
  return (filename.substr(0, kS3Scheme.size()) == kS3Scheme);
}

inline void bucketAndKeyFromS3Path(
    const std::string& path,
    std::string& bucket,
    std::string& key) {
  auto firstSep = path.find_first_of(kSep);
  bucket = path.substr(0, firstSep);
  key = path.substr(firstSep + 1);
}

inline std::string s3Path(const std::string_view& path) {
  // Remove the prefix S3:// from the given path
  return std::string(path.substr(kS3Scheme.length()));
}

inline Aws::String awsString(const std::string& s) {
  return Aws::String(s.begin(), s.end());
}

#define VELOX_CHECK_AWS_OUTCOME(outcome, header, bucket, key) \
  {                                                           \
    if (!outcome.IsSuccess()) {                               \
      auto error = outcome.GetError();                        \
      VELOX_FAIL(                                             \
          "{} with bucket '{}' and key '{}' due to {}:{}",    \
          header,                                             \
          bucket,                                             \
          key,                                                \
          error.GetErrorType(),                               \
          error.GetMessage());                                \
    }                                                         \
  }

} // namespace facebook::velox
