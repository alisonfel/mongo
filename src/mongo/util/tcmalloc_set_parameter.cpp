/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#ifdef _WIN32
#define NVALGRIND
#endif

#include "mongo/platform/basic.h"

#include <absl/types/optional.h>
#include <algorithm>
#include <tcmalloc/malloc_extension.h>
#include <valgrind/valgrind.h>

#include "mongo/base/init.h"
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/str.h"
#include "mongo/util/tcmalloc_parameters_gen.h"

namespace mongo {
namespace {

StatusWith<size_t> getProperty(StringData propname) {
    absl::optional<size_t> prop;
    prop = tcmalloc::MallocExtension::GetNumericProperty(propname.toString().c_str());
    if (!prop.has_value()) {
        return {ErrorCodes::InternalError,
                str::stream() << "Failed to retreive tcmalloc prop: " << propname};
    }
    return prop.value();
}

StatusWith<size_t> validateTCMallocValue(StringData name, const BSONElement& newValueElement) {
    if (!newValueElement.isNumber()) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << "Expected server parameter " << name
                              << " to have numeric type, but found "
                              << newValueElement.toString(false) << " of type "
                              << typeName(newValueElement.type())};
    }
    long long valueAsLongLong = newValueElement.safeNumberLong();
    if (valueAsLongLong < 0 ||
        static_cast<unsigned long long>(valueAsLongLong) > std::numeric_limits<size_t>::max()) {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Value " << newValueElement.toString(false) << " is out of range for "
                          << name << "; expected a value between 0 and "
                          << std::min<unsigned long long>(std::numeric_limits<size_t>::max(),
                                                          std::numeric_limits<long long>::max()));
    }
    return static_cast<size_t>(valueAsLongLong);
}

}  // namespace

namespace {

MONGO_INITIALIZER_GENERAL(TcmallocConfigurationDefaults, (), ("BeginStartupOptionHandling"))
(InitializerContext*) {
    // Before processing the command line options, if the user has not specified a value in via
    // the environment, set tcmalloc.max_total_thread_cache_bytes to its default value.
    if (getenv("TCMALLOC_MAX_TOTAL_THREAD_CACHE_BYTES")) {
        return;
    }

    ProcessInfo pi;
    size_t systemMemorySizeMB = pi.getMemSizeMB();
    size_t defaultTcMallocCacheSize = 1024 * 1024 * 1024;  // 1024MB in bytes
    size_t derivedTcMallocCacheSize =
        (systemMemorySizeMB / 8) * 1024 * 1024;  // 1/8 of system memory in bytes
    size_t cacheSize = std::min(defaultTcMallocCacheSize, derivedTcMallocCacheSize);

    tcmalloc::MallocExtension::SetMaxTotalThreadCacheBytes(cacheSize);
}

}  // namespace

// setParameter for tcmalloc_release_rate
void TCMallocReleaseRateServerParameter::append(OperationContext*,
                                                BSONObjBuilder& builder,
                                                const std::string& fieldName) {
    auto value = tcmalloc::MallocExtension::GetBackgroundReleaseRate();
    builder.append(fieldName, value);
}

Status TCMallocReleaseRateServerParameter::setFromString(const std::string& tcmalloc_release_rate) {
    size_t value;
    Status status = NumberParser{}(tcmalloc_release_rate, &value);
    if (!status.isOK()) {
        return status;
    }
    if (value < 0) {
        return {ErrorCodes::BadValue,
                str::stream() << "tcmallocReleaseRate cannot be negative: "
                              << tcmalloc_release_rate};
    }

    tcmalloc::MallocExtension::BytesPerSecond releaseRate =
        static_cast<tcmalloc::MallocExtension::BytesPerSecond>(value);
    ;
    tcmalloc::MallocExtension::SetBackgroundReleaseRate(releaseRate);
    return Status::OK();
}

}  // namespace mongo
