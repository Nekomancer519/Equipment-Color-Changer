#include "Equipment.h"
#include "RuntimeRenderer.h"
#include "SKSEMenuFramework.h"

namespace ER {
namespace {
void __stdcall Draw() {
    namespace UI = ImGuiMCP;
    static RE::FormID selected = 0;
    static std::string surface;
    static std::unordered_map<std::string, Color> colors;
    static std::string colorKey;
    static Color color;
    static std::uint64_t revision = 0;
    auto view = GetView();
    if (revision != view.revision) {
        revision = view.revision;
        colors.clear(); colorKey.clear();
        for (const auto& saved : view.colors)
            colors[std::format("{:08X}|{}", saved.item, saved.source)] = saved.color;
    }
    UI::TextUnformatted("Equipment Color Runtime | 0.2.2 | No generated textures");
    UI::TextWrapped("Equip your gear, select it below, choose a color, then apply. Close F1 to inspect it in third person.");
    UI::Separator();
    if (UI::Button("Refresh equipment")) Refresh();
    auto it = std::find_if(view.items.begin(), view.items.end(), [&](const Item& i) { return i.id == selected; });
    if (it == view.items.end() && !view.items.empty()) { it = view.items.begin(); selected = it->id; surface.clear(); colorKey.clear(); }
    if (UI::BeginCombo("Equipment", it == view.items.end() ? "No supported equipped meshes" : it->name.c_str())) {
        for (const auto& item : view.items) {
            auto label = std::format("{}##{:08X}", item.name, item.id);
            if (UI::Selectable(label.c_str(), item.id == selected)) { selected = item.id; surface.clear(); colorKey.clear(); }
        }
        UI::EndCombo();
    }
    it = std::find_if(view.items.begin(), view.items.end(), [&](const Item& i) { return i.id == selected; });
    if (it != view.items.end()) {
        if (!surface.empty() && std::none_of(it->surfaces.begin(), it->surfaces.end(), [&](const Surface& s) { return s.texture == surface; })) surface.clear();
        if (UI::BeginCombo("Section", surface.empty() ? "Whole item" : surface.c_str())) {
            if (UI::Selectable("Whole item", surface.empty())) { surface.clear(); colorKey.clear(); }
            for (const auto& s : it->surfaces) {
                auto label = s.name + " - " + std::filesystem::path(s.texture).filename().string() + "##" + s.texture;
                if (UI::Selectable(label.c_str(), surface == s.texture)) { surface = s.texture; colorKey.clear(); }
            }
            UI::EndCombo();
        }
        UI::TextWrapped("Sections sharing one texture are recolored together. Skin, eyes and hair are excluded.");
    }
    const auto newColorKey = std::format("{:08X}|{}", selected, surface);
    if (newColorKey != colorKey) {
        colorKey = newColorKey;
        if (auto stored = colors.find(colorKey); stored != colors.end()) color = stored->second;
        else {
            color = Color{};
            auto saved = std::find_if(view.colors.begin(), view.colors.end(), [&](const SavedColor& s) {
                return s.item == selected && (surface.empty() || s.source == surface);
            });
            if (saved != view.colors.end()) color = saved->color;
        }
    }
    UI::BeginDisabled(it == view.items.end());
    if (UI::Button("APPLY COLOR TO SELECTED ITEM", UI::ImVec2{360, 48})) {
        colors[colorKey] = color; Apply(selected, surface, color);
    }
    if (UI::Button("Return to default", UI::ImVec2{240, 36})) {
        colors.erase(colorKey); color = Color{}; Reset(selected, surface);
    }
    UI::EndDisabled();
    UI::TextWrapped("%s", view.status.c_str());
    UI::TextUnformatted("Choose a color below, then press APPLY above.");
    UI::SetNextItemWidth(260.0f);
    UI::ColorPicker3("Color", color.rgb.data(), UI::ImGuiColorEditFlags_DisplayRGB |
        UI::ImGuiColorEditFlags_DisplayHex | UI::ImGuiColorEditFlags_PickerHueBar);
    UI::SliderFloat("Strength", &color.strength, 0.0f, 1.0f, "%.2f");
    UI::SliderFloat("Brightness", &color.brightness, 0.25f, 2.0f, "%.2f");
    colors[colorKey] = color;
    if (UI::Button("Reset ALL colors")) { colors.clear(); color = Color{}; ResetAll(); }
    UI::Separator();
    UI::TextWrapped("%s", view.status.c_str());
    if (view.busy) UI::TextUnformatted("Applying runtime tint...");
    UI::Text("Saved color sections: %llu / 256", static_cast<unsigned long long>(view.recipeCount));
    UI::TextWrapped("%s", RendererStatus().c_str());
    UI::TextWrapped("Colors are saved with your game. Original texture resolution is preserved. Applies to the player's equipped copies of an item type; NPCs and dropped items retain their appearance.");
}
}
void RegisterMenu() {
    auto module = GetModuleHandleW(L"SKSEMenuFramework.dll");
    // Check the actual loaded ABI, not only the existence of a DLL on disk.
    for (const char* name : {"AddSectionItem", "igColorPicker3", "igBeginDisabled", "igTextUnformatted", "igBeginCombo"}) {
        if (!module || !GetProcAddress(module, name)) {
            logger::error("SKSE Menu Framework export {} unavailable; menu was not registered", name); return;
        }
    }
    SKSEMenuFramework::SetSection("Equipment Color Runtime");
    SKSEMenuFramework::AddSectionItem("Recolor equipment", Draw);
    logger::info("Menu registered: F1 > Equipment Color Runtime > Recolor equipment");
}
}
