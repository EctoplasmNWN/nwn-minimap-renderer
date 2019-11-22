#include "FileFormats/Bif.hpp"
#include "FileFormats/Erf.hpp"
#include "FileFormats/Gff.hpp"
#include "FileFormats/Key.hpp"

#include "lodepng.cpp"
#include "TGA.h"

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct AreaTile
{
    int id;
    int orientation;
};

struct Area
{
    std::string name;
    std::string resref;
    int width;
    int height;
    std::string tileset;
    std::vector<AreaTile> tiles; // tile id
};

struct Tga
{
    Tga() { };
    Tga(uint8_t* tga, int size) { data = tga_load_memory(tga, size, &width, &height, &bpp); }
    ~Tga() { free(data); }

    Tga(Tga&& other) { *this = std::move(other); }
    Tga& operator=(Tga&& other)
    { 
        data = other.data; 
        width = other.width;
        height = other.height;
        bpp = other.bpp;
        other.data = nullptr;
        return *this;
    }

    Tga& operator=(const Tga&) = delete;
    Tga(const Tga&) = delete;

    uint8_t* data = nullptr;
    int width;
    int height;
    int bpp;
};

struct TilesetTile
{
    std::string resref;
    int orientation;
    Tga tga;
};

struct Tileset
{
    std::unordered_map<int, TilesetTile> tiles;
};

Tileset load_tileset(const OwningDataBlock& data)
{
    Tileset tileset;

    // Format is basically an INI. We care about [TILEx] ImageMap2D.
    const char* head = (const char*) data.GetData();

    static auto copy_til_newline = [](char* out, const char* head)
    {
        for (int i = 0; ; ++i)
        {
            char ch = *head++;
            if (ch == '\n' || ch == '\r')
            {
                out[i] = '\0';
                break;
            }

            out[i] = ch;
        }
    };

    while (head = std::strstr(head + 1, "[TILE"))
    {
        char label[256];
        copy_til_newline(label, head);

        if (std::strstr(label, "[TILES]") || std::strstr(label, "DOOR"))
        {
            continue;
        }

        int id = std::atoi(label + 5);
        const char* image_map = std::strstr(head, "ImageMap2D=");
        ASSERT(image_map);

        image_map += 11;
        char image[32];
        copy_til_newline(image, image_map);

        const char* orientation = std::strstr(head, "Orientation=");
        ASSERT(orientation);

        TilesetTile tile;
        tile.resref = image;
        tile.orientation = std::atoi(orientation + 11);
        std::transform(std::begin(tile.resref), std::end(tile.resref), std::begin(tile.resref), ::tolower);
        tileset.tiles[id] = std::move(tile);
    }

    return tileset;
}

std::unordered_map<std::string, OwningDataBlock> load_data(const char* path_key, const char* path_game_dir,
    std::function<bool(const FileFormats::Key::Friendly::KeyBifReferencedResource&)> comparator)
{
    using namespace FileFormats;

    std::unordered_map<std::string, OwningDataBlock> data;

    std::map<std::size_t, std::vector<Key::Friendly::KeyBifReferencedResource>> res_map;
    std::vector<Key::Friendly::KeyBifReference> bif_refs;

    {
        Key::Raw::Key key_raw;
        bool loaded = Key::Raw::Key::ReadFromFile(path_key, &key_raw);
        ASSERT(loaded);

        Key::Friendly::Key key(std::move(key_raw));

        for (const Key::Friendly::KeyBifReferencedResource& res : key.GetReferencedResources())
        {
            if (comparator(res))
            {
                res_map[res.m_ReferencedBifIndex].emplace_back(res);
            }
        }

        bif_refs = key.GetReferencedBifs();
    }

    for (std::size_t i = 0; i < bif_refs.size(); ++i)
    {
        const Key::Friendly::KeyBifReference& bif_ref = bif_refs[i];
        std::string bif_path = std::string(path_game_dir) + "/" + bif_ref.m_Path;

        Bif::Raw::Bif bif_raw;
        bool loaded = Bif::Raw::Bif::ReadFromFile(bif_path.c_str(), &bif_raw);
        ASSERT(loaded);

        Bif::Friendly::Bif bif(std::move(bif_raw));
        const Bif::Friendly::Bif::BifResourceMap& bif_res_map = bif.GetResources();

        for (const Key::Friendly::KeyBifReferencedResource& bif_ref_res : res_map[i])
        {
            auto res_in_bif = bif_res_map.find(bif_ref_res.m_ReferencedBifResId);
            ASSERT(res_in_bif != std::end(bif_res_map));

            OwningDataBlock block;
            block.m_Data.resize(res_in_bif->second.m_DataBlock->GetDataLength());
            std::memcpy(block.m_Data.data(), res_in_bif->second.m_DataBlock->GetData(), block.m_Data.size());
            data[bif_ref_res.m_ResRef] = std::move(block);
        }
    }

    return data;
}

int main(int argc, char** argv)
{
    const char* path_out = argv[1];
    const char* path_mod = argv[2];
    const char* path_key = argv[3];
    const char* path_game_dir = argv[4];

    using namespace FileFormats;

    Erf::Raw::Erf erf_raw;
    if (!Erf::Raw::Erf::ReadFromFile(path_mod, &erf_raw))
    {
        std::printf("Failed to load the module.");
        return 1;
    }

    Erf::Friendly::Erf erf(std::move(erf_raw));

    std::unordered_map<std::string, Area> areas;
    std::unordered_map<std::string, Tileset> tilesets;
    std::unordered_map<std::string, OwningDataBlock> tileset_markup = load_data(path_key, path_game_dir,
        [](const Key::Friendly::KeyBifReferencedResource& res) { return res.m_ResType == Resource::ResourceType::SET; });

    std::unordered_set<std::string> tga_files_to_load;

    // Find icons we must load
    for (const auto& kvp : tileset_markup)
    {
        Tileset& tileset = load_tileset(kvp.second);

        for (const auto& set_kvp : tileset.tiles)
        {
            tga_files_to_load.insert(set_kvp.second.resref);
        }

        tilesets[kvp.first] = std::move(tileset);
    }
    
    // Load them.
    std::unordered_map<std::string, OwningDataBlock> tileset_icons = load_data(path_key, path_game_dir, 
        [&tga_files_to_load](const Key::Friendly::KeyBifReferencedResource& res)
        { 
            return res.m_ResType == Resource::ResourceType::TGA && tga_files_to_load.contains(res.m_ResRef);
        });

    // Hook them up.
    for (auto& kvp : tilesets)
    {
        for (auto& tile_kvp : kvp.second.tiles)
        {
            auto iter = tileset_icons.find(tile_kvp.second.resref);

            if (iter == std::end(tileset_icons))
            {
                std::printf("Failed to load icon %s; using default.\n", tile_kvp.second.resref.c_str());
                iter = tileset_icons.find("mide01_a09");
                ASSERT(iter != std::end(tileset_icons));
            }

            int width, height, bpp;
            tile_kvp.second.tga = Tga((uint8_t*)iter->second.GetData(), iter->second.GetDataLength());
        }
    }

    // Scan all module resources for the stuff we need.
    for (const Erf::Friendly::ErfResource& resource : erf.GetResources())
    {
        if (resource.m_ResType == Resource::ResourceType::ARE ||
            resource.m_ResType == Resource::ResourceType::GIT)
        {
            Gff::Raw::Gff gff_raw;
            if (!Gff::Raw::Gff::ReadFromBytes(resource.m_DataBlock->GetData(), &gff_raw))
            {
                std::printf("Failed to load area resource %s; skipping.\n", resource.m_ResRef.c_str());
                continue;
            }

            Gff::Friendly::Gff gff(std::move(gff_raw));
            std::printf("Processing resource %s [%zu].\n", resource.m_ResRef.c_str(), resource.m_DataBlock->GetDataLength());

            Area& area = areas[resource.m_ResRef];

            if (resource.m_ResType == Resource::ResourceType::ARE)
            {
                Gff::Friendly::Type_CExoLocString name;
                Gff::Friendly::Type_CResRef resref;
                Gff::Friendly::Type_INT width, height;
                Gff::Friendly::Type_CResRef tileset;
                Gff::Friendly::Type_List tile_list;

                gff.GetTopLevelStruct().ReadField("Name", &name);
                gff.GetTopLevelStruct().ReadField("ResRef", &resref);
                   gff.GetTopLevelStruct().ReadField("Width", &width);
                   gff.GetTopLevelStruct().ReadField("Height", &height);
                   gff.GetTopLevelStruct().ReadField("Tileset", &tileset);
                   gff.GetTopLevelStruct().ReadField("Tile_List", &tile_list);

                area.resref = std::string(resref.m_String, resref.m_Size);
                area.width = width;
                area.height = height;
                area.tileset = std::string(tileset.m_String, tileset.m_Size);

                for (const Gff::Friendly::Type_Struct&  entry : tile_list.GetStructs())
                {
                    Gff::Friendly::Type_INT id, orientation;
                    entry.ReadField("Tile_ID", &id);
                    entry.ReadField("Tile_Orientation", &orientation);

                    AreaTile tile;
                    tile.id = id;
                    tile.orientation = orientation;
                    area.tiles.emplace_back(std::move(tile));
                }
            }

            else if (resource.m_ResType == Resource::ResourceType::GIT)
            {
                // Transitions, creatures, add custom map pins...
            }
        }
    }

    static auto blit = [](unsigned char* dst,
        int dst_width, int dst_height,
        int dst_target_width, int dst_target_height, 
        int dst_target_x, int dst_target_y, 
        int orientation, const Tga& src)
    {
        static auto rotate = [](int* x, int* y,
            int src_x, int src_y, int src_width, int src_height, int degrees)
        {
            ASSERT(src_width % src_height == 0);

            if (degrees == 90)
            {
                *x = src_y;
                *y = src_width - 1 - src_x;
            }
            else if (degrees == 180)
            {
                *x = src_width - 1 - src_x;
                *y = src_height - 1 - src_y;
            }
            else if (degrees == 270)
            {
                *x = src_height - 1 - src_y;
                *y = src_x;
            }
        };

        for (int dst_y = dst_target_y; dst_y < dst_target_y + dst_target_height; ++dst_y)
        {
            for (int dst_x = dst_target_x; dst_x < dst_target_x + dst_target_width; ++dst_x)
            {
                int src_x = (int)((dst_x - dst_target_x) * ((float)src.width / dst_target_width));
                int src_y = (int)((dst_y - dst_target_y) * ((float)src.height / dst_target_height));
                rotate(&src_x, &src_y, src_x, src_y, src.width, src.height, orientation);

                int src_index = src_x + (src_y * src.width);
                src_index *= src.bpp;

                int dst_index = dst_x + ((dst_height - 1 - dst_y) * dst_width);
                dst_index *= 4;

                dst[dst_index] = src.data[src_index];
                dst[dst_index + 1] = src.data[src_index + 1];
                dst[dst_index + 2] = src.data[src_index + 2];
                dst[dst_index + 3] = 255;
            }
        }
    };

    // For each area, render.
    for (const auto& kvp : areas)
    {
        Tileset& tileset = tilesets[kvp.second.tileset];

        constexpr int pixels_per_tile = 64;
        int width = kvp.second.width * pixels_per_tile;
        int height = kvp.second.height * pixels_per_tile;
        unsigned char* image = new unsigned char[width * height * 4];

        for (int i = 0; i < kvp.second.tiles.size(); ++i)
        {
            const AreaTile& tile = kvp.second.tiles[i];
            const TilesetTile& tilesetTile = tileset.tiles[tile.id];

            int base_orientation = tilesetTile.orientation;
            int our_orientation = tile.orientation * 90;
            int result_orientation = (our_orientation - base_orientation) % 360;

            blit(image,
                width,
                height,
                pixels_per_tile,
                pixels_per_tile,
                (i * pixels_per_tile) % width,
                (i * pixels_per_tile) / width * pixels_per_tile,
                result_orientation,
                tileset.tiles[tile.id].tga);
        }

        char out_path_area[512];
        std::sprintf(out_path_area, "%s/%s.png", path_out, kvp.first.c_str());

        lodepng_encode32_file(out_path_area, image, width, height);
        std::printf("Wrote %s.\n", out_path_area);

        delete[] image;
    }

    return 0;
}
