/*
 * Copyright (c) 2014,2015,2017 Reed A. Cartwright
 * Authors:  Reed A. Cartwright <reed@cartwrig.ht>
 *
 * This file is part of DeNovoGear.
 *
 * DeNovoGear is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once
#ifndef DNG_READ_GROUP_H
#define DNG_READ_GROUP_H

#include <boost/range/begin.hpp>
#include <boost/range/end.hpp>
#include <boost/range/size.hpp>
#include <boost/range/algorithm/lower_bound.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm/unique.hpp>
#include <boost/range/algorithm_ext/erase.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/identity.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/member.hpp>

#include <map>

#include <dng/utility.h>
#include <dng/library.h>

namespace dng {

namespace rg {
struct id {};
struct lb {};
struct sm {};
struct nx {};
}

// @RG ID: str->index; index->lb_index;
// @RG LB: str->index; index->str;
// @RG SM: str->index; index->lb_indies;

namespace detail {
namespace mi = boost::multi_index;
using boost::multi_index_container;
using mi::indexed_by;
using mi::tag;
using mi::member;
using mi::identity;
using mi::ordered_non_unique;
using mi::ordered_unique;
using mi::random_access;

struct rg_t {
    std::string id;
    std::string library;
    std::string sample;
};

typedef boost::multi_index_container<rg_t, indexed_by<
        ordered_unique<tag<rg::id>, member<rg_t, std::string, &rg_t::id>>,
        ordered_non_unique<tag<rg::lb>, member<rg_t, std::string, &rg_t::library>>,
        ordered_non_unique<tag<rg::sm>, member<rg_t, std::string, &rg_t::sample>>,
        random_access<tag<rg::nx>>
    >> DataBase;
}

class ReadGroups {
public:
    typedef utility::StringSet StrSet;
    typedef detail::rg_t ReadGroup;
    typedef detail::DataBase DataBase;

    // Parse a list of BAM/SAM files into read groups
    void ParseHeaderText(const char *text, const std::string &lbtag = "LB");

    template<typename InFiles>
    void ParseHeaderText(InFiles &range, const std::string &lbtag = "LB");

    // Parse a VCF file
    template<typename InFiles>
    void ParseSamples(InFiles &range);

    // Parse Library information from an AD file
    void ParseLibraries(const libraries_t& libs);

    inline const StrSet &groups() const { return groups_; }
    inline const StrSet &libraries() const { return libraries_; }
    inline const StrSet &samples() const { return samples_; }
    inline const DataBase &data() const { return data_; }

    // Returns the library index based on read group sorted id
    inline std::size_t library_from_id(std::size_t n) const {
        return library_from_id_[n];
    }
    inline std::vector<std::size_t> library_from_id() const {
        return library_from_id_;
    }

    // Returns the library index based on read group order of discovery
    inline std::size_t library_from_index(std::size_t n) const {
        return library_from_index_[n];
    }
    inline std::vector<std::size_t> library_from_index() const {
        return library_from_index_;
    }

    inline libraries_t GetLibraries() const {
        libraries_t ret;
        std::map<std::string,std::string> samples;
        for(auto &&a : data_) {
            samples[a.library] = a.sample;
        }
        for(auto &&a : samples) {
            ret.names.push_back(a.first);
            ret.samples.push_back(a.second);
        }
        return ret;
    }

    template<typename Range>
    void SelectLibraries(Range &range) {
        StrSet temp = libraries_;
        for(auto && a : range) {
            temp.erase(a);
        }
        EraseLibraries(temp);
    }

    template<typename Range>
    inline void EraseLibraries(Range &range) {
        for(auto && lib : range) {
            data_.get<rg::lb>().erase(lib);
        }
        ReloadData();
    }

    void KeepTheseOnly(std::vector<int> keep){
        //PR_NOTE(SW): This is the wrong and stupid work around for multi_index_container
        DataBase data2;
        int count = 0;
        for(auto && a : data_) {
            if( std::find(keep.begin(), keep.end(), count) != keep.end() ){
                data2.insert(a);
            }
            count++;
        }
        auto t = data_;
        data_ = data2;
        data2 = t;
        ReloadData();
    }

protected:
    DataBase data_;

    StrSet groups_;
    StrSet libraries_;
    StrSet samples_;

    std::vector<std::size_t> library_from_id_;
    std::vector<std::size_t> library_from_index_;

    void ReloadData();
};

namespace rg {
inline std::size_t index(const ReadGroups::StrSet &set,
                         const std::string &query) {
    auto it = set.find(query);
    return (it != set.end()) ? static_cast<std::size_t>(it - set.begin()) : -1;
}
}

template<typename InFiles>
void ReadGroups::ParseHeaderText(InFiles &range, const std::string &lbtag) {
    // iterate through each file in the range
    for(auto && f : range) {
        // get header text and continue on failure
        ParseHeaderText(const_cast<const char*>(f.header()->text), lbtag);
    }
}

/**
 * Parses an VCF file into read groups, sample names and libraries
 * fname - file path and name string for input vcf file
 */
template<typename InFile>
void ReadGroups::ParseSamples(InFile &file) {
    // Read through samples from the header
    auto samples = file.samples();
    for(int a = 0; a < samples.second; ++a) {
        // Each sample column in a vcf file typically corresponds to an @RG SM: tag.
        // However, we prefer to work with @RG LB: tags as our base data.
        // In order to extract library information from VCF data, we support column
        // ids in the following format SAMPLE:LIBRARY.

        const char *sm = samples.first[a];
        ReadGroup val{sm, sm};

        const char *p = strchr(sm, ':');
        if(p == nullptr || *(p + 1) == '\0') {
            val.sample = sm;
        } else {
            val.sample.assign(sm, p);
        }
        data_.insert(std::move(val));
    }
    ReloadData();
}

inline
void ReadGroups::ParseLibraries(const libraries_t& libs) {
    for(size_t i=0; i < libs.names.size(); ++i) {
        data_.insert({libs.names[i],libs.names[i],libs.samples[i]});
    }
    ReloadData();
}


} // namespace dng

#endif
