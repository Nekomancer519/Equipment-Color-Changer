#pragma once
#include "Color.h"
#include <memory>
namespace ER {
struct RenderFeedback {
    std::atomic<std::uint64_t> passes{0};
    std::atomic<bool> unsupported{false};
};
struct RenderBinding {
    RE::NiPointer<RE::BSGeometry> geometry;
    RE::NiPointer<RE::BSShaderProperty> property;
    Color color;
    std::shared_ptr<RenderFeedback> feedback{std::make_shared<RenderFeedback>()};
};
using RenderBindings=std::unordered_map<RE::BSGeometry*,RenderBinding>;
bool InstallRenderer();
void PublishBindings(std::shared_ptr<const RenderBindings> bindings);
bool RendererReady();
std::string RendererStatus();
}
