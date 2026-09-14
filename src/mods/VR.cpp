#define NOMINMAX

#include <fstream>

#include <windows.h>
#include <dbt.h>

#include <imgui.h>
#include <utility/Module.hpp>
#include <utility/Registry.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/Globals.hpp>
#include <sdk/CVar.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/FBoolProperty.hpp>
#include <sdk/UEngine.hpp>

#include <tracy/Tracy.hpp>

#include "Framework.hpp"
#include "frameworkConfig.hpp"

#include "utility/Logging.hpp"

#include "VR.hpp"

std::shared_ptr<VR>& VR::get() {
    //static std::shared_ptr<VR> instance = std::make_shared<VR>();
    return g_framework->vr();
}

// SEH wrapper (no C++ objects needing unwinding in this frame) around VR::update_action_states().
// update_action_states() -> update_dpad_gestures() -> trigger_haptic_vibration() -> is_using_controllers()
// has been observed to crash with an access violation reading a garbage address, most likely due to
// hook/trampoline or exception-recovery-scanner offsets shifting after a rebuild (see FFakeStereoRenderingHook.cpp
// exception handler diagnostics). Rather than let this take down the whole process, catch it here, log it,
// and skip this tick's action/haptic/dpad update so the game and rendering can keep running.
static bool call_update_action_states_seh(VR* vr) {
    __try {
        vr->update_action_states();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Out-of-line, non-inlined on purpose. See declaration comment in VR.hpp for why.
__declspec(noinline) bool VR::is_using_controllers() const {
    if (m_controller_test_mode) {
        return true;
    }

    if (!m_controllers_allowed->value()) {
        return false;
    }

    if (!is_hmd_active()) {
        return false;
    }

    if (m_controllers.empty()) {
        return false;
    }

    const auto inactivity_timer_seconds = (int32_t)m_motion_controls_inactivity_timer->value();
    const auto elapsed = std::chrono::steady_clock::now() - m_last_controller_update;

    return elapsed <= std::chrono::seconds(inactivity_timer_seconds);
}

// Called when the mod is initialized
std::optional<std::string> VR::clean_initialize() try {
    ZoneScopedN(__FUNCTION__);

    auto openvr_error = initialize_openvr();

    if (openvr_error || !m_openvr->loaded) {
        if (m_openvr->error) {
            spdlog::info("OpenVR failed to load: {}", *m_openvr->error);
        }

        m_openvr->is_hmd_active = false;
        m_openvr->was_hmd_active = false;
        m_openvr->needs_pose_update = false;

        // Attempt to load OpenXR instead
        auto openxr_error = initialize_openxr();

        if (openxr_error || !m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }
    } else {
        m_openxr->error = "OpenVR loaded first.";
    }

    if (!get_runtime()->loaded) {
        // this is okay. we're not going to fail the whole thing entirely
        // so we're just going to return OK, but
        // when the VR mod draws its menu, it'll say "VR is not available"
        return Mod::on_initialize();
    }

    // Check whether the user has Hardware accelerated GPU scheduling enabled
    const auto hw_schedule_value = utility::get_registry_dword(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "HwSchMode");

    if (hw_schedule_value) {
        m_has_hw_scheduling = *hw_schedule_value == 2;
    }

    m_init_finished = true;

    // all OK
    return Mod::on_initialize();
} catch(...) {
    spdlog::error("Exception occurred in VR::on_initialize()");

    m_runtime->error = "Exception occurred in VR::on_initialize()";
    m_openxr->dll_missing = false;
    m_openvr->dll_missing = false;
    m_openxr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->loaded = false;
    m_openvr->is_hmd_active = false;
    m_openxr->loaded = false;
    m_init_finished = false;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr() {
    ZoneScopedN(__FUNCTION__);

    spdlog::info("Attempting to load OpenVR");

    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;

    const auto wants_openxr = m_requested_runtime_name->value() == "openxr_loader.dll";

    SPDLOG_INFO("[VR] Requested runtime: {}", m_requested_runtime_name->value());

    if (wants_openxr && GetModuleHandleW(L"openxr_loader.dll") != nullptr) {
        // pre-injected
        m_openvr->dll_missing = true;
        m_openvr->error = "OpenXR already loaded";
        return Mod::on_initialize();
    }

    if (GetModuleHandleW(L"openvr_api.dll") == nullptr) {
        // pre-injected
        if (GetModuleHandleW(L"openxr_loader.dll") != nullptr) {
            m_openvr->dll_missing = true;
            m_openvr->error = "OpenXR already loaded";
            return Mod::on_initialize();
        }


        if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
            spdlog::info("[VR] Could not load openvr_api.dll");

            m_openvr->dll_missing = true;
            m_openvr->error = "Could not load openvr_api.dll";
            return Mod::on_initialize();
        }
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    spdlog::info("Attempting to call vr::VR_Init");

    auto error = vr::VRInitError_None;
	m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    // check if error
    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return Mod::on_initialize();
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return Mod::on_initialize();
    }

    // get render target size
    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return Mod::on_initialize();
    }

    auto input_error = initialize_openvr_input();

    if (input_error) {
        m_openvr->error = *input_error;
        return Mod::on_initialize();
    }

    auto overlay_error = m_overlay_component.on_initialize_openvr();

    if (overlay_error) {
        m_openvr->error = *overlay_error;
        return Mod::on_initialize();
    }
    
    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr_input() {
    ZoneScopedN(__FUNCTION__);

    const auto module_directory = Framework::get_persistent_dir();

    // write default actions and bindings with the static strings we have
    for (auto& it : m_binding_files) {
        spdlog::info("Writing default binding file {}", it.first);

        std::ofstream file{ module_directory / it.first };
        file << it.second;
    }

    const auto actions_path = module_directory / "actions.json";
    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    // get action set
    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    if (m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set handle.";
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        if (error != vr::VRInputError_None) {
            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }

        if (it.second == vr::k_ulInvalidActionHandle) {
            return "VRInput failed to get action handle: (" + it.first + ")";
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    m_openvr->pose_action = m_action_pose;
    m_openvr->grip_pose_action = m_action_grip_pose;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    ZoneScopedN(__FUNCTION__);

    m_openxr.reset();
    m_openxr = std::make_shared<runtimes::OpenXR>();

    spdlog::info("[VR] Initializing OpenXR");

    if (GetModuleHandleW(L"openxr_loader.dll") == nullptr) {
        if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
            spdlog::info("[VR] Could not load openxr_loader.dll");

            m_openxr->loaded = false;
            m_openxr->error = "Could not load openxr_loader.dll";

            return std::nullopt;
        }
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    // Step 1: Create an instance
    spdlog::info("[VR] Creating OpenXR instance");

    XrResult result{XR_SUCCESS};

    // We may just be restarting OpenXR, so try to find an existing instance first
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{};

        if (g_framework->is_dx12()) {
            extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        } else {
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        }

        // Enumerate available extensions and enable depth extension if available
        uint32_t extension_count{};
        result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);

        std::vector<XrExtensionProperties> extension_properties(extension_count, {XR_TYPE_EXTENSION_PROPERTIES});

        if (!XR_FAILED(result)) try {
            result = xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extension_properties.data());

            if (!XR_FAILED(result)) {
                for (const auto& extension_property : extension_properties) {
                    spdlog::info("[VR] Found OpenXR extension: {}", extension_property.extensionName);
                }

                const std::unordered_set<std::string> wanted_extensions {
                    XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
                    XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME
                    // To be seen if we need more!
                };

                for (const auto& extension_property : extension_properties) {
                    if (wanted_extensions.contains(extension_property.extensionName)) {
                        spdlog::info("[VR] Enabling {} extension", extension_property.extensionName);
                        m_openxr->enabled_extensions.insert(extension_property.extensionName);
                        extensions.push_back(extension_property.extensionName);
                    }
                }
            }
        } catch(...) {
            spdlog::error("[VR] Unknown error while enumerating OpenXR extensions");
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.next = nullptr;
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        std::string application_name{"UEVR"};

        // Append the current executable name to the application base name
        {
            const auto exe = utility::get_executable();
            const auto full_path = utility::get_module_pathw(exe);

            if (full_path) {
                const auto fs_path = std::filesystem::path(*full_path);
                const auto filename = fs_path.stem().string();

                application_name += "_" + filename;

                // Trim the name to 127 characters
                if (application_name.length() >= XR_MAX_APPLICATION_NAME_SIZE) {
                    application_name = application_name.substr(0, XR_MAX_APPLICATION_NAME_SIZE - 1);
                }
            }
        }

        spdlog::info("[VR] Application name: {}", application_name);

        strcpy(instance_create_info.applicationInfo.applicationName, application_name.c_str());
        instance_create_info.applicationInfo.applicationName[XR_MAX_APPLICATION_NAME_SIZE - 1] = '\0';
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        
        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);

        // we can't convert the result to a string here
        // because the function requires the instance to be valid
        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            if (result == XR_ERROR_LIMIT_REACHED) {
                m_openxr->error = "Could not create openxr instance: XR_ERROR_LIMIT_REACHED\n"
                    "Ensure that the OpenXR plugin has been renamed or deleted from the game's binaries folder.";
            }
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr instance");
    }
    
    // Step 2: Create a system
    spdlog::info("[VR] Creating OpenXR system");

    // We may just be restarting OpenXR, so try to find an existing system first
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr system");
    }

    // Step 3: Create a session
    spdlog::info("[VR] Initializing graphics info");

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};

    if (g_framework->is_dx12()) {
        m_d3d12.openxr().initialize(session_create_info);
    } else {
        m_d3d11.openxr().initialize(session_create_info);
    }

    spdlog::info("[VR] Creating OpenXR session");
    session_create_info.systemId = m_openxr->system;
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    // Step 4: Create a space
    spdlog::info("[VR] Creating OpenXR space");

    // We may just be restarting OpenXR, so try to find an existing space first

    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    // Step 5: Get the system properties
    spdlog::info("[VR] Getting OpenXR system properties");

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->on_system_properties_acquired(system_properties);

    // Step 6: Get the view configuration properties
    m_openxr->update_render_target_size();

    // Step 7: Create a view
    if (!m_openxr->view_configs.empty()){
        m_openxr->views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
        m_openxr->stage_views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
    }

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    detect_controllers();

    if (m_init_finished) {
        // This is usually done in on_config_load
        // but the runtime can be reinitialized, so we do it here instead
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    ZoneScopedN(__FUNCTION__);

    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        m_openxr->error = err.value();
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }
    
    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (decltype(it.second)::type)m_openxr->action_set.action_map[openxr_action_name];
            spdlog::info("[VR] Successfully mapped action {} to {}", it.first, openxr_action_name);
        }
    }

    m_left_joystick = (decltype(m_left_joystick))VRRuntime::Hand::LEFT;
    m_right_joystick = (decltype(m_right_joystick))VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    ZoneScopedN(__FUNCTION__);

    // This depends on the config being loaded.
    if (!m_init_finished) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating OpenXR swapchain");

    const auto supported_swapchain_formats = m_openxr->get_supported_swapchain_formats();

    // Log
    for (auto f : supported_swapchain_formats) {
        spdlog::info("[VR] Supported swapchain format: {}", (uint32_t)f);
    }

    if (g_framework->is_dx12()) {
        auto err = m_d3d12.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());

            return m_openxr->error;
        }
    } else {
        auto err = m_d3d11.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());
            return m_openxr->error;
        }
    }

    return std::nullopt;
}

bool VR::detect_controllers() {
    ZoneScopedN(__FUNCTION__);

    // already detected
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        auto left_joystick_origin_error = vr::EVRInputError::VRInputError_None;
        auto right_joystick_origin_error = vr::EVRInputError::VRInputError_None;

        vr::InputOriginInfo_t left_joystick_origin_info{};
        vr::InputOriginInfo_t right_joystick_origin_info{};

        // Get input origin info for the joysticks
        // get the source input device handles for the joysticks
        auto left_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick);

        if (left_joystick_error != vr::VRInputError_None) {
            return false;
        }

        auto right_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick);

        if (right_joystick_error != vr::VRInputError_None) {
            return false;
        }

        m_openvr->left_controller_handle = m_left_joystick;
        m_openvr->right_controller_handle = m_right_joystick;

        left_joystick_origin_info = {};
        right_joystick_origin_info = {};

        left_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_joystick_origin_info, sizeof(left_joystick_origin_info));
        right_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_joystick_origin_info, sizeof(right_joystick_origin_info));
        if (left_joystick_origin_error != vr::EVRInputError::VRInputError_None || right_joystick_origin_error != vr::EVRInputError::VRInputError_None) {
            return false;
        }

        // Instead of manually going through the devices,
        // We do this. The order of the devices isn't always guaranteed to be
        // Left, and then right. Using the input state handles will always
        // Get us the correct device indices.
        m_controllers.push_back(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers.push_back(right_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(right_joystick_origin_info.trackedDeviceIndex);

        spdlog::info("Left Hand: {}", left_joystick_origin_info.trackedDeviceIndex);
        spdlog::info("Right Hand: {}", right_joystick_origin_info.trackedDeviceIndex);

        m_openvr->left_controller_index = left_joystick_origin_info.trackedDeviceIndex;
        m_openvr->right_controller_index = right_joystick_origin_info.trackedDeviceIndex;
    } else if (get_runtime()->is_openxr()) {
        // ezpz
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);

        spdlog::info("Left Hand: {}", 1);
        spdlog::info("Right Hand: {}", 2);
    }


    return true;
}

bool VR::is_any_action_down() {
    ZoneScopedN(__FUNCTION__);

    if (!m_runtime->ready()) {
        return false;
    }

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();

    if (glm::length(left_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    if (glm::length(right_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    const auto left_joystick = get_left_joystick();
    const auto right_joystick = get_right_joystick();

    for (auto& it : m_action_handles) {
        // These are too easy to trigger
        if (it.second == m_action_thumbrest_touch_left || it.second == m_action_thumbrest_touch_right) {
            continue;
        }

        if (it.second == m_action_a_button_touch_left || it.second == m_action_a_button_touch_right) {
            continue;
        }

        if (it.second == m_action_b_button_touch_left || it.second == m_action_b_button_touch_right) {
            continue;
        }

        if (is_action_active(it.second, left_joystick) || is_action_active(it.second, right_joystick)) {
            return true;
        }
    }

    return false;
}

bool VR::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    ZoneScopedN(__FUNCTION__);

    if (message == WM_DEVICECHANGE && !m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Received WM_DEVICECHANGE");
        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    return true;
}

void VR::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
    ZoneScopedN(__FUNCTION__);

    if (std::chrono::steady_clock::now() - m_last_engine_tick > std::chrono::seconds(1)) {
        SPDLOG_INFO_EVERY_N_SEC(1, "[VR] XInputGetState called, but engine tick hasn't been called in over a second. Is the game loading?");
        update_action_states();
    }

    if (*retval == ERROR_SUCCESS) {
        // Once here for normal gamepads, and once for the spoofed gamepad at the end
        update_imgui_state_from_xinput_state(*state, false);
        gamepad_snapturn(*state);
    }

    const auto now = std::chrono::steady_clock::now();

    if (now - m_last_xinput_update > std::chrono::seconds(2)) {
        m_lowest_xinput_user_index = user_index;
    }

    if (user_index < m_lowest_xinput_user_index) {
        m_lowest_xinput_user_index = user_index;
        spdlog::info("[VR] Changed lowest XInput user index to {}", user_index);
    }

    if (user_index != m_lowest_xinput_user_index) {
        if (!m_spoofed_gamepad_connection && is_using_controllers()) {
            spdlog::info("[VR] XInputGetState called, but user index is {}", user_index);
        }

        return;
    }

    if (!m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Successfully spoofed gamepad connection @ {}", user_index);
    }
    
    m_last_xinput_update = now;
    m_spoofed_gamepad_connection = true;

    auto runtime = get_runtime();

    auto do_pause_select = [&]() {
        if (!runtime->ready()) {
            return;
        }

        if (runtime->handle_pause) {
            // Spoof the start button being pressed
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
            *retval = ERROR_SUCCESS;
            runtime->handle_pause = false;
            runtime->handle_select_button = false;
        }

        if (runtime->handle_select_button) {
            // Spoof the back button being pressed
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
            *retval = ERROR_SUCCESS;
            runtime->handle_select_button = false;
            runtime->handle_pause = false;
        }
    };

    do_pause_select();

    if (is_using_controllers_within(std::chrono::minutes(5))) {
        *retval = ERROR_SUCCESS;
    }

    if (!is_using_controllers()) {
        return;
    }

    // Clear button state for VR controllers
    if (is_using_controllers_within(std::chrono::seconds(5))) {
        state->Gamepad.wButtons = 0;
        state->Gamepad.bLeftTrigger = 0;
        state->Gamepad.bRightTrigger = 0;
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;
        state->Gamepad.sThumbRX = 0;
        state->Gamepad.sThumbRY = 0;
    }

    const auto left_joystick = get_left_joystick();
    const auto right_joystick = get_right_joystick();
    const auto wants_swap = m_swap_controllers->value();

    runtime->handle_pause_select(is_action_active_any_joystick(m_action_system_button));
    do_pause_select();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;

    const auto is_right_a_button_down = is_action_active_any_joystick(a_button_right);
    const auto is_left_a_button_down = is_action_active_any_joystick(a_button_left);

    if (is_right_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    }

    if (is_left_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    }

    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    const auto is_right_b_button_down = is_action_active_any_joystick(b_button_right);
    const auto is_left_b_button_down = is_action_active_any_joystick(b_button_left);

    if (is_right_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    }

    if (is_left_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    }

    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, right_joystick);

    if (is_left_joystick_click_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    }

    if (is_right_joystick_click_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    }

    const auto is_left_trigger_down = is_action_active(m_action_trigger, left_joystick);
    const auto is_right_trigger_down = is_action_active(m_action_trigger, right_joystick);

    if (is_left_trigger_down) {
        state->Gamepad.bLeftTrigger = 255;
    }

    if (is_right_trigger_down) {
        state->Gamepad.bRightTrigger = 255;
    }

    const auto is_right_grip_down = is_action_active(m_action_grip, right_joystick);
    const auto is_left_grip_down = is_action_active(m_action_grip, left_joystick);

    if (is_right_grip_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }

    if (is_left_grip_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }

    const auto is_dpad_up_down = is_action_active_any_joystick(m_action_dpad_up);

    if (is_dpad_up_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    const auto is_dpad_right_down = is_action_active_any_joystick(m_action_dpad_right);

    if (is_dpad_right_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    const auto is_dpad_down_down = is_action_active_any_joystick(m_action_dpad_down);

    if (is_dpad_down_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const auto is_dpad_left_down = is_action_active_any_joystick(m_action_dpad_left);

    if (is_dpad_left_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_joystick_axis = get_joystick_axis(left_joystick);
    const auto right_joystick_axis = get_joystick_axis(right_joystick);

    const auto true_left_joystick_axis = get_joystick_axis(m_left_joystick);
    const auto true_right_joystick_axis = get_joystick_axis(m_right_joystick);

    state->Gamepad.sThumbLX = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbLX + left_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    state->Gamepad.sThumbLY = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbLY + left_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    state->Gamepad.sThumbRX = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbRX + right_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    state->Gamepad.sThumbRY = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbRY + right_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    bool already_dpad_shifted{false};

    if (m_dpad_gesture_state.direction != DPadGestureState::Direction::NONE) {
        already_dpad_shifted = true;

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::UP) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::RIGHT) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::DOWN) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::LEFT) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        // Zero out the thumbstick values
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;

        std::scoped_lock _{m_dpad_gesture_state.mtx};
        m_dpad_gesture_state.direction = DPadGestureState::Direction::NONE;
    }

    // Touching the thumbrest allows us to use the thumbstick as a dpad.  Additional options are for controllers without capacitives/games that rely solely on DPad
    if (!already_dpad_shifted && m_dpad_shifting->value()) {
        bool button_touch_inactive{true};
        bool thumbrest_check{false};

        DPadMethod dpad_method = get_dpad_method();
        if (dpad_method == DPadMethod::RIGHT_TOUCH) {
            thumbrest_check = is_action_active_any_joystick(m_action_thumbrest_touch_right);
            button_touch_inactive = !is_action_active_any_joystick(m_action_a_button_touch_right) && !is_action_active_any_joystick(m_action_b_button_touch_right);
        }
        if (dpad_method == DPadMethod::LEFT_TOUCH) {
            thumbrest_check = is_action_active_any_joystick(m_action_thumbrest_touch_left);
            button_touch_inactive = !is_action_active_any_joystick(m_action_a_button_touch_left) && !is_action_active_any_joystick(m_action_b_button_touch_left);
        }

        const auto dpad_active = (button_touch_inactive && thumbrest_check) || dpad_method == DPadMethod::LEFT_JOYSTICK || dpad_method == DPadMethod::RIGHT_JOYSTICK;

        if (dpad_active) {
            float ty{0.0f};
            float tx{0.0f};
            //SHORT ThumbY{0};
            //SHORT ThumbX{0};
            // If someone is accidentally touching both thumbrests while also moving a joystick, this will default to left joystick.
            if (dpad_method == DPadMethod::RIGHT_TOUCH || dpad_method == DPadMethod::LEFT_JOYSTICK) {
                //ThumbY = state->Gamepad.sThumbLY;
                //ThumbX = state->Gamepad.sThumbLX;
                ty = true_left_joystick_axis.y;
                tx = true_left_joystick_axis.x;
            }
            else if (dpad_method == DPadMethod::LEFT_TOUCH || dpad_method == DPadMethod::RIGHT_JOYSTICK) {
                //ThumbY = state->Gamepad.sThumbRY;
                //ThumbX = state->Gamepad.sThumbRX;
                ty = true_right_joystick_axis.y;
                tx = true_right_joystick_axis.x;
            }
            
            if (ty >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            }

            if (ty <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            }

            if (tx >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            }

            if (tx <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
            }

            if (dpad_method == DPadMethod::RIGHT_TOUCH || dpad_method == DPadMethod::LEFT_JOYSTICK) {
                if (!wants_swap) {
                    state->Gamepad.sThumbLY = 0;
                    state->Gamepad.sThumbLX = 0;
                } else {
                    state->Gamepad.sThumbRY = 0;
                    state->Gamepad.sThumbRX = 0;
                }
            }
            else if (dpad_method == DPadMethod::LEFT_TOUCH || dpad_method == DPadMethod::RIGHT_JOYSTICK) {
                if (!wants_swap) {
                    state->Gamepad.sThumbRY = 0;
                    state->Gamepad.sThumbRX = 0;
                } else {
                    state->Gamepad.sThumbLY = 0;
                    state->Gamepad.sThumbLX = 0;
                }
            }
        }
    }

    // Determine if snapturn should be run on frame
    if (m_snapturn->value()) {
        DPadMethod dpad_method = get_dpad_method();
        const auto snapturn_deadzone = get_snapturn_js_deadzone();
        float stick_axis{};

        if (!m_was_snapturn_run_on_input) {
            if (dpad_method == RIGHT_JOYSTICK) {
                stick_axis = true_left_joystick_axis.x;
                if (glm::abs(stick_axis) >= snapturn_deadzone) {
                    if (stick_axis < 0) {
                        m_snapturn_left = true;
                    }
                    m_snapturn_on_frame = true;
                    m_was_snapturn_run_on_input = true;
                }
            }
            else {
                stick_axis = right_joystick_axis.x;
                const auto& thumbrest_touch_left = !wants_swap ? m_action_thumbrest_touch_left : m_action_thumbrest_touch_right;
                if (glm::abs(stick_axis) >= snapturn_deadzone && !(dpad_method == DPadMethod::LEFT_TOUCH && is_action_active_any_joystick(thumbrest_touch_left))) {
                    if (stick_axis < 0) {
                        m_snapturn_left = true;
                    }
                    m_snapturn_on_frame = true;
                    m_was_snapturn_run_on_input = true;
                }
            }
        }
        else {
            if (dpad_method == RIGHT_JOYSTICK) {
                if (glm::abs(true_left_joystick_axis.x) < snapturn_deadzone) {
                    m_was_snapturn_run_on_input = false;
                } else {
                    state->Gamepad.sThumbLY = 0;
                    state->Gamepad.sThumbLX = 0;
                }
            }
            else {
                if (glm::abs(right_joystick_axis.x) < snapturn_deadzone) {
                    m_was_snapturn_run_on_input = false;
                } else {
                    state->Gamepad.sThumbRY = 0;
                    state->Gamepad.sThumbRX = 0;
                }
            }
        }
    }
    
    // Do it again after all the VR buttons have been spoofed
    update_imgui_state_from_xinput_state(*state, true);
}

void VR::on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) {
    ZoneScopedN(__FUNCTION__);

    if (user_index != m_lowest_xinput_user_index) {
        return;
    }

    if (!is_using_controllers()) {
        return;
    }

    const auto left_amplitude = ((float)vibration->wLeftMotorSpeed / 65535.0f) * 5.0f;
    const auto right_amplitude = ((float)vibration->wRightMotorSpeed / 65535.0f) * 5.0f;

    if (left_amplitude > 0.0f) {
        trigger_haptic_vibration(0.0f, 0.1f, 1.0f, left_amplitude, get_left_joystick());
    }

    if (right_amplitude > 0.0f) {
        trigger_haptic_vibration(0.0f, 0.1f, 1.0f, right_amplitude, get_right_joystick());
    }
}

// Allows imgui navigation to work with the controllers
void VR::update_imgui_state_from_xinput_state(XINPUT_STATE& state, bool is_vr_controller) {
    ZoneScopedN(__FUNCTION__);

    bool is_using_this_controller = true;

    const auto is_using_vr_controller_recently = is_using_controllers_within(std::chrono::seconds(1));
    const auto is_gamepad = !is_vr_controller;

    if (is_vr_controller && !is_using_vr_controller_recently) {
        is_using_this_controller = false;
    } else if (is_gamepad && is_using_vr_controller_recently) { // dont allow gamepad navigation if using vr controllers
        is_using_this_controller = false;
    }

    // L3 + R3 to open the menu
    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 && (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0) {
        if (!FrameworkConfig::get()->is_enable_l3_r3_toggle()) {
            return;
        }

        bool should_open = true;

        const auto now = std::chrono::steady_clock::now();

        if (FrameworkConfig::get()->is_l3_r3_long_press() && !g_framework->is_drawing_ui()) {
            if (!m_xinput_context.menu_longpress_begin_held) {
                m_xinput_context.menu_longpress_begin = now;
            }

            m_xinput_context.menu_longpress_begin_held = true;
            should_open = (now - m_xinput_context.menu_longpress_begin) >= std::chrono::seconds(1);
        } else {
            m_xinput_context.menu_longpress_begin_held = false;
        }

        if (should_open && now - m_last_xinput_l3_r3_menu_open >= std::chrono::seconds(1)) {
            m_last_xinput_l3_r3_menu_open = std::chrono::steady_clock::now();
            g_framework->set_draw_ui(!g_framework->is_drawing_ui());

            state.Gamepad.wButtons &= ~(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB); // so input doesn't go through to the game
        }
    } else if (is_using_this_controller) {
        m_xinput_context.headlocked_begin_held = false;
        m_xinput_context.menu_longpress_begin_held = false;
    }

    // We need to adjust the stick values based on the selected movement orientation value if the user wants to do this
    // It will either need to be adjusted by the HMD rotation or one of the controllers.
    if (is_using_this_controller && m_movement_orientation->value() != VR::AimMethod::GAME && m_movement_orientation->value() != m_aim_method->value()) {
        const auto left_stick_og = glm::vec2((float)state.Gamepad.sThumbLX, (float)state.Gamepad.sThumbLY );
        const auto left_stick_magnitude = glm::clamp(glm::length(left_stick_og), -32767.0f, 32767.0f);
        const auto left_stick = glm::normalize(left_stick_og);
        const auto left_stick_angle = glm::atan2(left_stick.y, left_stick.x);

        if (this->is_controller_movement_enabled() && is_vr_controller) {
            const auto controller_index = this->get_movement_orientation() == VR::AimMethod::LEFT_CONTROLLER ? get_left_controller_index() : get_right_controller_index();
            const auto controller_rotation = utility::math::flatten(m_rotation_offset * glm::quat{get_rotation(controller_index)});
            const auto controller_forward = controller_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
            const auto controller_angle = glm::atan2(controller_forward.x, controller_forward.z);

            // Normalize angles to [0, 2Ï€]
            const auto normalized_left_stick_angle = left_stick_angle < 0 ? left_stick_angle + 2 * glm::pi<float>() : left_stick_angle;
            const auto normalized_controller_angle = controller_angle < 0 ? controller_angle + 2 * glm::pi<float>() : controller_angle;

            // Add the angles together
            const auto new_left_stick_angle = utility::math::fix_angle(normalized_left_stick_angle + normalized_controller_angle);
            const auto new_left_stick = glm::vec2(glm::cos(new_left_stick_angle), glm::sin(new_left_stick_angle)) * left_stick_magnitude;

            state.Gamepad.sThumbLX = (int16_t)new_left_stick.x;
            state.Gamepad.sThumbLY = (int16_t)new_left_stick.y;
        } else { // Fallback to head aim
            // Rotate the left stick by the HMD rotation
            const auto hmd_rotation = utility::math::flatten(m_rotation_offset * glm::quat{get_rotation(0)});
            const auto hmd_forward = hmd_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
            const auto hmd_angle = glm::atan2(hmd_forward.x, hmd_forward.z);

            // Normalize angles to [0, 2Ï€]
            const auto normalized_left_stick_angle = left_stick_angle < 0 ? left_stick_angle + 2 * glm::pi<float>() : left_stick_angle;
            const auto normalized_hmd_angle = hmd_angle < 0 ? hmd_angle + 2 * glm::pi<float>() : hmd_angle;

            // Add the angles together
            const auto new_left_stick_angle = utility::math::fix_angle(normalized_left_stick_angle + normalized_hmd_angle);
            const auto new_left_stick = glm::vec2{glm::cos(new_left_stick_angle), glm::sin(new_left_stick_angle)} * left_stick_magnitude;

            state.Gamepad.sThumbLX = (int16_t)new_left_stick.x;
            state.Gamepad.sThumbLY = (int16_t)new_left_stick.y;
        }
    }

    if (!g_framework->is_drawing_ui()) {
        m_rt_modifier.draw = false;
        return;
    }

    if (!is_using_this_controller) {
        return;
    }

    // Gamepad navigation when the menu is open
    m_xinput_context.enqueue(is_vr_controller, state, [this](const XINPUT_STATE& state, bool is_vr_controller){
        static auto last_time = std::chrono::high_resolution_clock::now();

        const auto delta = std::chrono::duration<float>((std::chrono::high_resolution_clock::now() - last_time)).count();
        last_time = std::chrono::high_resolution_clock::now();

        auto& io = ImGui::GetIO();
        auto& gamepad = state.Gamepad;

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        // Headlocked aim toggle
        if (!FrameworkConfig::get()->is_l3_r3_long_press()) {
            if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 && (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0) {
                if (!m_xinput_context.headlocked_begin_held) {
                    m_xinput_context.headlocked_begin = std::chrono::steady_clock::now();
                    m_xinput_context.headlocked_begin_held = true;
                }
            } else {
                m_xinput_context.headlocked_begin_held = false;
            }
        }

        // Now that we're drawing the UI, check for special button combos the user can use as shortcuts
        // like recenter view, set standing origin, camera offset modification, etc.
        m_rt_modifier.draw = gamepad.bRightTrigger >= 128;

        if (!m_rt_modifier.draw) {
            m_rt_modifier.page = 0;
            m_rt_modifier.was_moving_left = false;
            m_rt_modifier.was_moving_right = false;
        }

        // If user holding down RT with menu open...
        if (m_rt_modifier.draw) {
            // Camera offset modification
            const auto right_ratio = (float)gamepad.sThumbLX / 32767.0f;
            const auto forward_ratio = (float)gamepad.sThumbLY / 32767.0f;
            const auto up_ratio = (float)gamepad.sThumbRY / 32767.0f;

            if (right_ratio <= -0.25f || right_ratio >= 0.25f) {
                const auto right_offset = right_ratio * delta * 150.0f;
                m_camera_right_offset->value() += right_offset;
            }

            if (forward_ratio <= -0.25f || forward_ratio >= 0.25f) {
                const auto forward_offset = forward_ratio * delta * 150.0f;
                m_camera_forward_offset->value() += forward_offset;
            }

            if (up_ratio <= -0.25f || up_ratio >= 0.25f) {
                const auto up_offset = up_ratio * delta * 150.0f;
                m_camera_up_offset->value() += up_offset;
            }

            if (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) {
                if (!m_rt_modifier.was_moving_left) {
                    if (m_rt_modifier.page > 0) {
                        m_rt_modifier.page--;
                    } else {
                        m_rt_modifier.page = m_rt_modifier.num_pages - 1;
                    }

                    m_rt_modifier.was_moving_left = true;
                }
            } else {
                m_rt_modifier.was_moving_left = false;
            }

            if (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) {
                if (!m_rt_modifier.was_moving_right) {
                    if (m_rt_modifier.page < m_rt_modifier.num_pages - 1) {
                        m_rt_modifier.page++;
                    } else {
                        m_rt_modifier.page = 0;
                    }

                    m_rt_modifier.was_moving_right = true;
                }
            } else {
                m_rt_modifier.was_moving_right = false;
            }

            // Reset camera offset
            switch (m_rt_modifier.page) {
            case 2:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    save_camera(2);
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    save_camera(1);
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    save_camera(0);
                }
                break;
            
            case 1:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    load_camera(2);
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    load_camera(1);
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    load_camera(0);
                }

                break; 
            case 0:
            default:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    m_camera_right_offset->value() = 0.0f;
                    m_camera_forward_offset->value() = 0.0f;
                    m_camera_up_offset->value() = 0.0f;
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    this->recenter_view();
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    this->set_standing_origin(this->get_position(0));
                }
                
                break;
            }

            // ignore everything else
            return;
        }

        // From imgui_impl_win32.cpp
        #define IM_SATURATE(V)                      (V < 0.0f ? 0.0f : V > 1.0f ? 1.0f : V)
        #define MAP_BUTTON(KEY_NO, BUTTON_ENUM)     { io.AddKeyEvent(KEY_NO, (gamepad.wButtons & BUTTON_ENUM) != 0); }
        #define MAP_ANALOG(KEY_NO, VALUE, V0, V1)   { float vn = (float)(VALUE - V0) / (float)(V1 - V0); io.AddKeyAnalogEvent(KEY_NO, vn > 0.10f, IM_SATURATE(vn)); }

        MAP_BUTTON(ImGuiKey_GamepadStart,           XINPUT_GAMEPAD_START);
        MAP_BUTTON(ImGuiKey_GamepadBack,            XINPUT_GAMEPAD_BACK);
        MAP_BUTTON(ImGuiKey_GamepadFaceLeft,        XINPUT_GAMEPAD_X);
        MAP_BUTTON(ImGuiKey_GamepadFaceRight,       XINPUT_GAMEPAD_B);
        MAP_BUTTON(ImGuiKey_GamepadFaceUp,          XINPUT_GAMEPAD_Y);
        MAP_BUTTON(ImGuiKey_GamepadFaceDown,        XINPUT_GAMEPAD_A);
        MAP_BUTTON(ImGuiKey_GamepadDpadLeft,        XINPUT_GAMEPAD_DPAD_LEFT);
        MAP_BUTTON(ImGuiKey_GamepadDpadRight,       XINPUT_GAMEPAD_DPAD_RIGHT);
        MAP_BUTTON(ImGuiKey_GamepadDpadUp,          XINPUT_GAMEPAD_DPAD_UP);
        MAP_BUTTON(ImGuiKey_GamepadDpadDown,        XINPUT_GAMEPAD_DPAD_DOWN);
        MAP_ANALOG(ImGuiKey_GamepadL2,              gamepad.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
        MAP_ANALOG(ImGuiKey_GamepadR2,              gamepad.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
        MAP_BUTTON(ImGuiKey_GamepadL3,              XINPUT_GAMEPAD_LEFT_THUMB);
        MAP_BUTTON(ImGuiKey_GamepadR3,              XINPUT_GAMEPAD_RIGHT_THUMB);

        if (!is_vr_controller) {
            MAP_ANALOG(ImGuiKey_GamepadLStickLeft,      gamepad.sThumbLX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
            MAP_ANALOG(ImGuiKey_GamepadLStickRight,     gamepad.sThumbLX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
            MAP_ANALOG(ImGuiKey_GamepadLStickUp,        gamepad.sThumbLY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
            MAP_ANALOG(ImGuiKey_GamepadLStickDown,      gamepad.sThumbLY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
            MAP_BUTTON(ImGuiKey_GamepadL1,              XINPUT_GAMEPAD_LEFT_SHOULDER);
            MAP_BUTTON(ImGuiKey_GamepadR1,              XINPUT_GAMEPAD_RIGHT_SHOULDER);
        } else {
            // Map it to the dpad
            const auto left_stick_left = gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_left.was_pressed(left_stick_left)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, true);
            } else if (!left_stick_left) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, false);
            }

            const auto left_stick_right = gamepad.sThumbLX > +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_right.was_pressed(left_stick_right)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, true);
            } else if (!left_stick_right) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
            }

            const auto left_stick_up = gamepad.sThumbLY > +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_up.was_pressed(left_stick_up)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, true);
            } else if (!left_stick_up) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, false);
            }

            const auto left_stick_down = gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_down.was_pressed(left_stick_down)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
            } else if (!left_stick_down) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
            }
        }

        MAP_ANALOG(ImGuiKey_GamepadRStickLeft,      gamepad.sThumbRX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
        MAP_ANALOG(ImGuiKey_GamepadRStickRight,     gamepad.sThumbRX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiKey_GamepadRStickUp,        gamepad.sThumbRY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiKey_GamepadRStickDown,      gamepad.sThumbRY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
    });

    // Zero out the state so we don't send input to the game.
    ZeroMemory(&state.Gamepad, sizeof(XINPUT_GAMEPAD));
}

void VR::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    ZoneScopedN(__FUNCTION__);

    m_cvar_manager->on_pre_engine_tick(engine, delta);
    m_last_engine_tick = std::chrono::steady_clock::now();

    // Heartbeat log so we can confirm from the log timeline whether the game thread tick is
    // actually still running (vs. stalled) during a stuck level transition.
    {
        static auto last_heartbeat = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (m_diag_verbose_logging && now - last_heartbeat >= std::chrono::seconds(1)) {
            last_heartbeat = now;
            SPDLOG_INFO("[VR] on_pre_engine_tick heartbeat (engine={})", (void*)engine);
        }
    }

    // Detect a stale/tearing-down scene capture world as early as possible on the game thread,
    // rather than waiting for the next render-thread call into begin_render_viewfamily_real().
    // A full level transition (as opposed to sub-level streaming) can begin tearing down the
    // world the capture actor lives in well before the next render call arrives, so checking
    // here closes that window and unhooks the capture setup before the unload proceeds further.
    // DIAG: manual glitch marker. Press the marker key (NumPad0) the instant a blur/double-image
    // moment is perceived in the headset. Logs a precise, greppable timestamp plus the live
    // scene-capture stall state, so it can be correlated against the exact log window afterward
    // instead of relying on estimated minute-level timestamps.
    {
        static bool s_marker_key_was_down = false;
        const auto marker_key_down = is_glitch_marker_key_down();

        if (marker_key_down && !s_marker_key_was_down) {
            const auto stall_ms = m_fake_stereo_hook != nullptr ? m_fake_stereo_hook->get_scene_capture_stall_duration_ms() : 0;
            SPDLOG_WARN("[VR][GLITCH-MARKER] User-reported glitch at this moment (scene_capture_stall_ms={})", stall_ms);
            request_glitch_frame_dump();
            request_glitch_eye_diag_window();
        }

        s_marker_key_was_down = marker_key_down;
    }

    // DIAG: dash-blur numeric capture. NumPad1 arms a per-frame (unthrottled) capture of the NSF
    // sync-pose values for the next 200 frames/3s; NumPad2 stamps the exact frame the blur was
    // perceived within that window, without stopping the capture. See request_dash_capture()/
    // request_dash_capture_mark() in VR.hpp and the consuming log in calculate_stereo_view_offset().
    {
        static bool s_arm_key_was_down = false;
        const auto arm_key_down = is_dash_capture_arm_key_down();

        if (arm_key_down && !s_arm_key_was_down) {
            SPDLOG_WARN("[VR][DASH-CAPTURE] Capture ARMED - logging every frame's NSF sync-pose values for up to {} frames/{}ms",
                kDashCaptureMaxFrames, kDashCaptureTimeoutMs);
            request_dash_capture();
        }

        s_arm_key_was_down = arm_key_down;

        static bool s_mark_key_was_down = false;
        const auto mark_key_down = is_dash_capture_mark_key_down();

        if (mark_key_down && !s_mark_key_was_down) {
            SPDLOG_WARN("[VR][DASH-CAPTURE] Blur MARKED at this moment");
            request_dash_capture_mark();
        }

        s_mark_key_was_down = mark_key_down;
    }

    if (m_fake_stereo_hook != nullptr) {
        if (auto rtm = m_fake_stereo_hook->get_render_target_manager(); rtm != nullptr && rtm->is_scene_capture_world_stale()) {
            auto engine_ptr = sdk::UEngine::get();
            auto world = engine_ptr != nullptr ? engine_ptr->get_world() : nullptr;
            SPDLOG_INFO("[VR] on_pre_engine_tick: scene capture's world is stale (changed or tearing down), destroying proactively "
                        "(current_world={})",
                        (void*)world);
            rtm->destroy_scene_capture();
        }
    }

    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

    SPDLOG_INFO_ONCE("VR: Pre-engine tick");

    m_render_target_pool_hook->on_pre_engine_tick(engine, delta);

    update_statistics_overlay(engine);

    // Dont update action states on AFR frames
    // TODO: fix this for actual AFR, but we dont really care about pure AFR since synced beats it most of the time
    if (m_fake_stereo_hook != nullptr && !m_fake_stereo_hook->is_ignoring_next_viewport_draw()) {
        static uint32_t s_action_states_fault_count = 0;

        if (!call_update_action_states_seh(this)) {
            ++s_action_states_fault_count;
            SPDLOG_ERROR("[VR] update_action_states() faulted (SEH caught), skipping this tick's action/haptic/dpad update (fault_count={})", s_action_states_fault_count);
        }
    }
}

void VR::on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation, 
                                             const float world_to_meters, Vector3f* view_location, bool is_double)
{
    if (!is_hmd_active()) {
        m_camera_freeze.position_wants_freeze = false;
        m_camera_freeze.rotation_wants_freeze = false;
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const auto delta = std::chrono::duration<float, std::chrono::seconds::period>(now - m_last_lerp_update).count();

    Rotator<double>* view_rotation_double = (Rotator<double>*)view_rotation;
    Vector3d* view_location_double = (Vector3d*)view_location;

    glm::vec3 target_rotation = is_double ? glm::vec3{*(glm::vec<3, double>*)view_rotation_double} : *(glm::vec<3, float>*)view_rotation;

    const auto should_lerp_pitch = m_lerp_camera_pitch->value();
    const auto should_lerp_yaw = m_lerp_camera_yaw->value();
    const auto should_lerp_roll = m_lerp_camera_roll->value();

    auto lerp_angle = [](auto a, auto b, auto t) {
        const auto diff = b - a;
        if constexpr (std::is_same_v<decltype(a), double>) {
            if (diff > 180.0) {
                b -= 360.0;
            } else if (diff < -180.0) {
                b += 360.0;
            }
        } else {
            if (diff > 180.0f) {
                b -= 360.0f;
            } else if (diff < -180.0f) {
                b += 360.0f;
            }
        }

        return glm::lerp(a, b, t);
    };

    const auto lerp_t = m_lerp_camera_speed->value() * delta;

    if (should_lerp_pitch) {
        if (is_double) {
            view_rotation_double->pitch = lerp_angle((double)m_camera_lerp.last_rotation.x, (double)target_rotation.x, (double)lerp_t);
        } else {
            view_rotation->pitch = lerp_angle(m_camera_lerp.last_rotation.x, target_rotation.x, lerp_t);
        }
    }

    if (should_lerp_yaw) {
        if (is_double) {
            view_rotation_double->yaw = lerp_angle((double)m_camera_lerp.last_rotation.y, (double)target_rotation.y, (double)lerp_t);
        } else {
            view_rotation->yaw = lerp_angle(m_camera_lerp.last_rotation.y, target_rotation.y, lerp_t);
        }
    }

    if (should_lerp_roll) {
        if (is_double) {
            view_rotation_double->roll = lerp_angle((double)m_camera_lerp.last_rotation.z, (double)target_rotation.z, (double)lerp_t);
        } else {
            view_rotation->roll = lerp_angle(m_camera_lerp.last_rotation.z, target_rotation.z, lerp_t);
        }
    }

    if (is_double) {
        m_camera_lerp.last_rotation = glm::vec3{ (float)view_rotation_double->pitch, (float)view_rotation_double->yaw, (float)view_rotation_double->roll };
    } else {
        m_camera_lerp.last_rotation = glm::vec3{ view_rotation->pitch, view_rotation->yaw, view_rotation->roll };
    }

    m_last_lerp_update = std::chrono::high_resolution_clock::now();

    if (m_camera_freeze.position_wants_freeze) {
        if (is_double) {
            m_camera_freeze.position = glm::vec3{ (float)view_location_double->x, (float)view_location_double->y, (float)view_location_double->z };
        } else {
            m_camera_freeze.position = glm::vec3{ view_location->x, view_location->y, view_location->z };
        }

        m_camera_freeze.position_wants_freeze = false;
        m_camera_freeze.position_frozen = true;
    }

    if (m_camera_freeze.rotation_wants_freeze) {
        if (is_double) {
            m_camera_freeze.rotation = glm::vec3{ (float)view_rotation_double->pitch, (float)view_rotation_double->yaw, (float)view_rotation_double->roll };
        } else {
            m_camera_freeze.rotation = glm::vec3{ view_rotation->pitch, view_rotation->yaw, view_rotation->roll };
        }

        m_camera_freeze.rotation_wants_freeze = false;
        m_camera_freeze.rotation_frozen = true;
    }

    if (m_camera_freeze.position_frozen) {
        if (is_double) {
            view_location_double->x = m_camera_freeze.position.x;
            view_location_double->y = m_camera_freeze.position.y;
            view_location_double->z = m_camera_freeze.position.z;
        } else {
            view_location->x = m_camera_freeze.position.x;
            view_location->y = m_camera_freeze.position.y;
            view_location->z = m_camera_freeze.position.z;
        }
    }

    if (m_camera_freeze.rotation_frozen) {
        if (is_double) {
            view_rotation_double->pitch = m_camera_freeze.rotation.x;
            view_rotation_double->yaw = m_camera_freeze.rotation.y;
            view_rotation_double->roll = m_camera_freeze.rotation.z;
        } else {
            view_rotation->pitch = m_camera_freeze.rotation.x;
            view_rotation->yaw = m_camera_freeze.rotation.y;
            view_rotation->roll = m_camera_freeze.rotation.z;
        }
    }
}

void VR::on_pre_viewport_client_draw(void* viewport_client, void* viewport, void* canvas){
    ZoneScopedN(__FUNCTION__);

    if (m_custom_z_near_enabled->value()) {
        SPDLOG_INFO_ONCE("Attempting to set custom z near");
        sdk::globals::get_near_clipping_plane() = m_custom_z_near->value();
    }
}

bool VR::is_in_cinematic_mode() {
    const auto world = sdk::UEngine::get()->get_world();

    if (world == nullptr) {
        return false;
    }

    const auto controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0);

    if (controller == nullptr) {
        return false;
    }

    if (controller->get_class() == nullptr) {
        return false;
    }

    static const auto boolprop = (sdk::FBoolProperty*)controller->get_class()->find_property(L"bCinematicMode");

    if (boolprop == nullptr) {
        return false;
    }

    const auto in_cinematic = boolprop->get_value_from_object(controller);

    static bool was_in_cinematic = false;

    if (in_cinematic != was_in_cinematic) {
        SPDLOG_INFO("[VR][CINEMATIC-MODE] bCinematicMode transitioned to {}", in_cinematic);
        was_in_cinematic = in_cinematic;
    }

    return in_cinematic;
}

bool VR::is_ui_blanked_for_mirror_trigger() const {
    if (m_fake_stereo_hook == nullptr) {
        return false;
    }

    const auto silence_ms = m_fake_stereo_hook->get_lgui_draw_silence_duration_ms();
    const auto blanked = silence_ms >= m_lgui_draw_silence_threshold_ms;

    static bool was_blanked = false;

    if (blanked != was_blanked) {
        SPDLOG_INFO("[VR][UI-BLANK] LGUI draw-silence trigger transitioned to {} (silence_ms={})", blanked, silence_ms);
        was_blanked = blanked;
    }

    return blanked;
}

void VR::update_hmd_state(bool from_view_extensions, uint32_t frame_count) {
    ZoneScopedN(__FUNCTION__);

    std::scoped_lock _{m_reinitialize_mtx};

    // DIAG: correlate update_hmd_state() call timing against left/right eye render calls (see
    // calculate_stereo_view_offset_'s matching [DIAG] NSF-POSE-REFRESH-RACE log) to confirm whether
    // a pose refresh can land in between a same-frame eye pair, which would explain the double-vision/
    // ghosting symptom identically for both NSF and Scene View compatibility (both just read whatever
    // this function last wrote into the shared pose_mtx-protected snapshot).
    if (m_diag_log_pose_refresh_timing) {
        ++m_diag_pose_refresh_call_count;
        SPDLOG_INFO("[VR][NSF-POSE-REFRESH-RACE] update_hmd_state call #{} from_view_extensions={} requested_frame_count={}",
            m_diag_pose_refresh_call_count, from_view_extensions, frame_count);
    }

    auto runtime = get_runtime();
    if (m_uncap_framerate->value()) {
        sdk::set_cvar_data_float(L"Engine", L"t.MaxFPS", 500.0f);
    }

    // Allows games running in HDR mode to not have a black UI overlay
    if (m_disable_hdr_compositing->value()) {
        sdk::set_cvar_data_int(L"SlateRHIRenderer", L"r.HDR.UI.CompositeMode", 0);
    }

    if (m_disable_blur_widgets->value()) {
        if (auto val = sdk::get_cvar_int(L"Slate", L"Slate.AllowBackgroundBlurWidgets"); val && *val != 0) {
            sdk::set_cvar_int(L"Slate", L"Slate.AllowBackgroundBlurWidgets", 0);
        }
    }

    if (!is_using_afr()) {
        const auto is_hzbo_frozen_by_cvm = m_cvar_manager != nullptr && m_cvar_manager->is_hzbo_frozen_and_enabled();

        // Forcefully disable r.HZBOcclusion, it doesn't work with native stereo mode (sometimes)
        // Except when the user sets it to 1 with the CVar Manager, we need to respect that
        if (m_disable_hzbocclusion->value() && !is_hzbo_frozen_by_cvm) {
            const auto r_hzb_occlusion_value = sdk::get_cvar_int(L"Renderer", L"r.HZBOcclusion");

            // Only set it once, otherwise we'll be spamming a Set call every frame
            if (r_hzb_occlusion_value && *r_hzb_occlusion_value != 0) {
                sdk::set_cvar_int(L"Renderer", L"r.HZBOcclusion", 0);
            }
        }

        if (m_disable_instance_culling->value()) {
            const auto r_instance_culling_value = sdk::get_cvar_int(L"Renderer", L"r.InstanceCulling.OcclusionCull");

            if (r_instance_culling_value && *r_instance_culling_value != 0) {
                sdk::set_cvar_int(L"Renderer", L"r.InstanceCulling.OcclusionCull", 0);
            }
        }

        // See m_disable_motion_blur_nsf declaration for rationale: motion blur is only ever force-
        // disabled below for AFR; NSF left it fully enabled, and its mono-authored per-pixel motion
        // vectors can diverge between the two independently-rendered eyes during large screen-space
        // motion (fast pans, skill/ability VFX, cutscene blends), reading as ghosting/double-vision.
        if (m_disable_motion_blur_nsf->value()) {
            const auto motion_blur_value = sdk::get_cvar_int(L"Engine", L"r.DefaultFeature.MotionBlur");

            if (motion_blur_value && *motion_blur_value != 0) {
                sdk::set_cvar_data_int(L"Engine", L"r.DefaultFeature.MotionBlur", 0);
            }
        }

        // DIAG: see m_disable_skin_cache_nsf declaration for rationale. Applied continuously (rather
        // than once) so it stays disabled even if the engine/game re-enables it after a level/menu
        // transition. Off by default; this is a safe, console-free alternative to running
        // "r.SkinCache.Mode 0" since the in-game console crashes the game.
        if (m_disable_skin_cache_nsf->value()) {
            const auto skin_cache_value = sdk::get_cvar_int(L"Engine", L"r.SkinCache.Mode");

            if (skin_cache_value && *skin_cache_value != 0) {
                sdk::set_cvar_data_int(L"Engine", L"r.SkinCache.Mode", 0);
            }
        }
    }

    if (frame_count != 0 && is_using_afr() && frame_count % 2 == 0) {
        if (runtime->is_openxr()) {
            std::scoped_lock __{ m_openxr->sync_assignment_mtx };

            const auto last_frame = (frame_count - 1) % runtimes::OpenXR::QUEUE_SIZE;
            const auto now_frame = frame_count % runtimes::OpenXR::QUEUE_SIZE;
            m_openxr->pipeline_states[now_frame] = m_openxr->pipeline_states[last_frame];
            m_openxr->pipeline_states[now_frame].frame_count = now_frame;
        } else {
            const auto last_frame = (frame_count - 1) % m_openvr->pose_queue.size();
            const auto now_frame = frame_count % m_openvr->pose_queue.size();
            m_openvr->pose_queue[now_frame] = m_openvr->pose_queue[last_frame];
        }

        // Forcefully disable motion blur because it freaks out with AFR
        sdk::set_cvar_data_int(L"Engine", L"r.DefaultFeature.MotionBlur", 0);
        return;
    }
    
    runtime->update_poses(from_view_extensions, frame_count);

    // Update the poses used for the game
    // If we used the data directly from the WaitGetPoses call, we would have to lock a different mutex and wait a long time
    // This is because the WaitGetPoses call is blocking, and we don't want to block any game logic
    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        std::unique_lock _{ runtime->pose_mtx };
        set_rotation_offset(glm::identity<glm::quat>());
        m_standing_origin = get_position_unsafe(vr::k_unTrackedDeviceIndex_Hmd);

        runtime->wants_reset_origin = false;
    }

    runtime->update_matrices(m_nearz, m_farz);

    runtime->got_first_poses = true;
}

void VR::update_action_states() {
    ZoneScopedN(__FUNCTION__);

    std::scoped_lock _{m_actions_mtx};

    auto runtime = get_runtime();

    if (runtime == nullptr || runtime->wants_reinitialize) {
        return;
    }

    static bool once = true;

    if (once) {
        spdlog::info("VR: Updating action states");
        once = false;
    }


    if (runtime->is_openvr()) {
        const auto start_time = std::chrono::high_resolution_clock::now();

        auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            spdlog::error("VRInput failed to update action state: {}", (uint32_t)error);
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto time_delta = end_time - start_time;

        m_last_input_delay = time_delta;
        m_avg_input_delay = (m_avg_input_delay + time_delta) / 2;

        if ((end_time - start_time) >= std::chrono::milliseconds(30)) {
            spdlog::warn("VRInput update action state took too long: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

            //reinitialize_openvr();
            runtime->wants_reinitialize = true;
        }   
    } else {
        get_runtime()->update_input();
    }

    bool actively_using_controller = false;

    if (is_any_action_down()) {
        m_last_controller_update = std::chrono::steady_clock::now();
        actively_using_controller = true;
    }

    const auto last_xinput_update_is_late = std::chrono::steady_clock::now() - m_last_xinput_update >= std::chrono::seconds(2);
    const auto should_be_spoofing = (actively_using_controller || get_runtime()->handle_pause);

    if (m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        m_spoofed_gamepad_connection = false;
    }

    if (!m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        spdlog::info("[VR] Attempting to spoof gamepad connection");
        g_framework->post_message(WM_DEVICECHANGE, 0, 0);
        g_framework->activate_window();

        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    /*if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (m_set_standing_key->is_key_down_once()) {
        set_standing_origin(get_position(0));
    }*/

    static bool once2 = true;

    if (once2) {
        spdlog::info("VR: Updated action states");
        once2 = false;
    }

    update_dpad_gestures();
}

void VR::update_dpad_gestures() {
    if (!is_hmd_active()) {
        return;
    }

    const auto dpad_method = get_dpad_method();
    if (dpad_method != DPadMethod::GESTURE_HEAD && dpad_method != DPadMethod::GESTURE_HEAD_RIGHT) {
        return;
    }

    const auto wanted_index = dpad_method == DPadMethod::GESTURE_HEAD ? get_left_controller_index() : get_right_controller_index();

    const auto controller_pos = glm::vec3{get_position(wanted_index)};
    const auto hmd_transform = get_hmd_transform(m_frame_count);

    // Check if controller is near HMD
    const auto dist = glm::length(controller_pos - glm::vec3{hmd_transform[3]});

    if (dist > 0.2f) {
        return;
    }

    const auto dir_to_left = glm::normalize(controller_pos - glm::vec3{hmd_transform[3]});
    const auto hmd_dir = glm::quat{glm::extractMatrixRotation(hmd_transform)} * glm::vec3{0.0f, 0.0f, 1.0f};

    const auto angle = glm::acos(glm::dot(dir_to_left, hmd_dir));

    constexpr float threshold = glm::radians(120.0f);

    if (angle > threshold) {
        return;
    }

    // Make sure the angle is to the left/right of the HMD
    if (dpad_method == DPadMethod::GESTURE_HEAD_RIGHT) {
        if (glm::cross(dir_to_left, hmd_dir).y > 0.0f) {
            return;
        }
    } else if (glm::cross(dir_to_left, hmd_dir).y < 0.0f) {
        return;
    }

    // Send a vibration pulse to the controller
    const auto chosen_joystick = dpad_method == DPadMethod::GESTURE_HEAD ? m_left_joystick : m_right_joystick;
    trigger_haptic_vibration(0.0f, 0.1f, 1.0f, 5.0f, chosen_joystick);

    std::scoped_lock _{m_dpad_gesture_state.mtx};

    const auto left_joystick_axis = get_joystick_axis(chosen_joystick);

    if (left_joystick_axis.x < -0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::LEFT;
    } else if (left_joystick_axis.x > 0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::RIGHT;
    } 
    
    if (left_joystick_axis.y < -0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::DOWN;
    } else if (left_joystick_axis.y > 0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::UP;
    }
}

void VR::on_config_load(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (get_runtime() != nullptr && get_runtime()->loaded) {
        get_runtime()->on_config_load(cfg, set_defaults);

        // Run the rest of OpenXR initialization code here that depends on config values
        if (m_first_config_load) {
            m_first_config_load = false; // because the frontend can request config reloads

            if (get_runtime()->is_openxr()) {
                spdlog::info("[VR] Finishing up OpenXR initialization");
                initialize_openxr_swapchains();
            }
        }
    }

    if (m_fake_stereo_hook != nullptr) {
        m_fake_stereo_hook->on_config_load(cfg, set_defaults);
    }

    m_overlay_component.on_config_load(cfg, set_defaults);

    if (m_cvar_manager != nullptr) {
        m_cvar_manager->on_config_load(cfg, set_defaults);   
    }

    // Load camera offsets
    load_cameras();
}

void VR::on_config_save(utility::Config& cfg) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }

    if (m_fake_stereo_hook != nullptr) {
        m_fake_stereo_hook->on_config_save(cfg);
    }

    if (get_runtime()->loaded) {
        get_runtime()->on_config_save(cfg);
    }

    m_overlay_component.on_config_save(cfg);

    // Save camera offsets
    save_cameras();
}

void VR::load_cameras() try {
    ZoneScopedN(__FUNCTION__);

    const auto cameras_txt = Framework::get_persistent_dir("cameras.txt");

    if (std::filesystem::exists(cameras_txt)) {
        spdlog::info("[VR] Loading camera offsets from {}", cameras_txt.string());

        utility::Config cfg{cameras_txt.string()};

        for (auto i = 0; i < m_camera_datas.size(); i++) {
            auto& data = m_camera_datas[i];

            if (auto offs = cfg.get<float>(std::format("camera_right_offset{}", i))) {
                data.offset.x = *offs;
            }

            if (auto offs = cfg.get<float>(std::format("camera_up_offset{}", i))) {
                data.offset.y = *offs;
            }

            if (auto offs = cfg.get<float>(std::format("camera_forward_offset{}", i))) {
                data.offset.z = *offs;
            }

            if (auto scale = cfg.get<float>(std::format("world_scale{}", i))) {
                data.world_scale = *scale;
            }

            if (auto decoupled_pitch = cfg.get<bool>(std::format("decoupled_pitch{}", i))) {
                data.decoupled_pitch = *decoupled_pitch;
            }

            if (auto decoupled_pitch_ui_adjust = cfg.get<bool>(std::format("decoupled_pitch_ui_adjust{}", i))) {
                data.decoupled_pitch_ui_adjust = *decoupled_pitch_ui_adjust;
            }
        }
    }
} catch(...) {
    spdlog::error("[VR] Failed to load camera offsets");
}

void VR::load_camera(int index) {
    ZoneScopedN(__FUNCTION__);

    if (index < 0 || index >= m_camera_datas.size()) {
        return;
    }

    const auto& data = m_camera_datas[index];

    m_camera_right_offset->value() = data.offset.x;
    m_camera_up_offset->value() = data.offset.y;
    m_camera_forward_offset->value() = data.offset.z;
    m_world_scale->value() = data.world_scale;
    m_decoupled_pitch->value() = data.decoupled_pitch;
    m_decoupled_pitch_ui_adjust->value() = data.decoupled_pitch_ui_adjust;
}

void VR::save_camera(int index) {
    ZoneScopedN(__FUNCTION__);

    if (index < 0 || index >= m_camera_datas.size()) {
        return;
    }

    auto& data = m_camera_datas[index];

    data.offset = {
        m_camera_right_offset->value(),
        m_camera_up_offset->value(),
        m_camera_forward_offset->value()
    };

    data.world_scale = m_world_scale->value();
    data.decoupled_pitch = m_decoupled_pitch->value();
    data.decoupled_pitch_ui_adjust = m_decoupled_pitch_ui_adjust->value();

    save_cameras();
}

void VR::save_cameras() try {
    ZoneScopedN(__FUNCTION__);

    const auto cameras_txt = Framework::get_persistent_dir("cameras.txt");

    spdlog::info("[VR] Saving camera offsets to {}", cameras_txt.string());

    utility::Config cfg{cameras_txt.string()};

    for (auto i = 0; i < m_camera_datas.size(); i++) {
        const auto& data = m_camera_datas[i];
        cfg.set<float>(std::format("camera_right_offset{}", i), data.offset.x);
        cfg.set<float>(std::format("camera_up_offset{}", i), data.offset.y);
        cfg.set<float>(std::format("camera_forward_offset{}", i), data.offset.z);
        cfg.set<float>(std::format("world_scale{}", i), m_camera_datas[i].world_scale);
        cfg.set<bool>(std::format("decoupled_pitch{}", i), m_camera_datas[i].decoupled_pitch);
        cfg.set<bool>(std::format("decoupled_pitch_ui_adjust{}", i), m_camera_datas[i].decoupled_pitch_ui_adjust);
    }

    cfg.save(cameras_txt.string());
} catch(...) {
    spdlog::error("[VR] Failed to save camera offsets");
}


void VR::on_pre_imgui_frame() {
    ZoneScopedN(__FUNCTION__);

    m_xinput_context.update();

    if (!get_runtime()->ready()) {
        return;
    }

    if (!m_disable_overlay) {
        m_overlay_component.on_pre_imgui_frame();
    }
}

void VR::handle_keybinds() {
    ZoneScopedN(__FUNCTION__);

    if (m_keybind_recenter->is_key_down_once()) {
        recenter_view();
    }

    if (m_keybind_recenter_horizon->is_key_down_once()) {
        recenter_horizon();
    }

    if (m_keybind_load_camera_0->is_key_down_once()) {
        load_camera(0);
    }

    if (m_keybind_load_camera_1->is_key_down_once()) {
        load_camera(1);
    }

    if (m_keybind_load_camera_2->is_key_down_once()) {
        load_camera(2);
    }

    if (m_keybind_set_standing_origin->is_key_down_once()) {
        m_standing_origin = get_position(0);
    }

    if (m_keybind_toggle_2d_screen->is_key_down_once()) {
        m_2d_screen_mode->toggle();
    }

    if (m_keybind_disable_vr->is_key_down_once()) {
        m_disable_vr = !m_disable_vr; // definitely should not be persistent
    }

    // The Slate UI
    if (m_keybind_toggle_gui->is_key_down_once()) {
        m_enable_gui->toggle();
    }
}

void VR::on_frame() {
    ZoneScopedN(__FUNCTION__);

    m_cvar_manager->on_frame();
    handle_keybinds();

    if (!get_runtime()->ready()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto is_allowed_draw_window = now - m_last_xinput_update < std::chrono::seconds(2);

    if (!is_allowed_draw_window) {
        m_rt_modifier.draw = false;
    }

    if (is_allowed_draw_window && m_xinput_context.headlocked_begin_held && !FrameworkConfig::get()->is_l3_r3_long_press()) {
        const auto rt_size = g_framework->get_rt_size();

        ImGui::Begin("AimMethod Notification", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);

        ImGui::Text("Continue holding down L3 + R3 to toggle aim method");

        if (std::chrono::steady_clock::now() - m_xinput_context.headlocked_begin >= std::chrono::seconds(1)) {
            if (m_aim_method->value() == VR::AimMethod::GAME) {
                m_aim_method->value() = m_previous_aim_method;
            } else {
                m_aim_method->value() = VR::AimMethod::GAME; // turns it off
            }

            m_xinput_context.headlocked_begin_held = false;
        } else {
            if (m_aim_method->value() != VR::AimMethod::GAME) {
                m_previous_aim_method = (VR::AimMethod)m_aim_method->value();
            } else if (m_previous_aim_method == VR::AimMethod::GAME) {
                m_previous_aim_method = VR::AimMethod::HEAD; // so it will at least be something
            }
        }

        const auto window_size = ImGui::GetWindowSize();

        const auto centered_x = (rt_size.x / 2) - (window_size.x / 2);
        const auto centered_y = (rt_size.y / 2) - (window_size.y / 2);
        ImGui::SetWindowPos(ImVec2(centered_x, centered_y), ImGuiCond_Always);

        ImGui::End();
    }

    if (m_rt_modifier.draw) {
        const auto rt_size = g_framework->get_rt_size();

        ImGui::Begin("RT Modifier Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Left Stick: Camera left/right/forward/back");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Right Stick: Camera up/down");
        
        ImGui::Text("Page: %d", m_rt_modifier.page + 1);
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "DPad Left: Previous page | DPad Right: Next page");

        switch (m_rt_modifier.page) {
        case 2:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Save Camera 2");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Save Camera 1");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Save Camera 0");
            break;

        case 1:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Load Camera 2");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Load Camera 1");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Load Camera 0");
            break;

        case 0:
        default:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Reset camera offset");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Recenter view");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Reset standing origin");
            m_rt_modifier.page = 0;
            break;
        }

        const auto window_size = ImGui::GetWindowSize();

        const auto centered_x = (rt_size.x / 2) - (window_size.x / 2);
        const auto centered_y = (rt_size.y / 2) - (window_size.y / 2);
        ImGui::SetWindowPos(ImVec2(centered_x, centered_y), ImGuiCond_Always);
        ImGui::End();
    }
}

void VR::on_present() {
    ZoneScopedN(__FUNCTION__);

    m_present_thread_id = GetCurrentThreadId();

    utility::ScopeGuard _guard {[&]() {
        if (!is_using_afr() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
            SetEvent(m_present_finished_event);
        }

        m_last_frame_count = m_render_frame_count;
    }};

    m_frame_count = get_runtime()->internal_render_frame_count;

    // DIAG: trace whether m_frame_count (sourced from the runtime's internal_render_frame_count)
    // is actually advancing. If it's frozen here, the AFR eye-parity logic downstream
    // (m_render_frame_count, is_left_eye_frame/is_right_eye_frame) can never toggle eyes.
    if (is_diag_verbose_logging_enabled()) {
        static uint32_t s_last_diag_present_frame_count = 0xFFFFFFFF;
        if (m_frame_count != s_last_diag_present_frame_count) {
            SPDLOG_INFO("[DIAG] VR::on_present: m_frame_count changed {} -> {} (is_using_afr={})", s_last_diag_present_frame_count, m_frame_count, is_using_afr());
            s_last_diag_present_frame_count = m_frame_count;
        } else {
            SPDLOG_INFO_EVERY_N_SEC(2, "[DIAG] VR::on_present: m_frame_count stuck at {} (is_using_afr={})", m_frame_count, is_using_afr());
        }
    }

    if (!is_using_afr() || m_render_frame_count % 2 == m_left_eye_interval) {
        ResetEvent(m_present_finished_event);
    }

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        m_fake_stereo_hook->on_frame(); // Just let all the hooks engage, whatever.
        return;
    }

    runtime->consume_events(nullptr);

    m_fake_stereo_hook->on_frame();

    auto openvr = get_runtime<runtimes::OpenVR>();

    if (runtime->is_openvr()) {
        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            const auto now = std::chrono::system_clock::now();

            if (now - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;

            // upon headset re-entry, reinitialize OpenVR
            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                return;
            }
        } else {
            openvr->is_hmd_active = true; // We need to force out an initial WaitGetPoses call
            openvr->was_hmd_active = true;
        }
    }

    // attempt to fix crash when reinitializing openvr
    std::scoped_lock _{m_openvr_mtx};
    m_submitted = false;

    const auto renderer = g_framework->get_renderer_type();
    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    const auto is_left_eye_frame = is_using_afr() ? (m_render_frame_count % 2 == m_left_eye_interval) : true;

    if (is_left_eye_frame && get_synchronize_stage() == VR::SynchronizeStage::LATE) {
        const auto had_sync = runtime->got_first_sync;
        runtime->synchronize_frame();

        if (!runtime->got_first_poses || !had_sync) {
            update_hmd_state();
        }
    }

    if (renderer == Framework::RendererType::D3D11) {
        // if we don't do this then D3D11 OpenXR freezes for some reason.
        if (!runtime->got_first_sync) {
            SPDLOG_INFO_EVERY_N_SEC(1, "Attempting to sync!");
            if (get_synchronize_stage() == VR::SynchronizeStage::LATE) {
                runtime->synchronize_frame();
            }

            update_hmd_state();
        }

        m_is_d3d12 = false;
        e = m_d3d11.on_frame(this);
    } else if (renderer == Framework::RendererType::D3D12) {
        m_is_d3d12 = true;
        e = m_d3d12.on_frame(this);
    }

    // force a waitgetposes call to fix this...
    if (e == vr::EVRCompositorError::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
        openvr->got_first_poses = false;
        openvr->needs_pose_update = true;
    }

    if (m_submitted) {
        if (m_submitted) {
            if (!m_disable_overlay) {
                m_overlay_component.on_post_compositor_submit();
            }

            if (runtime->is_openvr()) {
                //vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
                //vr::VRCompositor()->PostPresentHandoff();
            }
        }

        //runtime->needs_pose_update = true;
        m_submitted = false;

        // On the first ever submit, we need to activate the window and set the mouse to the center
        // so the user doesn't have to click on the window to get input.
        if (m_first_submit) {
            m_first_submit = false;

            // for some reason this doesn't work if called directly from here
            // so we have to do it in a separate thread
            std::thread worker([]() {
                g_framework->activate_window();
                g_framework->set_mouse_to_center();
                spdlog::info("Finished first submit from worker thread!");
            });
            worker.detach();
        }
    }
}

void VR::on_post_present() {
    FrameMarkNamed("Present");
    ZoneScopedN(__FUNCTION__);

    const auto is_same_frame = m_render_frame_count > 0 && m_render_frame_count == m_frame_count;

    // DIAG: trace whether m_render_frame_count is actually advancing relative to m_frame_count.
    // is_same_frame==true every single call is the direct upstream cause of the AFR eye-parity
    // check in D3D12Component::on_frame() being permanently stuck on one eye.
    if (is_diag_verbose_logging_enabled()) {
        static uint32_t s_last_diag_render_frame_count = 0xFFFFFFFF;
        SPDLOG_INFO_EVERY_N_SEC(2, "[DIAG] VR::on_post_present: m_render_frame_count={} m_frame_count={} is_same_frame={}",
            m_render_frame_count, m_frame_count, is_same_frame);
        s_last_diag_render_frame_count = m_render_frame_count;
    }

    m_render_frame_count = m_frame_count;

    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    std::scoped_lock _{m_openvr_mtx};

    if (!m_is_d3d12) {
        m_d3d11.on_post_present(this);
    } else {
        m_d3d12.on_post_present(this);
    }

    detect_controllers();

    const auto is_left_eye_frame = is_using_afr() ? (is_same_frame || (m_render_frame_count % 2 == m_left_eye_interval)) : true;

    if (is_left_eye_frame) {
        if (get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE || !runtime->got_first_sync) {
            const auto had_sync = runtime->got_first_sync;
            runtime->synchronize_frame();

            if (!runtime->got_first_poses || !had_sync) {
                update_hmd_state();
            }
        }

        if (runtime->is_openxr() && runtime->ready() && get_synchronize_stage() > VR::SynchronizeStage::EARLY) {
            if (!m_openxr->frame_began) {
                m_openxr->begin_frame();
            }
        }
    }

    if (runtime->wants_reinitialize) {
        std::scoped_lock _{m_reinitialize_mtx};

        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }
    }
}

uint32_t VR::get_hmd_width() const {
    if (m_2d_screen_mode->value()) {
        if (get_runtime()->is_openxr()) {
            return g_framework->get_rt_size().x * m_openxr->resolution_scale->value();
        }

        return g_framework->get_rt_size().x;
    }

    if (m_extreme_compat_mode->value()) {
        return g_framework->get_rt_size().x;
    }

    return std::max<uint32_t>(get_runtime()->get_width(), 128);
}

uint32_t VR::get_hmd_height() const {
    if (m_2d_screen_mode->value()) {
        if (get_runtime()->is_openxr()) {
            return g_framework->get_rt_size().y * m_openxr->resolution_scale->value();
        }

        return g_framework->get_rt_size().y;
    }

    if (m_extreme_compat_mode->value()) {
        return g_framework->get_rt_size().y;
    }

    return std::max<uint32_t>(get_runtime()->get_height(), 128);
}

void VR::on_draw_sidebar_entry(std::string_view name) {
    const auto hash = utility::hash(name.data());

    // Draw the ui thats always drawn first.
    on_draw_ui();

    /*const auto made_child = ImGui::BeginChild("VRChild", ImVec2(0, 0), true, ImGuiWindowFlags_::ImGuiWindowFlags_NavFlattened);

    utility::ScopeGuard sg([made_child]() {
        if (made_child) {
            ImGui::EndChild();
        }
    });*/

    enum SelectedPage {
        PAGE_RUNTIME,
        PAGE_UNREAL,
        PAGE_INPUT,
        PAGE_CAMERA,
        PAGE_KEYBINDS,
        PAGE_CONSOLE,
        PAGE_COMPATIBILITY,
        PAGE_DEBUG,
    };

    SelectedPage selected_page = PAGE_RUNTIME;

    /*ImGui::BeginTable("VRTable", 2, ImGuiTableFlags_::ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_::ImGuiTableFlags_BordersOuterV | ImGuiTableFlags_::ImGuiTableFlags_SizingFixedFit);
    ImGui::TableSetupColumn("LeftPane", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("RightPane", ImGuiTableColumnFlags_WidthStretch);

    // Draw left pane
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); // Set to the first column

        ImGui::BeginGroup();

        auto dcs = [&](const char* label, SelectedPage page_value) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
            utility::ScopeGuard sg3([]() {
                ImGui::PopStyleVar();
            });
            if (ImGui::Selectable(label, selected_page == page_value)) {
                selected_page = page_value;
                return true;
            }
            return false;
        };

        dcs("Runtime", PAGE_RUNTIME);
        dcs("Unreal", PAGE_UNREAL);
        dcs("Input", PAGE_INPUT);
        dcs("Camera", PAGE_CAMERA);
        dcs("Console/CVars", PAGE_CONSOLE);
        dcs("Compatibility", PAGE_COMPATIBILITY);
        dcs("Debug", PAGE_DEBUG);

        ImGui::EndGroup();
    }

    ImGui::TableNextColumn(); // Move to the next column (right)
    ImGui::BeginGroup();*/

    switch (hash) {
    case "Runtime"_fnv:
        selected_page = PAGE_RUNTIME;
        break;
    case "Unreal"_fnv:
        selected_page = PAGE_UNREAL;
        break;
    case "Input"_fnv:
        selected_page = PAGE_INPUT;
        break;
    case "Camera"_fnv:
        selected_page = PAGE_CAMERA;
        break;
    case "Keybinds"_fnv:
        selected_page = PAGE_KEYBINDS;
        break;
    case "Console/CVars"_fnv:
        selected_page = PAGE_CONSOLE;
        break;
    case "Compatibility"_fnv:
        selected_page = PAGE_COMPATIBILITY;
        break;
    case "Debug"_fnv:
        selected_page = PAGE_DEBUG;
        break;
    default:
        ImGui::Text("Unknown page selected");
        break;
    }

    if (selected_page == PAGE_RUNTIME) {
        if (m_has_hw_scheduling) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: Hardware-accelerated GPU scheduling is enabled. This may cause the game to run slower.");
            ImGui::TextWrapped("Go into your Windows Graphics settings and disable \"Hardware-accelerated GPU scheduling\"");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Note: This is only necessary if you are experiencing performance issues.");
        }

        if (GetModuleHandleW(L"nvngx_dlssg.dll") != nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: DLSS Frame Generation has been detected. Make sure it is disabled within in-game settings.");
            ImGui::PopStyleColor();
        }

        ImGui::Text((std::string{"Runtime Information ("} + get_runtime()->name().data() + ")").c_str());

        m_desktop_fix->draw("Desktop Spectator View");
        ImGui::SameLine();
        m_2d_screen_mode->draw("2D Screen Mode");

        ImGui::TextWrapped("Render Resolution (per-eye): %d x %d", get_runtime()->get_width(), get_runtime()->get_height());
        ImGui::TextWrapped("Total Render Resolution: %d x %d", get_runtime()->get_width() * 2, get_runtime()->get_height());

        if (get_runtime()->is_openvr()) {
            ImGui::TextWrapped("Resolution can be changed in SteamVR");
        }

        get_runtime()->on_draw_ui();

        m_overlay_component.on_draw_ui();

        ImGui::TreePop();
    }

    if (selected_page == PAGE_UNREAL) {
        m_rendering_method->draw("Rendering Method");
        m_synced_afr_method->draw("Synced Sequential Method");

        m_world_scale->draw("World Scale");
        m_depth_scale->draw("Depth Scale");

        m_disable_hzbocclusion->draw("Disable HZBOcclusion");
        m_disable_instance_culling->draw("Disable Instance Culling");
        m_disable_hdr_compositing->draw("Disable HDR Composition");
        m_disable_blur_widgets->draw("Disable Blur Widgets");
        m_uncap_framerate->draw("Uncap Framerate");
        m_enable_gui->draw("Enable GUI");
        m_enable_depth->draw("Enable Depth-based Latency Reduction");
        m_load_blueprint_code->draw("Load Blueprint Code");
        m_ghosting_fix->draw("Ghosting Fix");

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Native Stereo Fix")) {
            m_native_stereo_fix->draw("Enabled");
            m_native_stereo_fix_right_eye_shadows->draw("Right Eye Shadow Fix");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("While the right eye renders, marks its view as the shadow-owning (primary) eye\nso whole-scene shadows are generated for it. Field offsets are discovered at\nruntime by comparing the two eye views. Camera/projection are untouched, so\nstereo depth is preserved.");
            }
            m_native_stereo_fix_same_pass->draw("Use Same Stereo Pass (unstable)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Legacy approach: hides the view family from the FSceneView constructor and\nforces PRIMARY on the secondary view's init options. Crashes in this game\n(null deref inside the engine). Superseded by Right Eye Shadow Fix.");
            }
            // Stereo Setup Diagnostics Toggle
            m_enable_stereopass_diagnostics->draw("Enable Stereo Pass Diagnostics");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Emits consolidated per-frame summaries (viewports, call counts, AFR status) to spdlog.\n"
                                  "Useful for diagnosing eye synchronization issues; keep off during normal play.");
            }
            m_native_stereo_fix_auto_suspend->draw("Auto-Suspend During Level Transitions");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Automatically turns Native Stereo Fix off while a loading screen / level\ntransition is detected (tick stall, missing pawn/controller, stale world) and\nback on once the world settles - the same as manually toggling it. Untick to\ntest whether the fix is still needed now that the scene capture is no longer\nrooted across LoadMap.");
            }

            ImGui::Separator();
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
            if (ImGui::TreeNode("Sync Pose / Double-Vision Fix")) {
                ImGui::TextWrapped(
                    "WHY THE DOUBLE-VISION GLITCH HAPPENS: Native Stereo Fix renders the left eye (Pass 1)\n"
                    "and right eye (Pass 2) as two separate calls into the SAME animated frame. If the\n"
                    "camera's position/rotation changes between those two calls (a cutscene blend, dash,\n"
                    "ability camera shift, or menu/conversation view-target switch), each eye samples a\n"
                    "DIFFERENT camera pose, so the two eyes render from two different world transforms -\n"
                    "seen in-headset as a sudden double-image/ghosting 'cut'. This is different from normal\n"
                    "stereo parallax, which is small and expected.\n\n"
                    "THE FIX: cache Pass 1's rotation+position for the frame and force Pass 2 to reuse it\n"
                    "(below). This guarantees both eyes share one pose on frames where a big cut is\n"
                    "detected, eliminating the double image.\n\n"
                    "WHY DASH/FAST-TURN MOTION LOOKS BLURRY: ordinary player-driven motion (dashing,\n"
                    "spinning the camera) also produces continuous small pose divergence between Pass 1\n"
                    "and Pass 2, frame after frame - NOT a one-off cut. If the position-sync snap fires on\n"
                    "every one of those ordinary motion frames too, it keeps force-snapping Pass 2 toward a\n"
                    "one-frame-stale cached position every single frame of continuous motion - fighting\n"
                    "normal per-eye parallax the whole time, which reads as a slight, constant blur/doubling\n"
                    "during dashes and fast turns even though no real cut is happening.\n\n"
                    "HOW TO CONFIGURE THE SCALERS: if you still see blur during dashing/spinning, LOWER the\n"
                    "Rotation Gate Threshold and/or RAISE the Sustained-Motion Suppression Frames slightly\n"
                    "so more ordinary motion gets classified as 'plateau' and skipped. If a real cut\n"
                    "(skill/teleport/cutscene) stops fully correcting (double image comes back), that means\n"
                    "a legitimate impulse is being caught by the gates - RAISE the Rotation Gate Threshold\n"
                    "and/or LOWER the Pos-Delta Ceiling so fewer real impulses slip through. The Pos-Delta\n"
                    "Ceiling is the final safety net: it should sit between the largest pos_delta you ever\n"
                    "see from normal dashing (use NumPad1/NumPad2 to capture and check your own log) and\n"
                    "the smallest pos_delta you see during an actual teleport/skill-cut glitch. If you\n"
                    "cannot find a ceiling value that separates the two cleanly for your character/skills,\n"
                    "prefer erring toward a LOWER ceiling (protects against double-vision at the cost of\n"
                    "occasionally still showing a touch of dash blur) rather than a higher one.\n\n"
                    "THE TWO SCALERS BELOW exist to tell dash/turn motion apart from a real cut so the\n"
                    "snap only fires when it's actually needed:");
                m_diag_double_vision_fix_master->draw("Fix Double-Vision / Eye Blur (Sync Pose + Exclude View Index 1)");


                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("One-click shortcut for the confirmed double-image/blur fix combo: enables Sync\nEye Pose at full (1.0) blend alpha, AND excludes view_index=1 from the NSF\nsync-pose cache (see Debug tab's NS Double Vision Diagnostics for the individual\ntoggles, including the manual Sync Eye Pose toggle). Disabling this turns all of\nthem back off. Prefer this single toggle over manually setting the others - use\nthe Debug tab only if you need to re-tune blend alpha or the excluded index for\nfurther testing.");
                }
                m_diag_sync_pose_verbose_logging->draw("Verbose Sync-Pose Logging (NSF-POSE-DIVERGE / NSF-SYNC-FORCE-FULL)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Gates the high-frequency NSF-POSE-DIVERGE/-TRACE, NSF-SYNC-FORCE-FULL, and\nNSF-EXCLUDED-INDEX-SYNCED log lines fired every Pass2 call (throttled to\n1-2/sec, but still constant spam for as long as Sync Pose is enabled). Only\nturn this on briefly while actively reproducing a glitch or tuning the\nscalers below - leave it off otherwise to keep the log clean.");
                }
                m_diag_rotation_gated_position_sync->draw("Rotation-Gated Position Sync (fix #1 for dash blur)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("TEST TOGGLE. Dash-blur capture data showed the blurry frames all have rot_delta_deg\n"
                                      "== 0.0000 (no real rotational eye mismatch) while pos_delta stays moderately elevated\n"
                                      "every frame - the position sync is force-snapping Pass2 toward a stale cached\n"
                                      "position every frame of continuous motion, fighting normal per-eye parallax instead\n"
                                      "of fixing an actual cut. Real glitches always show a substantial rot_delta_deg\n"
                                      "instead. Enabling this skips the POSITION portion of the sync for the current frame\n"
                                      "whenever rot_delta_deg is below the threshold slider below (rotation sync is\n"
                                      "unaffected). A/B test against Sustained-Motion Suppression below and against both off.");
                }
                m_diag_rotation_gated_position_sync_threshold_deg->draw("Rotation Gate Threshold (x0.1 deg)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Minimum rot_delta_deg required this frame for the position sync to still apply\n"
                                      "while Rotation-Gated Position Sync above is enabled. Slider is in tenths of a\n"
                                      "degree, so 10 == 1.0 degree. Defaults to 10 (1.0 deg) - comfortably above float\n"
                                      "noise but well below the smallest real hard-cut rot_delta_deg observed (~2.4 deg).\n"
                                      "Raise this if dash blur still shows a small rotation reading during motion; lower\n"
                                      "it if a real cut with slight rotation stops getting fully corrected.");
                }
                m_diag_sustained_motion_position_sync_suppression->draw("Sustained-Motion Position Sync Suppression (fix #2 for dash blur)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("TEST TOGGLE, alternative/complementary to Rotation-Gated Position Sync above.\n"
                                      "Tracks a streak of consecutive Pass2 frames whose rot_delta_deg stays below the\n"
                                      "rotation gate threshold slider above; once that streak reaches the frame count\n"
                                      "below, the position sync snap is suppressed for the rest of the streak, treating\n"
                                      "sustained near-zero-rotation divergence as continuous player-driven motion\n"
                                      "(dash/fast turn) rather than a one-off cut. A single frame with rot_delta_deg\n"
                                      "above the threshold resets the streak, so a real hard cut (always shows a\n"
                                      "rotation spike) still gets the full instant snap. Works well together with fix #1:\n"
                                      "fix #1 covers steady-state motion immediately, fix #2 catches the streak-based\n"
                                      "case; combining both leaves at most a very small residual blur on extremely fast\n"
                                      "sustained translation (the unavoidable one-frame lag of a cached pose).");
                }
                m_diag_sustained_motion_position_sync_suppression_frames->draw("Sustained-Motion Suppression Frames");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("How many consecutive low-rotation Pass2 frames must elapse before the position sync\n"
                                      "is suppressed while Sustained-Motion Position Sync Suppression above is enabled.\n"
                                      "Defaults to 3 - ignores a single-frame reporting fluke but still reacts within\n"
                                      "~50-100ms of a dash starting. Lower this to react faster (may miss the very first\n"
                                      "dash frame less often); raise it to be more conservative about treating motion as\n"
                                      "'sustained' before suppressing.");
                }
                m_diag_position_sync_suppression_pos_delta_ceiling->draw("Position Sync Suppression Pos-Delta Ceiling");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Safety ceiling shared by BOTH position-sync suppression fixes above. Real\n"
                                      "capture proved some hard cuts are PURELY positional (large scripted position jump,\n"
                                      "e.g. a skill/teleport, with almost no camera rotation) - which the rotation gate\n"
                                      "alone would wrongly treat as dash-like motion and suppress, bringing back double\n"
                                      "vision. Suppression only applies when pos_delta is ALSO below this ceiling; a large\n"
                                      "positional jump always forces the full snap regardless of rotation. Defaults to 120\n"
                                      "units (above the largest dash-only pos_delta observed, ~90; below the smallest\n"
                                      "pure-position-cut pos_delta observed, ~300). Lower it toward ~90 if a real\n"
                                      "positional cut is still being suppressed; raise it toward ~300 if legitimate fast\n"
                                      "dash motion is still being treated as a cut.");
                }
                ImGui::TreePop();
            }
            ImGui::Separator();

            m_disable_motion_blur_nsf->draw("Disable Motion Blur (Native Stereo Fix)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Forces r.DefaultFeature.MotionBlur off while Native Stereo Fix (non-AFR) is active.\\n"
                                  "AFR already force-disables motion blur (see update_hmd_state) because its mono-\\n"
                                  "authored per-pixel velocity buffer does not account for a second independent eye;\\n"
                                  "NSF never had the same protection. If left on, the two eyes' blur trails can\\n"
                                  "diverge during large screen-space motion (fast pans, skill/ability VFX, cutscene\\n"
                                  "blends), which reads as ghosting/double-vision. Disable this toggle to A/B test\\n"
                                  "whether motion blur is contributing to reported blur during skills/VFX.");
            }
            m_native_stereo_fix_mirror->draw("Mirror Right Eye (No Scene Capture)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Skips spawning the scene-capture actor entirely. The right eye is\njust a flat mirror of the left/game view (no stereoscopic depth).\nUse this as a stable fallback if the scene capture is causing\nstuck loading screens during level transitions, or if an eye ever\nbreaks/goes black due to future code changes. See the Debug tab's\n'NS Double Vision Diagnostics' section for the auto-mirror triggers\n(cutscene/UI-blank/motion) and other double-vision investigation toggles.");
            }
            ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
            if (ImGui::TreeNode("UI Fixes for Shadow LGUI")) {
                m_native_stereo_fix_tall_ui->draw("Fit UI To Full Canvas (Fix Bottom Cutoff)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("With Native Stereo Fix on, LGUI lays its UI canvas out at the per-eye view\nrect height, which is taller than the scene render target, so the bottom of\nthe UI is clipped. This grows the UI render target to the full canvas height\nso the whole UI is captured, then presents it cropped/stretched back to 16:9.\nOnly applies while Native Stereo Fix is active - in AFR / NSF Off the canvas\nis not tall, so growing the target would distort the UI. Requires a texture\nrecreation (toggle NSF off/on or reload) to take effect.");
                }

                m_disable_lgui_ui_redirect->draw("Disable LGUI UI Redirect (Shadow-Copy Pool Off)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Skips submitting the redirected LGUI UI texture to the compositor every frame\n"
                                       "(wait/acquire/copy/clear/draw on the UI swapchain), without changing how the\n"
                                       "game itself renders its UI. Use this to A/B test whether the UI redirect submission\n"
                                       "is contributing to a visual/perf issue, at the cost of falling back to the\n"
                                       "original HMD-attached UI behavior.");
                }

                m_enable_lgui_restrict_ref_scan_to_known_offsets->draw("Restrict LGUI Ref Patch To Known-Good Offsets (Submenu Crash Fix)");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("The a2 ref scan normally checks EVERY 8-byte slot from 0x0 to 0x3d0 and patches any\n"
                                       "slot whose raw value equals the a3 pointer, even though only offsets 0x270/0x348/0x3a0\n"
                                       "are actually dereferenced by LGUI's Execute pass. During submenu/nested-popup churn, a\n"
                                       "freed and reused address can make an unrelated a2 field collide with a stale/reused a3\n"
                                       "value, causing the blind scan to patch a field that was never a real render-target\n"
                                       "reference - a confirmed cause of submenu crashes. When enabled, only the three\n"
                                       "known-good offsets are checked/patched, removing that collision risk entirely.");
                }          
                m_enable_lgui_logging->draw("Enable LGUI Screen-Pass Texture Logging");
                if (ImGui::IsItemHovered()) {
                
                }
                ImGui::TreePop();
            }
            m_native_stereo_fix_allow_with_afr->draw("Allow While Using AFR/Synchronized Sequential");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Normally Native Stereo Fix only runs when NOT using AFR/Synchronized\nSequential rendering. Enabling this allows it to also run in those modes.\nFixes 2D-screen UI being scaled to HMD resolution instead of the desktop\nresolution, and the resulting loss of native gamepad UI confirm/navigation\ninput, in Synchronized Sequential mode.");
            }
            m_unify_afr_frame_parity->draw("Unify AFR Eye-Parity With Compositor (2D Screen)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Makes AdjustViewRect/calculate_stereo_view_offset derive which eye is\ncurrently being rendered from the SAME frame-parity source the D3D11/D3D12\ncompositor uses, instead of each hook maintaining its own independent\ncounter. Fixes UI scaling/interaction issues AND desktop-spectator/TV\nscreen output in Synchronized Sequential 2D-screen mode. Automatically has\nno effect outside of 2D-screen mode, since it was found to break true\nstereoscopic VR rendering (right-eye black/flicker, lost HMD tracking).");
            }
            m_disable_loading_guards->draw("Disable Loading Guards (A/B testing)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Bypasses all loading-screen/level-transition detection heuristics\n(tick-stall, missing player controller/pawn, boot-phase) used to gate\nscene capture creation. Use this to test whether these heuristics are\ncontributing to a stuck loading screen.");
            }
            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Near Clip Plane")) {
            m_custom_z_near_enabled->draw("Enable");

            if (m_custom_z_near_enabled->value()) {
                m_custom_z_near->draw("Value");

                if (m_custom_z_near->value() <= 0.0f) {
                    m_custom_z_near->value() = 0.01f;
                }
            }

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_INPUT) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Controller")) {
            m_joystick_deadzone->draw("VR Joystick Deadzone");
            m_controller_pitch_offset->draw("Controller Pitch Offset");

            m_dpad_shifting->draw("DPad Shifting");
            ImGui::SameLine();
            m_swap_controllers->draw("Left-handed Controller Inputs");
            m_dpad_shifting_method->draw("DPad Shifting Method");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Aim Method")) {
            ImGui::TextWrapped("Some games may not work with this enabled.");
            if (m_aim_method->draw("Type")) {
                m_previous_aim_method = (AimMethod)m_aim_method->value();
            }

            m_aim_speed->draw("Speed");
            m_aim_interp->draw("Smoothing");

            m_aim_modify_player_control_rotation->draw("Modify Player Control Rotation");
            ImGui::SameLine();
            m_aim_use_pawn_control_rotation->draw("Use Pawn Control Rotation");

            m_aim_multiplayer_support->draw("Multiplayer Support");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Snap Turn")) {
            m_snapturn->draw("Enabled");
            m_snapturn_angle->draw("Angle");
            m_snapturn_joystick_deadzone->draw("Deadzone");
        
            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Movement Orientation")) {
            m_movement_orientation->draw("Type");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Roomscale Movement")) {
            m_roomscale_movement->draw("Enabled");

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, headset movement will affect the movement of the player character.");
            }

            ImGui::SameLine();
            m_roomscale_sweep->draw("Sweep Movement");
            // Draw description of option
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, roomscale movement will use a sweep to prevent the player from moving through walls.\nThis also allows physics objects to interact with the player, like doors.");
            }

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_CAMERA) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Freeze")) {
            float camera_offset[] = {m_camera_forward_offset->value(), m_camera_right_offset->value(), m_camera_up_offset->value()};
            if (ImGui::SliderFloat3("Camera Offset", camera_offset, -4000.0f, 4000.0f)) {
                m_camera_forward_offset->value() = camera_offset[0];
                m_camera_right_offset->value() = camera_offset[1];
                m_camera_up_offset->value() = camera_offset[2];
            }

            for (auto i = 0; i < m_camera_datas.size(); ++i) {
                auto& data = m_camera_datas[i];

                if (ImGui::Button(std::format("Save Camera {}", i).data())) {
                    save_camera(i);
                }

                ImGui::SameLine();

                if (ImGui::Button(std::format("Load Camera {}", i).data())) {
                    load_camera(i);
                }
            }

            bool pos_freeze = m_camera_freeze.position_frozen || m_camera_freeze.position_wants_freeze;
            if (ImGui::Checkbox("Freeze Position", &pos_freeze)) {
                if (pos_freeze) {
                    m_camera_freeze.position_wants_freeze = true;
                } else {
                    m_camera_freeze.position_frozen = false;
                }
            }

            ImGui::SameLine();
            bool rot_freeze = m_camera_freeze.rotation_frozen || m_camera_freeze.rotation_wants_freeze;
            if (ImGui::Checkbox("Freeze Rotation", &rot_freeze)) {
                if (rot_freeze) {
                    m_camera_freeze.rotation_wants_freeze = true;
                } else {
                    m_camera_freeze.rotation_frozen = false;
                }
            }

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Lerp")) {
            m_lerp_camera_pitch->draw("Lerp Pitch");
            ImGui::SameLine();
            m_lerp_camera_yaw->draw("Lerp Yaw");
            ImGui::SameLine();
            m_lerp_camera_roll->draw("Lerp Roll");
            m_lerp_camera_speed->draw("Lerp Speed");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Decoupled Pitch")) {
            m_decoupled_pitch->draw("Enabled");
            m_decoupled_pitch_ui_adjust->draw("Auto Adjust UI");

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_KEYBINDS) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Playspace Keys")) {
            m_keybind_recenter->draw("Recenter View Key");
            m_keybind_recenter_horizon->draw("Recenter Horizon Key");
            m_keybind_set_standing_origin->draw("Set Standing Origin Key");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Keys")) {
            m_keybind_load_camera_0->draw("Load Camera 0 Key");
            m_keybind_load_camera_1->draw("Load Camera 1 Key");
            m_keybind_load_camera_2->draw("Load Camera 2 Key");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Overlay/Runtime Keys")) {
            m_keybind_toggle_2d_screen->draw("Toggle 2D Screen Mode Key");
            m_keybind_toggle_gui->draw("Toggle In-Game UI Key");
            m_keybind_disable_vr->draw("Disable VR Key");

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_CONSOLE) {
        m_cvar_manager->on_draw_ui();
    }

    if (selected_page == PAGE_COMPATIBILITY) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Compatibility Options")) {
            m_compatibility_ahud->draw("AHUD UI Compatibility");
            m_compatibility_skip_uobjectarray_init->draw("Skip UObjectArray Init");
            m_compatibility_skip_pip->draw("Skip PostInitProperties");
            m_sceneview_compatibility_mode->draw("SceneView Compatibility Mode");
            m_extreme_compat_mode->draw("Extreme Compatibility Mode");

            // changes to any of these options should trigger a regeneration of the eye projection matrices
            const auto horizontal_projection_changed = m_horizontal_projection_override->draw("Horizontal Projection");
            const auto vertical_projection_changed = m_vertical_projection_override->draw("Vertical Projection");
            const auto scale_render = m_grow_rectangle_for_projection_cropping->draw("Scale Render Target");
            const auto scale_render_changed = get_runtime()->is_modifying_eye_texture_scale != scale_render;
            get_runtime()->is_modifying_eye_texture_scale = scale_render;
            get_runtime()->should_recalculate_eye_projections = horizontal_projection_changed || vertical_projection_changed || scale_render_changed;

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Splitscreen Compatibility")) {
            m_splitscreen_compatibility_mode->draw("Enabled");
            m_splitscreen_view_index->draw("Index");
            ImGui::TreePop();
        }
    }
    
    if (selected_page == PAGE_DEBUG) {
        if (m_fake_stereo_hook != nullptr) {
            m_fake_stereo_hook->on_draw_ui();
        }

        m_disable_lgui_uaf_store_recovery->draw("DIAG: Disable LGUI UAF Store Recovery (crash-fast vs. hang-avoidance)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Controls how the generalized game-exe UAF exception handler reacts to a WRITE\n"
                               "access violation on the known stale/freed-object fault family. OFF (default)\n"
                               "keeps the legacy 'skip the write' recovery, which let the game keep running\n"
                               "through most occurrences and was stable in the majority of sessions. Turning\n"
                               "this ON removes that recovery so those faults propagate as a real, immediate\n"
                               "crash instead - useful if you'd rather get a diagnosable crash dump for a\n"
                               "specific repro than risk the skipped write later causing a permanent hang\n"
                               "(observed in one session: the write turned out to be load-bearing for\n"
                               "game-thread progress, and skipping it stalled forever instead of crashing).");
        }

        m_disable_lgui_uaf_load_store_guard->draw("DIAG: Disable LGUI UAF Load-Store Guard (A/B test for black eye)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Controls the forward-store-skip guard added to the generalized game-exe MOV/MOVZX\n"
                               "load-recovery path. After forcing a stale/freed load result to 0, that guard scans\n"
                               "a short window forward and skips a downstream store that uses a register tainted\n"
                               "by the zeroed value, to prevent a follow-on write access violation. OFF (default)\n"
                               "keeps the guard active. Turn this ON to fully disable it (only the load is zeroed;\n"
                               "any follow-on store through the derived value is allowed to fault normally) - use\n"
                               "this to A/B test whether the guard itself is responsible for a black/no-visual\n"
                               "left eye or a related crash, since skipping the wrong store could leave engine\n"
                               "state (e.g. a view/eye index or render target selector) incorrectly zeroed.");
        }

        m_disable_lgui_uaf_float_guard->draw("DIAG: Disable LGUI UAF Float Guard (A/B test for black eye)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Controls a guard in the generalized game-exe MOV/MOVZX load-recovery path that\n"
                               "checks whether the faulting base register's raw bits reinterpret as a plausible\n"
                               "small float (e.g. observed: 0xbf800000 == -1.0f, a typical per-eye scale/sign\n"
                               "constant) rather than garbage/pointer-like bits. OFF (default) keeps the guard\n"
                               "active: when the base register looks like a plausible float, the zero-forcing\n"
                               "recovery is skipped and the fault is allowed to propagate instead, since forcing\n"
                               "a real scale constant to 0 could corrupt eye/view state without ever being a\n"
                               "true use-after-free. Turn this ON to fully disable the guard and always force the\n"
                               "load result to 0 regardless of what the base register's bits look like (the old,\n"
                               "unconditional behavior) - use this to A/B test whether this guard is what's\n"
                               "needed to fix a black/no-visual left eye, or whether it's unrelated.");
        }

        m_disable_lgui_frt_redirect->draw("EXPERIMENT: Disable LGUI FRenderTarget Redirect (uncheck to enable UI redirect via ui_target)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("This hooks the shared FRenderTarget::GetRenderTargetTexture() vtable\n"
                               "slot to redirect LGUI's render target to ui_target as an alternative to the a3\n"
                               "shadow-copy/quarantine pool. Two earlier attempts caused DXGI_ERROR_DEVICE_REMOVED\n"
                               "crashes because they returned pointers tied to a3's transient per-frame RDG\n"
                               "lifetime. This version instead returns the address of the render-target-manager's\n"
                               "own persistent ui_target member (mirroring the already-working AHUD compatibility\n"
                               "viewport hook), guarded against dangling/freed pointers, so there is no per-frame\n"
                               "lifetime race. Default: DISABLED (checked) - uncheck this box to enable the\n"
                               "redirect. When enabled, the a3 shadow-copy pool below is skipped entirely for\n"
                               "this call path (see is_lgui_frt_redirect_disabled() gating).");
        }

        m_enable_lgui_shadow_ui_texture->draw("EXPERIMENT: LGUI Shadow UI Texture (size-matched, for use with FRT Redirect)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("[LGUI_FRT_DIAG] proved ui_target is intentionally wider than what the FRT\n"
                               "redirect call site actually wants (e.g. ui_target 3840x3193 vs wanted 2699x3193),\n"
                               "which caused DXGI_ERROR_DEVICE_REMOVED when redirecting straight to ui_target.\n"
                               "Enabling this creates a SECOND persistent texture at the exact size the redirect\n"
                               "wants, alongside ui_target (not replacing it), and the redirect will only use it\n"
                               "once its cached size matches - otherwise it safely falls back to the original\n"
                               "unredirected result. Requires the FRT redirect above to be enabled (unchecked).\n"
                               "Default: OFF - test with the FRT redirect enabled to validate this fixes the\n"
                               "device-removed crash instead of abandoning the redirect approach.");
        }

        m_enable_lgui_clear_pool_on_new_a3->draw("EXPERIMENT: Clear LGUI Shadow-Copy Pool When New UI Element Appears");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("QUICK FIX ATTEMPT for stale/racing shadow-copy slots when a menu/submenu opens.\n"
                               "The a3 shadow-copy pool (lgui_copy_pool) normally only ADDS a new slot when a\n"
                               "brand-new a3 pointer appears, leaving all existing slots untouched. When this is\n"
                               "enabled, that same 'pool grew this frame' event instead force-clears the ENTIRE\n"
                               "pool (and its quarantine list) first, so every currently-visible piece of LGUI\n"
                               "content is forced to re-acquire a fresh shadow copy on its very next redirect\n"
                               "call. This is a blunt A/B test, not a targeted fix: it may cause a brief visual\n"
                               "hiccup for content whose existing copy was still legitimately in use. Default: OFF.");
        }

        m_enable_lgui_rhi_keyed_copy_pool->draw("EXPERIMENT: LGUI Shadow-Copy Pool Keyed By Stable RHI Resource");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The a3 shadow-copy pool normally keys its slots by the a3 pointer itself - a\n"
                               "transient per-frame FRDGTexture WRAPPER that Unreal's RDG allocator recreates at a\n"
                               "new address every frame ([LGUI_A3_CHURN] logs hundreds of distinct a3 addresses per\n"
                               "session). Every submenu/nested-popup crash traced so far has been a variation of\n"
                               "that per-wrapper bookkeeping (aging out slots, quarantining them, or the engine\n"
                               "reusing a freed a3 address for something else - see \"[LGUI_A3_LIFECYCLE] address\n"
                               "REUSE\") racing against real RDG execute timing.\n"
                               "[LGUI_RHI_STABILITY] already shows the underlying GPU resource each a3 wraps is\n"
                               "drawn from a small, STABLE set (usually 2-8 resources, matching eyes/menu layers,\n"
                               "with high reuse) - only the wrapper churns, not the real resource. This experiment\n"
                               "keys the shadow-copy pool by that stable RHI resource pointer instead: a slot is\n"
                               "only created once per real GPU resource, is never aged out by a frame-count guess,\n"
                               "and is immune to a3-address-reuse confusion since a3's address is never used for\n"
                               "identity, only to redirect the current call into the resource-keyed slot.\n"
                               "Default OFF (old a3-keyed pool active). Turn ON to A/B test this against the\n"
                               "existing pool, especially for reproducing the submenu crash.");
        }

        m_disable_depth_submission->draw("DIAG: Disable Depth Submission (compositor reprojection A/B test)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Stops submitting a depth layer (XR_KHR_composition_layer_depth) to the runtime's\n"
                               "compositor, even if the extension is available. The runtime's own depth-based\n"
                               "reprojection/timewarp is entirely dependent on this depth data; use this to A/B\n"
                               "test whether a transient double-image/ghosting artifact seen only in-headset\n"
                               "(not in screenshots) during animation/camera-cut transitions is caused by the\n"
                               "runtime's depth-based reprojection misinterpreting a momentarily unstable\n"
                               "near-clip-plane-derived depth range, rather than anything in the game/UEVR's\n"
                               "own rendering pipeline.");
        }

        if (ImGui::TreeNode("NS Double Vision Diagnostics")) {
            ImGui::TextWrapped("Prefer 'Fix Double-Vision / Eye Blur' in Native Stereo Fix above for normal use.\nThe toggles below are for manual re-testing/tuning only.");
            m_native_stereo_fix_sync_pose->draw("Sync Eye Pose During Camera Transitions");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Caches the left eye's (Pass 1) camera rotation AND position for this frame and\n"
                                  "forces the right eye (Pass 2) to reuse both, instead of independently re-sampling\n"
                                  "the live animated camera. Fixes a momentary left/right eye desync during camera-\n"
                                  "transition animations (cutscene blends, dashes, ability camera shifts, menu/\n"
                                  "conversation view-target switches) where the pose can change between the two eyes'\n"
                                  "render calls within the same frame. Position and rotation are always synced\n"
                                  "together now (previously separate toggles) - direct in-headset testing with the\n"
                                  "Camera Freeze debug tool proved that freezing only ONE of position/rotation still\n"
                                  "produced the desync; both must be pinned together to eliminate it, since a camera\n"
                                  "pose is the combination of both and correcting only one still leaves the two eyes\n"
                                  "rendering from different world transforms. The position portion is applied BEFORE\n"
                                  "the per-eye IPD/eye-separation offset, so stereo depth/parallax is preserved. Head\n"
                                  "tracking/parallax on all other frames is unaffected. NOTE: has no effect while the\n"
                                  "master toggle above is disabled, since that's a prerequisite for the fix combo.");
            }
            m_native_stereo_fix_sync_pose_blend_alpha->draw("Sync Pose Blend Alpha");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Controls how strongly the right eye (Pass 2) is forced toward the left eye's (Pass 1)\n"
                                  "cached pose. 1.0 (default) fully snaps Pass2 to Pass1's pose - this eliminates\n"
                                  "desync but makes the right eye feel completely frozen/lagging behind the live animated\n"
                                  "camera for that frame, which can read as disorienting one-sided lag. Lowering this lets\n"
                                  "Pass2 keep some of its own live motion, splitting the residual error across both eyes\n"
                                  "instead - trading a smaller, more symmetric-feeling desync for the previous frozen-right-\n"
                                  "eye behavior. Try values around 0.5 if the full snap feels too disorienting. NOTE: has\n"
                                  "no effect while the master toggle above is enabled, since that forces this to 1.0.");
            }
            m_native_stereo_fix_same_pass_force_primary->draw("DIAG: Force Pass2 StereoPass=PRIMARY (known black-screen risk)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Re-arms the previously disabled StereoPass=PRIMARY override for the secondary\n(right eye) view inside sceneview_constructor. A prior session confirmed this\ncauses full black-screen (main menu characters black, both eyes black in-game)\nunder some conditions. Only enable this deliberately to re-test with the added\ndecisive [VR][SAME-PASS-FORCE] logging around set_stereo_pass() and the\nFSceneView constructor entry, to correlate exactly when/why it recurs.\nRequires 'Use Same Stereo Pass (unstable)' above to also be enabled.");
            }
            m_native_stereo_fix_null_pass2_view_state->draw("DIAG: Null View State For Pass 2 (foliage test)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Renders the right-eye (Pass 2) view with no FSceneViewState: no occlusion or TAA history for that eye. "
                                  "If distant trees stop freezing in the right eye with this on, shared per-view-state occlusion history is the cause. "
                                  "Expect aliasing/flicker in the right eye while enabled.");
            }
            m_native_stereo_fix_dual_write_projection->draw("DIAG: Dual-Write Missing Eye Projection Matrix (double vision fallback)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("This title only calls CalculateStereoProjectionMatrix ONCE per frame (Instanced Stereo\\n"
                                  "Rendering). Forcing vr.InstancedStereo=0 did not change this. When enabled, directly\\n"
                                  "patches the OTHER (never-called) eye's cached FSceneView::ViewProjectionMatrix right\\n"
                                  "after the real call writes the called eye's matrix, using the same offset init_canvas()\\n"
                                  "resolves. Experimental - may crash or produce garbage if the cached view pointer for\\n"
                                  "the other eye is stale or the offset does not apply to it.");
            }
            m_native_stereo_fix_sync_pose_force_full->draw("DIAG: Force Full Sync Every Frame (bypass hard-cut gate)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Normally the full 1.0 blend_alpha snap above only applies on a detected hard-cut-grade\n"
                                  "spike; ordinary small per-frame divergence during continuous motion (walking/dashing)\n"
                                  "uses the slider's blend_alpha instead, and near-zero divergence is left unsynced.\n"
                                  "Enable this to force the FULL 1.0 snap unconditionally on every single Pass2 call,\n"
                                  "regardless of the spike-detector's verdict, and to log every application (not\n"
                                  "throttled) so it can be confirmed the sync is engaging on every frame during a\n"
                                  "reported glitch window. Only for deliberate diagnostic re-testing.");
            }
            m_diag_gradual_hard_cut_convergence->draw("DIAG: Gradual Hard-Cut Convergence (test, replaces instant snap)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("TEST TOGGLE. Normally, once a hard-cut-grade pose divergence is detected, Pass2 (right\n"
                                  "eye) is snapped INSTANTLY (blend_alpha=1.0, applied in a single frame) to Pass1's (left\n"
                                  "eye) cached pose. Enable this to instead ramp the effective blend alpha from 0 up to 1\n"
                                  "LINEARLY over the duration below, so the two eyes ease back into agreement over roughly\n"
                                  "a second instead of one eye teleporting into place. Purely additive - A/B test this\n"
                                  "against the default instant snap before deciding which feels better.");
            }
            m_diag_gradual_hard_cut_convergence_duration_ms->draw("DIAG: Gradual Hard-Cut Convergence Duration (ms)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How long (in milliseconds) the ramp above takes to go from 0 to full (1.0) blend alpha\n"
                                  "after a hard cut is detected. Defaults to 1000ms (~1 second). Only used while DIAG:\n"
                                  "Gradual Hard-Cut Convergence above is enabled.");
            }
            m_diag_post_hard_cut_sensitivity_boost->draw("DIAG: Post-Hard-Cut Sensitivity Boost (reduce dash/fast-turn blur)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("TEST TOGGLE. Normally the full-strength sync-pose snap only applies to the single\n"
                                  "frame that crosses the hard-cut spike threshold; ordinary sub-threshold jitter during\n"
                                  "the REST of that same skill/dash/camera-transition animation falls back to the\n"
                                  "gentler blend_alpha slider, which can still read as momentary blur. Enable this to\n"
                                  "temporarily lower the spike detector's sensitivity for the window below after a hard\n"
                                  "cut fires, so residual jitter throughout the same animation also gets fully corrected,\n"
                                  "not just the one frame that originally tripped it. A/B test against the default.");
            }
            m_diag_post_hard_cut_sensitivity_boost_window_ms->draw("DIAG: Sensitivity Boost Window (ms)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How long (in milliseconds) after a hard cut fires the boosted sensitivity stays\n"
                                  "active. Defaults to 500ms, covering most skill/dash animation lengths. Only used\n"
                                  "while DIAG: Post-Hard-Cut Sensitivity Boost above is enabled.");
            }
            m_diag_post_hard_cut_sensitivity_boost_multiplier->draw("DIAG: Sensitivity Boost Multiplier (x0.1)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Replacement spike-detector multiplier used while inside the boost window (normal\n"
                                  "baseline is 6.0x; this slider is in tenths, so 20 == 2.0x). Lower values make the\n"
                                  "spike test trip on smaller relative deltas, applying the full snap more readily\n"
                                  "during the boost window. Defaults to 20 (2.0x). Only used while DIAG: Post-Hard-Cut\n"
                                  "Sensitivity Boost above is enabled.");
            }
            ImGui::TextWrapped("Rotation-Gated Position Sync, Sustained-Motion Position Sync Suppression, and the\nPos-Delta Ceiling have moved to Native Stereo Fix > Sync Pose / Double-Vision Fix.");
            m_disable_skin_cache_nsf->draw("DIAG: Disable GPU Skin Cache (r.SkinCache.Mode = 0)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Forces r.SkinCache.Mode to 0 (disabled) every frame while active. Safe, console-\n"
                                  "free equivalent of running \"r.SkinCache.Mode 0\" in the in-game console, which is\n"
                                  "unstable and crashes the game. GPU Skin Cache double-buffers bone transform data\n"
                                  "per render call rather than per game-tick; since NSF renders both eyes as separate\n"
                                  "render calls within the same frame, this can theoretically make one eye sample a\n"
                                  "stale skeletal pose relative to the other during fast animation. Off by default;\n"
                                  "enable to A/B test whether it changes/removes the reported eye-pose divergence.");
            }
            m_diag_log_final_eye_pose->draw("DIAG: Log Final Per-Eye Camera Pose (spammy)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Logs the FULLY RESOLVED per-eye position/rotation (what each eye actually renders\n"
                                  "from, after head_offset/eye_separation/sync-pose) every single frame, for BOTH\n"
                                  "eyes, tagged [VR][NSF-FINAL-EYE-POSE] eye=0/1. Enable this, reproduce the glitch\n"
                                  "(e.g. open a UI menu after a skill), then disable it again and grab the log. You\n"
                                  "can then directly compare eye=0 vs eye=1 pos/rot line-by-line across the same\n"
                                  "frame range to see exactly how each eye's camera moves: whether one eye jumps\n"
                                  "immediately to the new position while the other's pos/rot values visibly\n"
                                  "interpolate toward it over several frames (proving an asymmetric camera-return\n"
                                  "lerp), or whether both eyes already match every frame (which would instead point\n"
                                  "to a rendering/compositing-side cause rather than the camera pose itself). This is\n"
                                  "very verbose (every frame, both eyes) - only leave it on while reproducing.");
            }
            m_diag_suppress_extra_view->draw("DIAG: Suppress Extra View Index (unreliable - see tooltip)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Log evidence (NSF-FINAL-EYE-POSE + GLITCH-EYE-DIAG) proved calculate_stereo_view_\n"
                                  "offset() is actually called at least THREE times per frame during NSF, not two.\n"
                                  "CAUTION: cross-session captures then proved the raw view_index assigned to that\n"
                                  "extra call is NOT stable - it was 3 in one session, but suppressing 2 or 3 in\n"
                                  "later sessions instead stuck a REAL eye to the HMD (confirmed in-headset). Do\n"
                                  "not assume a fixed index; use DIAG: Log True-Index Alias / Log Final Per-Eye\n"
                                  "Camera Pose to re-derive the correct index for THIS session before ever\n"
                                  "enabling this toggle, and disable it immediately if an eye sticks to the HMD.");
            }
            m_diag_suppress_view_index->draw("DIAG: Suppressed View Index");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("The exact view_index to suppress when the toggle above is enabled. IMPORTANT:\n"
                                  "the raw view_index for the extra call is NOT stable across sessions - it was 3 in\n"
                                  "one capture, but suppressing 2 or 3 in later sessions instead stuck a REAL eye to\n"
                                  "the HMD, proving this value must be re-verified every session (see\n"
                                  "m_diag_log_final_eye_pose / DIAG: Log True-Index Alias below) rather than assumed.");
            }
            m_diag_treat_view_index_1_as_monoscopic->draw("DIAG: Treat View Index 1 As Monoscopic (experimental)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("EXPERIMENT: on titles where the two real eyes are observed at raw view_index 2/3\n"
                                  "(instead of the stock 1/2), this treats view_index 1 as an extra/monoscopic view\n"
                                  "(mirroring index 0's existing eSSP_FULL skip once index_was_ever_two is true) and\n"
                                  "ignores it in calculate_stereo_view_offset, so it can't pollute the NSF sync-pose\n"
                                  "cache or eye-offset math. Only takes effect after view_index 2 has been observed\n"
                                  "at least once this session. Test with double vision reproduction steps and check\n"
                                  "the [VR][DIAG-INDEX1-MONO] log line to confirm it's firing.");
            }
            m_diag_log_true_index_alias->draw("DIAG: Log True-Index Alias");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("true_index (which decides left vs right eye, and is what the sync-pose cache and\n"
                                  "eye-offset math key off of) is derived purely from view_index's PARITY, so two\n"
                                  "DIFFERENT view_index values sharing the same parity silently ALIAS onto the same\n"
                                  "true_index - meaning an 'extra' render call can overwrite/compete with a real\n"
                                  "eye's cached pose for that frame instead of just being an ignorable third view.\n"
                                  "Enable this, reproduce the glitch, then grab the log and search for\n"
                                  "[VR][NSF-TRUE-INDEX-ALIAS] - each hit shows the exact frame and both colliding\n"
                                  "view_index values, which pinpoints the aliasing directly instead of guessing a\n"
                                  "fixed view_index to suppress.");
            }
            m_diag_log_world_to_meters->draw("DIAG: Log World-To-Meters Per-Eye");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Logs the world_to_meters value passed in on every stereo view-offset call,\n"
                                  "keyed by the real true_index (consistent with true_index aliasing above),\n"
                                  "and warns with [VR][NSF-WTM-MISMATCH] whenever a call's world_to_meters\n"
                                  "differs from the last value seen for the OTHER eye within the same frame.\n"
                                  "Every call is also logged as [VR][NSF-WTM-VALUE] so values can be compared\n"
                                  "directly. Use this to determine whether the doubled-image symptom could be\n"
                                  "caused by a per-eye world-scale mismatch rather than (or in addition to)\n"
                                  "camera-position/true_index aliasing.");
            }
            m_diag_log_raw_view_index->draw("DIAG: Log Raw View Index (ALL indices, unconditional)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Logs [VR][NSF-RAW-VIEW-INDEX] for EVERY raw view_index this hook is\n"
                                  "called with (0-8), alongside true_index, is_full_pass, is_using_afr,\n"
                                  "and a running per-index call count/recency. Use this to build one\n"
                                  "authoritative table of exactly which raw indices occur and how often,\n"
                                  "instead of piecing it together from aliasing/pose diagnostics alone.\n"
                                  "Very verbose - enable only during a short repro window.");
            }
            m_diag_hook_dual_view_gate->draw("DIAG: Hook Dual-View Gate Function (statically-confirmed RVA)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Installs an inline hook directly on the dual-view gate function itself, at\n"
                                  "client-win64-shippingbase.dll + the RVA below. WARNING: this crashed in\n"
                                  "testing (access violation inside the relocated trampoline, likely due to\n"
                                  "hooking deep into hot AVX/vectorized code). Prefer the caller-hook toggle\n"
                                  "below instead unless specifically comparing the two approaches. Logs both\n"
                                  "pointers, both StereoPass bytes, g_frame_count, and the original return\n"
                                  "value, then calls through unchanged (pure observer). Requires a restart to\n"
                                  "take effect. Toggle only ONE of the two Dual-View Gate hooks at a time.");
            }
            m_diag_dual_view_gate_rva->draw("DIAG: Dual-View Gate RVA");
            m_diag_hook_dual_view_gate_caller->draw("DIAG: Hook Dual-View Gate CALLER Function (safer alternative)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Alternative to the direct dual-view-gate hook above. Hooks the CALL\n"
                                  "instruction site that calls into the gate function instead, which is\n"
                                  "far more reliably relocatable than hooking deep into a hot vectorized\n"
                                  "function body. IMPORTANT: the RVA below defaults to 0 (disabled/invalid)\n"
                                  "- you must find the real caller call-site RVA yourself in a debugger\n"
                                  "first (break on the gate function entry, check the return address on\n"
                                  "the call stack, subtract the module base). Requires a restart to take\n"
                                  "effect. Toggle only ONE of the two Dual-View Gate hooks at a time.");
            }
            m_diag_dual_view_gate_caller_rva->draw("DIAG: Dual-View Gate CALLER RVA");
            if (ImGui::Button("DIAG: Dump All Stereo Addresses to Log")) {
                if (auto& hook = get_fake_stereo_hook(); hook != nullptr) {
                    hook->dump_stereo_addresses();
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Logs every stereo-rendering-related address this hook has resolved so far\n"
                                  "(FSceneView constructor, AdjustViewRect, CalculateStereoViewOffset,\n"
                                  "CalculateStereoProjectionMatrix, GetViewPassForIndex, BeginRenderViewFamily,\n"
                                  "UGameEngine::Tick, and more) as one contiguous [VR][ADDR-DUMP] block, with\n"
                                  "both the raw (ASLR'd) VA and a module-relative RVA for each. Click this AFTER\n"
                                  "the game has fully loaded so as many of these hooks as possible have had the\n"
                                  "chance to resolve, then grep the log for ADDR-DUMP to gather every address in\n"
                                  "one place for static cross-referencing instead of hunting through the whole\n"
                                  "session log for each one's individual first-resolved log line.");
            }
            m_diag_exclude_view_index_from_sync_cache->draw("DIAG: Exclude View Index From Sync-Pose Cache");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Excludes the specified view_index from ever writing to/reading from the NSF\n"
                                  "sync-pose cache used by calculate_stereo_view_offset - it does NOT skip\n"
                                  "rendering of that index (unlike DIAG: Suppress Extra View Index above), so it\n"
                                  "is much safer to leave on. Confirmed in-headset that fully suppressing\n"
                                  "view_index=1's rendering caused no stuck/frozen/blank eye, and\n"
                                  "NSF-TRUE-INDEX-ALIAS showed view_index=1 racing with the real view_index=3\n"
                                  "eye call on the same true_index - this excludes it from that race instead.\n"
                                  "Disable immediately if an eye sticks or goes blank.");
            }
            m_diag_exclude_view_index_from_sync_cache_index->draw("DIAG: Sync-Cache-Excluded View Index");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("The view_index to exclude from the sync-pose cache when the toggle above is\n"
                                  "enabled. Defaults to 1 based on the current session's NSF-RAW-VIEW-INDEX /\n"
                                  "NSF-TRUE-INDEX-ALIAS evidence, but re-verify per session as raw indices are\n"
                                  "not guaranteed stable across sessions.");
            }
            m_diag_apply_synced_pose_to_excluded_view_index->draw("DIAG: Apply Synced Pose To Excluded View Index");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Only has an effect while DIAG: Exclude View Index From Sync-Pose Cache is also\n"
                                  "enabled. Instead of leaving the excluded view_index's pose completely untouched,\n"
                                  "feeds it the SAME fully-converged rotation/position the real eyes settle on this\n"
                                  "frame (read-only - does not write back into the cache). Intended to fix\n"
                                  "shadow-cascade/occlusion-culling desync (e.g. inconsistent foliage-sway culling)\n"
                                  "attributed to that index, since NSF-VIEWINDEX-IDENTITY shows it never gets its own\n"
                                  "FSceneView and fires a variable number of times per frame, consistent with a\n"
                                  "sub-view pass rather than a renderable eye. Disable if culling/shadows get worse.");
            }
            ImGui::Separator();
            ImGui::TextWrapped("Mirror-gated toggles (require Native Stereo Fix's manual Mirror Right Eye fallback behavior):");
            m_native_stereo_fix_auto_mirror_on_cinematic->draw("Auto-Mirror Right Eye During Cutscenes (bCinematicMode)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Automatically applies the same mirror fallback (no scene capture,\nflat right eye) whenever the game reports APlayerController::bCinematicMode\n== true (Sequencer/Matinee cutscenes). Targets GPU-side VFX/foliage blur\nand desync during cutscene camera transitions that CPU-side frame-counter\nfixes could not address, without disabling stereo depth during normal\ngameplay. Independent of the manual mirror toggle in Native Stereo Fix.");
            }
            m_native_stereo_fix_auto_mirror_on_ui_blank->draw("Auto-Mirror Right Eye When UI Blanks (Skill/VFX Animations)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Automatically applies the same mirror fallback whenever LGUI's own\nUI/quad draw hook stops firing for a short window - this happens during\nmany characters' skill/VFX-heavy animations, where the UI goes blank and\nreturns to normal once the animation ends. Unlike the cutscene toggle\nabove (bCinematicMode never fires for this case), this directly detects\nthe UI-hidden window itself. Independent of the other mirror toggles.\nNOTE: testing showed this only fires on world load/menu transitions, not\nduring actual skill/VFX animations - off by default, prefer the motion\ntoggle below for that case.");
            }
            m_native_stereo_fix_auto_mirror_on_motion->draw("Auto-Mirror Right Eye On Camera Motion Divergence");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Automatically applies the same mirror fallback for a short window after\nNSF's sync-pose logic detects the live camera pose actually diverging between\nthe left/right eye render calls within the same frame (see the NSF-POSE-DIVERGE\nlog and the Sync Pose toggles above) - i.e. real camera motion caused by\nskill/VFX/dash animations. This is a direct measurement of the animation-driven\nmotion that causes the disorienting right-eye lag, unlike bCinematicMode or the\nUI-blank toggle above which do not correlate with it.");
            }
            m_scene_capture_stall_indicator->draw("DIAG: Scene Capture Stall Visual Indicator");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Tints the right eye red when the scene capture render target has been\ncontinuously null for a visually-meaningful duration and the compositor\nis presenting a stale last-known-good texture instead. Use this to see\nin real time whether a perceived blur/double-image moment lines up with\nthis specific freeze condition. Press NumPad0 the instant you see a\nglitch to also log a precise marker timestamp for later correlation.");
            }
            ImGui::TreePop();
        }

        //ImGui::Combo("Sync Mode", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0");
        m_sync_mode->draw("Sync Mode");
        ImGui::DragFloat4("Right Bounds", (float*)&m_right_bounds, 0.005f, -2.0f, 2.0f);
        ImGui::DragFloat4("Left Bounds", (float*)&m_left_bounds, 0.005f, -2.0f, 2.0f);
        ImGui::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
        ImGui::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
        ImGui::Checkbox("Disable Backbuffer Size Override", &m_disable_backbuffer_size_override);
        ImGui::Checkbox("Disable VR Overlay", &m_disable_overlay);
        ImGui::Checkbox("Disable VR Entirely", &m_disable_vr);
        ImGui::Checkbox("DIAG: Force is_using_afr() off", &m_diag_force_afr_off);
        ImGui::Checkbox("DIAG: Disable forced 2nd-eye draw", &m_diag_disable_forced_second_draw);
        ImGui::Checkbox("DIAG: Verbose Sync/Stall Logging", &m_diag_verbose_logging);
        ImGui::Checkbox("DIAG: Log Pose-Refresh Timing (NSF/SVC race)", &m_diag_log_pose_refresh_timing);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Logs every update_hmd_state() call plus per-eye pose-read counters in calculate_stereo_view_offset_ "
                "to detect whether the shared HMD pose snapshot gets refreshed in between a same-frame left/right eye pair "
                "(would affect NSF and Scene View compatibility identically). Look for [NSF-POSE-REFRESH-RACE] warnings.");
        }
        ImGui::Combo("DIAG: NSF Pass2 Frame Count Mode (shadow test)", &m_diag_nsf_pass2_frame_count_mode, "Decrement (default)\0None\0Increment\0");
        if (ImGui::Button("DIAG: NSF Frame-Diff Logger (capture ~60 frames)")) {
            m_diag_nsf_frame_diff_logger = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Requires Native Stereo Fix ON. Samples the two eye FSceneViews and the FSceneViewFamily for ~60 frames, then logs "
                              "dwords that change every frame but are identical in both eyes (per-frame state Pass 2 reads stale, e.g. a wind/time value) "
                              "and view-family dwords that change every frame. Look for [NSF-DIFF] in the log.");
        }
        ImGui::Text("DIAG: NSF Pass2 eye-field bisect (Right Eye Shadow Fix)");
        for (int i = 0; i < 8; ++i) {
            if (i > 0) ImGui::SameLine();
            char label[8]{};
            snprintf(label, sizeof(label), "F%d", i);
            ImGui::CheckboxFlags(label, (unsigned int*)&m_diag_nsf_pass2_eye_field_mask, 1u << i);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Each bit enables flipping one runtime-discovered eye identity field on the\nPass2 view (order as logged by 'sceneview_xref: eye identity fields'). Use to\nisolate which field(s) affect distant foliage culling in the right eye.");
        }
        ImGui::Checkbox("Stereo Emulation Mode", &m_stereo_emulation_mode);
        ImGui::Checkbox("Wait for Present", &m_wait_for_present);
        m_controllers_allowed->draw("Controllers allowed");
        ImGui::Checkbox("Controller test mode", &m_controller_test_mode);
        m_show_fps->draw("Show FPS");
        m_show_statistics->draw("Show Engine Statistics");

        const double min_ = 0.0;
        const double max_ = 25.0;
        ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);

        ImGui::DragFloat4("Raw Left", (float*)&m_raw_projections[0], 0.01f, -100.0f, 100.0f);
        ImGui::DragFloat4("Raw Right", (float*)&m_raw_projections[1], 0.01f, -100.0f, 100.0f);

        const auto left_stick_axis = get_left_stick_axis();
        const auto right_stick_axis = get_right_stick_axis();

        ImGui::DragFloat2("Left Stick", (float*)&left_stick_axis, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat2("Right Stick", (float*)&right_stick_axis, 0.01f, -1.0f, 1.0f);

        ImGui::TextWrapped("Hardware scheduling: %s", m_has_hw_scheduling ? "Enabled" : "Disabled");
    }

    ImGui::EndGroup();
    //ImGui::EndTable();
}

void VR::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    // create VR tree entry in menu (imgui)
    ImGui::PushID("VR");
    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (!m_fake_stereo_hook->has_attempted_to_hook_engine() || !m_fake_stereo_hook->has_attempted_to_hook_slate()) {
        std::string adjusted_name = get_name().data();
        adjusted_name += " (Loading...)";

        /*if (!ImGui::CollapsingHeader(adjusted_name.data())) {
            ImGui::PopID();
            return;
        }*/

        ImGui::TextWrapped("Loading...");
    } else {
        /*if (!ImGui::CollapsingHeader(get_name().data())) {
            ImGui::PopID();
            return;
        }*/
    }
    ImGui::PopID();

    auto display_error = [](auto& runtime, std::string dll_name) {
        if (runtime == nullptr || !runtime->error && runtime->loaded) {
            return;
        }

        if (runtime->error && runtime->dll_missing) {
            ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name.data());
            ImGui::TextWrapped("Please select %s from the loader if you want to use %s", runtime->name().data(), runtime->name().data());
        } else if (runtime->error) {
            ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
        } else {
            ImGui::TextWrapped("%s not loaded: Unknown error", runtime->name().data());
        }

        ImGui::Separator();
    };

    if (!get_runtime()->loaded || get_runtime()->error) {
        display_error(m_openxr, "openxr_loader.dll");
        display_error(m_openvr, "openvr_api.dll");
    }

    if (!get_runtime()->loaded) {
        ImGui::TextWrapped("No runtime loaded.");

        if (ImGui::Button("Attempt to reinitialize")) {
            clean_initialize();
        }

        return;
    }

    if (ImGui::Button("Set Standing Height")) {
        m_standing_origin.y = get_position(0).y;
    }

    ImGui::SameLine();

    if (ImGui::Button("Set Standing Origin")) {
        m_standing_origin = get_position(0);
    }

    ImGui::SameLine();

    if (ImGui::Button("Recenter View")) {
        recenter_view();
    }

    ImGui::SameLine();

    if (ImGui::Button("Recenter Horizon")) {
        recenter_horizon();
    }

    if (ImGui::Button("Reinitialize Runtime")) {
        get_runtime()->wants_reinitialize = true;
    }
}

Vector4f VR::get_position(uint32_t index, bool grip) const {
    return get_transform(index, grip)[3];
}

Vector4f VR::get_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_velocity_unsafe(index);
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_angular_velocity_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            const auto pose = m_openvr->get_current_hmd_pose();
            auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
            auto result = glm::rowMajor4(matrix)[3];
            result.w = 1.0f;

            return result;
        }

        if (index == get_left_controller_index()) {
            return m_openvr->grip_matrices[VRRuntime::Hand::LEFT][3];
        }

        if (index == get_right_controller_index()) {
            return m_openvr->grip_matrices[VRRuntime::Hand::RIGHT][3];
        }

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto result = glm::rowMajor4(matrix)[3];
        result.w = 1.0f;

        return result;
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // HMD position
        if (index == 0 && !m_openxr->stage_views.empty()) {
            const auto vspl = m_openxr->get_current_view_space_location();
            return Vector4f{ *(Vector3f*)&vspl.pose.position, 1.0f };
        } else if (index > 0) {
            if (index == get_left_controller_index()) {
                return m_openxr->grip_matrices[VRRuntime::Hand::LEFT][3];
            } else if (index == get_right_controller_index()) {
                return m_openxr->grip_matrices[VRRuntime::Hand::RIGHT][3];
            }
        }

        return Vector4f{};
    } 

    return Vector4f{};
}

Vector4f VR::get_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& velocity = pose.vVelocity;

        return Vector4f{ velocity.v[0], velocity.v[1], velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }

        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].grip_velocity.linearVelocity, 0.0f };
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& angular_velocity = pose.vAngularVelocity;

        return Vector4f{ angular_velocity.v[0], angular_velocity.v[1], angular_velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }
    
        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].grip_velocity.angularVelocity, 0.0f };
    }

    return Vector4f{};
}

Matrix4x4f VR::get_hmd_rotation(uint32_t frame_count) const {
    return glm::extractMatrixRotation(get_hmd_transform(frame_count));
}

Matrix4x4f VR::get_hmd_transform(uint32_t frame_count) const {
    ZoneScopedN(__FUNCTION__);

    if (get_runtime()->is_openvr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };

        const auto pose = m_openvr->get_hmd_pose(frame_count);
        const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock __{ get_runtime()->eyes_mtx };

        const auto vspl = m_openxr->get_view_space_location(frame_count);
        auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(vspl.pose.orientation)};
        mat[3] = Vector4f{*(Vector3f*)&vspl.pose.position, 1.0f};

        return mat;
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_rotation(uint32_t index, bool grip) const {
    return glm::extractMatrixRotation(get_transform(index, grip));
}

Matrix4x4f VR::get_transform(uint32_t index, bool grip) const {
    ZoneScopedN(__FUNCTION__);

    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            const auto pose = m_openvr->get_current_hmd_pose();
            const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
            return glm::rowMajor4(matrix);
        }

        if (index == get_left_controller_index()) {
            return grip ? m_openvr->grip_matrices[VRRuntime::Hand::LEFT] : m_openvr->aim_matrices[VRRuntime::Hand::LEFT];
        } else if (index == get_right_controller_index()) {
            return grip ? m_openvr->grip_matrices[VRRuntime::Hand::RIGHT] : m_openvr->aim_matrices[VRRuntime::Hand::RIGHT];
        }

        const auto& pose = get_openvr_poses()[index];
        const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            const auto vspl = m_openxr->get_current_view_space_location();
            auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(vspl.pose.orientation)};
            mat[3] = Vector4f{*(Vector3f*)&vspl.pose.position, 1.0f};
            return mat;
        } else if (index > 0) {
            if (index == get_left_controller_index()) {
                return grip ? m_openxr->grip_matrices[VRRuntime::Hand::LEFT] : m_openxr->aim_matrices[VRRuntime::Hand::LEFT];
            } else if (index == get_right_controller_index()) {
                return grip ? m_openxr->grip_matrices[VRRuntime::Hand::RIGHT] : m_openxr->aim_matrices[VRRuntime::Hand::RIGHT];
            }
        }
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_grip_transform(uint32_t index) const {
    return get_transform(index);
}

Matrix4x4f VR::get_aim_transform(uint32_t index) const {
    return get_transform(index, false);
}

vr::HmdMatrix34_t VR::get_raw_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return vr::HmdMatrix34_t{};
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            return m_openvr->get_current_hmd_pose();
        }

        auto& pose = get_openvr_poses()[index];
        return pose.mDeviceToAbsoluteTracking;
    } else {
        spdlog::error("VR: get_raw_transform: not implemented for {}", get_runtime()->name());
        return vr::HmdMatrix34_t{};
    }
}

Vector4f VR::get_eye_offset(VRRuntime::Eye eye) const {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (eye == VRRuntime::Eye::LEFT) {
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
}

Vector4f VR::get_current_offset() {
    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (m_frame_count % 2 == m_left_eye_interval) {
        //return Vector4f{m_eye_distance * -1.0f, 0.0f, 0.0f, 0.0f};
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
    //return Vector4f{m_eye_distance, 0.0f, 0.0f, 0.0f};
}

Matrix4x4f VR::get_eye_transform(uint32_t index) {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active() || index > 2) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    return get_runtime()->eyes[index];
}

Matrix4x4f VR::get_current_eye_transform(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (m_frame_count % 2 == mod_count) {
        return get_runtime()->eyes[vr::Eye_Left];
    }

    return get_runtime()->eyes[vr::Eye_Right];
}

Matrix4x4f VR::get_projection_matrix(VRRuntime::Eye eye, bool flip) {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    if ((eye == VRRuntime::Eye::LEFT && !flip) || (eye == VRRuntime::Eye::RIGHT && flip)) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

Matrix4x4f VR::get_current_projection_matrix(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (m_frame_count % 2 == mod_count) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

bool VR::is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded) {
        return false;
    }

    if (action == vr::k_ulInvalidActionHandle) {
        return false;
    }
    
    bool active = false;

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        active = data.bActive && data.bState;
    } else if (get_runtime()->is_openxr()) {
        active = m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }

    return active;
}

Vector2f VR::get_joystick_axis(vr::VRInputValueHandle_t handle) const {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto deadzone = m_joystick_deadzone->value();
        auto out = Vector2f{ data.x, data.y };

        //return glm::length(out) > deadzone ? out : Vector2f{};
        if (glm::abs(out.x) < deadzone) {
            out.x = 0.0f;
        }

        if (glm::abs(out.y) < deadzone) {
            out.y = 0.0f;
        }

        return out;
    } else if (get_runtime()->is_openxr()) {
        // Not using get_left/right_joystick here because it flips the controllers
        if (handle == m_left_joystick) {
            auto out = m_openxr->get_left_stick_axis();
            //return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
            // okay.. instead of that actually clamp x/y to the proper deadzone
            if (glm::abs(out.x) < m_joystick_deadzone->value()) {
                out.x = 0.0f;
            }

            if (glm::abs(out.y) < m_joystick_deadzone->value()) {
                out.y = 0.0f;
            }

            return out;
        } else if (handle == m_right_joystick) {
            auto out = m_openxr->get_right_stick_axis();
            //return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};

            if (glm::abs(out.x) < m_joystick_deadzone->value()) {
                out.x = 0.0f;
            }

            if (glm::abs(out.y) < m_joystick_deadzone->value()) {
                out.y = 0.0f;
            }

            return out;
        }
    }

    return Vector2f{};
}

Vector2f VR::get_left_stick_axis() const {
    return get_joystick_axis(get_left_joystick());
}

Vector2f VR::get_right_stick_axis() const {
    return get_joystick_axis(get_right_joystick());
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source) {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}

float VR::get_standing_height() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin.y;
}

Vector4f VR::get_standing_origin() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin;
}

void VR::set_standing_origin(const Vector4f& origin) {
    ZoneScopedN(__FUNCTION__);

    std::unique_lock _{ get_runtime()->pose_mtx };
    
    m_standing_origin = origin;
}

glm::quat VR::get_rotation_offset() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ m_rotation_mtx };

    return m_rotation_offset;
}

void VR::set_rotation_offset(const glm::quat& offset) {
    ZoneScopedN(__FUNCTION__);

    std::unique_lock _{ m_rotation_mtx };

    m_rotation_offset = offset;
}

void VR::recenter_view() {
    ZoneScopedN(__FUNCTION__);

    const auto new_rotation_offset = glm::normalize(glm::inverse(utility::math::flatten(glm::quat{get_rotation(0)})));

    set_rotation_offset(new_rotation_offset);
}

void VR::recenter_horizon() {
    ZoneScopedN(__FUNCTION__);

    const auto new_rotation_offset = glm::normalize(glm::inverse(glm::quat{get_rotation(0)}));

    set_rotation_offset(new_rotation_offset);
}

void VR::gamepad_snapturn(XINPUT_STATE& state) {
    if (!m_snapturn->value()) {
        return;
    }

    if (!is_hmd_active()) {
        return;
    }

    const auto stick_axis = (float)state.Gamepad.sThumbRX / (float)std::numeric_limits<SHORT>::max();

    if (!m_was_snapturn_run_on_input) {
        if (glm::abs(stick_axis) > m_snapturn_joystick_deadzone->value()) {
            m_snapturn_left = stick_axis < 0.0f;
            m_snapturn_on_frame = true;
            m_was_snapturn_run_on_input = true;
            state.Gamepad.sThumbRX = 0;
        }
    } else {
        if (glm::abs(stick_axis) < m_snapturn_joystick_deadzone->value()) {
            m_was_snapturn_run_on_input = false;
        } else {
            state.Gamepad.sThumbRX = 0;
        }
    }
}

void VR::process_snapturn() {
    if (!m_snapturn_on_frame) {
        return;
    }

    const auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        return;
    }

    const auto world = engine->get_world();

    if (world == nullptr) {
        return;
    }

    if (const auto controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0); controller != nullptr) {
        auto controller_rot = controller->get_control_rotation();
        auto turn_degrees = get_snapturn_angle();
        
        if (m_snapturn_left) {
            turn_degrees = -turn_degrees;
            m_snapturn_left = false;
        }

        controller_rot.y += turn_degrees;
        controller->set_control_rotation(controller_rot);
    }
        
    m_snapturn_on_frame = false;
}

void VR::update_statistics_overlay(sdk::UGameEngine* engine) {
    if (engine == nullptr) {
        return;
    }
    
    if (m_show_fps_state != m_show_fps->value()) {
        engine->exec(L"stat fps");
        m_show_fps_state = m_show_fps->value();
    }
    
    if (m_show_statistics_state != m_show_statistics->value()) {
        engine->exec(L"stat unit");
        m_show_statistics_state = m_show_statistics->value();
    }
}
