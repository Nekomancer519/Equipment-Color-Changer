#include "Equipment.h"
#include "RuntimeRenderer.h"
#include <spdlog/sinks/basic_file_sink.h>

namespace {
void Message(SKSE::MessagingInterface::Message* message) {
    if (!message) return;
    switch (message->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        ER::RegisterMenu(); ER::Start(); break;
    case SKSE::MessagingInterface::kPreLoadGame:
        ER::SetSession(false); break;
    case SKSE::MessagingInterface::kPostLoadGame:
        ER::SetSession(message->data != nullptr); break;
    case SKSE::MessagingInterface::kNewGame:
        ER::SetSession(true); break;
    default: break;
    }
}
}
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    if (skse->IsEditor()) return false;
    auto path = SKSE::log::log_directory();
    if (!path) return false;
    auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*path / "EquipmentColorRuntime.log").string(), true);
    auto log = std::make_shared<spdlog::logger>("EquipmentColorRuntime", std::move(sink));
    log->set_level(spdlog::level::info); log->flush_on(spdlog::level::info);
    spdlog::set_default_logger(std::move(log));
    // Initial test target is the user's installed runtime; do not advertise untested ABI support.
    if (skse->RuntimeVersion() != REL::Version{1, 6, 1170, 0}) {
        logger::error("Prototype requires Skyrim 1.6.1170; runtime was {}", skse->RuntimeVersion().string()); return false;
    }
    SKSE::Init(skse, false);
    if (!SKSE::GetTaskInterface() || !SKSE::GetSerializationInterface() || !SKSE::GetMessagingInterface()) return false;
    ER::RegisterSerialization();
    if (!SKSE::GetMessagingInterface()->RegisterListener(Message)) return false;
    // Keep the DLL resident if hook installation fails: UI reports the failure.
    ER::InstallRenderer();
    logger::info("Equipment Color Runtime 0.2.2 loaded; code-entry draw interception; no context-vtable overwrites; no texture generation.");
    return true;
}
