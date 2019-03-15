#pragma once

#include "virtual_container.hpp"
#include "virtual_file_system.hpp"
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace jkgm {

    typedef std::unordered_map<std::string, virtual_file const *> vfs_map;

    class jk_virtual_file_system : public virtual_file_system {
    private:
        fs::path const resource_path;
        std::optional<fs::path> const game_path;

        vfs_map resource_file_map;
        vfs_map game_file_map;
        vfs_map episode_file_map;

        std::vector<std::unique_ptr<virtual_container>> containers;

    public:
        jk_virtual_file_system(fs::path const &resource_path);
        jk_virtual_file_system(fs::path const &resource_path, fs::path const &game_path);

        void set_current_episode(virtual_container const &episode_ctr);

        virtual std::unique_ptr<jkgm::input_stream> open(fs::path const &filename) const override;
        virtual std::tuple<fs::path, std::unique_ptr<jkgm::input_stream>>
            find(fs::path const &filename, std::vector<fs::path> const &prefixes) const override;

        std::map<std::string, std::string> list_files() const;
    };
}
