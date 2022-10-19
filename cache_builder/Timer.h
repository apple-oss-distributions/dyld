/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
*
* Copyright (c) 2017 Apple Inc. All rights reserved.
*
* @APPLE_LICENSE_HEADER_START@
*
* This file contains Original Code and/or Modifications of Original Code
* as defined in and that are subject to the Apple Public Source License
* Version 2.0 (the 'License'). You may not use this file except in
* compliance with the License. Please obtain a copy of the License at
* http://www.opensource.apple.com/apsl/ and read it before using this
* file.
*
* The Original Code and all software distributed under the License are
* distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
* EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
* INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
* Please see the License for the specific language governing rights and
* limitations under the License.
*
* @APPLE_LICENSE_HEADER_END@
*/

#ifndef Timer_hpp
#define Timer_hpp

#include "BuilderOptions.h"

#include <os/signpost.h>
#include <string_view>

namespace cache_builder
{

struct BuilderConfig;

struct Timer
{
    Timer();

    struct Scope
    {
        Scope(const BuilderConfig& config, std::string_view name);
        ~Scope();

    private:
        const BuilderConfig&    config;
        std::string_view        name;
        os_log_t                log;
        os_signpost_id_t        signpost;
        uint64_t                startTimeNanos = 0;
    };

    // Similar to Scope, but aggregates multiple clients to a single time.  Eg, every dylib
    // has a "binding" Timer::Scope, and its too noisy to print them all for 2000 dylibs
    struct AggregateTimer
    {
        AggregateTimer(const BuilderConfig& config);
        ~AggregateTimer();
        void record(std::string_view name, uint64_t startTime, uint64_t endTime);

        // FIXME: Should we just have an AggregateTimer* in Timer::Scope instead?
        struct Scope
        {
            Scope(AggregateTimer& timer, std::string_view name);
            ~Scope();

        private:
            AggregateTimer&         timer;
            std::string_view        name;
            uint64_t                startTimeNanos = 0;
        };

    private:
        const BuilderConfig&                                config;
        std::unordered_map<std::string_view, uint32_t>      timeMap;
        std::vector<std::pair<std::string_view, uint64_t>>  timesNanos;
        pthread_mutex_t                                     mapLock;
    };

private:
    os_log_t            log;
    os_signpost_id_t    signpost;
};

struct Stats
{
    Stats(const BuilderConfig& config);
    ~Stats();

    void add(const char* format, ...)  __attribute__((format(printf, 2, 3)));

private:
    const BuilderConfig& config;
    std::vector<std::string> stats;
};

struct Logger
{
    Logger(const BuilderOptions& options);

    void log(const char* format, ...)  const __attribute__((format(printf, 2, 3)));

    bool printTimers    = false;
    bool printStats     = false;

private:
    std::string logPrefix;
};

} // namespace cache_builder

#endif /* Timer_hpp */
