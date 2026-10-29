#include "zscan/schema/materials.hpp"

#include <toml++/toml.hpp>

#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <set>

namespace zscan::schema {
namespace {
    template <typename T> Result<T> fail(std::string location, std::string message)
    {
        return std::unexpected(SchemaError {
            ErrorCode::InvalidValue, std::move(message), { }, { }, std::move(location) });
    }
}

Result<MaterialCatalog> parse_material_catalog(std::string_view text)
{
    try {
        const auto root = toml::parse(text);
        MaterialCatalog catalog;
        const auto version = root["schema_version"].value<std::int64_t>();
        const auto wavelength = root["wavelength_m"].value<double>();
        if (!version || !wavelength || *version != 1)
            return fail<MaterialCatalog>("schema_version", "unsupported material catalog version");
        catalog.schema_version = 1;
        catalog.wavelength_m = *wavelength;
        const auto* materials = root["materials"].as_array();
        if (!materials)
            return fail<MaterialCatalog>("materials", "missing material array");
        for (const auto& node : *materials) {
            const auto* table = node.as_table();
            if (!table)
                return fail<MaterialCatalog>("materials", "material entry must be a table");
            const auto kind = table->at("kind").value<std::string>();
            if (!kind || (*kind != "entrance_window" && *kind != "solvent"))
                return fail<MaterialCatalog>("materials.kind", "unknown material kind");
            MaterialPreset material { table->at("material_id").value_or(""),
                table->at("name").value_or(""),
                *kind == "entrance_window" ? MaterialKind::EntranceWindow : MaterialKind::Solvent,
                table->at("refractive_index").value_or(0.0),
                table->at("relative_standard_uncertainty").value_or(0.0),
                table->at("status").value_or(""), table->at("provenance_id").value_or("") };
            catalog.materials.push_back(std::move(material));
        }
        if (auto valid = validate(catalog); !valid)
            return std::unexpected(valid.error());
        return catalog;
    } catch (const toml::parse_error& error) {
        return fail<MaterialCatalog>("toml", std::string(error.description()));
    }
}

Result<MaterialCatalog> load_material_catalog(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::unexpected(SchemaError {
            ErrorCode::FilesystemError, "cannot open material catalog", path, { }, "materials" });
    return parse_material_catalog(std::string((std::istreambuf_iterator<char>(input)), { }));
}

Result<std::string> format_material_catalog(const MaterialCatalog& catalog)
{
    if (auto valid = validate(catalog); !valid)
        return std::unexpected(valid.error());
    toml::table root;
    root.insert("schema_version", static_cast<std::int64_t>(catalog.schema_version));
    root.insert("wavelength_m", catalog.wavelength_m);
    toml::array materials;
    for (const auto& material : catalog.materials) {
        toml::table entry;
        entry.insert("material_id", material.material_id);
        entry.insert("name", material.name);
        entry.insert("kind", material.kind == MaterialKind::Solvent ? "solvent" : "entrance_window");
        entry.insert("refractive_index", material.refractive_index);
        entry.insert("relative_standard_uncertainty", material.relative_standard_uncertainty);
        entry.insert("status", material.status);
        entry.insert("provenance_id", material.provenance_id);
        materials.push_back(std::move(entry));
    }
    root.insert("materials", std::move(materials));
    std::ostringstream output;
    output << root;
    return output.str();
}

Result<void> save_material_catalog(const std::filesystem::path& path,
    const MaterialCatalog& catalog)
{
    auto formatted = format_material_catalog(catalog);
    if (!formatted)
        return std::unexpected(formatted.error());
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return std::unexpected(SchemaError { ErrorCode::FilesystemError,
                "cannot create material catalog", path, { }, "materials" });
        output << *formatted;
        output.flush();
        if (!output)
            return std::unexpected(SchemaError { ErrorCode::FilesystemError,
                "cannot write material catalog", path, { }, "materials" });
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error)
        return std::unexpected(SchemaError { ErrorCode::FilesystemError,
            error.message(), path, { }, "materials" });
    return { };
}

Result<void> validate(const MaterialCatalog& catalog)
{
    if (!std::isfinite(catalog.wavelength_m) || catalog.wavelength_m <= 0
        || catalog.materials.empty())
        return std::unexpected(SchemaError { ErrorCode::InvalidValue,
            "catalog needs one positive wavelength", { }, { }, "wavelength_m" });
    std::set<std::string> ids;
    for (const auto& material : catalog.materials) {
        if (material.material_id.empty() || material.name.empty()
            || !ids.insert(material.material_id).second || !std::isfinite(material.refractive_index)
            || material.refractive_index <= 0
            || !std::isfinite(material.relative_standard_uncertainty)
            || material.relative_standard_uncertainty <= 0 || material.status.empty()
            || material.provenance_id.empty())
            return std::unexpected(SchemaError { ErrorCode::InvalidValue,
                "invalid or duplicate material preset", { }, { }, "materials" });
    }
    return { };
}

} // namespace zscan::schema
