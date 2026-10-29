#pragma once

#include "zscan/schema/types.hpp"

namespace zscan::schema {

enum class MaterialKind { EntranceWindow, Solvent };

struct MaterialPreset {
    std::string material_id;
    std::string name;
    MaterialKind kind { MaterialKind::Solvent };
    double refractive_index { };
    double relative_standard_uncertainty { };
    std::string status;
    std::string provenance_id;
};

struct MaterialCatalog {
    std::uint32_t schema_version { };
    double wavelength_m { };
    std::vector<MaterialPreset> materials;
};

[[nodiscard]] Result<MaterialCatalog> parse_material_catalog(std::string_view text);
[[nodiscard]] Result<MaterialCatalog> load_material_catalog(const std::filesystem::path& path);
[[nodiscard]] Result<std::string> format_material_catalog(const MaterialCatalog& catalog);
[[nodiscard]] Result<void> save_material_catalog(const std::filesystem::path& path,
    const MaterialCatalog& catalog);
[[nodiscard]] Result<void> validate(const MaterialCatalog& catalog);

} // namespace zscan::schema
