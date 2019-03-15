#pragma once

#include "gob_virtual_file.hpp"
#include "virtual_container.hpp"
#include <vector>

namespace jkgm {
    class gob_virtual_container : public virtual_container {
    private:
        std::vector<gob_virtual_file> files;

    public:
        explicit gob_virtual_container(fs::path const &container_filename);

        virtual gob_virtual_file const &get_file(size_t index) const override;
        virtual size_t size() const override;
    };
}
