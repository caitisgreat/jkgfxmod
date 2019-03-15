#pragma once

#include "at_least_one_input.hpp"
#include "bare_multi_value_option.hpp"
#include "base/format.hpp"
#include "base/lexical_cast.hpp"
#include "base/log.hpp"
#include "base/string_search.hpp"
#include "dependent_option.hpp"
#include "multi_value_option.hpp"
#include "mutual_exclusion.hpp"
#include "range_argument_queue.hpp"
#include "required_option.hpp"
#include "switch_option.hpp"
#include "value_option.hpp"
#include <algorithm>
#include <memory>
#include <unordered_map>

namespace jkgm {
    class options {
    private:
        std::vector<std::unique_ptr<abstract_option>> opts;
        std::unordered_map<std::string, abstract_option *> opt_map;
        std::unordered_map<std::string, abstract_option *> alias_map;
        std::vector<std::unique_ptr<option_constraint>> constraints;
        std::unique_ptr<abstract_bare_option> bare_option;

    public:
        bool has_value(std::string const &name) const;
        bool has_bare_value() const;

        template <typename OptionT>
        OptionT const &get_option(std::string const &name) const
        {
            auto it = opt_map.find(name);
            if(it == opt_map.end()) {
                LOG_ERROR("Option ", name, " has not been defined");
                throw std::logic_error("Option has not been defined");
            }

            OptionT const *cast_opt = dynamic_cast<OptionT const *>(it->second);
            if(!cast_opt) {
                LOG_ERROR("Option ", name, " type mismatch");
                throw std::logic_error("Option type mismatch");
            }

            return *cast_opt;
        }

        void insert(std::unique_ptr<abstract_option> &&opt);

        template <typename OptionT, typename... ArgT>
        void emplace(std::string const &name, ArgT &&... args)
        {
            insert(std::make_unique<OptionT>(name, std::forward<ArgT>(args)...));
        }

        void insert_bare(std::unique_ptr<abstract_bare_option> &&opt);

        template <typename ConstraintT, typename... ArgT>
        void emplace_constraint(ArgT &&... args)
        {
            constraints.emplace_back(std::make_unique<ConstraintT>(std::forward<ArgT>(args)...));
        }

        void load_from_arg_queue(abstract_argument_queue &args);

        template <typename ArgRangeT>
        void load_from_arg_list(ArgRangeT const &range)
        {
            auto arg_list = make_range_argument_queue(range.begin(), range.end());
            load_from_arg_queue(arg_list);
        }

        void add_alias(std::string const &option, std::string const &alias);
    };
}
