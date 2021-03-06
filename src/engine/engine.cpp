#include "engine.h"
#include "core/blob.h"
#include "core/crc32.h"
#include "core/fs/os_file.h"
#include "core/input_system.h"
#include "core/log.h"
#include "core/path.h"
#include "core/profiler.h"
#include "core/resource_manager.h"
#include "core/timer.h"
#include "core/fs/disk_file_device.h"
#include "core/fs/file_system.h"
#include "core/fs/memory_file_device.h"
#include "core/mtjd/manager.h"
#include "debug/debug.h"
#include "engine/iplugin.h"
#include "engine/property_descriptor.h"
#include "engine/property_register.h"
#include "plugin_manager.h"
#include "universe/hierarchy.h"
#include "universe/universe.h"
#include <lua.hpp>


namespace Lumix
{

static const uint32 SERIALIZED_ENGINE_MAGIC = 0x5f4c454e; // == '_LEN'
static const uint32 HIERARCHY_HASH = crc32("hierarchy");


enum class SerializedEngineVersion : int32
{
	BASE,
	SPARSE_TRANFORMATIONS,
	FOG_PARAMS,
	SCENE_VERSION,
	HIERARCHY_COMPONENT,
	SCENE_VERSION_CHECK,

	LATEST // must be the last one
};


#pragma pack(1)
class SerializedEngineHeader
{
public:
	uint32 m_magic;
	SerializedEngineVersion m_version;
	uint32 m_reserved; // for crc
};
#pragma pack()


class EngineImpl : public Engine
{
public:
	EngineImpl(const char* base_path0, const char* base_path1, FS::FileSystem* fs, IAllocator& allocator)
		: m_allocator(allocator)
		, m_resource_manager(m_allocator)
		, m_mtjd_manager(nullptr)
		, m_fps(0)
		, m_is_game_running(false)
		, m_component_types(m_allocator)
		, m_last_time_delta(0)
		, m_path_manager(m_allocator)
		, m_time_multiplier(1.0f)
		, m_paused(false)
		, m_next_frame(false)
	{
		m_state = lua_newstate(luaAllocator, &m_allocator);
		luaL_openlibs(m_state);

		m_mtjd_manager = MTJD::Manager::create(m_allocator);
		if (!fs)
		{
			m_file_system = FS::FileSystem::create(m_allocator);

			m_mem_file_device = LUMIX_NEW(m_allocator, FS::MemoryFileDevice)(m_allocator);
			m_disk_file_device = LUMIX_NEW(m_allocator, FS::DiskFileDevice)(base_path0, base_path1, m_allocator);

			m_file_system->mount(m_mem_file_device);
			m_file_system->mount(m_disk_file_device);
			m_file_system->setDefaultDevice("memory:disk");
			m_file_system->setSaveGameDevice("memory:disk");
		}
		else
		{
			m_file_system = fs;
			m_mem_file_device = nullptr;
			m_disk_file_device = nullptr;
		}

		m_resource_manager.create(*m_file_system);

		m_timer = Timer::create(m_allocator);
		m_fps_timer = Timer::create(m_allocator);
		m_fps_frame = 0;
		PropertyRegister::init(m_allocator);
	}


	static void* luaAllocator(void* ud, void* ptr, size_t osize, size_t nsize)
	{
		auto& allocator = *static_cast<IAllocator*>(ud);
		if (nsize == 0)
		{
			allocator.deallocate(ptr);
			return nullptr;
		}
		if (nsize > 0 && ptr == nullptr) return allocator.allocate(nsize);

		void* new_mem = allocator.allocate(nsize);
		copyMemory(new_mem, ptr, Math::minValue(osize, nsize));
		allocator.deallocate(ptr);
		return new_mem;
	}


	void registerProperties()
	{
		PropertyRegister::registerComponentType("hierarchy", "Hierarchy");
		PropertyRegister::add(
			"hierarchy",
			LUMIX_NEW(m_allocator, EntityPropertyDescriptor<Hierarchy>)(
				"parent", &Hierarchy::getParent, &Hierarchy::setParent, m_allocator));
	}


	bool create()
	{
		m_plugin_manager = PluginManager::create(*this);
		if (!m_plugin_manager)
		{
			return false;
		}

		HierarchyPlugin* hierarchy = LUMIX_NEW(m_allocator, HierarchyPlugin)(m_allocator);
		m_plugin_manager->addPlugin(hierarchy);

		m_input_system = InputSystem::create(m_allocator);
		if (!m_input_system)
		{
			return false;
		}

		registerProperties();

		return true;
	}


	~EngineImpl()
	{
		PropertyRegister::shutdown();
		Timer::destroy(m_timer);
		Timer::destroy(m_fps_timer);
		PluginManager::destroy(m_plugin_manager);
		if (m_input_system) InputSystem::destroy(*m_input_system);
		if (m_disk_file_device)
		{
			FS::FileSystem::destroy(m_file_system);
			LUMIX_DELETE(m_allocator, m_mem_file_device);
			LUMIX_DELETE(m_allocator, m_disk_file_device);
		}

		m_resource_manager.destroy();
		MTJD::Manager::destroy(*m_mtjd_manager);
		lua_close(m_state);
	}


	void setPlatformData(const PlatformData& data) override
	{
		m_platform_data = data;
	}


	const PlatformData& getPlatformData() override
	{
		return m_platform_data;
	}



	IAllocator& getAllocator() override { return m_allocator; }


	Universe& createUniverse() override
	{
		Universe* universe = LUMIX_NEW(m_allocator, Universe)(m_allocator);
		const Array<IPlugin*>& plugins = m_plugin_manager->getPlugins();
		for (auto* plugin : plugins)
		{
			IScene* scene = plugin->createScene(*universe);
			if (scene)
			{
				universe->addScene(scene);
			}
		}

		return *universe;
	}


	MTJD::Manager& getMTJDManager() override { return *m_mtjd_manager; }


	void destroyUniverse(Universe& universe) override
	{
		auto& scenes = universe.getScenes();
		for (int i = scenes.size() - 1; i >= 0; --i)
		{
			auto* scene = scenes[i];
			scenes.pop();
			scene->getPlugin().destroyScene(scene);
		}
		LUMIX_DELETE(m_allocator, &universe);
		m_resource_manager.removeUnreferenced();
	}


	PluginManager& getPluginManager() override
	{
		return *m_plugin_manager;
	}


	FS::FileSystem& getFileSystem() override { return *m_file_system; }
	FS::DiskFileDevice* getDiskFileDevice() override { return m_disk_file_device; }

	void startGame(Universe& context) override
	{
		ASSERT(!m_is_game_running);
		m_is_game_running = true;
		for (auto* scene : context.getScenes())
		{
			scene->startGame();
		}
	}


	void stopGame(Universe& context) override
	{
		ASSERT(m_is_game_running);
		m_is_game_running = false;
		for (auto* scene : context.getScenes())
		{
			scene->stopGame();
		}
	}


	void pause(bool pause) override
	{
		m_paused = pause;
	}


	void nextFrame() override
	{
		m_next_frame = true;
	}


	void setTimeMultiplier(float multiplier) override
	{
		m_time_multiplier = multiplier;
	}


	void update(Universe& context) override
	{
		PROFILE_FUNCTION();
		float dt;
		++m_fps_frame;
		if (m_fps_timer->getTimeSinceTick() > 0.5f)
		{
			m_fps = m_fps_frame / m_fps_timer->tick();
			m_fps_frame = 0;
		}
		dt = m_timer->tick() * m_time_multiplier;
		if (m_next_frame)
		{
			m_paused = false;
			dt = 1 / 30.0f;
		}
		m_last_time_delta = dt;
		{
			PROFILE_BLOCK("update scenes");
			for (auto* scene : context.getScenes())
			{
				scene->update(dt, m_paused);
			}
		}
		m_plugin_manager->update(dt, m_paused);
		m_input_system->update(dt);
		getFileSystem().updateAsyncTransactions();

		if (m_next_frame)
		{
			m_paused = true;
			m_next_frame = false;
		}
	}


	InputSystem& getInputSystem() override { return *m_input_system; }


	ResourceManager& getResourceManager() override
	{
		return m_resource_manager;
	}


	float getFPS() const override { return m_fps; }


	void serializerSceneVersions(OutputBlob& serializer, Universe& ctx)
	{
		serializer.write(ctx.getScenes().size());
		for (auto* scene : ctx.getScenes())
		{
			serializer.write(crc32(scene->getPlugin().getName()));
			serializer.write(scene->getVersion());
		}
	}


	void serializePluginList(OutputBlob& serializer)
	{
		serializer.write((int32)m_plugin_manager->getPlugins().size());
		for (auto* plugin : m_plugin_manager->getPlugins())
		{
			serializer.writeString(plugin->getName());
		}
	}


	bool hasSupportedSceneVersions(InputBlob& serializer, Universe& ctx)
	{
		int32 count;
		serializer.read(count);
		for (int i = 0; i < count; ++i)
		{
			uint32 hash;
			serializer.read(hash);
			auto* scene = ctx.getScene(hash);
			int version;
			serializer.read(version);
			if (version > scene->getVersion())
			{
				g_log_error.log("Core") << "Plugin " << scene->getPlugin().getName() << " is too old";
				return false;
			}
		}
		return true;
	}


	bool hasSerializedPlugins(InputBlob& serializer)
	{
		int32 count;
		serializer.read(count);
		for (int i = 0; i < count; ++i)
		{
			char tmp[32];
			serializer.readString(tmp, sizeof(tmp));
			if (!m_plugin_manager->getPlugin(tmp))
			{
				g_log_error.log("Core") << "Missing plugin " << tmp;
				return false;
			}
		}
		return true;
	}


	uint32 serialize(Universe& ctx, OutputBlob& serializer) override
	{
		SerializedEngineHeader header;
		header.m_magic = SERIALIZED_ENGINE_MAGIC; // == '_LEN'
		header.m_version = SerializedEngineVersion::LATEST;
		header.m_reserved = 0;
		serializer.write(header);
		serializePluginList(serializer);
		serializerSceneVersions(serializer, ctx);
		m_path_manager.serialize(serializer);
		int pos = serializer.getSize();
		ctx.serialize(serializer);
		m_plugin_manager->serialize(serializer);
		serializer.write((int32)ctx.getScenes().size());
		for (auto* scene : ctx.getScenes())
		{
			serializer.writeString(scene->getPlugin().getName());
			serializer.write(scene->getVersion());
			scene->serialize(serializer);
		}
		uint32 crc = crc32((const uint8*)serializer.getData() + pos,
							 serializer.getSize() - pos);
		return crc;
	}


	bool deserialize(Universe& ctx, InputBlob& serializer) override
	{
		SerializedEngineHeader header;
		serializer.read(header);
		if (header.m_magic != SERIALIZED_ENGINE_MAGIC)
		{
			g_log_error.log("Core") << "Wrong or corrupted file";
			return false;
		}
		if (header.m_version > SerializedEngineVersion::LATEST)
		{
			g_log_error.log("Core") << "Unsupported version";
			return false;
		}
		if (!hasSerializedPlugins(serializer))
		{
			return false;
		}
		if (header.m_version > SerializedEngineVersion::SCENE_VERSION_CHECK &&
			!hasSupportedSceneVersions(serializer, ctx))
		{
			return false;
		}

		m_path_manager.deserialize(serializer);
		ctx.deserialize(serializer);

		if (header.m_version <= SerializedEngineVersion::HIERARCHY_COMPONENT)
		{
			ctx.getScene(HIERARCHY_HASH)->deserialize(serializer, 0);
		}

		m_plugin_manager->deserialize(serializer);
		int32 scene_count;
		serializer.read(scene_count);
		for (int i = 0; i < scene_count; ++i)
		{
			char tmp[32];
			serializer.readString(tmp, sizeof(tmp));
			IScene* scene = ctx.getScene(crc32(tmp));
			int scene_version = -1;
			if (header.m_version > SerializedEngineVersion::SCENE_VERSION)
			{
				serializer.read(scene_version);
			}
			scene->deserialize(serializer, scene_version);
		}
		m_path_manager.clear();
		return true;
	}


	lua_State* getState() override { return m_state; }
	PathManager& getPathManager() override{ return m_path_manager; }
	float getLastTimeDelta() override { return m_last_time_delta; }

private:
	struct ComponentType
	{
		ComponentType(IAllocator& allocator)
			: m_name(allocator)
			, m_id(allocator)
		{
		}

		string m_name;
		string m_id;

		uint32 m_id_hash;
		uint32 m_dependency;
	};

private:
	Debug::Allocator m_allocator;

	FS::FileSystem* m_file_system;
	FS::MemoryFileDevice* m_mem_file_device;
	FS::DiskFileDevice* m_disk_file_device;

	ResourceManager m_resource_manager;
	
	MTJD::Manager* m_mtjd_manager;

	Array<ComponentType> m_component_types;
	PluginManager* m_plugin_manager;
	InputSystem* m_input_system;
	Timer* m_timer;
	Timer* m_fps_timer;
	int m_fps_frame;
	float m_time_multiplier;
	float m_fps;
	float m_last_time_delta;
	bool m_is_game_running;
	bool m_paused;
	bool m_next_frame;
	PlatformData m_platform_data;
	PathManager m_path_manager;
	lua_State* m_state;

private:
	void operator=(const EngineImpl&);
	EngineImpl(const EngineImpl&);
};


static void showLogInVS(const char* system, const char* message)
{
	Debug::debugOutput(system);
	Debug::debugOutput(" : ");
	Debug::debugOutput(message);
	Debug::debugOutput("\n");
}


static FS::OsFile g_error_file;
static bool g_is_error_file_opened = false;


static void logErrorToFile(const char*, const char* message)
{
	if (!g_is_error_file_opened) return;
	g_error_file.write(message, stringLength(message));
	g_error_file.flush();
}


Engine* Engine::create(const char* base_path0,
	const char* base_path1,
	FS::FileSystem* fs,
	IAllocator& allocator)
{
	g_log_info.log("Core") << "Creating engine...";
	Profiler::setThreadName("Main");
	installUnhandledExceptionHandler();

	g_is_error_file_opened = g_error_file.open("error.log", FS::Mode::CREATE | FS::Mode::WRITE, allocator);

	g_log_error.getCallback().bind<logErrorToFile>();
	g_log_info.getCallback().bind<showLogInVS>();
	g_log_warning.getCallback().bind<showLogInVS>();
	g_log_error.getCallback().bind<showLogInVS>();

	EngineImpl* engine = LUMIX_NEW(allocator, EngineImpl)(base_path0, base_path1, fs, allocator);
	if (!engine->create())
	{
		g_log_error.log("Core") << "Failed to create engine.";
		LUMIX_DELETE(allocator, engine);
		return nullptr;
	}
	g_log_info.log("Core") << "Engine created.";
	return engine;
}


void Engine::destroy(Engine* engine, IAllocator& allocator)
{
	LUMIX_DELETE(allocator, engine);

	g_error_file.close();
}


} // ~namespace Lumix
