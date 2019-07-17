#pragma once


#include "engine/delegate.h"
#include "engine/math.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"


struct lua_State;


namespace Lumix
{

namespace ffr { struct TextureHandle; struct UniformHandle; }

struct Draw2D;
struct IAllocator;
class Model;
class Path;
class Renderer;
class RenderScene;
struct Viewport;
template <typename T> class Delegate;


struct PipelineResource : Resource
{
	static ResourceType TYPE;

	PipelineResource(const Path& path, ResourceManager& owner, Renderer& renderer, IAllocator& allocator);

	void unload() override;
	bool load(u64 size, const u8* mem) override;
	ResourceType getType() const override { return TYPE; }

	Array<char> content;
};


class LUMIX_RENDERER_API Pipeline
{
public:
	struct Stats
	{
		int draw_call_count;
		int instance_count;
		int triangle_count;
	};

	struct CustomCommandHandler
	{
		Delegate<void ()> callback;
		char name[30];
		u32 hash;
	};

public:
	static Pipeline* create(Renderer& renderer, PipelineResource* resource, const char* define, IAllocator& allocator);
	static void destroy(Pipeline* pipeline);
	static void renderModel(Model& model, const Matrix& mtx, ffr::UniformHandle mtx_uniform);

	virtual ~Pipeline() {}

	virtual bool render(bool only_2d) = 0;
	virtual void setScene(RenderScene* scene) = 0;
	virtual RenderScene* getScene() const = 0;
	virtual CustomCommandHandler& addCustomCommandHandler(const char* name) = 0;
	virtual void setWindowHandle(void* data) = 0;
	virtual bool isReady() const = 0;
	virtual const Stats& getStats() const = 0;
	virtual const Path& getPath() = 0;
	virtual void callLuaFunction(const char* func) = 0;
	virtual void setViewport(const Viewport& viewport) = 0;

	virtual Draw2D& getDraw2D() = 0;
	virtual void clearDraw2D() = 0;
	virtual ffr::TextureHandle getOutput() = 0;
};

} // namespace Lumix
