#include "studio_app.h"
#include "asset_browser.h"
#include "audio/audio_scene.h"
#include "editor/asset_compiler.h"
#include "editor/file_system_watcher.h"
#include "editor/gizmo.h"
#include "editor/prefab_system.h"
#include "editor/render_interface.h"
#include "editor/world_editor.h"
#include "engine/command_line_parser.h"
#include "engine/crc32.h"
#include "engine/debug.h"
#include "engine/allocator.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/geometry.h"
#include "engine/input_system.h"
#include "engine/job_system.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/mt/thread.h"
#include "engine/os.h"
#include "engine/path_utils.h"
#include "engine/plugin_manager.h"
#include "engine/profiler.h"
#include "engine/reflection.h"
#include "engine/resource_manager.h"
#include "engine/universe/universe.h"
#include "imgui/imgui.h"
#include "imgui/IconsFontAwesome4.h"
#include "log_ui.h"
#include "profiler_ui.h"
#include "property_grid.h"
#include "settings.h"
#include "utils.h"


namespace Lumix
{


#define NO_ICON "       "


struct LuaPlugin : public StudioApp::GUIPlugin
{
	LuaPlugin(StudioApp& app, const char* src, const char* filename)
		: editor(app.getWorldEditor())
	{
		L = lua_newthread(editor.getEngine().getState());						 // [thread]
		thread_ref = luaL_ref(editor.getEngine().getState(), LUA_REGISTRYINDEX); // []

		lua_newtable(L);						  // [env]
												  // reference environment
		lua_pushvalue(L, -1);					  // [env, env]
		env_ref = luaL_ref(L, LUA_REGISTRYINDEX); // [env]

		// environment's metatable & __index
		lua_pushvalue(L, -1);	// [env, env]
		lua_setmetatable(L, -2); // [env]
		lua_pushvalue(L, LUA_GLOBALSINDEX);
		lua_setfield(L, -2, "__index"); // [env]

		bool errors = luaL_loadbuffer(L, src, stringLength(src), filename) != 0; // [env, func]

		lua_pushvalue(L, -2); // [env, func, env]
		lua_setfenv(L, -2);   // function's environment [env, func]

		errors = errors || lua_pcall(L, 0, 0, 0) != 0; // [env]
		if (errors)
		{
			logError("Editor") << filename << ": " << lua_tostring(L, -1);
			lua_pop(L, 1);
		}
		lua_pop(L, 1); // []

		const char* name = "LuaPlugin";
		lua_getglobal(L, "plugin_name");
		if (lua_type(L, -1) == LUA_TSTRING)
		{
			name = lua_tostring(L, -1);
		}

		Action* action = LUMIX_NEW(editor.getAllocator(), Action)(name, name, name);
		action->func.bind<LuaPlugin, &LuaPlugin::onAction>(this);
		app.addWindowAction(action);
		m_is_open = false;

		lua_pop(L, 1); // plugin_name
	}


	~LuaPlugin()
	{
		lua_State* L = editor.getEngine().getState();
		luaL_unref(L, LUA_REGISTRYINDEX, env_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, thread_ref);
	}


	const char* getName() const override { return "lua_script"; }


	void onAction() { m_is_open = !m_is_open; }


	void onWindowGUI() override
	{
		if (!m_is_open) return;
		lua_getglobal(L, "onGUI");
		if (lua_type(L, -1) == LUA_TFUNCTION)
		{
			if (lua_pcall(L, 0, 0, 0) != 0)
			{
				logError("Editor") << "LuaPlugin:" << lua_tostring(L, -1);
				lua_pop(L, 1);
			}
		}
		else
		{
			lua_pop(L, 1);
		}
	}

	WorldEditor& editor;
	lua_State* L;
	int thread_ref;
	int env_ref;
	bool m_is_open;
};


class StudioAppImpl final : public StudioApp
{
public:
	StudioAppImpl()
		: m_is_entity_list_open(true)
		, m_is_save_as_dialog_open(false)
		, m_finished(false)
		, m_deferred_game_mode_exit(false)
		, m_profiler_ui(nullptr)
		, m_asset_browser(nullptr)
		, m_asset_compiler(nullptr)
		, m_property_grid(nullptr)
		, m_actions(m_allocator)
		, m_window_actions(m_allocator)
		, m_toolbar_actions(m_allocator)
		, m_is_welcome_screen_open(true)
		, m_is_pack_data_dialog_open(false)
		, m_editor(nullptr)
		, m_settings(*this)
		, m_gui_plugins(m_allocator)
		, m_plugins(m_allocator)
		, m_add_cmp_plugins(m_allocator)
		, m_component_labels(m_allocator)
		, m_confirm_load(false)
		, m_confirm_new(false)
		, m_confirm_exit(false)
		, m_exit_code(0)
		, m_allocator(m_main_allocator)
		, m_universes(m_allocator)
		, m_events(m_allocator)
	{
		JobSystem::init(MT::getCPUsCount(), m_allocator);
	}


	void onEvent(const OS::Event& event)
	{
		m_events.push(event);
		switch (event.type) {
			case OS::Event::Type::MOUSE_BUTTON: {
				ImGuiIO& io = ImGui::GetIO();
				m_editor->setToggleSelection(io.KeyCtrl);
				m_editor->setSnapMode(io.KeyShift, io.KeyCtrl);
				io.MouseDown[(int)event.mouse_button.button] = event.mouse_button.down;
				break;
			}
			case OS::Event::Type::MOUSE_WHEEL: {
				ImGuiIO& io = ImGui::GetIO();
				io.MouseWheel = event.mouse_wheel.amount;
				break;
			}
			case OS::Event::Type::MOUSE_MOVE: {
				ImGuiIO& io = ImGui::GetIO();
				const OS::Point cp = OS::getMousePos(event.window);
				m_mouse_move += {(float)event.mouse_move.xrel, (float)event.mouse_move.yrel};
				io.MousePos.x = (float)cp.x;
				io.MousePos.y = (float)cp.y;
				break;
			}
			case OS::Event::Type::WINDOW_SIZE:
				if (event.window == m_window && event.win_size.h > 0 && event.win_size.w > 0) {
					m_settings.m_window.w = event.win_size.w;
					m_settings.m_window.h = event.win_size.h;
					m_settings.m_is_maximized = OS::isMaximized(m_window);
				}
				break;
			case OS::Event::Type::WINDOW_MOVE:
				if (event.window == m_window) {
					m_settings.m_window.x = event.win_move.x;
					m_settings.m_window.y = event.win_move.y;
					m_settings.m_is_maximized = OS::isMaximized(m_window);
				}
				break;
			case OS::Event::Type::WINDOW_CLOSE:
			case OS::Event::Type::QUIT:
				exit();
				break;
			case OS::Event::Type::CHAR: {
				ImGuiIO& io = ImGui::GetIO();
				char utf8[5];
				OS::UTF32ToUTF8(event.text_input.utf32, utf8);
				utf8[4] = 0;
				io.AddInputCharactersUTF8(utf8);
				break;
			}
			case OS::Event::Type::KEY: {
				ImGuiIO& io = ImGui::GetIO();
				io.KeysDown[(int)event.key.keycode] = event.key.down;
				io.KeyShift = OS::isKeyDown(OS::Keycode::SHIFT);
				io.KeyCtrl = OS::isKeyDown(OS::Keycode::CTRL);
				io.KeyAlt = OS::isKeyDown(OS::Keycode::MENU);
				checkShortcuts();
				break;
			}
			case OS::Event::Type::DROP_FILE:
				for(int i = 0, c = OS::getDropFileCount(event); i < c; ++i) {
					char tmp[MAX_PATH_LENGTH];
					OS::getDropFile(event, i, tmp, lengthOf(tmp));
					for (GUIPlugin* plugin : m_gui_plugins) {
						if (plugin->onDropFile(tmp)) break;
					}
				}
				break;
		}
	}


	void onIdle() override
	{
		update();

		if (m_sleep_when_inactive && OS::getFocused() != m_window) {
			const float frame_time = m_fps_timer.tick();
			const float wanted_fps = 5.0f;

			if (frame_time < 1 / wanted_fps) {
				PROFILE_BLOCK("sleep");
				MT::sleep(u32(1000 / wanted_fps - frame_time * 1000));
			}
			m_fps_timer.tick();
		}

		Profiler::frame();
		m_events.clear();
	}


	void run() override
	{
		JobSystem::SignalHandle finished = JobSystem::INVALID_HANDLE;
		JobSystem::runEx(this, [](void* data) {
			Lumix::OS::run(*(StudioAppImpl*)data);
		}, &finished, JobSystem::INVALID_HANDLE, 0);
		Profiler::setThreadName("Main thread");
		JobSystem::wait(finished);
	}


	void onInit() override
	{
		OS::Timer init_timer;

		m_add_cmp_root.label[0] = '\0';
		m_template_name[0] = '\0';
		m_open_filter[0] = '\0';

		checkWorkingDirectory();

		char saved_data_dir[MAX_PATH_LENGTH] = {};
		OS::InputFile cfg_file;
		if (cfg_file.open(".lumixuser")) {
			cfg_file.read(saved_data_dir, minimum(lengthOf(saved_data_dir), (int)cfg_file.size()));
			cfg_file.close();
		}

		char current_dir[MAX_PATH_LENGTH];
		OS::getCurrentDirectory(current_dir, lengthOf(current_dir));

		char data_dir[MAX_PATH_LENGTH] = {};
		checkDataDirCommandLine(data_dir, lengthOf(data_dir));
		m_engine = Engine::create(data_dir[0] ? data_dir : (saved_data_dir[0] ? saved_data_dir : current_dir)
			, m_allocator);
		createLua();

		m_editor = WorldEditor::create(current_dir, *m_engine, m_allocator);
		m_window = m_editor->getWindow();
		m_settings.m_editor = m_editor;
		scanUniverses();
		loadUserPlugins();
		addActions();

		m_asset_compiler = AssetCompiler::create(*this);
		m_asset_browser = LUMIX_NEW(m_allocator, AssetBrowser)(*this);
		m_property_grid = LUMIX_NEW(m_allocator, PropertyGrid)(*this);
		m_profiler_ui = ProfilerUI::create(*m_engine);
		m_log_ui = LUMIX_NEW(m_allocator, LogUI)(m_editor->getAllocator());

		ImGui::CreateContext();
		loadSettings();
		initIMGUI();
#ifdef _WIN32
// TODO
// ImGui::GetPlatformIO().ImeWindowHandle = m_window;
#endif

		m_custom_pivot_action = LUMIX_NEW(m_editor->getAllocator(), Action)("Set Custom Pivot",
			"Set Custom Pivot",
			"set_custom_pivot",
			OS::Keycode::K,
			OS::Keycode::INVALID,
			OS::Keycode::INVALID);
		m_custom_pivot_action->is_global = false;
		addAction(m_custom_pivot_action);

		setStudioApp();
		loadIcons();
		loadSettings();
		loadUniverseFromCommandLine();
		findLuaPlugins("plugins/lua/");

		m_asset_compiler->onInitFinished();
		m_sleep_when_inactive = shouldSleepWhenInactive();

		checkScriptCommandLine();

		logInfo("Editor") << "Startup took " << init_timer.getTimeSinceStart() << " s"; 
	}


	~StudioAppImpl()
	{
		ImGuiIO& io = ImGui::GetIO();
		if (io.WantSaveIniSettings) {
			size_t size;
			const char* data = ImGui::SaveIniSettingsToMemory(&size);
			FileSystem& fs = m_editor->getEngine().getFileSystem();
			OS::OutputFile file;
			if (fs.open("imgui.ini", &file)) {
				file.write(data ,size);
				file.close();
			}
		}

		if (m_watched_plugin.watcher) FileSystemWatcher::destroy(m_watched_plugin.watcher);

		saveSettings();
		unloadIcons();

		while (m_editor->getEngine().getFileSystem().hasWork())
		{
			m_editor->getEngine().getFileSystem().updateAsyncTransactions();
		}

		m_editor->newUniverse();

		destroyAddCmpTreeNode(m_add_cmp_root.child);

		for (auto* i : m_plugins)
		{
			LUMIX_DELETE(m_editor->getAllocator(), i);
		}
		m_plugins.clear();
		PrefabSystem::destroyEditorPlugins(*this);
		ASSERT(m_gui_plugins.empty());

		for (auto* i : m_add_cmp_plugins)
		{
			LUMIX_DELETE(m_editor->getAllocator(), i);
		}
		m_add_cmp_plugins.clear();

		for (auto* a : m_actions)
		{
			LUMIX_DELETE(m_editor->getAllocator(), a);
		}
		m_actions.clear();

		ProfilerUI::destroy(*m_profiler_ui);
		LUMIX_DELETE(m_allocator, m_asset_browser);
		LUMIX_DELETE(m_allocator, m_property_grid);
		LUMIX_DELETE(m_allocator, m_log_ui);
		AssetCompiler::destroy(*m_asset_compiler);
		WorldEditor::destroy(m_editor, m_allocator);
		Engine::destroy(m_engine, m_allocator);
		m_engine = nullptr;
		m_editor = nullptr;
		
		OS::destroyWindow(m_window);
		JobSystem::shutdown();
	}


	bool makeFile(const char* path, const char* content) override
	{
		OS::OutputFile file;
		if (!m_engine->getFileSystem().open(path, &file)) return false;
		file << content;
		file.close();
		return file.isError();
	}


	void destroyAddCmpTreeNode(AddCmpTreeNode* node)
	{
		if (!node) return;
		destroyAddCmpTreeNode(node->child);
		destroyAddCmpTreeNode(node->next);
		LUMIX_DELETE(m_allocator, node);
	}


	const char* getComponentTypeName(ComponentType cmp_type) const override
	{
		auto iter = m_component_labels.find(cmp_type);
		if (iter == m_component_labels.end()) return "Unknown";
		return iter.value().c_str();
	}


	const AddCmpTreeNode& getAddComponentTreeRoot() const override { return m_add_cmp_root; }


	void addPlugin(IAddComponentPlugin& plugin)
	{
		int i = 0;
		while (i < m_add_cmp_plugins.size() && compareString(plugin.getLabel(), m_add_cmp_plugins[i]->getLabel()) > 0)
		{
			++i;
		}
		m_add_cmp_plugins.insert(i, &plugin);

		auto* node = LUMIX_NEW(m_allocator, AddCmpTreeNode);
		copyString(node->label, plugin.getLabel());
		node->plugin = &plugin;
		insertAddCmpNode(m_add_cmp_root, node);
	}


	static void insertAddCmpNodeOrdered(AddCmpTreeNode& parent, AddCmpTreeNode* node)
	{
		if (!parent.child)
		{
			parent.child = node;
			return;
		}
		if (compareString(parent.child->label, node->label) > 0)
		{
			node->next = parent.child;
			parent.child = node;
			return;
		}
		auto* i = parent.child;
		while (i->next && compareString(i->next->label, node->label) < 0)
		{
			i = i->next;
		}
		node->next = i->next;
		i->next = node;
	}


	void insertAddCmpNode(AddCmpTreeNode& parent, AddCmpTreeNode* node)
	{
		for (auto* i = parent.child; i; i = i->next)
		{
			if (!i->plugin && startsWith(node->label, i->label))
			{
				insertAddCmpNode(*i, node);
				return;
			}
		}
		const char* rest = node->label + stringLength(parent.label);
		if (parent.label[0] != '\0') ++rest; // include '/'
		const char* slash = findSubstring(rest, "/");
		if (!slash)
		{
			insertAddCmpNodeOrdered(parent, node);
			return;
		}
		auto* new_group = LUMIX_NEW(m_allocator, AddCmpTreeNode);
		copyNString(new_group->label, (int)sizeof(new_group->label), node->label, int(slash - node->label));
		insertAddCmpNodeOrdered(parent, new_group);
		insertAddCmpNode(*new_group, node);
	}


	void registerComponentWithResource(const char* type,
		const char* label,
		ResourceType resource_type,
		const Reflection::PropertyBase& property) override
	{
		struct Plugin final : public IAddComponentPlugin
		{
			void onGUI(bool create_entity, bool from_filter) override
			{
				ImGui::SetNextWindowSize(ImVec2(300, 300));
				const char* last = reverseFind(label, nullptr, '/');
				if (!ImGui::BeginMenu(last && !from_filter ? last + 1 : label)) return;
				char buf[MAX_PATH_LENGTH];
				bool create_empty = ImGui::MenuItem("Empty");
				if (asset_browser->resourceList(buf, lengthOf(buf), resource_type, 0) || create_empty)
				{
					if (create_entity)
					{
						EntityRef entity = editor->addEntity();
						editor->selectEntities(&entity, 1, false);
					}

					editor->addComponent(type);
					if (!create_empty)
					{
						editor->setProperty(type,
							-1,
							*property,
							&editor->getSelectedEntities()[0],
							editor->getSelectedEntities().size(),
							buf,
							stringLength(buf) + 1);
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}


			const char* getLabel() const override { return label; }

			PropertyGrid* property_grid;
			AssetBrowser* asset_browser;
			WorldEditor* editor;
			ComponentType type;
			ResourceType resource_type;
			const Reflection::PropertyBase* property;
			char label[50];
		};

		auto& allocator = m_editor->getAllocator();
		auto* plugin = LUMIX_NEW(allocator, Plugin);
		plugin->property_grid = m_property_grid;
		plugin->asset_browser = m_asset_browser;
		plugin->type = Reflection::getComponentType(type);
		plugin->editor = m_editor;
		plugin->property = &property;
		plugin->resource_type = resource_type;
		copyString(plugin->label, label);
		addPlugin(*plugin);

		m_component_labels.insert(plugin->type, String(label, m_allocator));
	}


	void registerComponent(const char* id, IAddComponentPlugin& plugin) override
	{
		addPlugin(plugin);
		m_component_labels.insert(Reflection::getComponentType(id), String(plugin.getLabel(), m_allocator));
	}


	void registerComponent(const char* type, const char* label) override
	{
		struct Plugin final : public IAddComponentPlugin
		{
			void onGUI(bool create_entity, bool from_filter) override
			{
				const char* last = reverseFind(label, nullptr, '/');
				if (ImGui::MenuItem(last && !from_filter ? last + 1 : label))
				{
					if (create_entity)
					{
						EntityRef entity = editor->addEntity();
						editor->selectEntities(&entity, 1, false);
					}

					editor->addComponent(type);
				}
			}


			const char* getLabel() const override { return label; }

			WorldEditor* editor;
			PropertyGrid* property_grid;
			ComponentType type;
			char label[64];
		};

		auto& allocator = m_editor->getAllocator();
		auto* plugin = LUMIX_NEW(allocator, Plugin);
		plugin->property_grid = m_property_grid;
		plugin->editor = m_editor;
		plugin->type = Reflection::getComponentType(type);
		copyString(plugin->label, label);
		addPlugin(*plugin);

		m_component_labels.insert(plugin->type, String(label, m_allocator));
	}


	const Array<Action*>& getActions() override { return m_actions; }


	Array<Action*>& getToolbarActions() override { return m_toolbar_actions; }


	void guiBeginFrame() const
	{
		PROFILE_FUNCTION();

		ImGuiIO& io = ImGui::GetIO();
		const OS::Point size = OS::getWindowClientSize(m_window);
		if (size.x > 0 && size.y > 0) {
			io.DisplaySize = ImVec2(float(size.x), float(size.y));
		}
		else if(io.DisplaySize.x <= 0) {
			io.DisplaySize.x = 800;
			io.DisplaySize.y = 600;
		}
		io.DeltaTime = m_engine->getLastTimeDelta();
		io.KeyShift = OS::isKeyDown(OS::Keycode::SHIFT);
		io.KeyCtrl = OS::isKeyDown(OS::Keycode::CTRL);
		io.KeyAlt = OS::isKeyDown(OS::Keycode::MENU);
		ImGui::NewFrame();
		ImGui::PushFont(m_font);
	}


	float showMainToolbar(float menu_height)
	{
		if (m_toolbar_actions.empty())
		{
			ImGui::SetCursorPosY(menu_height);
			return menu_height;
		}

		auto frame_padding = ImGui::GetStyle().FramePadding;
		float padding = frame_padding.y * 2;
		ImVec2 toolbar_size(ImGui::GetIO().DisplaySize.x, 24 + padding);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
		if (ImGui::BeginToolbar("main_toolbar", ImVec2(1, menu_height), toolbar_size))
		{
			for (auto* action : m_toolbar_actions)
			{
				action->toolbarButton();
			}
		}
		ImGui::EndToolbar();
		ImGui::PopStyleVar();
		return menu_height + 24 + padding;
	}


	void guiEndFrame()
	{
		if (m_is_welcome_screen_open)
		{
			ImGuiID dockspace_id = ImGui::GetID("MyDockspace");
			ImGui::DockSpace(dockspace_id, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly);
			showWelcomeScreen();
		}
		else
		{
			if (ImGui::GetIO().DisplaySize.y > 0)
			{
				auto pos = ImVec2(0, 0);
				auto size = ImGui::GetIO().DisplaySize;
				size.y -= pos.y;
				ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
										 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
										 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
				ImGui::SetNextWindowSize(size);
				ImGui::SetNextWindowPos({0, 0}, ImGuiCond_FirstUseEver);
				if (ImGui::Begin("MainDockspace", nullptr, flags))
				{
					float menu_height = showMainMenu();
					showMainToolbar(menu_height);
					ImGuiID dockspace_id = ImGui::GetID("MyDockspace");
					ImGui::Dummy(ImVec2(2, 2));
					ImGui::DockSpace(dockspace_id, ImVec2(0, 0));
				}
				ImGui::End();

				// TODO
				// ImGui::RootDock(pos, size);
			}
			m_profiler_ui->onGUI();
			m_asset_browser->onGUI();
			m_log_ui->onGUI();
			m_property_grid->onGUI();
			onEntityListGUI();
			onEditCameraGUI();
			onSaveAsDialogGUI();
			for (auto* plugin : m_gui_plugins)
			{
				plugin->onWindowGUI();
			}
			m_settings.onGUI();
			onPackDataGUI();
		}
		ImGui::PopFont();
		ImGui::Render();
		for (auto* plugin : m_gui_plugins)
		{
			plugin->guiEndFrame();
		}
	}

	void update()
	{
		PROFILE_FUNCTION();
		Profiler::blockColor(0x7f, 0x7f, 0x7f);
		m_asset_compiler->update();
		if (m_watched_plugin.reload_request) tryReloadPlugin();

		guiBeginFrame();

		float time_delta = m_editor->getEngine().getLastTimeDelta();

		ImGuiIO& io = ImGui::GetIO();
		if (!io.KeyShift)
		{
			m_editor->setSnapMode(false, false);
		}
		else if (io.KeyCtrl)
		{
			m_editor->setSnapMode(io.KeyShift, io.KeyCtrl);
		}
		if (m_custom_pivot_action->isActive())
		{
			m_editor->setCustomPivot();
		}

		m_editor->setMouseSensitivity(m_settings.m_mouse_sensitivity.x, m_settings.m_mouse_sensitivity.y);
		m_editor->update();
		m_engine->update(*m_editor->getUniverse());

		if (m_deferred_game_mode_exit)
		{
			m_deferred_game_mode_exit = false;
			m_editor->toggleGameMode();
		}

		for (auto* plugin : m_gui_plugins)
		{
			plugin->update(time_delta);
		}
		m_asset_browser->update();
		m_log_ui->update(time_delta);

		guiEndFrame();
		m_mouse_move.set(0, 0);
	}


	void showWelcomeScreen()
	{
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
								 ImGuiWindowFlags_NoSavedSettings;
		const OS::Point size = OS::getWindowClientSize(m_window);
		ImGui::SetNextWindowSize(ImVec2((float)size.x, (float)size.y));
		ImGui::SetNextWindowPos({0, 0}, ImGuiCond_FirstUseEver);
		if (ImGui::Begin("Welcome", nullptr, flags))
		{
			ImGui::Text("Welcome to Lumix Studio");

			ImVec2 half_size = ImGui::GetContentRegionAvail();
			half_size.x = half_size.x * 0.5f - ImGui::GetStyle().FramePadding.x;
			half_size.y *= 0.75f;
			auto right_pos = ImGui::GetCursorPos();
			right_pos.x += half_size.x + ImGui::GetStyle().FramePadding.x;
			if (ImGui::BeginChild("left", half_size, true))
			{
				ImGui::Text("Working directory: %s", m_engine->getFileSystem().getBasePath());
				ImGui::SameLine();
				if (ImGui::Button("Change...")) {
					char dir[MAX_PATH_LENGTH];
					if (OS::getOpenDirectory(dir, lengthOf(dir), ".")) {
						OS::OutputFile cfg_file;
						if (cfg_file.open(".lumixuser")) {
							cfg_file << dir;
							cfg_file.close();
						}
						m_engine->getFileSystem().setBasePath(dir);
						scanUniverses();
					}
				}
				ImGui::Separator();
				if (ImGui::Button("New universe")) m_is_welcome_screen_open = false;
				ImGui::Text("Open universe:");
				ImGui::Indent();
				if(m_universes.empty()) {
					ImGui::Text("No universes found");
				}
				for (auto& univ : m_universes)
				{
					if (ImGui::MenuItem(univ.data))
					{
						m_editor->loadUniverse(univ.data);
						setTitle(univ.data);
						m_is_welcome_screen_open = false;
					}
				}
				ImGui::Unindent();
			}
			ImGui::EndChild();

			ImGui::SetCursorPos(right_pos);

			if (ImGui::BeginChild("right", half_size, true))
			{
				ImGui::Text("Using NVidia PhysX");

				if (ImGui::Button("Wiki"))
				{
					OS::shellExecuteOpen("https://github.com/nem0/LumixEngine/wiki");
				}

				if (ImGui::Button("Download new version"))
				{
					OS::shellExecuteOpen(
						"https://github.com/nem0/lumixengine_data/archive/master.zip");
				}

				if (ImGui::Button("Show major releases"))
				{
					OS::shellExecuteOpen("https://github.com/nem0/LumixEngine/releases");
				}

				if (ImGui::Button("Show latest commits"))
				{
					OS::shellExecuteOpen("https://github.com/nem0/LumixEngine/commits/master");
				}

				if (ImGui::Button("Show issues"))
				{
					OS::shellExecuteOpen("https://github.com/nem0/lumixengine/issues");
				}
			}
			ImGui::EndChild();
		}
		ImGui::End();
	}


	void setTitle(const char* title) const
	{
		char tmp[100];
		copyString(tmp, "Lumix Studio - ");
		catString(tmp, title);
		OS::setWindowTitle(m_window, tmp);
	}


	static void getShortcut(const Action& action, char* buf, int max_size)
	{
		buf[0] = 0;
		for (int i = 0; i < lengthOf(action.shortcut); ++i) {
			char tmp[64];
			OS::getKeyName(action.shortcut[i], tmp, sizeof(tmp));
			if (tmp[0] == 0) return;
			if (i > 0) catString(buf, max_size, " - ");
			catString(buf, max_size, tmp);
		}
	}


	void doMenuItem(Action& a, bool enabled) const
	{
		char buf[20];
		getShortcut(a, buf, sizeof(buf));
		if (ImGui::MenuItem(a.label_short, buf, a.is_selected.invoke(), enabled))
		{
			a.func.invoke();
		}
	}


	void save()
	{
		if (m_editor->isGameMode())
		{
			logError("Editor") << "Could not save while the game is running";
			return;
		}

		if (m_editor->getUniverse()->getName()[0])
		{
			m_editor->saveUniverse(m_editor->getUniverse()->getName(), true);
		}
		else
		{
			saveAs();
		}
	}


	void onSaveAsDialogGUI()
	{
		if (!m_is_save_as_dialog_open) return;

		if (ImGui::Begin("Save Universe As", &m_is_save_as_dialog_open))
		{
			static char name[64] = "";
			ImGui::InputText("Name", name, lengthOf(name));
			if (ImGui::Button("Save"))
			{
				m_is_save_as_dialog_open = false;
				setTitle(name);
				m_editor->saveUniverse(name, true);
				scanUniverses();
			}
			ImGui::SameLine();
			if (ImGui::Button("Close")) m_is_save_as_dialog_open = false;
		}
		ImGui::End();
	}


	void saveAs()
	{
		if (m_editor->isGameMode())
		{
			logError("Editor") << "Could not save while the game is running";
			return;
		}

		m_is_save_as_dialog_open = true;
	}


	void exit()
	{
		if (m_editor->isUniverseChanged())
		{
			m_confirm_exit = true;
		}
		else
		{
			OS::quit();
			m_finished = true;
		}
	}


	void newUniverse()
	{
		if (m_editor->isUniverseChanged())
		{
			m_confirm_new = true;
		}
		else
		{
			m_editor->newUniverse();
		}
	}


	GUIPlugin* getFocusedPlugin()
	{
		for (GUIPlugin* plugin : m_gui_plugins)
		{
			if (plugin->hasFocus()) return plugin;
		}
		return nullptr;
	}


	void undo() { m_editor->undo(); }
	void redo() { m_editor->redo(); }
	void copy() { m_editor->copyEntities(); }
	void paste() { m_editor->pasteEntities(); }
	void duplicate() { m_editor->duplicateEntities(); }
	bool isOrbitCamera() { return m_editor->isOrbitCamera(); }
	void toggleOrbitCamera() { m_editor->setOrbitCamera(!m_editor->isOrbitCamera()); }
	void setTopView() { m_editor->setTopView(); }
	void setFrontView() { m_editor->setFrontView(); }
	void setSideView() { m_editor->setSideView(); }
	void setLocalCoordSystem() { m_editor->getGizmo().setLocalCoordSystem(); }
	void setGlobalCoordSystem() { m_editor->getGizmo().setGlobalCoordSystem(); }
	void setPivotOrigin() { m_editor->getGizmo().setPivotOrigin(); }
	void setPivotCenter() { m_editor->getGizmo().setPivotCenter(); }
	void addEntity() { m_editor->addEntity(); }
	void toggleMeasure() { m_editor->toggleMeasure(); }
	void snapDown() { m_editor->snapDown(); }
	void setEditCamTransform() { m_is_edit_cam_transform_ui_open = !m_is_edit_cam_transform_ui_open; }
	void lookAtSelected() { m_editor->lookAtSelected(); }
	void toggleSettings() { m_settings.m_is_open = !m_settings.m_is_open; }
	bool areSettingsOpen() const { return m_settings.m_is_open; }
	void toggleEntityList() { m_is_entity_list_open = !m_is_entity_list_open; }
	bool isEntityListOpen() const { return m_is_entity_list_open; }
	void toggleAssetBrowser() { m_asset_browser->m_is_open = !m_asset_browser->m_is_open; }
	bool isAssetBrowserOpen() const { return m_asset_browser->m_is_open; }
	int getExitCode() const override { return m_exit_code; }
	AssetBrowser& getAssetBrowser() override
	{
		ASSERT(m_asset_browser);
		return *m_asset_browser;
	}
	AssetCompiler& getAssetCompiler() override
	{
		ASSERT(m_asset_compiler);
		return *m_asset_compiler;
	}
	PropertyGrid& getPropertyGrid() override
	{
		ASSERT(m_property_grid);
		return *m_property_grid;
	}
	LogUI& getLogUI() override
	{
		ASSERT(m_log_ui);
		return *m_log_ui;
	}
	void toggleGameMode() { m_editor->toggleGameMode(); }
	void setTranslateGizmoMode() { m_editor->getGizmo().setTranslateMode(); }
	void setRotateGizmoMode() { m_editor->getGizmo().setRotateMode(); }
	void setScaleGizmoMode() { m_editor->getGizmo().setScaleMode(); }


	void makeParent()
	{
		const auto& entities = m_editor->getSelectedEntities();
		ASSERT(entities.size() == 2);
		m_editor->makeParent(entities[0], entities[1]);
	}


	void unparent()
	{
		const auto& entities = m_editor->getSelectedEntities();
		ASSERT(entities.size() == 1);
		m_editor->makeParent(INVALID_ENTITY, entities[0]);
	}


	void savePrefab()
	{
		char filename[MAX_PATH_LENGTH];
		char tmp[MAX_PATH_LENGTH];
		if (OS::getSaveFilename(tmp, lengthOf(tmp), "Prefab files\0*.fab\0", "fab"))
		{
			PathUtils::normalize(tmp, filename, lengthOf(tmp));
			const char* base_path = m_engine->getFileSystem().getBasePath();
			if (startsWith(filename, base_path))
			{
				m_editor->getPrefabSystem().savePrefab(Path(filename + stringLength(base_path)));
			}
			else
			{
				m_editor->getPrefabSystem().savePrefab(Path(filename));
			}
		}
	}


	void autosnapDown()
	{
		auto& gizmo = m_editor->getGizmo();
		gizmo.setAutosnapDown(!gizmo.isAutosnapDown());
	}


	void destroySelectedEntity()
	{
		auto& selected_entities = m_editor->getSelectedEntities();
		if (selected_entities.empty()) return;
		m_editor->destroyEntities(&selected_entities[0], selected_entities.size());
	}


	void removeAction(Action* action) override
	{
		m_actions.eraseItem(action);
		m_window_actions.eraseItem(action);
	}


	void addWindowAction(Action* action) override
	{
		addAction(action);
		for (int i = 0; i < m_window_actions.size(); ++i)
		{
			if (compareString(m_window_actions[i]->label_long, action->label_long) > 0)
			{
				m_window_actions.insert(i, action);
				return;
			}
		}
		m_window_actions.push(action);
	}


	void addAction(Action* action) override
	{
		for (int i = 0; i < m_actions.size(); ++i)
		{
			if (compareString(m_actions[i]->label_long, action->label_long) > 0)
			{
				m_actions.insert(i, action);
				return;
			}
		}
		m_actions.push(action);
	}


	template <void (StudioAppImpl::*Func)()>
	Action& addAction(const char* label_short, const char* label_long, const char* name)
	{
		auto* a = LUMIX_NEW(m_editor->getAllocator(), Action)(label_short, label_long, name);
		a->func.bind<StudioAppImpl, Func>(this);
		addAction(a);
		return *a;
	}


	template <void (StudioAppImpl::*Func)()>
	void addAction(const char* label_short,
		const char* label_long,
		const char* name,
		OS::Keycode shortcut0,
		OS::Keycode shortcut1,
		OS::Keycode shortcut2)
	{
		auto* a =
			LUMIX_NEW(m_editor->getAllocator(), Action)(label_short, label_long, name, shortcut0, shortcut1, shortcut2);
		a->func.bind<StudioAppImpl, Func>(this);
		addAction(a);
	}


	Action* getAction(const char* name) override
	{
		for (auto* a : m_actions)
		{
			if (equalStrings(a->name, name)) return a;
		}
		return nullptr;
	}


	static void showAddComponentNode(const StudioApp::AddCmpTreeNode* node, const char* filter)
	{
		if (!node) return;

		if (filter[0])
		{
			if (!node->plugin)
				showAddComponentNode(node->child, filter);
			else if (stristr(node->plugin->getLabel(), filter))
				node->plugin->onGUI(false, true);
			showAddComponentNode(node->next, filter);
			return;
		}

		if (node->plugin)
		{
			node->plugin->onGUI(true, false);
			showAddComponentNode(node->next, filter);
			return;
		}

		const char* last = reverseFind(node->label, nullptr, '/');
		if (ImGui::BeginMenu(last ? last + 1 : node->label))
		{
			showAddComponentNode(node->child, filter);
			ImGui::EndMenu();
		}
		showAddComponentNode(node->next, filter);
	}


	void onCreateEntityWithComponentGUI()
	{
		doMenuItem(*getAction("createEntity"), true);
		ImGui::Separator();
		ImGui::LabellessInputText("Filter", m_component_filter, sizeof(m_component_filter));
		showAddComponentNode(m_add_cmp_root.child, m_component_filter);
	}


	void entityMenu()
	{
		if (!ImGui::BeginMenu("Entity")) return;

		const auto& selected_entities = m_editor->getSelectedEntities();
		bool is_any_entity_selected = !selected_entities.empty();
		if (ImGui::BeginMenu(ICON_FA_PLUS_SQUARE_O "Create"))
		{
			onCreateEntityWithComponentGUI();
			ImGui::EndMenu();
		}
		doMenuItem(*getAction("destroyEntity"), is_any_entity_selected);
		doMenuItem(*getAction("savePrefab"), selected_entities.size() == 1);
		doMenuItem(*getAction("makeParent"), selected_entities.size() == 2);
		bool can_unparent =
			selected_entities.size() == 1 && m_editor->getUniverse()->getParent(selected_entities[0]).isValid();
		doMenuItem(*getAction("unparent"), can_unparent);
		ImGui::EndMenu();
	}


	void editMenu()
	{
		if (!ImGui::BeginMenu("Edit")) return;

		bool is_any_entity_selected = !m_editor->getSelectedEntities().empty();
		doMenuItem(*getAction("undo"), m_editor->canUndo());
		doMenuItem(*getAction("redo"), m_editor->canRedo());
		ImGui::Separator();
		doMenuItem(*getAction("copy"), is_any_entity_selected);
		doMenuItem(*getAction("paste"), m_editor->canPasteEntities());
		doMenuItem(*getAction("duplicate"), is_any_entity_selected);
		ImGui::Separator();
		doMenuItem(*getAction("orbitCamera"), is_any_entity_selected || m_editor->isOrbitCamera());
		doMenuItem(*getAction("setTranslateGizmoMode"), true);
		doMenuItem(*getAction("setRotateGizmoMode"), true);
		doMenuItem(*getAction("setScaleGizmoMode"), true);
		doMenuItem(*getAction("setPivotCenter"), true);
		doMenuItem(*getAction("setPivotOrigin"), true);
		doMenuItem(*getAction("setLocalCoordSystem"), true);
		doMenuItem(*getAction("setGlobalCoordSystem"), true);
		if (ImGui::BeginMenu(ICON_FA_CAMERA "View", true))
		{
			doMenuItem(*getAction("viewTop"), true);
			doMenuItem(*getAction("viewFront"), true);
			doMenuItem(*getAction("viewSide"), true);
			ImGui::EndMenu();
		}
		ImGui::EndMenu();
	}


	void fileMenu()
	{
		if (!ImGui::BeginMenu("File")) return;

		doMenuItem(*getAction("newUniverse"), true);
		if (ImGui::BeginMenu(NO_ICON "Open"))
		{
			ImGui::LabellessInputText("Filter", m_open_filter, sizeof(m_open_filter));
			for (auto& univ : m_universes)
			{
				if ((m_open_filter[0] == '\0' || stristr(univ.data, m_open_filter)) && ImGui::MenuItem(univ.data))
				{
					if (m_editor->isUniverseChanged())
					{
						copyString(m_universe_to_load, univ.data);
						m_confirm_load = true;
					}
					else
					{
						m_editor->loadUniverse(univ.data);
						setTitle(univ.data);
					}
				}
			}
			ImGui::EndMenu();
		}
		doMenuItem(*getAction("save"), !m_editor->isGameMode());
		doMenuItem(*getAction("saveAs"), !m_editor->isGameMode());
		doMenuItem(*getAction("exit"), true);
		ImGui::EndMenu();
	}


	void toolsMenu()
	{
		if (!ImGui::BeginMenu("Tools")) return;

		bool is_any_entity_selected = !m_editor->getSelectedEntities().empty();
		doMenuItem(*getAction("setEditCamTransform"), true);
		doMenuItem(*getAction("lookAtSelected"), is_any_entity_selected);
		doMenuItem(*getAction("toggleGameMode"), true);
		doMenuItem(*getAction("toggleMeasure"), true);
		doMenuItem(*getAction("snapDown"), is_any_entity_selected);
		doMenuItem(*getAction("autosnapDown"), true);
		doMenuItem(*getAction("pack_data"), true);
		ImGui::EndMenu();
	}


	void viewMenu()
	{
		if (!ImGui::BeginMenu("View")) return;

		ImGui::MenuItem(ICON_FA_CUBES "Asset browser", nullptr, &m_asset_browser->m_is_open);
		doMenuItem(*getAction("entityList"), true);
		ImGui::MenuItem(ICON_FA_RSS "Log", nullptr, &m_log_ui->m_is_open);
		ImGui::MenuItem(ICON_FA_AREA_CHART "Profiler", nullptr, &m_profiler_ui->m_is_open);
		ImGui::MenuItem(ICON_FA_SLIDERS "Properties", nullptr, &m_property_grid->m_is_open);
		doMenuItem(*getAction("settings"), true);
		ImGui::Separator();
		for (Action* action : m_window_actions)
		{
			doMenuItem(*action, true);
		}
		ImGui::EndMenu();
	}


	float showMainMenu()
	{
		if (m_confirm_exit)
		{
			ImGui::OpenPopup("confirm_exit");
			m_confirm_exit = false;
		}
		if (ImGui::BeginPopupModal("confirm_exit"))
		{
			ImGui::Text("All unsaved changes will be lost, do you want to continue?");
			if (ImGui::Button("Continue"))
			{
				OS::quit();
				m_finished = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		if (m_confirm_new)
		{
			ImGui::OpenPopup("confirm_new");
			m_confirm_new = false;
		}
		if (ImGui::BeginPopupModal("confirm_new"))
		{
			ImGui::Text("All unsaved changes will be lost, do you want to continue?");
			if (ImGui::Button("Continue"))
			{
				m_editor->newUniverse();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (m_confirm_load)
		{
			ImGui::OpenPopup("Confirm");
			m_confirm_load = false;
		}
		if (ImGui::BeginPopupModal("Confirm"))
		{
			ImGui::Text("All unsaved changes will be lost, do you want to continue?");
			if (ImGui::Button("Continue"))
			{
				m_editor->loadUniverse(m_universe_to_load);
				setTitle(m_universe_to_load);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		float menu_height = 0;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
		if (ImGui::BeginMainMenuBar())
		{
			fileMenu();
			editMenu();
			entityMenu();
			toolsMenu();
			viewMenu();

			StaticString<200> stats("");
			if (m_engine->getFileSystem().hasWork()) stats << "Loading... | ";
			stats << "FPS: ";
			stats << m_engine->getFPS();
			if (OS::getFocused() != m_window) stats << " - inactive window";
			auto stats_size = ImGui::CalcTextSize(stats);
			ImGui::SameLine(ImGui::GetContentRegionMax().x - stats_size.x);
			ImGui::Text("%s", (const char*)stats);

			if (m_log_ui->getUnreadErrorCount() == 1)
			{
				ImGui::SameLine(ImGui::GetContentRegionMax().x - stats_size.x);
				auto error_stats_size = ImGui::CalcTextSize("1 error | ");
				ImGui::SameLine(ImGui::GetContentRegionMax().x - stats_size.x - error_stats_size.x);
				ImGui::TextColored(ImVec4(1, 0, 0, 1), "1 error | ");
			}
			else if (m_log_ui->getUnreadErrorCount() > 1)
			{
				StaticString<50> error_stats("", m_log_ui->getUnreadErrorCount(), " errors | ");
				ImGui::SameLine(ImGui::GetContentRegionMax().x - stats_size.x);
				auto error_stats_size = ImGui::CalcTextSize(error_stats);
				ImGui::SameLine(ImGui::GetContentRegionMax().x - stats_size.x - error_stats_size.x);
				ImGui::TextColored(ImVec4(1, 0, 0, 1), "%s", (const char*)error_stats);
			}
			menu_height = ImGui::GetWindowSize().y;
			ImGui::EndMainMenuBar();
		}
		ImGui::PopStyleVar();
		return menu_height;
	}


	void showHierarchy(EntityRef entity, const Array<EntityRef>& selected_entities)
	{
		char buffer[1024];
		Universe* universe = m_editor->getUniverse();
		getEntityListDisplayName(*m_editor, buffer, sizeof(buffer), entity);
		bool selected = selected_entities.indexOf(entity) >= 0;
		ImGui::PushID(entity.index);
		ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_AllowItemOverlap;
		bool has_child = universe->getFirstChild(entity).isValid();
		if (!has_child) flags = ImGuiTreeNodeFlags_Leaf;
		if (selected) flags |= ImGuiTreeNodeFlags_Selected;
		bool node_open = ImGui::TreeNodeEx(buffer, flags);
		if (ImGui::IsItemClicked(0)) m_editor->selectEntities(&entity, 1, true);
		if (ImGui::IsMouseReleased(1) && ImGui::IsItemHovered()) ImGui::OpenPopup("entity_context_menu");
		if (ImGui::BeginPopup("entity_context_menu"))
		{
			if (ImGui::MenuItem("Create child"))
			{
				m_editor->beginCommandGroup(crc32("create_child_entity"));
				EntityRef child = m_editor->addEntity();
				m_editor->makeParent(entity, child);
				const DVec3 pos = m_editor->getUniverse()->getPosition(entity);
				m_editor->setEntitiesPositions(&child, &pos, 1);
				m_editor->endCommandGroup();
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
		if (ImGui::BeginDragDropSource())
		{
			ImGui::Text("%s", buffer);
			ImGui::SetDragDropPayload("entity", &entity, sizeof(entity));
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget())
		{
			if (auto* payload = ImGui::AcceptDragDropPayload("entity"))
			{
				EntityRef dropped_entity = *(EntityRef*)payload->Data;
				if (dropped_entity != entity)
				{
					m_editor->makeParent(entity, dropped_entity);
					ImGui::EndDragDropTarget();
					if (node_open) ImGui::TreePop();
					return;
				}
			}

			ImGui::EndDragDropTarget();
		}

		if (node_open)
		{
			for (EntityPtr e_ptr = universe->getFirstChild(entity); e_ptr.isValid();
				 e_ptr = universe->getNextSibling((EntityRef)e_ptr))
			{
				showHierarchy((EntityRef)e_ptr, selected_entities);
			}
			ImGui::TreePop();
		}
	}


	void onEditCameraGUI()
	{
		if (!m_is_edit_cam_transform_ui_open) return;
		if (ImGui::Begin("Edit camera")) {
			Viewport vp = m_editor->getViewport();
			if (ImGui::DragScalarN("Position", ImGuiDataType_Double, &vp.pos.x, 3, 1.f)) {
				m_editor->setViewport(vp);
			}
			Vec3 angles = vp.rot.toEuler();
			if (ImGui::DragFloat3("Rotation", &angles.x, 0.01f)) {
				vp.rot.fromEuler(angles);
				m_editor->setViewport(vp);
			}
		}
		ImGui::End();
	}


	void onEntityListGUI()
	{
		PROFILE_FUNCTION();
		const Array<EntityRef>& entities = m_editor->getSelectedEntities();
		static char filter[64] = "";
		if (!m_is_entity_list_open) return;
		if (ImGui::Begin("Entity List", &m_is_entity_list_open))
		{
			auto* universe = m_editor->getUniverse();
			ImGui::LabellessInputText("Filter", filter, sizeof(filter));

			if (ImGui::BeginChild("entities"))
			{
				ImGui::PushItemWidth(ImGui::GetContentRegionAvailWidth() - ImGui::GetStyle().FramePadding.x);
				if (filter[0] == '\0')
				{
					for (EntityPtr e = universe->getFirstEntity(); e.isValid();
						 e = universe->getNextEntity((EntityRef)e))
					{
						const EntityRef e_ref = (EntityRef)e;
						if (!universe->getParent(e_ref).isValid())
						{
							showHierarchy(e_ref, entities);
						}
					}
				}
				else
				{
					for (EntityPtr e = universe->getFirstEntity(); e.isValid();
						 e = universe->getNextEntity((EntityRef)e))
					{
						char buffer[1024];
						getEntityListDisplayName(*m_editor, buffer, sizeof(buffer), e);
						if (stristr(buffer, filter) == nullptr) continue;
						ImGui::PushID(e.index);
						const EntityRef e_ref = (EntityRef)e;
						bool selected = entities.indexOf(e_ref) >= 0;
						if (ImGui::Selectable(buffer, &selected))
						{
							m_editor->selectEntities(&e_ref, 1, true);
						}
						if (ImGui::BeginDragDropSource())
						{
							ImGui::Text("%s", buffer);
							ImGui::SetDragDropPayload("entity", &e, sizeof(e));
							ImGui::EndDragDropSource();
						}
						ImGui::PopID();
					}
				}
				ImGui::PopItemWidth();
			}
			ImGui::EndChild();
			if (ImGui::BeginDragDropTarget())
			{
				if (auto* payload = ImGui::AcceptDragDropPayload("entity"))
				{
					EntityRef dropped_entity = *(EntityRef*)payload->Data;
					m_editor->makeParent(INVALID_ENTITY, dropped_entity);
				}
				ImGui::EndDragDropTarget();
			}
		}
		ImGui::End();
	}


	void dummy() {}


	void setFullscreen(bool fullscreen) override
	{
		ASSERT(false); // TODO
					   // SDL_SetWindowFullscreen(m_window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	}


	void saveSettings()
	{
		m_settings.m_is_asset_browser_open = m_asset_browser->m_is_open;
		m_settings.m_asset_browser_left_column_width = m_asset_browser->m_left_column_width;
		m_settings.m_is_entity_list_open = m_is_entity_list_open;
		m_settings.m_is_log_open = m_log_ui->m_is_open;
		m_settings.m_is_profiler_open = m_profiler_ui->m_is_open;
		m_settings.m_is_properties_open = m_property_grid->m_is_open;
		m_settings.m_mouse_sensitivity.x = m_editor->getMouseSensitivity().x;
		m_settings.m_mouse_sensitivity.y = m_editor->getMouseSensitivity().y;

		for (auto* i : m_gui_plugins) {
			i->onBeforeSettingsSaved();
		}

		m_settings.save();
	}


	ImFont* addFontFromFile(const char* path, float size, bool merge_icons) {
		FileSystem& fs = m_editor->getEngine().getFileSystem();
		Array<u8> data(m_allocator);
		if (!fs.getContentSync(Path(path), &data)) return nullptr;
		ImGuiIO& io = ImGui::GetIO();
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		auto font = io.Fonts->AddFontFromMemoryTTF(data.begin(), data.byte_size(), size, &cfg);
		if(merge_icons) {
			ImFontConfig config;
			config.MergeMode = true;
			config.FontDataOwnedByAtlas = false;
			config.GlyphMinAdvanceX = 20.0f; // Use if you want to make the icon monospaced
			static const ImWchar icon_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
			Array<u8> icons_data(m_allocator);
			if (fs.getContentSync(Path("editor/fonts/fontawesome-webfont.ttf"), &icons_data)) {
				ImFont* icons_font = io.Fonts->AddFontFromMemoryTTF(icons_data.begin(), icons_data.byte_size(), size * 0.75f, &config, icon_ranges);
				ASSERT(icons_font);
			}
		}

		return font;
	}


	void initIMGUI()
	{
		logInfo("Editor") << "Initializing imgui...";
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
		io.IniFilename = nullptr;
		const int dpi = OS::getDPI();
		float font_scale = dpi / 96.f;
		FileSystem& fs = getWorldEditor().getEngine().getFileSystem();
		
		Array<u8> ini_data(m_allocator);
		if (fs.getContentSync(Path("imgui.ini"), &ini_data)) {
			ImGui::LoadIniSettingsFromMemory((const char*)ini_data.begin(), ini_data.size());
		}

		m_font = addFontFromFile("editor/fonts/opensans-regular.ttf", (float)m_settings.m_font_size * font_scale, true);
		m_bold_font = addFontFromFile("editor/fonts/opensans-regular.ttf", (float)m_settings.m_font_size * font_scale, true);

		if (m_font) {
			m_font->DisplayOffset.y = 0;
			m_bold_font->DisplayOffset.y = 0;
		}
		else {
			OS::messageBox(
				"Could not open editor/fonts/opensans-regular.ttf\n"
				"It very likely means that data are not bundled with\n"
				"the exe and the exe is not in the correct directory.\n"
				"The program will eventually crash!"
			);
		}

		io.KeyMap[ImGuiKey_Space] = (int)OS::Keycode::SPACE;
		io.KeyMap[ImGuiKey_Tab] = (int)OS::Keycode::TAB;
		io.KeyMap[ImGuiKey_LeftArrow] = (int)OS::Keycode::LEFT;
		io.KeyMap[ImGuiKey_RightArrow] = (int)OS::Keycode::RIGHT;
		io.KeyMap[ImGuiKey_UpArrow] = (int)OS::Keycode::UP;
		io.KeyMap[ImGuiKey_DownArrow] = (int)OS::Keycode::DOWN;
		io.KeyMap[ImGuiKey_PageUp] = (int)OS::Keycode::PAGEUP;
		io.KeyMap[ImGuiKey_PageDown] = (int)OS::Keycode::PAGEDOWN;
		io.KeyMap[ImGuiKey_Home] = (int)OS::Keycode::HOME;
		io.KeyMap[ImGuiKey_End] = (int)OS::Keycode::END;
		io.KeyMap[ImGuiKey_Delete] = (int)OS::Keycode::DEL;
		io.KeyMap[ImGuiKey_Backspace] = (int)OS::Keycode::BACKSPACE;
		io.KeyMap[ImGuiKey_Enter] = (int)OS::Keycode::RETURN;
		io.KeyMap[ImGuiKey_Escape] = (int)OS::Keycode::ESCAPE;
		io.KeyMap[ImGuiKey_A] = (int)OS::Keycode::A;
		io.KeyMap[ImGuiKey_C] = (int)OS::Keycode::C;
		io.KeyMap[ImGuiKey_V] = (int)OS::Keycode::V;
		io.KeyMap[ImGuiKey_X] = (int)OS::Keycode::X;
		io.KeyMap[ImGuiKey_Y] = (int)OS::Keycode::Y;
		io.KeyMap[ImGuiKey_Z] = (int)OS::Keycode::Z;

		ImGuiStyle& style = ImGui::GetStyle();
		style.FramePadding.y = 0;
		style.ItemSpacing.y = 2;
		style.ItemInnerSpacing.x = 2;
	}


	Settings& getSettings() override
	{
		return m_settings;
	}


	void loadSettings()
	{
		logInfo("Editor") << "Loading settings...";
		char cmd_line[2048];
		OS::getCommandLine(cmd_line, lengthOf(cmd_line));

		CommandLineParser parser(cmd_line);
		while (parser.next())
		{
			if (!parser.currentEquals("-no_crash_report")) continue;

			m_settings.m_force_no_crash_report = true;
			break;
		}

		m_settings.load();
		for (auto* i : m_gui_plugins) {
			i->onSettingsLoaded();
		}

		m_asset_browser->m_is_open = m_settings.m_is_asset_browser_open;
		m_asset_browser->m_left_column_width = m_settings.m_asset_browser_left_column_width;
		m_is_entity_list_open = m_settings.m_is_entity_list_open;
		m_log_ui->m_is_open = m_settings.m_is_log_open;
		m_profiler_ui->m_is_open = m_settings.m_is_profiler_open;
		m_property_grid->m_is_open = m_settings.m_is_properties_open;

		if (m_settings.m_is_maximized)
		{
			OS::maximizeWindow(m_window);
		}
		else if (m_settings.m_window.w > 0)
		{
			OS::Rect r;
			r.left = m_settings.m_window.x;
			r.top = m_settings.m_window.y;
			r.width = m_settings.m_window.w;
			r.height = m_settings.m_window.h;
			OS::setWindowScreenRect(m_window, r);
		}
	}


	void addActions()
	{
		addAction<&StudioAppImpl::newUniverse>(ICON_FA_PLUS "New", "New universe", "newUniverse");
		addAction<&StudioAppImpl::save>(
			ICON_FA_FLOPPY_O "Save", "Save universe", "save", OS::Keycode::LCTRL, OS::Keycode::S, OS::Keycode::INVALID);
		addAction<&StudioAppImpl::saveAs>(
			NO_ICON "Save As", "Save universe as", "saveAs", OS::Keycode::LCTRL, OS::Keycode::LSHIFT, OS::Keycode::S);
		addAction<&StudioAppImpl::exit>(
			ICON_FA_SIGN_OUT "Exit", "Exit Studio", "exit", OS::Keycode::LCTRL, OS::Keycode::X, OS::Keycode::INVALID);
		addAction<&StudioAppImpl::redo>(
			ICON_FA_REPEAT "Redo", "Redo scene action", "redo", OS::Keycode::LCTRL, OS::Keycode::LSHIFT, OS::Keycode::Z);
		addAction<&StudioAppImpl::undo>(
			ICON_FA_UNDO "Undo", "Undo scene action", "undo", OS::Keycode::LCTRL, OS::Keycode::Z, OS::Keycode::INVALID);
		addAction<&StudioAppImpl::copy>(
			ICON_FA_CLIPBOARD "Copy", "Copy entity", "copy", OS::Keycode::LCTRL, OS::Keycode::C, OS::Keycode::INVALID);
		addAction<&StudioAppImpl::paste>(
			NO_ICON "Paste", "Paste entity", "paste", OS::Keycode::LCTRL, OS::Keycode::V, OS::Keycode::INVALID);
		addAction<&StudioAppImpl::duplicate>(
			ICON_FA_FILES_O "Duplicate", "Duplicate entity", "duplicate", OS::Keycode::LCTRL, OS::Keycode::D, OS::Keycode::INVALID);
		addAction<&StudioAppImpl::toggleOrbitCamera>(NO_ICON "Orbit camera", "Orbit camera", "orbitCamera")
			.is_selected.bind<StudioAppImpl, &StudioAppImpl::isOrbitCamera>(this);
		addAction<&StudioAppImpl::setTranslateGizmoMode>(ICON_FA_ARROWS "Translate", "Set translate mode", "setTranslateGizmoMode")
			.is_selected.bind<Gizmo, &Gizmo::isTranslateMode>(&m_editor->getGizmo());
		addAction<&StudioAppImpl::setRotateGizmoMode>(ICON_FA_REPEAT "Rotate", "Set rotate mode", "setRotateGizmoMode")
			.is_selected.bind<Gizmo, &Gizmo::isRotateMode>(&m_editor->getGizmo());
		addAction<&StudioAppImpl::setScaleGizmoMode>(NO_ICON "Scale", "Set scale mode", "setScaleGizmoMode")
			.is_selected.bind<Gizmo, &Gizmo::isScaleMode>(&m_editor->getGizmo());
		addAction<&StudioAppImpl::setTopView>(NO_ICON "Top", "Set top camera view", "viewTop");
		addAction<&StudioAppImpl::setFrontView>(NO_ICON "Front", "Set front camera view", "viewFront");
		addAction<&StudioAppImpl::setSideView>(NO_ICON "Side", "Set side camera view", "viewSide");
		addAction<&StudioAppImpl::setLocalCoordSystem>(NO_ICON "Local", "Set local transform system", "setLocalCoordSystem")
			.is_selected.bind<Gizmo, &Gizmo::isLocalCoordSystem>(&m_editor->getGizmo());
		addAction<&StudioAppImpl::setGlobalCoordSystem>(ICON_FA_GLOBE "Global", "Set global transform system", "setGlobalCoordSystem")
			.is_selected.bind<Gizmo, &Gizmo::isGlobalCoordSystem>(&m_editor->getGizmo());
		addAction<&StudioAppImpl::setPivotCenter>(ICON_FA_ALIGN_CENTER "Center", "Set center transform system", "setPivotCenter")
			.is_selected.bind<Gizmo, &Gizmo::isPivotCenter>(&m_editor->getGizmo());
		addAction<&StudioAppImpl::setPivotOrigin>(NO_ICON "Pivot", "Set pivot transform system", "setPivotOrigin")
			.is_selected.bind<Gizmo, &Gizmo::isPivotOrigin>(&m_editor->getGizmo());

		addAction<&StudioAppImpl::addEntity>(ICON_FA_PLUS_SQUARE_O "Create empty", "Create empty entity", "createEntity");
		addAction<&StudioAppImpl::destroySelectedEntity>(ICON_FA_MINUS_SQUARE_O "Destroy",
			"Destroy entity",
			"destroyEntity",
			OS::Keycode::DEL,
			OS::Keycode::INVALID,
			OS::Keycode::INVALID);
		addAction<&StudioAppImpl::savePrefab>(ICON_FA_FLOPPY_O "Save prefab", "Save selected entities as prefab", "savePrefab");
		addAction<&StudioAppImpl::makeParent>(ICON_FA_OBJECT_GROUP "Make parent", "Make entity parent", "makeParent");
		addAction<&StudioAppImpl::unparent>(ICON_FA_OBJECT_UNGROUP "Unparent", "Unparent entity", "unparent");

		addAction<&StudioAppImpl::toggleGameMode>(ICON_FA_PLAY "Game Mode", "Toggle game mode", "toggleGameMode")
			.is_selected.bind<WorldEditor, &WorldEditor::isGameMode>(m_editor);
		addAction<&StudioAppImpl::toggleMeasure>(NO_ICON "Toggle measure", "Toggle measure mode", "toggleMeasure")
			.is_selected.bind<WorldEditor, &WorldEditor::isMeasureToolActive>(m_editor);
		addAction<&StudioAppImpl::autosnapDown>(NO_ICON "Autosnap down", "Toggle autosnap down", "autosnapDown")
			.is_selected.bind<Gizmo, &Gizmo::isAutosnapDown>(&m_editor->getGizmo());
		addAction<&StudioAppImpl::snapDown>(NO_ICON "Snap down", "Snap entities down", "snapDown");
		addAction<&StudioAppImpl::setEditCamTransform>(NO_ICON "Camera transform", "Set camera transformation", "setEditCamTransform");
		addAction<&StudioAppImpl::lookAtSelected>(NO_ICON "Look at selected", "Look at selected entity", "lookAtSelected");
		addAction<&StudioAppImpl::toggleAssetBrowser>(ICON_FA_CUBES "Asset Browser", "Toggle asset browser", "assetBrowser")
			.is_selected.bind<StudioAppImpl, &StudioAppImpl::isAssetBrowserOpen>(this);
		addAction<&StudioAppImpl::toggleEntityList>(ICON_FA_LIST "Entity List", "Toggle entity list", "entityList")
			.is_selected.bind<StudioAppImpl, &StudioAppImpl::isEntityListOpen>(this);
		addAction<&StudioAppImpl::toggleSettings>(ICON_FA_COGS "Settings", "Toggle settings UI", "settings")
			.is_selected.bind<StudioAppImpl, &StudioAppImpl::areSettingsOpen>(this);
		addAction<&StudioAppImpl::showPackDataDialog>(ICON_FA_FILE_ARCHIVE_O "Pack data", "Pack data", "pack_data");
	}


	static bool copyPlugin(const char* src, int iteration, char (&out)[MAX_PATH_LENGTH])
	{
		char tmp_path[MAX_PATH_LENGTH];
		OS::getExecutablePath(tmp_path, lengthOf(tmp_path));
		StaticString<MAX_PATH_LENGTH> copy_path;
		PathUtils::getDir(copy_path.data, lengthOf(copy_path.data), tmp_path);
		copy_path << "plugins/" << iteration;
		OS::makePath(copy_path);
		PathUtils::getBasename(tmp_path, lengthOf(tmp_path), src);
		copy_path << "/" << tmp_path << "." << getPluginExtension();
#ifdef _WIN32
		StaticString<MAX_PATH_LENGTH> src_pdb(src);
		StaticString<MAX_PATH_LENGTH> dest_pdb(copy_path);
		if (PathUtils::replaceExtension(dest_pdb.data, "pdb") && PathUtils::replaceExtension(src_pdb.data, "pda"))
		{
			OS::deleteFile(dest_pdb);
			if (!OS::copyFile(src_pdb, dest_pdb))
			{
				copyString(out, src);
				return false;
			}
		}
#endif

		OS::deleteFile(copy_path);
		if (!OS::copyFile(src, copy_path))
		{
			copyString(out, src);
			return false;
		}
		copyString(out, copy_path);
		return true;
	}


	void loadUserPlugins()
	{
		char cmd_line[2048];
		OS::getCommandLine(cmd_line, lengthOf(cmd_line));

		CommandLineParser parser(cmd_line);
		auto& plugin_manager = m_editor->getEngine().getPluginManager();
		while (parser.next())
		{
			if (!parser.currentEquals("-plugin")) continue;
			if (!parser.next()) break;

			char src[MAX_PATH_LENGTH];
			parser.getCurrent(src, lengthOf(src));

			bool is_full_path = findSubstring(src, ".") != nullptr;
			Lumix::IPlugin* loaded_plugin;
			if (is_full_path)
			{
				char copy_path[MAX_PATH_LENGTH];
				copyPlugin(src, 0, copy_path);
				loaded_plugin = plugin_manager.load(copy_path);
			}
			else
			{
				loaded_plugin = plugin_manager.load(src);
			}

			if (!loaded_plugin)
			{
				logError("Editor") << "Could not load plugin " << src << " requested by command line";
			}
			else if (is_full_path && !m_watched_plugin.watcher)
			{
				char dir[MAX_PATH_LENGTH];
				char basename[MAX_PATH_LENGTH];
				PathUtils::getBasename(basename, lengthOf(basename), src);
				m_watched_plugin.basename = basename;
				PathUtils::getDir(dir, lengthOf(dir), src);
				m_watched_plugin.watcher = FileSystemWatcher::create(dir, m_allocator);
				m_watched_plugin.watcher->getCallback().bind<StudioAppImpl, &StudioAppImpl::onPluginChanged>(this);
				m_watched_plugin.dir = dir;
				m_watched_plugin.plugin = loaded_plugin;
			}
		}
	}


	static const char* getPluginExtension()
	{
		const char* ext =
#ifdef _WIN32
			"dll";
#elif defined __linux__
			"so";
#else
#error Unknown platform
#endif
		return ext;
	}


	void onPluginChanged(const char* path)
	{
		const char* ext = getPluginExtension();
		if (!PathUtils::hasExtension(path, ext)
#ifdef _WIN32
			&& !PathUtils::hasExtension(path, "pda")
#endif
		)
			return;

		char basename[MAX_PATH_LENGTH];
		PathUtils::getBasename(basename, lengthOf(basename), path);
		if (!equalIStrings(basename, m_watched_plugin.basename)) return;

		m_watched_plugin.reload_request = true;
	}


	void tryReloadPlugin()
	{
		m_watched_plugin.reload_request = false;

		StaticString<MAX_PATH_LENGTH> src(m_watched_plugin.dir, m_watched_plugin.basename, ".", getPluginExtension());
		char copy_path[MAX_PATH_LENGTH];
		++m_watched_plugin.iteration;

		if (!copyPlugin(src, m_watched_plugin.iteration, copy_path)) return;

		logInfo("Editor") << "Trying to reload plugin " << m_watched_plugin.basename;

		OutputMemoryStream blob(m_allocator);
		blob.reserve(16 * 1024);
		PluginManager& plugin_manager = m_editor->getEngine().getPluginManager();
		void* lib = plugin_manager.getLibrary(m_watched_plugin.plugin);

		Universe* universe = m_editor->getUniverse();
		for (IScene* scene : universe->getScenes())
		{
			if (&scene->getPlugin() != m_watched_plugin.plugin) continue;
			if (m_editor->isGameMode()) scene->stopGame();
			scene->serialize(blob);
			universe->removeScene(scene);
			scene->getPlugin().destroyScene(scene);
		}
		plugin_manager.unload(m_watched_plugin.plugin);

		// TODO try to delete the old version

		m_watched_plugin.plugin = plugin_manager.load(copy_path);
		if (!m_watched_plugin.plugin)
		{
			logError("Editor") << "Failed to load plugin " << copy_path << ". Reload failed.";
			return;
		}

		InputMemoryStream input_blob(blob);
		m_watched_plugin.plugin->createScenes(*universe);
		for (IScene* scene : universe->getScenes())
		{
			if (&scene->getPlugin() != m_watched_plugin.plugin) continue;
			scene->deserialize(input_blob);
			if (m_editor->isGameMode()) scene->startGame();
		}
		logInfo("Editor") << "Finished reloading plugin.";
	}


	bool shouldSleepWhenInactive()
	{
		char cmd_line[2048];
		OS::getCommandLine(cmd_line, lengthOf(cmd_line));

		CommandLineParser parser(cmd_line);
		while (parser.next())
		{
			if (parser.currentEquals("-no_sleep_inactive")) return false;
		}
		return true;
	}


	void loadUniverseFromCommandLine()
	{
		char cmd_line[2048];
		char path[MAX_PATH_LENGTH];
		OS::getCommandLine(cmd_line, lengthOf(cmd_line));

		CommandLineParser parser(cmd_line);
		while (parser.next())
		{
			if (!parser.currentEquals("-open")) continue;
			if (!parser.next()) break;

			parser.getCurrent(path, lengthOf(path));
			m_editor->loadUniverse(path);
			setTitle(path);
			m_is_welcome_screen_open = false;
			break;
		}
	}

	static void checkDataDirCommandLine(char* dir, int max_size)
	{
		char cmd_line[2048];
		OS::getCommandLine(cmd_line, lengthOf(cmd_line));

		CommandLineParser parser(cmd_line);
		while (parser.next())
		{
			if (!parser.currentEquals("-data_dir")) continue;
			if (!parser.next()) break;

			parser.getCurrent(dir, max_size);
			break;
		}
	}


	GUIPlugin* getPlugin(const char* name) override
	{
		for (auto* i : m_gui_plugins)
		{
			if (equalStrings(i->getName(), name)) return i;
		}
		return nullptr;
	}


	void initPlugins() override
	{
		for (int i = 1, c = m_plugins.size(); i < c; ++i)
		{
			for (int j = 0; j < i; ++j)
			{
				IPlugin* p = m_plugins[i];
				if (m_plugins[j]->dependsOn(*p))
				{
					m_plugins.erase(i);
					--i;
					m_plugins.insert(j, p);
				}
			}
		}

		for (IPlugin* plugin : m_plugins)
		{
			plugin->init();
		}
	}


	void addPlugin(IPlugin& plugin) override { m_plugins.push(&plugin); }


	void addPlugin(GUIPlugin& plugin) override
	{
		m_gui_plugins.push(&plugin);
		for (auto* i : m_gui_plugins)
		{
			i->pluginAdded(plugin);
			plugin.pluginAdded(*i);
		}
	}


	void removePlugin(GUIPlugin& plugin) override { m_gui_plugins.eraseItemFast(&plugin); }


	void setStudioApp()
	{
#ifdef STATIC_PLUGINS
		StudioApp::StaticPluginRegister::create(*this);
#else
		auto& plugin_manager = m_editor->getEngine().getPluginManager();
		for (auto* lib : plugin_manager.getLibraries())
		{
			auto* f = (StudioApp::IPlugin * (*)(StudioApp&)) getLibrarySymbol(lib, "setStudioApp");
			if (f)
			{
				StudioApp::IPlugin* plugin = f(*this);
				addPlugin(*plugin);
			}
		}
#endif
		PrefabSystem::createEditorPlugins(*this, m_editor->getPrefabSystem());
	}


	void runScript(const char* src, const char* script_name) override
	{
		lua_State* L = m_engine->getState();
		bool errors = luaL_loadbuffer(L, src, stringLength(src), script_name) != 0;
		errors = errors || lua_pcall(L, 0, 0, 0) != 0;
		if (errors)
		{
			logError("Editor") << script_name << ": " << lua_tostring(L, -1);
			lua_pop(L, 1);
		}
	}


	void savePrefabAs(const char* path) { m_editor->getPrefabSystem().savePrefab(Path(path)); }


	void destroyEntity(EntityRef e) { m_editor->destroyEntities(&e, 1); }


	void selectEntity(EntityRef e) { m_editor->selectEntities(&e, 1, false); }


	EntityRef createEntity() { return m_editor->addEntity(); }

	void createComponent(EntityRef e, int type)
	{
		m_editor->selectEntities(&e, 1, false);
		m_editor->addComponent({type});
	}

	void exitGameMode() { m_deferred_game_mode_exit = true; }


	void exitWithCode(int exit_code)
	{
		OS::quit();
		m_finished = true;
		m_exit_code = exit_code;
	}


	struct SetPropertyVisitor : public Reflection::IPropertyVisitor
	{
		void visit(const Reflection::Property<int>& prop) override
		{
			if (!equalStrings(property_name, prop.name)) return;
			if (!lua_isnumber(L, -1)) return;

			int val = (int)lua_tointeger(L, -1);
			editor->setProperty(cmp_type, 0, prop, &entity, 1, &val, sizeof(val));
		}


		// TODO
		void visit(const Reflection::Property<float>& prop) override { notSupported(prop); }
		void visit(const Reflection::Property<EntityPtr>& prop) override { notSupported(prop); }
		void visit(const Reflection::Property<IVec2>& prop) override { notSupported(prop); }
		void visit(const Reflection::Property<Vec2>& prop) override { notSupported(prop); }
		void visit(const Reflection::Property<Vec3>& prop) override { notSupported(prop); }
		void visit(const Reflection::Property<Vec4>& prop) override { notSupported(prop); }


		void visit(const Reflection::Property<const char*>& prop) override
		{
			if (!equalStrings(property_name, prop.name)) return;
			if (!lua_isstring(L, -1)) return;

			const char* str = lua_tostring(L, -1);
			editor->setProperty(cmp_type, 0, prop, &entity, 1, str, stringLength(str) + 1);
		}


		void visit(const Reflection::Property<Path>& prop) override
		{
			if (!equalStrings(property_name, prop.name)) return;
			if (!lua_isstring(L, -1)) return;

			const char* str = lua_tostring(L, -1);
			editor->setProperty(cmp_type, 0, prop, &entity, 1, str, stringLength(str) + 1);
		}


		void visit(const Reflection::Property<bool>& prop) override
		{
			if (!equalStrings(property_name, prop.name)) return;
			if (!lua_isboolean(L, -1)) return;

			bool val = lua_toboolean(L, -1) != 0;
			editor->setProperty(cmp_type, 0, prop, &entity, 1, &val, sizeof(val));
		}

		void visit(const Reflection::IArrayProperty& prop) override { notSupported(prop); }
		void visit(const Reflection::IEnumProperty& prop) override { notSupported(prop); }
		void visit(const Reflection::IBlobProperty& prop) override { notSupported(prop); }
		void visit(const Reflection::ISampledFuncProperty& prop) override { notSupported(prop); }


		void notSupported(const Reflection::PropertyBase& prop)
		{
			if (!equalStrings(property_name, prop.name)) return;
			logError("Lua Script") << "Property " << prop.name << " has unsupported type";
		}


		lua_State* L;
		EntityRef entity;
		ComponentType cmp_type;
		const char* property_name;
		WorldEditor* editor;
	};


	static int createEntityEx(lua_State* L)
	{
		auto* studio = LuaWrapper::checkArg<StudioAppImpl*>(L, 1);
		LuaWrapper::checkTableArg(L, 2);

		WorldEditor& editor = *studio->m_editor;
		editor.beginCommandGroup(crc32("createEntityEx"));
		EntityRef e = editor.addEntity();
		editor.selectEntities(&e, 1, false);

		lua_pushvalue(L, 2);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0)
		{
			const char* parameter_name = luaL_checkstring(L, -2);
			if (equalStrings(parameter_name, "position"))
			{
				const DVec3 pos = LuaWrapper::toType<DVec3>(L, -1);
				editor.setEntitiesPositions(&e, &pos, 1);
			}
			else if (equalStrings(parameter_name, "rotation"))
			{
				const Quat rot = LuaWrapper::toType<Quat>(L, -1);
				editor.setEntitiesRotations(&e, &rot, 1);
			}
			else
			{
				ComponentType cmp_type = Reflection::getComponentType(parameter_name);
				editor.addComponent(cmp_type);

				IScene* scene = editor.getUniverse()->getScene(cmp_type);
				if (scene)
				{
					ComponentUID cmp(e, cmp_type, scene);
					const Reflection::ComponentBase* cmp_des = Reflection::getComponent(cmp_type);
					if (cmp.isValid())
					{
						lua_pushvalue(L, -1);
						lua_pushnil(L);
						while (lua_next(L, -2) != 0)
						{
							const char* property_name = luaL_checkstring(L, -2);
							SetPropertyVisitor v;
							v.property_name = property_name;
							v.entity = (EntityRef)cmp.entity;
							v.cmp_type = cmp.type;
							v.L = L;
							v.editor = &editor;
							cmp_des->visit(v);

							lua_pop(L, 1);
						}
						lua_pop(L, 1);
					}
				}
			}
			lua_pop(L, 1);
		}
		lua_pop(L, 1);

		editor.endCommandGroup();
		LuaWrapper::push(L, e);
		return 1;
	}


	static int getResources(lua_State* L)
	{
		auto* studio = LuaWrapper::checkArg<StudioAppImpl*>(L, 1);
		auto* type = LuaWrapper::checkArg<const char*>(L, 2);

		AssetCompiler& compiler = studio->getAssetCompiler();
		if (ResourceType(type) == INVALID_RESOURCE_TYPE) return 0;
		auto& resources_paths = compiler.lockResources(ResourceType(type));

		lua_createtable(L, resources_paths.size(), 0);
		int i = 0;
		for (auto& path : resources_paths)
		{
			LuaWrapper::push(L, path.c_str());
			lua_rawseti(L, -2, i + 1);
			++i;
		}

		compiler.unlockResources();
		return 1;
	}


	void saveUniverseAs(const char* basename, bool save_path) { m_editor->saveUniverse(basename, save_path); }


	void saveUniverse() { save(); }


	void createLua()
	{
		lua_State* L = m_engine->getState();

		LuaWrapper::createSystemVariable(L, "Editor", "editor", this);

#define REGISTER_FUNCTION(F)                                                                                    \
	do                                                                                                          \
	{                                                                                                           \
		auto f = &LuaWrapper::wrapMethodClosure<StudioAppImpl, decltype(&StudioAppImpl::F), &StudioAppImpl::F>; \
		LuaWrapper::createSystemClosure(L, "Editor", this, #F, f);                                              \
	} while (false)


		REGISTER_FUNCTION(savePrefabAs);
		REGISTER_FUNCTION(selectEntity);
		REGISTER_FUNCTION(createEntity);
		REGISTER_FUNCTION(createComponent);
		REGISTER_FUNCTION(destroyEntity);
		REGISTER_FUNCTION(newUniverse);
		REGISTER_FUNCTION(saveUniverse);
		REGISTER_FUNCTION(saveUniverseAs);
		REGISTER_FUNCTION(exitWithCode);
		REGISTER_FUNCTION(exitGameMode);

#undef REGISTER_FUNCTION

		LuaWrapper::createSystemFunction(L, "Editor", "getResources", &getResources);
		LuaWrapper::createSystemFunction(L, "Editor", "createEntityEx", &createEntityEx);
	}


	void checkScriptCommandLine()
	{
		char command_line[1024];
		OS::getCommandLine(command_line, lengthOf(command_line));
		CommandLineParser parser(command_line);
		while (parser.next())
		{
			if (parser.currentEquals("-run_script"))
			{
				if (!parser.next()) break;
				char tmp[MAX_PATH_LENGTH];
				parser.getCurrent(tmp, lengthOf(tmp));
				OS::InputFile file;
				if (m_engine->getFileSystem().open(tmp, &file))
				{
					auto size = file.size();
					auto* src = (char*)m_allocator.allocate(size + 1);
					file.read(src, size);
					src[size] = 0;
					runScript((const char*)src, tmp);
					m_allocator.deallocate(src);
					file.close();
				}
				else
				{
					logError("Editor") << "Could not open " << tmp;
				}
				break;
			}
		}
	}


	static bool includeFileInPack(const char* filename)
	{
		if (filename[0] == '.') return false;
		if (compareStringN("bin/", filename, 4) == 0) return false;
		if (compareStringN("bin32/", filename, 4) == 0) return false;
		if (equalStrings("data.pak", filename)) return false;
		if (equalStrings("error.log", filename)) return false;
		return true;
	}


	static bool includeDirInPack(const char* filename)
	{
		if (filename[0] == '.') return false;
		if (compareStringN("bin", filename, 4) == 0) return false;
		if (compareStringN("bin32", filename, 4) == 0) return false;
		return true;
	}


#pragma pack(1)
	struct PackFileInfo
	{
		u32 hash;
		u64 offset;
		u64 size;

		char path[MAX_PATH_LENGTH];
	};
#pragma pack()


	void packDataScan(const char* dir_path, AssociativeArray<u32, PackFileInfo>& infos)
	{
		auto* iter = m_engine->getFileSystem().createFileIterator(dir_path);
		OS::FileInfo info;
		while (OS::getNextFile(iter, &info))
		{
			char normalized_path[MAX_PATH_LENGTH];
			PathUtils::normalize(info.filename, normalized_path, lengthOf(normalized_path));
			if (info.is_directory)
			{
				if (!includeDirInPack(normalized_path)) continue;

				char dir[MAX_PATH_LENGTH] = {0};
				if (dir_path[0] != '.') copyString(dir, dir_path);
				catString(dir, info.filename);
				catString(dir, "/");
				packDataScan(dir, infos);
				continue;
			}

			if (!includeFileInPack(normalized_path)) continue;

			StaticString<MAX_PATH_LENGTH> out_path;
			if (dir_path[0] == '.')
			{
				copyString(out_path.data, normalized_path);
			}
			else
			{
				copyString(out_path.data, dir_path);
				catString(out_path.data, normalized_path);
			}
			u32 hash = crc32(out_path.data);
			if (infos.find(hash) >= 0) continue;

			auto& out_info = infos.emplace(hash);
			copyString(out_info.path, out_path);
			out_info.hash = hash;
			out_info.size = OS::getFileSize(out_path.data);
			out_info.offset = ~0UL;
		}
		OS::destroyFileIterator(iter);
	}


	void packDataScanResources(AssociativeArray<u32, PackFileInfo>& infos)
	{
		ResourceManagerHub& rm = m_editor->getEngine().getResourceManager();
		for (auto iter = rm.getAll().begin(), end = rm.getAll().end(); iter != end; ++iter)
		{
			const auto& resources = iter.value()->getResourceTable();
			for (Resource* res : resources)
			{
				u32 hash = crc32(res->getPath().c_str());
				auto& out_info = infos.emplace(hash);
				copyString(out_info.path, MAX_PATH_LENGTH, res->getPath().c_str());
				out_info.hash = hash;
				out_info.size = OS::getFileSize(res->getPath().c_str());
				out_info.offset = ~0UL;
			}
		}
		packDataScan("pipelines/", infos);
		StaticString<MAX_PATH_LENGTH> unv_path;
		unv_path << "universes/" << m_editor->getUniverse()->getName() << "/";
		packDataScan(unv_path, infos);
		unv_path.data[0] = 0;
		unv_path << "universes/" << m_editor->getUniverse()->getName() << ".unv";
		u32 hash = crc32(unv_path);
		auto& out_info = infos.emplace(hash);
		copyString(out_info.path, MAX_PATH_LENGTH, unv_path);
		out_info.hash = hash;
		out_info.size = OS::getFileSize(unv_path);
		out_info.offset = ~0UL;
	}


	void showPackDataDialog() { m_is_pack_data_dialog_open = true; }


	void onPackDataGUI()
	{
		if (!m_is_pack_data_dialog_open) return;
		if (ImGui::Begin("Pack data", &m_is_pack_data_dialog_open))
		{
			ImGui::LabelText("Destination dir", "%s", m_pack.dest_dir.data);
			ImGui::SameLine();
			if (ImGui::Button("Choose dir"))
			{
				if (OS::getOpenDirectory(m_pack.dest_dir.data, lengthOf(m_pack.dest_dir.data), "."))
				{
					m_pack.dest_dir << "/";
				}
			}

			ImGui::Combo("Mode", (int*)&m_pack.mode, "All files\0Loaded universe\0");

			if (ImGui::Button("Pack")) packData();
		}
		ImGui::End();
	}


	void packData()
	{
		if (m_pack.dest_dir.empty()) return;

		char dest[MAX_PATH_LENGTH];

		static const char* OUT_FILENAME = "data.pak";
		copyString(dest, m_pack.dest_dir);
		catString(dest, OUT_FILENAME);
		AssociativeArray<u32, PackFileInfo> infos(m_allocator);
		infos.reserve(10000);

		switch (m_pack.mode)
		{
			case PackConfig::Mode::ALL_FILES: packDataScan("./", infos); break;
			case PackConfig::Mode::CURRENT_UNIVERSE: packDataScanResources(infos); break;
			default: ASSERT(false); break;
		}

		if (infos.size() == 0)
		{
			logError("Editor") << "No files found while trying to create " << dest;
			return;
		}

		OS::OutputFile file;
		if (!file.open(dest))
		{
			logError("Editor") << "Could not create " << dest;
			return;
		}

		int count = infos.size();
		file.write(&count, sizeof(count));
		u64 offset = sizeof(count) + (sizeof(u32) + sizeof(u64) * 2) * count;
		for (auto& info : infos)
		{
			info.offset = offset;
			offset += info.size;
		}

		for (auto& info : infos)
		{
			file.write(&info.hash, sizeof(info.hash));
			file.write(&info.offset, sizeof(info.offset));
			file.write(&info.size, sizeof(info.size));
		}

		for (auto& info : infos)
		{
			OS::InputFile src;
			size_t src_size = OS::getFileSize(info.path);
			if (!m_editor->getEngine().getFileSystem().open(info.path, &src))
			{
				file.close();
				logError("Editor") << "Could not open " << info.path;
				return;
			}
			u8 buf[4096];
			for (; src_size > 0; src_size -= minimum(sizeof(buf), src_size))
			{
				size_t batch_size = minimum(sizeof(buf), src_size);
				if (!src.read(buf, batch_size))
				{
					file.close();
					logError("Editor") << "Could not read " << info.path;
					return;
				}
				file.write(buf, batch_size);
			}
			src.close();
		}

		file.close();

		const char* bin_files[] = {"app.exe", "dbghelp.dll", "dbgcore.dll"};
		StaticString<MAX_PATH_LENGTH> src_dir("bin/");
		if (!OS::fileExists("bin/app.exe"))
		{
			char tmp[MAX_PATH_LENGTH];
			OS::getExecutablePath(tmp, lengthOf(tmp));
			PathUtils::getDir(src_dir.data, lengthOf(src_dir.data), tmp);
		}
		for (auto& file : bin_files)
		{
			StaticString<MAX_PATH_LENGTH> tmp(m_pack.dest_dir, file);
			StaticString<MAX_PATH_LENGTH> src(src_dir, file);
			if (!OS::copyFile(src, tmp))
			{
				logError("Editor") << "Failed to copy " << src << " to " << tmp;
			}
		}

		for (GUIPlugin* plugin : m_gui_plugins)
		{
			if (!plugin->packData(m_pack.dest_dir))
			{
				logError("Editor") << "Plugin " << plugin->getName() << " failed to pack data.";
			}
		}
	}


	void loadLuaPlugin(const char* dir, const char* filename)
	{
		StaticString<MAX_PATH_LENGTH> path(dir, filename);
		OS::InputFile file;

		if (m_engine->getFileSystem().open(path, &file))
		{
			const int size = (int)file.size();
			Array<u8> src(m_engine->getAllocator());
			src.resize(size + 1);
			file.read(src.begin(), size);
			src[size] = 0;

			LuaPlugin* plugin =
				LUMIX_NEW(m_editor->getAllocator(), LuaPlugin)(*this, (const char*)src.begin(), filename);
			addPlugin(*plugin);

			file.close();
		}
		else
		{
			logWarning("Editor") << "Failed to open " << path;
		}
	}


	void scanUniverses()
	{
		m_universes.clear();
		auto* iter = m_engine->getFileSystem().createFileIterator("universes");
		OS::FileInfo info;
		while (OS::getNextFile(iter, &info))
		{
			if (info.filename[0] == '.') continue;
			if (!info.is_directory) continue;
			if (startsWith(info.filename, "__")) continue;

			char basename[MAX_PATH_LENGTH];
			PathUtils::getBasename(basename, lengthOf(basename), info.filename);
			m_universes.emplace(basename);
		}
		OS::destroyFileIterator(iter);
	}


	void findLuaPlugins(const char* dir)
	{
		auto* iter = m_engine->getFileSystem().createFileIterator(dir);
		OS::FileInfo info;
		while (OS::getNextFile(iter, &info))
		{
			char normalized_path[MAX_PATH_LENGTH];
			PathUtils::normalize(info.filename, normalized_path, lengthOf(normalized_path));
			if (normalized_path[0] == '.') continue;
			if (info.is_directory)
			{
				char dir_path[MAX_PATH_LENGTH] = {0};
				if (dir[0] != '.') copyString(dir_path, dir);
				catString(dir_path, info.filename);
				catString(dir_path, "/");
				findLuaPlugins(dir_path);
			}
			else
			{
				char ext[5];
				PathUtils::getExtension(ext, lengthOf(ext), info.filename);
				if (equalStrings(ext, "lua"))
				{
					loadLuaPlugin(dir, info.filename);
				}
			}
		}
		OS::destroyFileIterator(iter);
	}


	const OS::Event* getEvents() const override { return m_events.empty() ? nullptr : &m_events[0]; }


	int getEventsCount() const override { return m_events.size(); }


	Vec2 getMouseMove() const override { return m_mouse_move; }


	static void checkWorkingDirectory()
	{
		if (!OS::fileExists("../LumixStudio.lnk")) return;

		if (!OS::dirExists("bin") && OS::dirExists("../bin") &&
			OS::dirExists("../pipelines"))
		{
			OS::setCurrentDirectory("../");
		}

		if (!OS::dirExists("bin"))
		{
			OS::messageBox("Bin directory not found, please check working directory.");
		}
		else if (!OS::dirExists("pipelines"))
		{
			OS::messageBox("Pipelines directory not found, please check working directory.");
		}
	}


	void unloadIcons()
	{
		auto& render_interface = *m_editor->getRenderInterface();
		for (auto* action : m_actions)
		{
			render_interface.unloadTexture(action->icon);
		}
	}


	void loadIcons()
	{
		logInfo("Editor") << "Loading icons...";
		RenderInterface& render_interface = *m_editor->getRenderInterface();
		FileSystem& fs = m_engine->getFileSystem();
		for (auto* action : m_actions)
		{
			char tmp[MAX_PATH_LENGTH];
			action->getIconPath(tmp, lengthOf(tmp));
			if (fs.fileExists(tmp))
			{
				action->icon = render_interface.loadTexture(Path(tmp));
			}
			else
			{
				action->icon = nullptr;
			}
		}
	}


	void checkShortcuts()
	{
		if (ImGui::IsAnyItemActive()) return;
		GUIPlugin* plugin = getFocusedPlugin();
		u32 pressed_modifiers = 0;
		if (OS::isKeyDown(OS::Keycode::SHIFT)) pressed_modifiers |= 1;
		if (OS::isKeyDown(OS::Keycode::CTRL)) pressed_modifiers |= 2;
		if (OS::isKeyDown(OS::Keycode::MENU)) pressed_modifiers |= 4;

		for (Action* a : m_actions) {
			if (!a->is_global || a->shortcut[0] == OS::Keycode::INVALID) continue;
			if (a->plugin != plugin) continue;

			u32 action_modifiers = 0;
			for (int i = 0; i < lengthOf(a->shortcut) + 1; ++i)
			{
				if ((i == lengthOf(a->shortcut) || a->shortcut[i] == OS::Keycode::INVALID) &&
					action_modifiers == pressed_modifiers)
				{
					a->func.invoke();
					return;
				}

				if (i == lengthOf(a->shortcut)) break;
				if (a->shortcut[i] == OS::Keycode::INVALID) break;

				if (!OS::isKeyDown(a->shortcut[i])) break;
				switch (a->shortcut[i]) {
					case OS::Keycode::LSHIFT:
					case OS::Keycode::RSHIFT:
					case OS::Keycode::SHIFT:
						action_modifiers |= 1;
						break;
					case OS::Keycode::CTRL:
					case OS::Keycode::LCTRL:
					case OS::Keycode::RCTRL:
						action_modifiers |= 2; 
						break;
					case OS::Keycode::MENU:
						action_modifiers |= 4;
						break;
				}
			}
		}
	}


	void* getWindow() override { return m_window; }


	WorldEditor& getWorldEditor() override
	{
		ASSERT(m_editor);
		return *m_editor;
	}


	ImFont* getBoldFont() override { return m_bold_font; }


	DefaultAllocator m_main_allocator;
	Debug::Allocator m_allocator;
	Engine* m_engine;
	OS::WindowHandle m_window;
	Array<Action*> m_actions;
	Array<Action*> m_window_actions;
	Array<Action*> m_toolbar_actions;
	Array<GUIPlugin*> m_gui_plugins;
	Array<IPlugin*> m_plugins;
	Array<IAddComponentPlugin*> m_add_cmp_plugins;
	Array<StaticString<MAX_PATH_LENGTH>> m_universes;
	AddCmpTreeNode m_add_cmp_root;
	HashMap<ComponentType, String> m_component_labels;
	WorldEditor* m_editor;
	Action* m_custom_pivot_action;
	bool m_confirm_exit;
	bool m_confirm_load;
	bool m_confirm_new;
	char m_universe_to_load[MAX_PATH_LENGTH];
	AssetBrowser* m_asset_browser;
	AssetCompiler* m_asset_compiler;
	PropertyGrid* m_property_grid;
	LogUI* m_log_ui;
	ProfilerUI* m_profiler_ui;
	Settings m_settings;
	Array<OS::Event> m_events;
	Vec2 m_mouse_move;
	OS::Timer m_fps_timer;
	char m_template_name[100];
	char m_open_filter[64];
	char m_component_filter[32];

	struct PackConfig
	{
		enum class Mode : int
		{
			ALL_FILES,
			CURRENT_UNIVERSE
		};

		Mode mode;
		StaticString<MAX_PATH_LENGTH> dest_dir;
	};

	PackConfig m_pack;
	bool m_finished;
	bool m_deferred_game_mode_exit;
	int m_exit_code;

	bool m_sleep_when_inactive;
	bool m_is_welcome_screen_open;
	bool m_is_pack_data_dialog_open;
	bool m_is_entity_list_open;
	bool m_is_save_as_dialog_open;
	bool m_is_edit_cam_transform_ui_open = false;
	ImFont* m_font;
	ImFont* m_bold_font;

	struct WatchedPlugin
	{
		FileSystemWatcher* watcher = nullptr;
		StaticString<MAX_PATH_LENGTH> dir;
		StaticString<MAX_PATH_LENGTH> basename;
		Lumix::IPlugin* plugin = nullptr;
		int iteration = 0;
		bool reload_request = false;
	} m_watched_plugin;
};


static size_t alignMask(size_t _value, size_t _mask)
{
	return (_value + _mask) & ((~0) & (~_mask));
}


static void* alignPtr(void* _ptr, size_t _align)
{
	union {
		void* ptr;
		size_t addr;
	} un;
	un.ptr = _ptr;
	size_t mask = _align - 1;
	size_t aligned = alignMask(un.addr, mask);
	un.addr = aligned;
	return un.ptr;
}


StudioApp* StudioApp::create()
{
	static char buf[sizeof(StudioAppImpl) * 2];
	return new (NewPlaceholder(), alignPtr(buf, alignof(StudioAppImpl))) StudioAppImpl;
}


void StudioApp::destroy(StudioApp& app)
{
	app.~StudioApp();
}


static StudioApp::StaticPluginRegister* s_first_plugin = nullptr;


StudioApp::StaticPluginRegister::StaticPluginRegister(const char* name, Creator creator)
{
	this->creator = creator;
	this->name = name;
	next = s_first_plugin;
	s_first_plugin = this;
}


void StudioApp::StaticPluginRegister::create(StudioApp& app)
{
	auto* i = s_first_plugin;
	while (i)
	{
		StudioApp::IPlugin* plugin = i->creator(app);
		if (plugin) app.addPlugin(*plugin);
		i = i->next;
	}
	app.initPlugins();
}


} // namespace Lumix