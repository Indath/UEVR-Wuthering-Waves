#define NOMINMAX

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include <utility/Config.hpp>
#include <utility/String.hpp>
#include <utility/Module.hpp>

#include <sdk/CVar.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/ConsoleManager.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/FArrayProperty.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/UFunction.hpp>
#include <sdk/Utility.hpp>

#include <array>
#include <intrin.h>

#include "Framework.hpp"
#include "utility/Logging.hpp"

#include "CVarManager.hpp"

#include <tracy/Tracy.hpp>

constexpr std::string_view cvars_standard_txt_name = "cvars_standard.txt";
constexpr std::string_view cvars_data_txt_name = "cvars_data.txt";
constexpr std::string_view user_script_txt_name = "user_script.txt";

CVarManager::CVarManager() {
    ZoneScopedN(__FUNCTION__);

    m_displayed_cvars.insert(m_displayed_cvars.end(), s_default_standard_cvars.begin(), s_default_standard_cvars.end());
    m_displayed_cvars.insert(m_displayed_cvars.end(), s_default_data_cvars.begin(), s_default_data_cvars.end());

    // Sort first by name, then by bool/int/float type. Bools get displayed first.
    std::sort(m_displayed_cvars.begin(), m_displayed_cvars.end(), [](const auto& a, const auto& b) {
        return a->get_name() < b->get_name();
    });

    std::sort(m_displayed_cvars.begin(), m_displayed_cvars.end(), [](const auto& a, const auto& b) {
        return (int)a->get_type() < (int)b->get_type();
    });

    m_all_cvars.insert(m_all_cvars.end(), m_displayed_cvars.begin(), m_displayed_cvars.end());

    // set m_hzbo (shared ptr) to the r.HZBOcclusion cvar in m_all_cvars
    for (auto& cvar : m_all_cvars) {
        if (cvar->get_name() == L"r.HZBOcclusion") {
            m_hzbo = cvar;
            break;
        }
    }
}

CVarManager::~CVarManager() {
    ZoneScopedN(__FUNCTION__);

    /*for (auto& cvar : m_cvars) {
        cvar->save();
    }*/
}

void CVarManager::spawn_console() {
    if (m_native_console_spawned) {
        return;
    }

    // Find Engine object and add the Console
    const auto engine = sdk::UGameEngine::get();

    if (engine != nullptr) {
        const auto console_class = engine->get_property<sdk::UClass*>(L"ConsoleClass");
        auto game_viewport = engine->get_property<sdk::UObject*>(L"GameViewport");

        if (console_class != nullptr && game_viewport != nullptr) {
            const auto console = sdk::UGameplayStatics::get()->spawn_object(console_class, game_viewport);

            if (console != nullptr) {
                game_viewport->get_property<sdk::UObject*>(L"ViewportConsole") = console;
                m_native_console_spawned = true;
            }
        }
    }
}

void CVarManager::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    ZoneScopedN(__FUNCTION__);

    s_last_engine_delta = delta;

    frame_counter_sweep_tick();

    for (auto& cvar : m_all_cvars) {
        cvar->update();
        cvar->freeze();
    }

    if (m_should_execute_console_script) {
        execute_console_script(engine, user_script_txt_name.data());
        m_should_execute_console_script = false;
    }

    if (!m_pending_exec.empty() && engine != nullptr) {
        for (const auto& cmd : m_pending_exec) {
            SPDLOG_INFO("[CVarManager] DIAG exec: {}", cmd);
            engine->exec(utility::widen(cmd));
        }
        m_pending_exec.clear();
    }
}

void CVarManager::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (ImGui::TreeNode("CVars")) {
        ImGui::TextWrapped("Note: Any changes here will be frozen.");

        uint32_t frozen_cvars = 0;

        for (auto& cvar : m_all_cvars) {
            if (cvar->is_frozen()) {
                ++frozen_cvars;
            }
        }

        ImGui::TextWrapped("Frozen CVars: %i", frozen_cvars);

        ImGui::Checkbox("Display Console", &m_wants_display_console);
        
        if (!m_native_console_spawned) {
            if (ImGui::Button("Spawn Native Console")) {
                spawn_console();
            }
        }

        if (ImGui::Button("Dump All CVars")) {
            GameThreadWorker::get().enqueue([this]() {
                dump_commands();
            });
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear Frozen CVars")) {
            for (auto& cvar : m_all_cvars) {
                cvar->unfreeze();
            }

            const auto cvars_txt = Framework::get_persistent_dir(cvars_standard_txt_name.data());

            try {
                if (std::filesystem::exists(cvars_txt)) {
                    std::filesystem::remove(cvars_txt);
                }
            } catch (const std::exception& e) {
                spdlog::error("Failed to remove {}: {}", cvars_standard_txt_name.data(), e.what());
            }

            const auto cvars_data_txt = Framework::get_persistent_dir(cvars_data_txt_name.data());

            try {
                if (std::filesystem::exists(cvars_data_txt)) {
                    std::filesystem::remove(cvars_data_txt);
                }
            } catch (const std::exception& e) {
                spdlog::error("Failed to remove {}: {}", cvars_data_txt_name.data(), e.what());
            }
        }
        
        for (auto& cvar : m_displayed_cvars) {
            cvar->draw_ui();
        }

        if (ImGui::Button("DIAG: Dump Foliage/Wind Systems (cvars + classes)")) {
            GameThreadWorker::get().enqueue([this]() {
                dump_foliage_systems();
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Logs every console object and every UClass whose name matches Wind/Foliage/Vegetation/Grass/Tree/Kuro/Impostor/Instanc/HISM. "
                              "Used to identify the game's custom vegetation system behind the frozen far-tree sway in NSF Pass 2. Look for [FOLIAGE-DUMP] in the log.");
        }

        if (ImGui::Button("DIAG: Dump KuroCS PlantAnim / Imposter instances")) {
            GameThreadWorker::get().enqueue([this]() {
                dump_plant_anim_instances();
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Finds live instances of KuroCSPlantAnim, KuroCSSkeltalPlant, KuroImposterUpdater, KuroImposterVer2Component, ImposterHISMComponent "
                              "and logs every reflected property (name, type, offset, value) plus native function names. Look for [PLANT-DUMP] in the log.");
        }

        ImGui::Text("DIAG: ImposterVer2 (far trees) wind A/B");
        if (ImGui::Button("WindCullDist 1e7")) {
            m_pending_exec.push_back("r.ImposterVer2.WindCullDistCm 10000000");
        }
        ImGui::SameLine();
        if (ImGui::Button("WindCullDist 0")) {
            m_pending_exec.push_back("r.ImposterVer2.WindCullDistCm 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("RoundRobin 100000")) {
            m_pending_exec.push_back("r.ImposterVer2.RoundRobinWindowSize 100000");
        }
        ImGui::SameLine();
        if (ImGui::Button("RoundRobin 128 (default)")) {
            m_pending_exec.push_back("r.ImposterVer2.RoundRobinWindowSize 128");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Executed via UEngine::Exec on the next engine tick (same path as the console). "
                              "WindCullDist controls the per-view distance beyond which impostor tree sway is disabled; "
                              "RoundRobin controls how many impostor components update per frame.");
        }

        ImGui::Text("DIAG: Kuro GPU wind field / grass interaction A/B");
        if (ImGui::Button("WindField OFF")) {
            m_pending_exec.push_back("r.KuroWindFieldInteraction.Enabled 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("WindField ON")) {
            m_pending_exec.push_back("r.KuroWindFieldInteraction.Enabled 1");
        }
        ImGui::SameLine();
        if (ImGui::Button("GrassInteract OFF")) {
            m_pending_exec.push_back("r.KuroInstanceGrassInteraction.Enabled 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("GrassInteract ON")) {
            m_pending_exec.push_back("r.KuroInstanceGrassInteraction.Enabled 1");
        }
        if (ImGui::Button("SeasonsTreeNearestK 0")) {
            m_pending_exec.push_back("r.KuroSeasonsTreeNearestK 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("SeasonsTreeNearestK 64")) {
            m_pending_exec.push_back("r.KuroSeasonsTreeNearestK 64");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("If turning the wind field OFF freezes the far trees in BOTH eyes, the sway comes from the GPU wind field "
                              "(once-per-tick dispatch). If it changes nothing, the sway lives elsewhere (impostor updater / material time).");
        }

        ImGui::Text("DIAG: Far shadow/lighting eye flicker A/B");
        if (ImGui::Button("CSMCaching 0")) {
            m_pending_exec.push_back("r.Shadow.CSMCaching 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("CSMCaching 1")) {
            m_pending_exec.push_back("r.Shadow.CSMCaching 1");
        }
        ImGui::SameLine();
        if (ImGui::Button("DFShadowing 0")) {
            m_pending_exec.push_back("r.DistanceFieldShadowing 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("DFShadowing 1")) {
            m_pending_exec.push_back("r.DistanceFieldShadowing 1");
        }
        if (ImGui::Button("FarShadow static 0")) {
            m_pending_exec.push_back("r.Shadow.FarShadowStaticMeshLODBias 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("MaxCSMResolution 4096")) {
            m_pending_exec.push_back("r.Shadow.MaxCSMResolution 4096");
        }
        ImGui::SameLine();
        if (ImGui::Button("CachedShadowsMovable 0")) {
            m_pending_exec.push_back("r.Shadow.CachedShadowsCastFromMovablePrimitives 0");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Far mountain shadow/lighting alternating between eyes: bisect between CSM caching (per-frame cascade cache "
                              "keyed on the Scene frame counter, which NSF Pass 2 decrements) and distance-field shadows. Also try "
                              "DIAG: NSF Pass2 Frame Count Mode = None in the VR settings.");
        }

        ImGui::Text("DIAG: Far cascade one-eye-only shadows (cascade bounds built from first view)");
        if (ImGui::Button("DistanceScale 1.3")) {
            m_pending_exec.push_back("r.Shadow.DistanceScale 1.3");
        }
        ImGui::SameLine();
        if (ImGui::Button("DistanceScale 1.0")) {
            m_pending_exec.push_back("r.Shadow.DistanceScale 1.0");
        }
        ImGui::SameLine();
        if (ImGui::Button("CSM.TransitionScale 0")) {
            m_pending_exec.push_back("r.Shadow.CSM.TransitionScale 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("CSM.TransitionScale 1")) {
            m_pending_exec.push_back("r.Shadow.CSM.TransitionScale 1");
        }
        if (ImGui::Button("RadiusThreshold 0")) {
            m_pending_exec.push_back("r.Shadow.RadiusThreshold 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("RadiusThreshold 0.01 (default)")) {
            m_pending_exec.push_back("r.Shadow.RadiusThreshold 0.01");
        }
        ImGui::SameLine();
        if (ImGui::Button("FarShadow static LODBias -2")) {
            m_pending_exec.push_back("r.Shadow.FarShadowStaticMeshLODBias -2");
        }
        ImGui::SameLine();
        if (ImGui::Button("Shadow.MaxCascades 10")) {
            m_pending_exec.push_back("r.Shadow.CSM.MaxCascades 10");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Far shadows that appear in only one eye and slide with head movement sit at a cascade/light-frustum edge "
                              "computed for the other eye. DistanceScale enlarges cascade coverage; RadiusThreshold 0 stops culling small "
                              "distant casters; MaxCascades/LODBias shift where the far cascade boundary falls.");
        }

        ImGui::Text("DIAG: Shadows/reflections that move with the HMD (screen-space effects)");
        if (ImGui::Button("Dump Shadow/Reflection Systems (cvars + classes)")) {
            GameThreadWorker::get().enqueue([this]() {
                dump_shadow_systems();
            });
        }
        if (ImGui::Button("SSR 0")) {
            m_pending_exec.push_back("r.SSR.Quality 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("SSR 3")) {
            m_pending_exec.push_back("r.SSR.Quality 3");
        }
        ImGui::SameLine();
        if (ImGui::Button("ContactShadows 0")) {
            m_pending_exec.push_back("r.ContactShadows 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("ContactShadows 1")) {
            m_pending_exec.push_back("r.ContactShadows 1");
        }
        ImGui::SameLine();
        if (ImGui::Button("CapsuleShadows 0")) {
            m_pending_exec.push_back("r.CapsuleShadows 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("CapsuleShadows 1")) {
            m_pending_exec.push_back("r.CapsuleShadows 1");
        }
        if (ImGui::Button("SSAO off (AmbientOcclusionLevels 0)")) {
            m_pending_exec.push_back("r.AmbientOcclusionLevels 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("SSAO on (-1)")) {
            m_pending_exec.push_back("r.AmbientOcclusionLevels -1");
        }
        ImGui::SameLine();
        if (ImGui::Button("ReflectionEnvironment 0")) {
            m_pending_exec.push_back("r.ReflectionEnvironment 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("ReflectionEnvironment 1")) {
            m_pending_exec.push_back("r.ReflectionEnvironment 1");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Screen-space effects (SSR, contact/capsule shadows, SSAO) reproject the previous frame or use the view's own projection. "
                              "If one of these OFF stops the sliding, that effect is reading Pass 1's (left eye) history buffers during Pass 2.");
        }

        ImGui::Text("DIAG: KuroImposterUpdater::UpdateImposters");
        if (s_imposter_update_hook == nullptr) {
            if (ImGui::Button("Hook UpdateImposters (log calls/tick)")) {
                GameThreadWorker::get().enqueue([this]() {
                    hook_imposter_updater();
                });
            }
        } else {
            ImGui::Text("hooked: fn=%p total=%u last_tick=%u reruns=%u obj=%p", (void*)s_imposter_update_fn,
                s_imposter_update_calls_total, s_imposter_update_calls_window, s_imposter_update_reruns, (void*)s_imposter_updater_last_obj);
            ImGui::Checkbox("Re-run UpdateImposters before NSF Pass 2", &s_rerun_imposter_update_before_pass2);
            ImGui::SameLine();
            ImGui::Checkbox("use real dt", &s_rerun_imposter_use_real_dt);
            ImGui::Text("captured DirLight=%p dt=%.5f", (void*)s_imposter_last_dir_light, s_imposter_last_delta_time);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Calls the native UpdateImposters a second time (on the render-submit path) right before the right-eye pass. "
                                  "If far-tree sway in the right eye starts moving, the impostor updater is the once-per-tick gate.");
            }
        }

        ImGui::Text("DIAG: Material Parameter Collections (wind/time uniforms)");
        if (ImGui::Button("Dump MPCs (press twice to diff scalars)")) {
            GameThreadWorker::get().enqueue([this]() {
                dump_material_parameter_collections();
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Logs every MaterialParameterCollection asset (scalar/vector params + defaults) and its live instances. "
                              "Press again a few seconds later: scalars that changed are flagged [CHANGED] - those are the per-tick wind/time uniforms.");
        }

        ImGui::Text("DIAG: Impostor distance (KuroImposterVer2Component Start/MaxDistance)");
        ImGui::SliderFloat("Impostor distance scale", &s_imposter_distance_scale, 1.0f, 8.0f, "%.2fx");
        if (ImGui::Button("Apply impostor distance scale")) {
            const auto scale = s_imposter_distance_scale;
            GameThreadWorker::get().enqueue([this, scale]() {
                apply_imposter_distance_scale(scale);
            });
        }
        ImGui::SameLine();
        if (ImGui::Button("Restore impostor distances")) {
            GameThreadWorker::get().enqueue([this]() {
                apply_imposter_distance_scale(0.0f);
            });
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Far trees are billboard impostors whose facing is baked for Pass 1 (left eye). Pushing the swap distance out replaces "
                              "the visible impostor band with real swaying meshes at a GPU cost. Re-apply after map loads (new instances).");
        }

        ImGui::Text("DIAG: Global frame counters (GFrameCounter etc.) for NSF Pass 2");
        if (ImGui::Button("Sweep for per-tick frame counters")) {
            s_frame_counter_sweep_requested = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Scans the game exe's writable data over ~12 engine ticks for dwords that increase by exactly 1 every tick. "
                              "Those are GFrameCounter/GFrameNumber and any engine/Kuro per-frame counters. Stand still while it runs.");
        }
        ImGui::SameLine();
        ImGui::Text("found=%u pass=%u bumps=%u", (uint32_t)s_frame_counters.size(), s_frame_counter_sweep_pass, s_frame_counter_bumps);
        if (!s_frame_counters.empty()) {
            ImGui::Checkbox("Bump frame counters (+1) around NSF Pass 2", &s_bump_frame_counters_pass2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Increments every found counter before Pass 2's BeginRenderViewFamily and restores after. "
                                  "If far-tree sway starts moving in the right eye, a per-frame 'already updated this frame' gate is the cause.");
            }
        }

        ImGui::TreePop();
    }
}

void CVarManager::on_frame() {
    if (m_wants_display_console) {
        display_console();
    }
}

void CVarManager::on_config_load(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    for (auto& cvar : m_all_cvars) {
        cvar->load(set_defaults);
    }

    // TODO: Add arbitrary cvars from the other configs the user can add.

    // calling UEngine::exec here causes a crash, defer to on_pre_engine_tick()
    if (!set_defaults) {
        m_should_execute_console_script = true;
    }
}

void CVarManager::dump_commands() {
    const auto console_manager = sdk::FConsoleManager::get();

    if (console_manager == nullptr) {
        return;
    }

    nlohmann::json json;

    for (auto obj : console_manager->get_console_objects()) {
        if (obj.value == nullptr || obj.key == nullptr || IsBadReadPtr(obj.key, sizeof(wchar_t))) {
            continue;
        }

        auto& entry = json[utility::narrow(obj.key)];
        
        entry["description"] = "";
        //entry["address"] = (std::stringstream{} << std::hex << (uintptr_t)obj.value).str();
        //entry["vtable"] = (std::stringstream{} << std::hex << *(uintptr_t*)obj.value).str();

        bool is_command = false;

        try {
            is_command = obj.value->AsCommand() != nullptr;
            if (is_command) {
                entry["command"] = true;
            } else {
                entry["value"] = ((sdk::IConsoleVariable*)obj.value)->GetFloat();
            }
        } catch(...) {
            SPDLOG_WARN("Failed to check if CVar is a command: {}", utility::narrow(obj.key));
        }

        const auto help_string = obj.value->GetHelp();

        if (help_string != nullptr && !IsBadReadPtr(help_string, sizeof(wchar_t))) {
            try {
                SPDLOG_INFO("Found CVar: {} {}", utility::narrow(obj.key), utility::narrow(help_string));
                entry["description"] = utility::narrow(help_string);
            } catch(...) {

            }
        }
        
        SPDLOG_INFO("Found CVar: {}", utility::narrow(obj.key));
    }

    const auto persistent_dir = g_framework->get_persistent_dir();

    // Dump all CVars to a JSON file.
    std::ofstream file(persistent_dir / "cvardump.json");

    if (file.is_open()) {
        file << json.dump(4);
        file.close();

        SPDLOG_INFO("Dumped CVars to {}", (persistent_dir / "cvardump.json").string());
    }
}

void CVarManager::dump_foliage_systems() {
    static const std::vector<std::string> needles{
        "wind", "foliage", "vegetation", "grass", "tree", "imposter", "impostor", "instanc", "hism", "speedtree", "leaf", "plant", "kurocs", "computeshader"
    };
    dump_systems_by_name("FOLIAGE-DUMP", needles);
}

void CVarManager::dump_shadow_systems() {
    static const std::vector<std::string> needles{
        "shadow", "csm", "cascade", "distancefield", "dfao", "capsule", "reflection", "ssr", "planar", "lightpropagation", "volumetric", "skylight", "lightmap", "vsm", "raytrac", "gi", "lumen", "kurolight", "kuroshadow", "kurogi"
    };
    dump_systems_by_name("SHADOW-DUMP", needles);
}

void CVarManager::dump_systems_by_name(const char* tag, const std::vector<std::string>& needles) {
    const auto matches = [&](std::string name) {
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        for (const auto& n : needles) {
            if (name.find(n) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    SPDLOG_INFO("[{}] ---- UClasses ----", tag);

    if (const auto uobjectarray = sdk::FUObjectArray::get(); uobjectarray != nullptr) {
        uint32_t count = 0;
        const auto uclass_t = sdk::UClass::static_class();

        for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
            const auto item = uobjectarray->get_object(i);
            if (item == nullptr || item->object == nullptr) {
                continue;
            }

            const auto object = (sdk::UObject*)item->object;
            if (uclass_t != nullptr && object->get_class() != uclass_t) {
                continue;
            }

            std::string name{};
            try {
                name = utility::narrow(object->get_full_name());
            } catch (...) {
                continue;
            }

            if (!matches(name)) {
                continue;
            }

            std::string super{};
            try {
                const auto s = ((sdk::UStruct*)object)->get_super_struct();
                if (s != nullptr) {
                    super = utility::narrow(s->get_full_name());
                }
            } catch (...) {
            }

            SPDLOG_INFO("[{}]   class {} : {}", tag, name, super);
            ++count;
        }

        SPDLOG_INFO("[{}] {} classes matched", tag, count);
    } else {
        SPDLOG_INFO("[{}] no UObjectArray", tag);
    }

    // Name-only: calling AsCommand()/GetFloat()/GetHelp() on Kuro's custom IConsoleObject subclasses
    // triggers vtable-index emulation in the SDK and hard-stalls/crashes the game.
    SPDLOG_INFO("[{}] ---- console objects (names only) ----", tag);

    if (const auto console_manager = sdk::FConsoleManager::get(); console_manager != nullptr) {
        uint32_t count = 0;

        for (auto obj : console_manager->get_console_objects()) {
            if (obj.value == nullptr || obj.key == nullptr || IsBadReadPtr(obj.key, sizeof(wchar_t))) {
                continue;
            }

            std::string key{};
            try {
                key = utility::narrow(obj.key);
            } catch (...) {
                continue;
            }

            if (!matches(key)) {
                continue;
            }

            SPDLOG_INFO("[{}]   cvar {}", tag, key);
            ++count;
        }

        SPDLOG_INFO("[{}] {} console objects matched", tag, count);
    } else {
        SPDLOG_INFO("[{}] no console manager", tag);
    }

    SPDLOG_INFO("[{}] ---- done ----", tag);
}

void CVarManager::dump_plant_anim_instances() {
    static const std::vector<std::wstring> wanted{
        L"KuroCSPlantAnim", L"KuroCSSkeltalPlant", L"KuroImposterUpdater", L"KuroImposterVer2Component", L"ImposterHISMComponent", L"KuroImposterComponent"
    };

    const auto uobjectarray = sdk::FUObjectArray::get();
    if (uobjectarray == nullptr) {
        SPDLOG_INFO("[PLANT-DUMP] no UObjectArray");
        return;
    }

    const auto uclass_t = sdk::UClass::static_class();
    std::vector<sdk::UClass*> classes{};

    for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
        const auto item = uobjectarray->get_object(i);
        if (item == nullptr || item->object == nullptr) continue;
        const auto object = (sdk::UObject*)item->object;
        if (object->get_class() != uclass_t) continue;

        try {
            const auto name = object->get_fname().to_string();
            for (const auto& w : wanted) {
                if (name == w) {
                    classes.push_back((sdk::UClass*)object);
                    break;
                }
            }
        } catch (...) {
        }
    }

    SPDLOG_INFO("[PLANT-DUMP] ---- {} target classes found ----", classes.size());

    // Per class: reflected layout (once), then up to a few live instances with values.
    for (auto uclass : classes) {
        const auto cname = utility::narrow(uclass->get_full_name());
        SPDLOG_INFO("[PLANT-DUMP] == {} (size={:x}) ==", cname, uclass->get_properties_size());

        for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
            const auto sname = utility::narrow(super->get_fname().to_string());
            if (sname == "Actor" || sname == "StaticMeshComponent" || sname == "SceneComponent" || sname == "ActorComponent" ||
                sname == "PrimitiveComponent" || sname == "MeshComponent" || sname == "Object" || sname == "InstancedStaticMeshComponent" ||
                sname == "HierarchicalInstancedStaticMeshComponent") {
                break; // engine base classes: not interesting
            }

            for (auto prop = super->get_child_properties(); prop != nullptr; prop = prop->get_next()) try {
                const auto pc = prop->get_class();
                if (pc == nullptr) continue;
                SPDLOG_INFO("[PLANT-DUMP]   prop {}::{} {} @{:x}", sname, utility::narrow(prop->get_field_name().to_string()),
                    utility::narrow(pc->get_name().to_string()), ((sdk::FProperty*)prop)->get_offset());
            } catch (...) {
            }

            for (auto child = super->get_children(); child != nullptr; child = child->get_next()) try {
                if (child->get_class() != nullptr && child->get_class()->get_fname().to_string() == L"Function") {
                    const auto fn = (sdk::UFunction*)child;
                    SPDLOG_INFO("[PLANT-DUMP]   func {}::{} native={:x}", sname, utility::narrow(fn->get_fname().to_string()), (uintptr_t)fn->get_native_function());
                }
            } catch (...) {
            }
        }

        // Live instances (values of numeric/bool props declared on the game classes only)
        uint32_t shown = 0;
        for (auto i = 0; i < uobjectarray->get_object_count() && shown < 3; ++i) {
            const auto item = uobjectarray->get_object(i);
            if (item == nullptr || item->object == nullptr) continue;
            const auto object = (sdk::UObject*)item->object;
            if (object->get_class() != uclass) continue;

            std::string oname{};
            try { oname = utility::narrow(object->get_full_name()); } catch (...) { continue; }
            if (oname.find("Default__") != std::string::npos) continue;

            SPDLOG_INFO("[PLANT-DUMP]   instance {} @{:x}", oname, (uintptr_t)object);
            ++shown;

            for (auto super = (sdk::UStruct*)uclass; super != nullptr; super = super->get_super_struct()) {
                const auto sname = utility::narrow(super->get_fname().to_string());
                if (sname == "Actor" || sname == "StaticMeshComponent" || sname == "SceneComponent" || sname == "ActorComponent" ||
                    sname == "PrimitiveComponent" || sname == "MeshComponent" || sname == "Object" || sname == "InstancedStaticMeshComponent" ||
                    sname == "HierarchicalInstancedStaticMeshComponent") {
                    break;
                }

                for (auto prop = super->get_child_properties(); prop != nullptr; prop = prop->get_next()) try {
                    const auto pc = prop->get_class();
                    if (pc == nullptr) continue;
                    const auto type = pc->get_name().to_string();
                    const auto fprop = (sdk::FProperty*)prop;
                    const auto addr = (uintptr_t)object + fprop->get_offset();
                    const auto pname = utility::narrow(prop->get_field_name().to_string());

                    if (type == L"BoolProperty") {
                        SPDLOG_INFO("[PLANT-DUMP]     {} = {}", pname, ((sdk::FBoolProperty*)prop)->get_value_from_object(object));
                    } else if (type == L"FloatProperty") {
                        SPDLOG_INFO("[PLANT-DUMP]     {} = {}", pname, *(float*)addr);
                    } else if (type == L"IntProperty" || type == L"UInt32Property") {
                        SPDLOG_INFO("[PLANT-DUMP]     {} = {}", pname, *(int32_t*)addr);
                    } else if (type == L"ByteProperty" || type == L"EnumProperty") {
                        SPDLOG_INFO("[PLANT-DUMP]     {} = {}", pname, *(uint8_t*)addr);
                    } else if (type == L"ObjectProperty") {
                        const auto o = *(sdk::UObject**)addr;
                        SPDLOG_INFO("[PLANT-DUMP]     {} = {:x} {}", pname, (uintptr_t)o, o != nullptr ? utility::narrow(o->get_full_name()) : "null");
                    } else if (type == L"ArrayProperty") {
                        SPDLOG_INFO("[PLANT-DUMP]     {} = TArray count={}", pname, *(int32_t*)(addr + 8));
                    }
                } catch (...) {
                }
            }
        }
    }

    SPDLOG_INFO("[PLANT-DUMP] ---- done ----");
}

void CVarManager::dump_material_parameter_collections() {
    const auto uobjectarray = sdk::FUObjectArray::get();
    if (uobjectarray == nullptr) {
        SPDLOG_INFO("[MPC-DUMP] no UObjectArray");
        return;
    }

    const auto mpc_class = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.MaterialParameterCollection");
    const auto mpci_class = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.MaterialParameterCollectionInstance");
    if (mpc_class == nullptr) {
        SPDLOG_INFO("[MPC-DUMP] MaterialParameterCollection class not found");
        return;
    }

    // Reflected layout of the collection + its parameter structs (once per dump, cheap).
    for (auto super = (sdk::UStruct*)mpc_class; super != nullptr; super = super->get_super_struct()) try {
        const auto sname = utility::narrow(super->get_fname().to_string());
        if (sname == "Object") break;
        for (auto prop = super->get_child_properties(); prop != nullptr; prop = prop->get_next()) try {
            const auto pc = prop->get_class();
            if (pc == nullptr) continue;
            SPDLOG_INFO("[MPC-DUMP] layout {}::{} {} @{:x}", sname, utility::narrow(prop->get_field_name().to_string()),
                utility::narrow(pc->get_name().to_string()), ((sdk::FProperty*)prop)->get_offset());
        } catch (...) {
        }
    } catch (...) {
    }

    const auto scalar_params_prop = mpc_class->find_property(L"ScalarParameters");
    const auto vector_params_prop = mpc_class->find_property(L"VectorParameters");
    const auto state_id_prop = mpc_class->find_property(L"StateId");

    // Element struct layout for FCollectionScalarParameter / FCollectionVectorParameter.
    struct ElemLayout { int32_t size{0}; int32_t name_off{-1}; int32_t value_off{-1}; };
    auto resolve_layout = [](sdk::FProperty* arr_prop) -> ElemLayout {
        ElemLayout l{};
        if (arr_prop == nullptr) return l;
        try {
            const auto inner = ((sdk::FArrayProperty*)arr_prop)->get_inner();
            if (inner == nullptr || inner->get_class() == nullptr) return l;
            if (inner->get_class()->get_name().to_string() != L"StructProperty") return l;
            const auto ustruct = (sdk::UStruct*)((sdk::FStructProperty*)inner)->get_struct();
            if (ustruct == nullptr) return l;
            l.size = ustruct->get_properties_size();
            const auto align = std::max<int32_t>(1, ustruct->get_min_alignment());
            l.size = (l.size + align - 1) / align * align;
            for (auto s = ustruct; s != nullptr; s = s->get_super_struct()) {
                for (auto p = s->get_child_properties(); p != nullptr; p = p->get_next()) {
                    const auto pn = p->get_field_name().to_string();
                    if (pn == L"ParameterName") l.name_off = ((sdk::FProperty*)p)->get_offset();
                    else if (pn == L"DefaultValue") l.value_off = ((sdk::FProperty*)p)->get_offset();
                }
            }
        } catch (...) {
        }
        return l;
    };

    const auto scalar_layout = resolve_layout(scalar_params_prop);
    const auto vector_layout = resolve_layout(vector_params_prop);
    SPDLOG_INFO("[MPC-DUMP] scalar elem size={:x} name@{:x} value@{:x} | vector elem size={:x} name@{:x} value@{:x}",
        scalar_layout.size, scalar_layout.name_off, scalar_layout.value_off, vector_layout.size, vector_layout.name_off, vector_layout.value_off);

    std::unordered_map<std::string, float> new_snapshot{};
    uint32_t collections = 0;
    uint32_t instances = 0;

    for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
        const auto item = uobjectarray->get_object(i);
        if (item == nullptr || item->object == nullptr) continue;
        const auto object = (sdk::UObject*)item->object;
        const auto oclass = object->get_class();
        if (oclass == nullptr) continue;

        if (oclass->is_a(mpc_class)) {
            std::string oname{};
            try { oname = utility::narrow(object->get_full_name()); } catch (...) { continue; }
            if (oname.find("Default__") != std::string::npos) continue;
            ++collections;

            std::string state{};
            if (state_id_prop != nullptr) {
                const auto g = (const uint32_t*)((uintptr_t)object + state_id_prop->get_offset());
                state = fmt::format("{:08x}{:08x}{:08x}{:08x}", g[0], g[1], g[2], g[3]);
            }
            SPDLOG_INFO("[MPC-DUMP] == collection {} @{:x} StateId={} ==", oname, (uintptr_t)object, state);

            if (scalar_params_prop != nullptr && scalar_layout.size > 0 && scalar_layout.name_off >= 0 && scalar_layout.value_off >= 0) {
                const auto arr = (uintptr_t)object + scalar_params_prop->get_offset();
                const auto data = *(uint8_t**)arr;
                const auto count = *(int32_t*)(arr + 8);
                for (int32_t k = 0; k < count && k < 256 && data != nullptr; ++k) try {
                    const auto elem = data + (size_t)k * scalar_layout.size;
                    const auto pname = utility::narrow(((sdk::FName*)(elem + scalar_layout.name_off))->to_string());
                    const auto value = *(float*)(elem + scalar_layout.value_off);
                    const auto key = oname + "::" + pname;
                    new_snapshot[key] = value;
                    const auto prev = s_mpc_scalar_snapshot.find(key);
                    const bool changed = prev != s_mpc_scalar_snapshot.end() && prev->second != value;
                    SPDLOG_INFO("[MPC-DUMP]   scalar {} = {}{}", pname, value, changed ? fmt::format("  [CHANGED from {}]", prev->second) : "");
                } catch (...) {
                }
            }

            if (vector_params_prop != nullptr && vector_layout.size > 0 && vector_layout.name_off >= 0 && vector_layout.value_off >= 0) {
                const auto arr = (uintptr_t)object + vector_params_prop->get_offset();
                const auto data = *(uint8_t**)arr;
                const auto count = *(int32_t*)(arr + 8);
                for (int32_t k = 0; k < count && k < 256 && data != nullptr; ++k) try {
                    const auto elem = data + (size_t)k * vector_layout.size;
                    const auto pname = utility::narrow(((sdk::FName*)(elem + vector_layout.name_off))->to_string());
                    const auto v = (float*)(elem + vector_layout.value_off);
                    SPDLOG_INFO("[MPC-DUMP]   vector {} = ({}, {}, {}, {})", pname, v[0], v[1], v[2], v[3]);
                } catch (...) {
                }
            }
        } else if (mpci_class != nullptr && oclass->is_a(mpci_class)) {
            std::string oname{};
            try { oname = utility::narrow(object->get_full_name()); } catch (...) { continue; }
            if (oname.find("Default__") != std::string::npos) continue;
            ++instances;

            // Instance-side: Collection + World + (runtime, non-reflected) overrides. Log the reflected part.
            SPDLOG_INFO("[MPC-DUMP] -- instance {} @{:x} --", oname, (uintptr_t)object);
            for (auto super = (sdk::UStruct*)oclass; super != nullptr; super = super->get_super_struct()) {
                if (super->get_fname().to_string() == L"Object") break;
                for (auto prop = super->get_child_properties(); prop != nullptr; prop = prop->get_next()) try {
                    const auto pc = prop->get_class();
                    if (pc == nullptr) continue;
                    const auto type = pc->get_name().to_string();
                    const auto fprop = (sdk::FProperty*)prop;
                    const auto addr = (uintptr_t)object + fprop->get_offset();
                    const auto pname = utility::narrow(prop->get_field_name().to_string());
                    if (type == L"ObjectProperty") {
                        const auto o = *(sdk::UObject**)addr;
                        SPDLOG_INFO("[MPC-DUMP]     {} = {:x} {}", pname, (uintptr_t)o, o != nullptr ? utility::narrow(o->get_full_name()) : "null");
                    } else if (type == L"BoolProperty") {
                        SPDLOG_INFO("[MPC-DUMP]     {} = {}", pname, ((sdk::FBoolProperty*)prop)->get_value_from_object(object));
                    } else if (type == L"FloatProperty") {
                        SPDLOG_INFO("[MPC-DUMP]     {} = {}", pname, *(float*)addr);
                    } else if (type == L"IntProperty") {
                        SPDLOG_INFO("[MPC-DUMP]     {} = {}", pname, *(int32_t*)addr);
                    } else {
                        SPDLOG_INFO("[MPC-DUMP]     {} : {} @{:x}", pname, utility::narrow(type), fprop->get_offset());
                    }
                } catch (...) {
                }
            }
        }
    }

    s_mpc_scalar_snapshot = std::move(new_snapshot);
    SPDLOG_INFO("[MPC-DUMP] ---- done: {} collections, {} instances ----", collections, instances);
}

void CVarManager::apply_imposter_distance_scale(float scale) {
    const auto uobjectarray = sdk::FUObjectArray::get();
    const auto uclass = sdk::find_uobject<sdk::UClass>(L"Class /Script/ImposterManager.KuroImposterVer2Component");
    if (uobjectarray == nullptr || uclass == nullptr) {
        SPDLOG_INFO("[IMPOSTER-DIST] KuroImposterVer2Component class or UObjectArray not found");
        return;
    }

    const auto start_prop = uclass->find_property(L"StartDistance");
    const auto max_prop = uclass->find_property(L"MaxDistance");
    if (start_prop == nullptr || max_prop == nullptr) {
        SPDLOG_INFO("[IMPOSTER-DIST] StartDistance/MaxDistance properties not found");
        return;
    }

    const bool restore = scale <= 0.0f;
    uint32_t touched = 0;
    float sample_start = 0.0f, sample_max = 0.0f;

    for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
        const auto item = uobjectarray->get_object(i);
        if (item == nullptr || item->object == nullptr) continue;
        const auto object = (sdk::UObject*)item->object;
        if (object->get_class() == nullptr || !object->get_class()->is_a(uclass)) continue;

        auto& start = *start_prop->get_data<float>(object);
        auto& maxd = *max_prop->get_data<float>(object);
        auto& orig = s_imposter_distance_originals.try_emplace((uintptr_t)object, std::make_pair(start, maxd)).first->second;

        if (restore) {
            start = orig.first;
            maxd = orig.second;
        } else {
            start = orig.first * scale;
            maxd = orig.second * scale;
        }

        if (touched == 0) {
            sample_start = start;
            sample_max = maxd;
        }
        ++touched;
    }

    if (restore) {
        s_imposter_distance_originals.clear();
    }

    SPDLOG_INFO("[IMPOSTER-DIST] {} {} instances (scale={:.2f}) sample Start={:.1f} Max={:.1f}",
        restore ? "restored" : "scaled", touched, scale, sample_start, sample_max);
}

void CVarManager::frame_counter_sweep_tick() {
    if (s_frame_counter_sweep_requested) {
        s_frame_counter_sweep_requested = false;
        s_bump_frame_counters_pass2 = false;
        s_frame_counters.clear();
        s_frame_counter_candidates.clear();
        s_frame_counter_strikes.clear();
        s_frame_counter_ranges.clear();
        s_frame_counter_snapshot.clear();
        s_frame_counter_sweep_pass = 0;

        // Collect writable, non-executable, committed regions of the main executable image (.data/.bss).
        const auto exe = utility::get_executable();
        const auto exe_size = utility::get_module_size(exe).value_or(0);
        uintptr_t p = (uintptr_t)exe;
        const uintptr_t end = p + exe_size;
        size_t total = 0;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) == 0) break;
            const auto region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            const bool writable = mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY);
            if (writable) {
                const auto rs = (uintptr_t)mbi.BaseAddress;
                const auto re = std::min(region_end, end);
                s_frame_counter_ranges.emplace_back(rs, (uint32_t)(re - rs));
                total += re - rs;
            }
            p = region_end;
        }

        s_frame_counter_snapshot.resize(total);
        size_t off = 0;
        for (const auto& [rs, sz] : s_frame_counter_ranges) {
            memcpy(s_frame_counter_snapshot.data() + off, (void*)rs, sz);
            off += sz;
        }

        s_frame_counter_sweep_pass = 1;
        SPDLOG_INFO("[FRAMECTR] sweep started: exe={:x} size={:x} writable regions={} bytes={:x}", (uintptr_t)exe, exe_size, s_frame_counter_ranges.size(), total);
        return;
    }

    if (s_frame_counter_sweep_pass == 0) {
        return;
    }

    ++s_frame_counter_sweep_pass;

    if (s_frame_counter_sweep_pass == 2) {
        // First diff: every dword that went up by exactly 1 becomes a candidate.
        size_t off = 0;
        for (const auto& [rs, sz] : s_frame_counter_ranges) {
            const auto old = (const uint32_t*)(s_frame_counter_snapshot.data() + off);
            const auto cur = (const uint32_t*)rs;
            for (uint32_t i = 0; i < sz / 4; ++i) {
                if (cur[i] == old[i] + 1 && cur[i] > 100) { // >100: skip tiny state machines
                    s_frame_counter_candidates.push_back((uint32_t*)&cur[i]);
                }
            }
            off += sz;
        }
        s_frame_counter_strikes.assign(s_frame_counter_candidates.size(), 0);
        // Re-snapshot only the candidate values (reuse snapshot as a flat array of candidate values).
        s_frame_counter_snapshot.resize(s_frame_counter_candidates.size() * 4);
        for (size_t i = 0; i < s_frame_counter_candidates.size(); ++i) {
            ((uint32_t*)s_frame_counter_snapshot.data())[i] = *s_frame_counter_candidates[i];
        }
        SPDLOG_INFO("[FRAMECTR] pass 2: {} initial +1 candidates", s_frame_counter_candidates.size());
        return;
    }

    // Subsequent ticks: candidates must keep incrementing by exactly 1 per tick (allow 1 strike for a dropped/duplicated tick).
    auto snap = (uint32_t*)s_frame_counter_snapshot.data();
    for (size_t i = 0; i < s_frame_counter_candidates.size(); ++i) {
        const auto cur = *s_frame_counter_candidates[i];
        if (cur != snap[i] + 1) {
            ++s_frame_counter_strikes[i];
        }
        snap[i] = cur;
    }

    if (s_frame_counter_sweep_pass >= 12) {
        s_frame_counters.clear();
        for (size_t i = 0; i < s_frame_counter_candidates.size(); ++i) {
            if (s_frame_counter_strikes[i] <= 1) {
                s_frame_counters.push_back(s_frame_counter_candidates[i]);
            }
        }
        const auto exe = (uintptr_t)utility::get_executable();
        SPDLOG_INFO("[FRAMECTR] sweep done: {} per-tick counters survive out of {} candidates", s_frame_counters.size(), s_frame_counter_candidates.size());
        for (auto c : s_frame_counters) {
            SPDLOG_INFO("[FRAMECTR]   exe+{:x} = {}", (uintptr_t)c - exe, *c);
        }
        if (s_frame_counters.size() > 64) {
            SPDLOG_WARN("[FRAMECTR] too many survivors ({}), keeping first 64", s_frame_counters.size());
            s_frame_counters.resize(64);
        }
        s_frame_counter_candidates.clear();
        s_frame_counter_strikes.clear();
        s_frame_counter_snapshot.clear();
        s_frame_counter_ranges.clear();
        s_frame_counter_sweep_pass = 0;
    }
}

void CVarManager::bump_frame_counters_for_pass2(bool begin) {
    if (!s_bump_frame_counters_pass2 || s_frame_counters.empty()) {
        return;
    }

    for (auto c : s_frame_counters) {
        if (begin) {
            ++*c;
        } else {
            --*c;
        }
    }

    if (begin) {
        ++s_frame_counter_bumps;
        if (s_frame_counter_bumps <= 3 || s_frame_counter_bumps % 600 == 0) {
            SPDLOG_INFO("[FRAMECTR] bumped {} counters for Pass 2 (#{}) thread={}", s_frame_counters.size(), s_frame_counter_bumps, GetCurrentThreadId());
        }
    }
}

void CVarManager::hook_imposter_updater() {
    if (s_imposter_update_hook != nullptr) {
        return;
    }

    if (sdk::UFunction::get_native_function_offset() == 0) {
        SPDLOG_ERROR("[IMPOSTER-HOOK] UFunction native function offset is 0");
        return;
    }

    const auto uclass = sdk::find_uobject<sdk::UClass>(L"Class /Script/KuroImposter.KuroImposterUpdater");
    if (uclass == nullptr) {
        SPDLOG_ERROR("[IMPOSTER-HOOK] KuroImposterUpdater class not found");
        return;
    }

    const auto fn = uclass->find_function(L"UpdateImposters");
    if (fn == nullptr) {
        SPDLOG_ERROR("[IMPOSTER-HOOK] UpdateImposters UFunction not found");
        return;
    }

    auto& native = fn->get_native_function();
    if (native == nullptr || IsBadReadPtr(native, sizeof(void*))) {
        SPDLOG_ERROR("[IMPOSTER-HOOK] UpdateImposters native pointer invalid ({:x})", (uintptr_t)native);
        return;
    }

    SPDLOG_INFO("[IMPOSTER-HOOK] UpdateImposters ufunction={:x} native={:x} flags={:x}", (uintptr_t)fn, (uintptr_t)native, fn->get_function_flags());

    s_imposter_update_fn = fn;
    s_imposter_update_hook = std::make_unique<PointerHook>((void**)&native, (void*)&imposter_updater_native_hook);
    SPDLOG_INFO("[IMPOSTER-HOOK] hooked UpdateImposters native");
}

void CVarManager::imposter_updater_native_hook(sdk::UObject* obj, void* frame, void* result) {
    ++s_imposter_update_calls_total;
    ++s_imposter_update_calls_window;
    s_imposter_updater_last_obj = obj;

    // Signature (from reflection): UpdateImposters(UObject* DirLight @0, float DeltaTime @8), params_size=0x10.
    // Capture the real arguments so the Pass 2 re-run can replay them. FFrame layout (UE4): vtable@0, Node@8,
    // Object@10, Code@18, Locals@20. Validate Locals points at a struct whose first pointer looks like a UObject.
    if (frame != nullptr && !IsBadReadPtr(frame, 0x40)) {
        if (s_imposter_update_calls_total == 1) {
            // One-time raw dump so the FFrame layout can be verified against this build.
            const auto q = (const uint64_t*)frame;
            SPDLOG_INFO("[IMPOSTER-HOOK] FFrame raw: +00={:x} +08={:x} +10={:x} +18={:x} +20={:x} +28={:x} +30={:x} +38={:x}",
                q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
            for (int slot = 3; slot <= 6; ++slot) {
                const auto p = (const uint8_t*)q[slot];
                if (p != nullptr && !IsBadReadPtr(p, 0x10)) {
                    SPDLOG_INFO("[IMPOSTER-HOOK]   deref +{:x}: ptr={:x} f32@8={:.6f} u32@8={:x}", slot * 8, *(const uint64_t*)p, *(const float*)(p + 8), *(const uint32_t*)(p + 8));
                }
            }
        }

        // NOTE: Locals@+0x20 is NOT the parameter block here (BP caller: the thunk Step()s each argument out of
        // bytecode), so no capture from the frame. See rerun_imposter_update_for_pass2() for the fallback.
    }

    static uint32_t last_logged_total = 0;
    if (s_imposter_update_calls_total <= 10 || s_imposter_update_calls_total - last_logged_total >= 600) {
        last_logged_total = s_imposter_update_calls_total;
        std::string oname{};
        try { oname = obj != nullptr ? utility::narrow(obj->get_full_name()) : "null"; } catch (...) { oname = "?"; }
        std::string lname{};
        try { lname = s_imposter_last_dir_light != nullptr ? utility::narrow(s_imposter_last_dir_light->get_full_name()) : "null"; } catch (...) { lname = "?"; }
        SPDLOG_INFO("[IMPOSTER-HOOK] UpdateImposters #{} obj={:x} ({}) DirLight={:x} ({}) dt={:.5f} ret={:x} thread={}",
            s_imposter_update_calls_total, (uintptr_t)obj, oname, (uintptr_t)s_imposter_last_dir_light, lname, s_imposter_last_delta_time,
            (uintptr_t)_ReturnAddress(), GetCurrentThreadId());
    }

    const auto original = s_imposter_update_hook->get_original<sdk::UFunction::NativeFunction>();
    if (original != nullptr) {
        original(obj, frame, result);
    }
}

static bool seh_process_event(sdk::UObject* obj, sdk::UFunction* fn, void* params) {
    __try {
        obj->process_event(fn, params);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CVarManager::rerun_imposter_update_for_pass2() {
    // Reset the per-tick window counter for the UI regardless.
    s_imposter_update_calls_window = 0;

    if (!s_rerun_imposter_update_before_pass2 || s_imposter_update_hook == nullptr || s_imposter_update_fn == nullptr) {
        return;
    }

    const auto obj = s_imposter_updater_last_obj;
    if (obj == nullptr || IsBadReadPtr(obj, sizeof(void*))) {
        return;
    }

    if (!s_imposter_params_captured || (s_imposter_last_dir_light != nullptr && IsBadReadPtr(s_imposter_last_dir_light, sizeof(void*)))) {
        // BP-invoked native: parameters arrive via Stack.Step() bytecode, not FFrame::Locals, so we can't
        // capture them. Resolve the world's directional light component ourselves (the updater only uses it
        // for the light direction) and use the engine tick delta.
        if (s_imposter_last_dir_light == nullptr || IsBadReadPtr(s_imposter_last_dir_light, sizeof(void*))) {
            s_imposter_last_dir_light = nullptr;
            if (const auto uobjectarray = sdk::FUObjectArray::get(); uobjectarray != nullptr) {
                const auto dlc_class = sdk::find_uobject<sdk::UClass>(L"Class /Script/Engine.DirectionalLightComponent");
                if (dlc_class != nullptr) {
                    for (auto i = 0; i < uobjectarray->get_object_count(); ++i) {
                        const auto item = uobjectarray->get_object(i);
                        if (item == nullptr || item->object == nullptr) continue;
                        const auto object = (sdk::UObject*)item->object;
                        if (!object->get_class()->is_a(dlc_class)) continue;
                        std::string oname{};
                        try { oname = utility::narrow(object->get_full_name()); } catch (...) { continue; }
                        if (oname.find("Default__") != std::string::npos || oname.find("PersistentLevel") == std::string::npos) continue;
                        s_imposter_last_dir_light = object;
                        SPDLOG_INFO("[IMPOSTER-HOOK] resolved DirectionalLightComponent {:x} ({})", (uintptr_t)object, oname);
                        break;
                    }
                }
            }
        }

        if (s_imposter_last_dir_light == nullptr) {
            SPDLOG_INFO_EVERY_N_SEC(5, "[IMPOSTER-HOOK] re-run skipped: no DirectionalLightComponent found in the loaded level");
            return;
        }

        s_imposter_last_delta_time = s_last_engine_delta;
        s_imposter_params_captured = true;
    }

    // UFunction is a UStruct: its properties are the parameters, so properties_size == params size.
    const auto params_size = (size_t)std::max<int32_t>(0, s_imposter_update_fn->get_properties_size());
    static std::vector<uint8_t> params{};
    params.assign(std::max<size_t>(params_size, 0x40), 0);
    *(sdk::UObject**)params.data() = s_imposter_last_dir_light;
    // Zero delta: this is the same tick, we just want the impostors re-evaluated for the second view.
    // (Set to the captured dt to also advance their internal time.)
    *(float*)(params.data() + 8) = s_rerun_imposter_use_real_dt ? s_imposter_last_delta_time : 0.0f;

    ++s_imposter_update_reruns;
    if (s_imposter_update_reruns <= 5 || s_imposter_update_reruns % 600 == 0) {
        SPDLOG_INFO("[IMPOSTER-HOOK] re-running UpdateImposters before Pass 2 (#{}) obj={:x} DirLight={:x} dt={:.5f} thread={} params_size={:x}",
            s_imposter_update_reruns, (uintptr_t)obj, (uintptr_t)s_imposter_last_dir_light, *(float*)(params.data() + 8), GetCurrentThreadId(), params_size);

        if (s_imposter_update_reruns == 1) {
            for (auto prop = s_imposter_update_fn->get_child_properties(); prop != nullptr; prop = prop->get_next()) try {
                const auto pc = prop->get_class();
                if (pc == nullptr) continue;
                const auto fprop = (sdk::FProperty*)prop;
                SPDLOG_INFO("[IMPOSTER-HOOK]   param {} {} @{:x} flags={:x}", utility::narrow(prop->get_field_name().to_string()),
                    utility::narrow(pc->get_name().to_string()), fprop->get_offset(), fprop->get_property_flags());
            } catch (...) {
            }
        }
    }

    // Go through ProcessEvent so the engine builds a real FFrame for the exec thunk.
    // Temporarily unhook so our own native hook doesn't count/log the re-entrant call.
    // PointerHook::remove() = unhook (write original back), restore() = re-apply the hook.
    // (Do NOT construct a new PointerHook here: it would capture our own hook as the "original".)
    s_imposter_update_hook->remove();
    const bool ok = seh_process_event(obj, s_imposter_update_fn, params.data());
    s_imposter_update_hook->restore();

    if (!ok) {
        SPDLOG_ERROR("[IMPOSTER-HOOK] SEH exception re-running UpdateImposters; disabling");
        s_rerun_imposter_update_before_pass2 = false;
    }
}

// Use ImGui to display a homebrew console.
void CVarManager::display_console() {
    if (!g_framework->is_drawing_ui()) {
        return;
    }

    bool open = true;

    ImGui::SetNextWindowSize(ImVec2(800, 512), ImGuiCond_::ImGuiCond_Once);
    if (ImGui::Begin("UEVRConsole", &open)) {
        const auto console_manager = sdk::FConsoleManager::get();

        if (console_manager == nullptr) {
            ImGui::TextWrapped("Failed to get FConsoleManager.");
            ImGui::End();
            return;
        }


        ImGui::TextWrapped("Note: This is a homebrew console. It is not the same as the in-game console.");

        ImGui::Separator();

        ImGui::Text("> ");
        ImGui::SameLine();

        ImGui::PushItemWidth(-1);

        std::scoped_lock _{m_console.autocomplete_mutex};

        // Do a preliminary parse of the input buffer to see if we can autocomplete.
        {
            const auto entire_command = std::string_view{ m_console.input_buffer.data() };

            if (entire_command != m_console.last_parsed_buffer) {
                std::vector<std::string> args{};

                // Use getline
                std::stringstream ss{ entire_command.data() };
                while (ss.good()) {
                    std::string arg{};
                    std::getline(ss, arg, ' ');
                    args.push_back(arg);
                }

                if (!args.empty()) {
                    GameThreadWorker::get().enqueue([console_manager, args, this]() {
                        std::scoped_lock _{m_console.autocomplete_mutex};
                        m_console.autocomplete.clear();

                        const auto possible_commands = console_manager->fuzzy_find(utility::widen(args[0]));

                        for (const auto& command : possible_commands) {
                            std::string value = "Command";
                            std::string description = "";

                            try {
                                if (command.value->AsCommand() == nullptr) {
                                    value = std::format("{}", ((sdk::IConsoleVariable*)command.value)->GetFloat());
                                }
                            } catch(...) {
                                value = "Failed to get value.";
                            }

                            try {
                                const auto help_string = command.value->GetHelp();

                                if (help_string != nullptr && !IsBadReadPtr(help_string, sizeof(wchar_t))) {
                                    description = utility::narrow(help_string);
                                }
                            } catch(...) {
                                description = "Failed to get description.";
                            }

                            m_console.autocomplete.emplace_back(AutoComplete{
                                command.value, 
                                utility::narrow(command.key),
                                value,
                                description
                            });
                        }
                    });
                }

                m_console.last_parsed_buffer = entire_command;
            }
        }

        if (ImGui::InputText("##UEVRConsoleInput", m_console.input_buffer.data(), m_console.input_buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_console.input_buffer[m_console.input_buffer.size() - 1] = '\0';

            if (m_console.input_buffer[0] != '\0') {
                const auto entire_command = std::string_view{ m_console.input_buffer.data() };

                // Split the command into the arguments via ' ' (space).
                std::vector<std::string> args{};

                // Use getline
                std::stringstream ss{ entire_command.data() };
                while (ss.good()) {
                    std::string arg{};
                    std::getline(ss, arg, ' ');
                    args.push_back(arg);
                }

                // Execute the command.
                if (args.size() >= 2) {
                    auto object = console_manager->find(utility::widen(args[0]));
                    const auto is_command = object != nullptr && object->AsCommand() != nullptr;

                    if (object != nullptr && !is_command) {
                        auto var = (sdk::IConsoleVariable*)object;
                        
                        GameThreadWorker::get().enqueue([var, value = utility::widen(args[1])]() {
                            var->Set(value.c_str());
                        });
                    } else if (object != nullptr && is_command) {
                        auto command = (sdk::IConsoleCommand*)object;

                        std::vector<std::wstring> widened_args{};
                        for (auto i = 1; i < args.size(); ++i) {
                            widened_args.push_back(utility::widen(args[i]));
                        }

                        GameThreadWorker::get().enqueue([command, widened_args]() {
                            command->Execute(widened_args);
                        });
                    } else if (object == nullptr) {
                        // Try UEngine::Exec
                        std::string entire_command_str{entire_command.data()};
                        GameThreadWorker::get().enqueue([entire_command_str]() {
                            auto engine = sdk::UGameEngine::get();
                            if (engine != nullptr) {
                                engine->exec(utility::widen(entire_command_str).data());
                            }
                        });
                    }
                }

                m_console.history.push_back(m_console.input_buffer.data());
                m_console.history_index = m_console.history.size();

                m_console.input_buffer.fill('\0');
            }
        }

        // Display autocomplete
        if (!m_console.autocomplete.empty()) {
            // Create a table of all the possible commands.
            if (ImGui::BeginTable("##UEVRAutocomplete", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (const auto& command : m_console.autocomplete) {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(command.name.c_str());

                    if (ImGui::IsItemClicked()) {
                        // Copy the command to the input buffer.
                        std::copy(command.name.begin(), command.name.end(), m_console.input_buffer.begin());
                        m_console.input_buffer[command.name.size()] = '\0';
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(command.current_value.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped(command.description.c_str());
                }

                ImGui::EndTable();
            }
        }

        ImGui::End();
    }
}

std::string CVarManager::CVar::get_key_name() {
    ZoneScopedN(__FUNCTION__);

    return std::format("{}_{}", utility::narrow(m_module), utility::narrow(m_name));
}

void CVarManager::CVar::load_internal(const std::string& filename, bool set_defaults) try {
    ZoneScopedN(__FUNCTION__);

    spdlog::info("[CVarManager] Loading {}...", filename);

    const auto cvars_txt = Framework::get_persistent_dir(filename);

    if (!std::filesystem::exists(cvars_txt)) {
        return;
    }

    auto cfg = utility::Config{cvars_txt.string()};
    auto value = cfg.get(get_key_name());

    if (!value) {
        // No need to freeze.
        return;
    }
    
    switch (m_type) {
    case Type::BOOL:
    case Type::INT:
        try {
            m_frozen_int_value = *cfg.get<int>(get_key_name());
        } catch(...) {
            m_frozen_int_value = (int)*cfg.get<float>(get_key_name());
        }
        break;
    case Type::FLOAT:
        try {
            m_frozen_float_value = *cfg.get<float>(get_key_name());
        } catch(...) {
            m_frozen_float_value = (float)*cfg.get<int>(get_key_name());
        }
        break;
    }

    m_frozen = true;
} catch(const std::exception& e) {
    spdlog::error("Failed to load {}: {}", filename, e.what());
}

void CVarManager::CVar::save_internal(const std::string& filename) try {
    ZoneScopedN(__FUNCTION__);
    
    spdlog::info("[CVarManager] Saving {}...", filename);

    const auto cvars_txt = Framework::get_persistent_dir(filename);

    auto cfg = utility::Config{cvars_txt.string()};

    switch (m_type) {
    case Type::BOOL:
    case Type::INT:
        cfg.set<int>(get_key_name(), m_frozen_int_value);
        break;
    case Type::FLOAT:
        cfg.set<float>(get_key_name(), m_frozen_float_value);
        break;
    };

    cfg.save(cvars_txt.string());
    m_frozen = true;
} catch (const std::exception& e) {
    spdlog::error("Failed to save {}: {}", filename, e.what());
}

void CVarManager::CVarStandard::load(bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    load_internal(cvars_standard_txt_name.data(), set_defaults);
}

void CVarManager::CVarStandard::save() {
    ZoneScopedN(__FUNCTION__);

    if (m_cvar == nullptr || *m_cvar == nullptr) {
        // CVar not found, don't save.
        return;
    }

    auto cvar = *m_cvar;

    switch (m_type) {
    case Type::BOOL:
        m_frozen_int_value = cvar->GetInt();
        break;
    case Type::INT:
        m_frozen_int_value = cvar->GetInt();
        break;
    case Type::FLOAT:
        m_frozen_float_value = cvar->GetFloat();
        break;
    default:
        break;
    }

    save_internal(cvars_standard_txt_name.data());
}

void CVarManager::CVarStandard::freeze() {
    ZoneScopedN(__FUNCTION__);

    if (!m_frozen) {
        return;
    }

    if (m_cvar == nullptr || *m_cvar == nullptr) {
        return;
    }

    if (!m_ever_frozen) {
        m_ever_frozen = true;
        SPDLOG_INFO("[CVarManager] (Standard) First time freezing \"{}\"...", utility::narrow(m_name));
    }

    switch(m_type) {
    case Type::BOOL:
        // Limiting the amount of times Set gets called with string conversions.
        if ((*m_cvar)->GetInt() != m_frozen_int_value) {
            (*m_cvar)->Set(std::to_wstring(m_frozen_int_value).c_str());
        }
        break;
    case Type::INT:
        if ((*m_cvar)->GetInt() != m_frozen_int_value) {
            (*m_cvar)->Set(std::to_wstring(m_frozen_int_value).c_str());
        }
        break;
    case Type::FLOAT:
        if ((*m_cvar)->GetFloat() != m_frozen_float_value) {
            (*m_cvar)->Set(std::to_wstring(m_frozen_float_value).c_str());
        }
        break;
    default:
        break;
    };
}

void CVarManager::CVarStandard::update() {
    ZoneScopedN(__FUNCTION__);

    if (m_cvar == nullptr) {
        m_cvar = sdk::find_cvar_cached(m_module, m_name);
    }
}

void CVarManager::CVarStandard::draw_ui() try {
    ZoneScopedN(__FUNCTION__);

    if (m_cvar == nullptr || *m_cvar == nullptr) {
        ImGui::TextWrapped("Failed to find cvar: %s", utility::narrow(m_name).c_str());
        return;
    }

    auto cvar = *m_cvar;
    const auto narrow_name = utility::narrow(m_name);
    
    switch (m_type) {
    case Type::BOOL: {
        auto value = (bool)cvar->GetInt();

        if (ImGui::Checkbox(narrow_name.c_str(), &value)) {
            GameThreadWorker::get().enqueue([sft = shared_from_this(), cvar, value]() {
                try {
                    cvar->Set(std::to_wstring(value).c_str());
                    sft->save();
                } catch (...) {
                    spdlog::error("Failed to set cvar: {}", utility::narrow(sft->get_name()));
                }
            });
        }
        break;
    }
    case Type::INT: {
        auto value = cvar->GetInt();

        if (ImGui::SliderInt(narrow_name.c_str(), &value, m_min_int_value, m_max_int_value)) {
            GameThreadWorker::get().enqueue([sft = shared_from_this(), cvar, value]() {
                try {
                    cvar->Set(std::to_wstring(value).c_str());
                    sft->save();
                } catch(...) {
                    spdlog::error("Failed to set cvar: {}", utility::narrow(sft->get_name()));
                }
            });
        }
        break;
    }
    case Type::FLOAT: {
        auto value = cvar->GetFloat();

        if (ImGui::SliderFloat(narrow_name.c_str(), &value, m_min_float_value, m_max_float_value)) {
            GameThreadWorker::get().enqueue([sft = shared_from_this(), cvar, value]() {
                try {
                    cvar->Set(std::to_wstring(value).c_str());
                    sft->save();
                } catch(...) {
                    spdlog::error("Failed to set cvar: {}", utility::narrow(sft->get_name()));
                }
            });
        }
        break;
    }
    default:
        ImGui::TextWrapped("Unimplemented cvar type: %s", utility::narrow(m_name).c_str());
        break;
    };
} catch(...) {
    ImGui::TextWrapped("Failed to read cvar: %s", utility::narrow(m_name).c_str());
}

void CVarManager::CVarData::load(bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    load_internal(cvars_data_txt_name.data(), set_defaults);
}

void CVarManager::CVarData::save() {
    ZoneScopedN(__FUNCTION__);

    if (!m_cvar_data) {
        return;
    }

    // Points to the same thing, just different data internally.
    auto cvar_int = m_cvar_data->get<int>();
    auto cvar_float = m_cvar_data->get<float>();

    if (cvar_int == nullptr) {
        return;
    }

    switch (m_type) {
    case Type::BOOL:
        m_frozen_int_value = cvar_int->get();
        break;
    case Type::INT:
        m_frozen_int_value = cvar_int->get();
        break;
    case Type::FLOAT:
        m_frozen_float_value = cvar_float->get();
        break;
    default:
        break;
    };

    save_internal(cvars_data_txt_name.data());
}

void CVarManager::CVarData::freeze() {
    ZoneScopedN(__FUNCTION__);

    if (!m_frozen) {
        return;
    }

    if (!m_cvar_data) {
        return;
    }

    if (!m_ever_frozen) {
        m_ever_frozen = true;
        SPDLOG_INFO("[CVarManager] (Data) First time freezing \"{}\"...", utility::narrow(m_name));
    }

    // Points to the same thing, just different data internally.
    auto cvar_int = m_cvar_data->get<int>();
    auto cvar_float = m_cvar_data->get<float>();

    if (cvar_int == nullptr) {
        return;
    }

    switch (m_type) {
    case Type::BOOL:
        cvar_int->set(m_frozen_int_value);
        break;
    case Type::INT:
        cvar_int->set(m_frozen_int_value);
        break;
    case Type::FLOAT:
        cvar_float->set(m_frozen_float_value);
        break;
    default:
        break;
    };
}

void CVarManager::CVarData::update() {
    ZoneScopedN(__FUNCTION__);

    if (!m_cvar_data) {
        m_cvar_data = sdk::find_cvar_data_cached(m_module, m_name);
    }
}

void CVarManager::CVarData::draw_ui() try {
    ZoneScopedN(__FUNCTION__);

    if (!m_cvar_data) {
        ImGui::TextWrapped("Failed to find cvar data: %s", utility::narrow(m_name).c_str());
        return;
    }

    // Points to the same thing, just different data internally.
    auto cvar_int = m_cvar_data->get<int>();
    auto cvar_float = m_cvar_data->get<float>();

    if (cvar_int == nullptr) {
        ImGui::TextWrapped("Failed to read cvar data: %s", utility::narrow(m_name).c_str());
        return;
    }

    const auto narrow_name = utility::narrow(m_name);

    switch (m_type) {
    case Type::BOOL: {
        auto value = (bool)cvar_int->get();

        if (ImGui::Checkbox(narrow_name.c_str(), &value)) {
            cvar_int->set((int)value); // no need to run on game thread, direct access
            this->save();
        }
        break;
    }
    case Type::INT: {
        auto value = cvar_int->get();

        if (ImGui::SliderInt(narrow_name.c_str(), &value, m_min_int_value, m_max_int_value)) {
            cvar_int->set(value); // no need to run on game thread, direct access
            this->save();
        }
        break;
    }
    case Type::FLOAT: {
        auto value = cvar_float->get();

        if (ImGui::SliderFloat(narrow_name.c_str(), &value, m_min_float_value, m_max_float_value)) {
            cvar_float->set(value); // no need to run on game thread, direct access
            this->save();
        }
        break;
    }
    default:
        ImGui::TextWrapped("Unimplemented cvar type: %s", narrow_name.c_str());
        break;
    }
} catch (...) {
    ImGui::TextWrapped("Failed to read cvar data: %s", utility::narrow(m_name).c_str());
}

static inline void trim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));

    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

void CVarManager::execute_console_script(sdk::UGameEngine* engine, const std::string& filename) {
    ZoneScopedN(__FUNCTION__);

    if (engine == nullptr) {
        spdlog::error("[execute_console_script] engine is null");
        return;
    }

    spdlog::info("[execute_console_script] Loading {}...", filename);

    const auto cscript_txt = Framework::get_persistent_dir(filename);

    if (!std::filesystem::exists(cscript_txt)) {
        return;
    }

    std::ifstream cscript_file(utility::widen(cscript_txt.string()));

    if (!cscript_file) {
        spdlog::error("[execute_console_script] Failed to open file {}...", filename);
        return;
    }

    for (std::string line{}; getline(cscript_file, line); ) {
        trim(line);

        // handle comments
        if (line.starts_with('#') || line.starts_with(';')) {
            continue;
        }

        if (line.contains('#')) {
            line = line.substr(0, line.find_first_of('#'));
            trim(line);
        }

        if (line.contains(';')) {
            line = line.substr(0, line.find_first_of(';'));
            trim(line);
        }

        if (line.length() == 0) {
            continue;
        }

        spdlog::debug("[execute_console_script] Attempting to execute \"{}\"", line);
        engine->exec(utility::widen(line));
    }

    spdlog::debug("[execute_console_script] done");
}
