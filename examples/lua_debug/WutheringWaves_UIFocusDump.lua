-- Diagnostic-only script: dumps LGUI class functions/properties relevant to
-- gamepad focus/navigation/click handling so we can identify the correct
-- API to hook for native Slate/UMG-style gamepad confirm/back support.
--
-- HOW TO USE:
-- 1. Copy this file into the same scripts folder as WutheringWaves_UIFix.lua
--    (%APPDATA%\UnrealVRMod\Client-Win64-Shipping\scripts\)
-- 2. Launch the game with UEVR attached, let it load into a menu.
-- 3. Open the UEVR Lua console/log output and look for lines prefixed with
--    "[UIFocusDump]".
-- 4. Copy the full dumped output back so we can pick the right hook target.
--
-- This script does not modify any game behavior. It only reads class
-- metadata (functions/properties) and prints it.

local api = uevr.api
local log_functions = uevr.params.functions

local function log(msg)
	print("[UIFocusDump] " .. msg)
	if log_functions and log_functions.log_warn then
		log_functions.log_warn("[UIFocusDump] " .. msg)
	end
end

-- UStruct:get_children() returns a UField* linked list (functions are UFunction,
-- a subclass of UField). Walk it via :get_next() until nil.
local function dump_class_functions(class_name)
	local klass = api:find_uobject(class_name)
	if klass == nil then
		log(string.format("Class not found: %s", class_name))
		return
	end

	log(string.format("---- Functions/fields on %s ----", class_name))

	local ok, err = pcall(function()
		local field = klass:get_children()
		local i = 0
		while field ~= nil do
			i = i + 1
			local ok2, name = pcall(function() return field:get_fname():to_string() end)
			local ok3, cls_name = pcall(function() return field:get_class():get_fname():to_string() end)
			log(string.format("  field[%d]: %s (class: %s)", i, ok2 and name or "?", ok3 and cls_name or "?"))
			field = field:get_next()
		end
		if i == 0 then
			log("  (no children found)")
		end
	end)

	if not ok then
		log(string.format("  (get_children failed: %s)", tostring(err)))
	end
end

-- UStruct:get_child_properties() returns an FField* linked list. Walk via :get_next().
local function dump_class_properties(class_name)
	local klass = api:find_uobject(class_name)
	if klass == nil then
		log(string.format("Class not found: %s", class_name))
		return
	end

	log(string.format("---- Properties on %s ----", class_name))

	local ok, err = pcall(function()
		local prop = klass:get_child_properties()
		local i = 0
		while prop ~= nil do
			i = i + 1
			local ok2, name = pcall(function() return prop:get_fname():to_string() end)
			local ok3, cls_name = pcall(function() return prop:get_class():get_fname():to_string() end)
			log(string.format("  prop[%d]: %s (class: %s)", i, ok2 and name or "?", ok3 and cls_name or "?"))
			prop = prop:get_next()
		end
		if i == 0 then
			log("  (no properties found)")
		end
	end)

	if not ok then
		log(string.format("  (get_child_properties failed: %s)", tostring(err)))
	end
end

local function dump_instances(class_name, max_count)
	local klass = api:find_uobject(class_name)
	if klass == nil then
		log(string.format("Class not found: %s", class_name))
		return
	end

	log(string.format("---- Instances of %s ----", class_name))

	local ok, err = pcall(function()
		local objs = klass:get_objects_matching(false)
		local count = 0
		for i, obj in ipairs(objs) do
			log(string.format("  obj[%d]: %s", i, obj:get_full_name()))
			count = count + 1
			if max_count and count >= max_count then
				break
			end
		end
	end)

	if not ok then
		log(string.format("  (enumeration failed: %s)", tostring(err)))
	end
end

-- For every live instance of the given class (expected: LGUIEventSystem), calls
-- GetCurrentInputModule() and dumps the returned object's class full name (plus its
-- own full name). This is the actual runtime input-module type (e.g. a specific
-- LGUIPointerInputModule/GamepadInputModule subclass) that each event system is
-- currently using, which is the key signal for diffing native-stereo vs SS/2D input.
local function dump_current_input_module_class(class_name)
	local klass = api:find_uobject(class_name)
	if klass == nil then
		log(string.format("Class not found: %s", class_name))
		return
	end

	log(string.format("---- GetCurrentInputModule() on instances of %s ----", class_name))

	local ok, err = pcall(function()
		local objs = klass:get_objects_matching(false)
		for i, obj in ipairs(objs) do
			local ok_name, obj_name = pcall(function() return obj:get_full_name() end)
			local ok_mod, mod = pcall(function() return obj:GetCurrentInputModule() end)
			if ok_mod and mod ~= nil then
				local ok_cls, cls_name = pcall(function() return mod:get_class():get_full_name() end)
				local ok_full, mod_full = pcall(function() return mod:get_full_name() end)
				log(string.format("  sys[%d]=%s input_module_class=%s input_module=%s", i,
					ok_name and obj_name or "?",
					ok_cls and cls_name or "?",
					ok_full and mod_full or "?"))
			else
				log(string.format("  sys[%d]=%s input_module=nil (ok_mod=%s)", i,
					ok_name and obj_name or "?", tostring(ok_mod)))
			end
		end
	end)

	if not ok then
		log(string.format("  (enumeration failed: %s)", tostring(err)))
	end
end

-- CAMERA-REFERENCE PROBE: walks every ObjectProperty on the given class, reads its
-- LIVE value off a real instance, and if that value resolves to a UObject, logs the
-- runtime class of that referenced object plus whether it looks camera-related
-- (CameraComponent/PlayerCameraManager/CameraActor/etc). This is how we find which
-- specific property LGUI's raycast/hit-test math actually reads its camera from,
-- since that's not discoverable from static reflection metadata alone (FProperty's
-- get_property_class() isn't exposed to Lua) - we have to inspect a live value.
local CAMERA_CLASS_HINTS = {
	"CameraComponent", "PlayerCameraManager", "CameraActor", "SceneComponent",
	"CineCameraComponent", "Camera",
}

local function looks_camera_related(cls_full_name)
	if cls_full_name == nil then
		return false
	end
	for _, hint in ipairs(CAMERA_CLASS_HINTS) do
		if cls_full_name:find(hint, 1, true) then
			return true
		end
	end
	return false
end

local function dump_object_property_values(class_name, instance_count)
	local klass = api:find_uobject(class_name)
	if klass == nil then
		log(string.format("Class not found: %s", class_name))
		return
	end

	log(string.format("---- Live ObjectProperty values on instances of %s ----", class_name))

	local ok, err = pcall(function()
		-- Collect property names once from the class's reflection data.
		local prop_names = {}
		local prop = klass:get_child_properties()
		while prop ~= nil do
			local ok2, name = pcall(function() return prop:get_fname():to_string() end)
			local ok3, cls_name = pcall(function() return prop:get_class():get_fname():to_string() end)
			if ok2 and ok3 and (cls_name == "ObjectProperty" or cls_name == "WeakObjectProperty"
				or cls_name == "SoftObjectProperty" or cls_name == "InterfaceProperty") then
				table.insert(prop_names, name)
			end
			prop = prop:get_next()
		end

		if #prop_names == 0 then
			log("  (no ObjectProperty-like fields found via reflection)")
		end

		local objs = klass:get_objects_matching(false)
		local count = 0
		for i, obj in ipairs(objs) do
			count = count + 1
			if instance_count and count > instance_count then
				break
			end

			local ok_name, obj_name = pcall(function() return obj:get_full_name() end)
			log(string.format("  instance[%d]: %s", i, ok_name and obj_name or "?"))

			for _, pname in ipairs(prop_names) do
				local ok_val, val = pcall(function() return obj[pname] end)
				if ok_val and val ~= nil then
					local ok_cls, cls_full = pcall(function() return val:get_class():get_full_name() end)
					local ok_full, val_full = pcall(function() return val:get_full_name() end)
					local is_camera = ok_cls and looks_camera_related(cls_full)
					log(string.format("    prop %s = %s (class: %s)%s", pname,
						ok_full and val_full or "?",
						ok_cls and cls_full or "?",
						is_camera and "  <-- CAMERA-RELATED CANDIDATE" or ""))
				elseif ok_val and val == nil then
					log(string.format("    prop %s = nil", pname))
				else
					log(string.format("    prop %s = (read failed)", pname))
				end
			end
		end
	end)

	if not ok then
		log(string.format("  (dump_object_property_values failed: %s)", tostring(err)))
	end
end

-- Dumps the parameter names/types of a specific UFunction (UFunction is itself
-- a UStruct, so get_child_properties() works the same way as on a class).
local function dump_function_params(class_name, function_name)
	local klass = api:find_uobject(class_name)
	if klass == nil then
		log(string.format("Class not found: %s", class_name))
		return
	end

	local fn = klass:find_function(function_name)
	if fn == nil then
		log(string.format("Function not found: %s::%s", class_name, function_name))
		return
	end

	log(string.format("---- Params on %s::%s ----", class_name, function_name))

	local ok, err = pcall(function()
		local prop = fn:get_child_properties()
		local i = 0
		while prop ~= nil do
			i = i + 1
			local ok2, name = pcall(function() return prop:get_fname():to_string() end)
			local ok3, cls_name = pcall(function() return prop:get_class():get_fname():to_string() end)
			local ok4, is_out = pcall(function() return prop:is_out_param() end)
			local ok5, is_ret = pcall(function() return prop:is_return_param() end)
			log(string.format("  param[%d]: %s (class: %s, out: %s, return: %s)", i,
				ok2 and name or "?", ok3 and cls_name or "?",
				ok4 and tostring(is_out) or "?", ok5 and tostring(is_ret) or "?"))

			-- If this is an enum property, try to dump the enum's valid names/values
			-- so we know what to pass into functions like InputTrigger's mouse-button-type.
			local ok6 = pcall(function()
				if ok3 and cls_name == "EnumProperty" then
					local ok7, enum_obj = pcall(function() return prop:get_enum() end)
					if ok7 and enum_obj ~= nil then
						local ok8, enum_name = pcall(function() return enum_obj:get_fname():to_string() end)
						log(string.format("    enum type: %s", ok8 and enum_name or "?"))
						local ok9, num_enums = pcall(function() return enum_obj:get_max_enum_value() end)
						if ok9 then
							log(string.format("    enum max value: %s", tostring(num_enums)))
						end
					end
				end
			end)
			prop = prop:get_next()
		end
		if i == 0 then
			log("  (no params found)")
		end
	end)

	if not ok then
		log(string.format("  (dump_function_params failed: %s)", tostring(err)))
	end
end

local hasDumped = false

function OnUIFocusDumpTick()
	if hasDumped then
		return
	end

	local local_pawn = api:get_local_pawn(0)
	if local_pawn == nil then
		return
	end

	hasDumped = true

	log("Starting LGUI focus/navigation API dump...")

	-- Known/likely classes involved in UI interaction.
	dump_class_functions("Class /Script/LGUI.UIItem")
	dump_class_functions("Class /Script/LGUI.LGUIBehaviour")
	dump_class_functions("Class /Script/LGUI.LGUIScreenSpaceInteraction")
	dump_class_functions("Class /Script/LGUI.LGUICanvas")

	-- Look for an event-system / navigation-manager style class.
	-- These names are guesses based on common LGUI plugin structure;
	-- if find_uobject fails silently to nil, dump_class_functions will log it.
	dump_class_functions("Class /Script/LGUI.LGUIEventSystem")
	dump_class_functions("Class /Script/LGUI.LGUIBaseRaycaster")
	dump_class_functions("Class /Script/LGUI.LGUIPointerEventData")

	dump_class_properties("Class /Script/LGUI.UIItem")

	dump_instances("Class /Script/LGUI.LGUIScreenSpaceInteraction", 5)

	-- Dump params on the promising interaction-state callback found on LGUIBehaviour.
	dump_function_params("Class /Script/LGUI.LGUIBehaviour", "OnUIInteractionStateChangedBP")

	-- Dump params on the key LGUIEventSystem navigation/selection functions.
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "GetLGUIEventSystemInstance")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "GetCurrentSelectedComponent")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "SetSelectComponent")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "SetSelectComponentWithDefault")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "InputTrigger")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "InputNavigationUp")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "InputNavigationDown")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "IsNavigationActive")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "Navigate")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "GetCurrentInputModule")
	dump_function_params("Class /Script/LGUI.LGUIEventSystem", "ClearEvent")
	dump_function_params("Class /Script/LGUI.UIItem", "GetPositionInViewPort")
	dump_function_params("Class /Script/LGUI.UIItem", "GetUIWorldPosition")

	dump_instances("Class /Script/LGUI.LGUIEventSystem", 5)

	-- Dump the actual runtime GetCurrentInputModule() class for each live
	-- LGUIEventSystem instance, to compare native-stereo vs SS/2D input modules.
	dump_current_input_module_class("Class /Script/LGUI.LGUIEventSystem")

	-- CAMERA-REFERENCE PROBE: for each class that plausibly performs/owns the
	-- raycast/hit-test math, dump the live value of every ObjectProperty-like
	-- field on real instances so we can spot the actual camera reference used
	-- for menu hit-testing (as opposed to the live HMD render pose).
	dump_object_property_values("Class /Script/LGUI.LGUIBaseRaycaster", 5)
	dump_object_property_values("Class /Script/LGUI.LGUIScreenSpaceInteraction", 5)
	dump_object_property_values("Class /Script/LGUI.LGUICanvas", 5)
	dump_object_property_values("Class /Script/LGUI.LGUIEventSystem", 5)
	dump_object_property_values("Class /Script/LGUI.UIItem", 3)

	log("Dump complete.")
end

-- Live hook: log every time OnUIInteractionStateChangedBP fires, along with
-- the object it fired on and (if readable) the interaction-state param.
-- This lets us see, in real time, which widget the game thinks is being
-- interacted with when pressing A/B in a menu.
local InteractionStateHook = nil

local function hook_interaction_state_changed()
	local LGUIBehaviour_c = api:find_uobject("Class /Script/LGUI.LGUIBehaviour")
	if LGUIBehaviour_c == nil then
		log("LGUIBehaviour_c not found, cannot hook interaction state.")
		return
	end

	InteractionStateHook = LGUIBehaviour_c:find_function("OnUIInteractionStateChangedBP")
	if InteractionStateHook == nil then
		log("OnUIInteractionStateChangedBP function not found.")
		return
	end

	InteractionStateHook:set_function_flags(InteractionStateHook:get_function_flags() | 0x400)
	InteractionStateHook:hook_ptr(function(fn, obj, locals, result)
		local ok, obj_name = pcall(function() return obj:get_full_name() end)
		local ok2, class_name = pcall(function() return obj:get_class():get_full_name() end)

		-- Try common param names for the interaction state; log whatever is present.
		local state_str = "?"
		local ok3 = pcall(function()
			if locals.InteractionState ~= nil then
				state_str = tostring(locals.InteractionState)
			elseif locals.NewState ~= nil then
				state_str = tostring(locals.NewState)
			elseif locals.State ~= nil then
				state_str = tostring(locals.State)
			end
		end)

		log(string.format("InteractionStateChanged obj=%s class=%s state=%s",
			ok and obj_name or "?", ok2 and class_name or "?", state_str))

		return false
	end)

	log("Hooked OnUIInteractionStateChangedBP.")
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
	OnUIFocusDumpTick()
end)

hook_interaction_state_changed()

-- Poll the currently selected LGUI component every tick and log whenever it
-- changes, along with A/B button state, so we can correlate gamepad input
-- with what the event system thinks is selected.
local lastSelected = nil
local eventSystemClass = nil

-- Returns a list of {instance, mode} pairs for every LGUIEventSystem object
-- currently alive. There may be more than one (e.g. one per level/world), and
-- the "real" interactive one may not be the first non-default match.
local function get_all_event_systems()
	if eventSystemClass == nil then
		eventSystemClass = api:find_uobject("Class /Script/LGUI.LGUIEventSystem")
	end
	if eventSystemClass == nil then
		return {}
	end

	local results = {}

	local ok, objs = pcall(function()
		return eventSystemClass:get_objects_matching(false)
	end)

	if ok and objs ~= nil then
		for _, obj in ipairs(objs) do
			table.insert(results, { instance = obj, mode = "non-default" })
		end
	end

	if #results == 0 then
		-- Fall back to allowing the CDO in case no runtime instance exists yet.
		local ok2, obj2 = pcall(function()
			return eventSystemClass:get_first_object_matching(true)
		end)

		if ok2 and obj2 ~= nil then
			table.insert(results, { instance = obj2, mode = "default-object-fallback" })
		end
	end

	return results
end

uevr.sdk.callbacks.on_xinput_get_state(function(retval, user_index, state)
	if state == nil then
		return
	end

	local ok, gamepad = pcall(function() return state.Gamepad end)
	if not ok or gamepad == nil then
		return
	end

	local a_pressed = gamepad.wButtons & XINPUT_GAMEPAD_A ~= 0
	local b_pressed = gamepad.wButtons & XINPUT_GAMEPAD_B ~= 0

	if a_pressed or b_pressed then
		local systems = get_all_event_systems()

		if #systems == 0 then
			log(string.format("Button press A=%s B=%s : no LGUIEventSystem instances found",
				tostring(a_pressed), tostring(b_pressed)))
			return
		end

		for idx, entry in ipairs(systems) do
			local sys = entry.instance
			local sel_name = "?"

			local ok2, sel = pcall(function() return sys:GetCurrentSelectedComponent() end)
			if ok2 and sel ~= nil then
				local ok3, n = pcall(function() return sel:get_full_name() end)
				sel_name = ok3 and n or "?"
			else
				sel_name = "nil"
			end

			local ok4, sys_name = pcall(function() return sys:get_full_name() end)

			log(string.format("Button press A=%s B=%s [sys %d/%d %s (%s)] selected=%s",
				tostring(a_pressed), tostring(b_pressed), idx, #systems,
				ok4 and sys_name or "?", entry.mode, sel_name))
		end
	end
end)

log("WutheringWaves_UIFocusDump.lua loaded. Waiting for local pawn to dump LGUI API info...")
