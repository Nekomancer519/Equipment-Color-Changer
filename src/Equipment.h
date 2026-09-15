#pragma once
#include "Color.h"

namespace ER {
struct Surface { std::string name; std::string texture; };
struct Item { RE::FormID id{}; std::string name; std::vector<Surface> surfaces; };
struct SavedColor { RE::FormID item{}; std::string source; Color color; };
struct View {
    std::vector<Item> items;
    std::string status{"Load a save, equip an item, then refresh."};
    bool busy{false};
    std::size_t recipeCount{};
    std::vector<SavedColor> colors;
    std::uint64_t revision{};
};
View GetView();
void Refresh();
void Apply(RE::FormID item, std::string texture, Color color);
void Reset(RE::FormID item, std::string texture);
void ResetAll();
void Start();
void SetSession(bool active);
void RegisterSerialization();
void RegisterMenu();
}
