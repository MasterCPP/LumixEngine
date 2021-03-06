#pragma once


#include "engine/resource.h"
#include "state_machine.h"


namespace Lumix
{


namespace Anim
{


class ControllerResource : public Resource
{
public:
	enum class Version : int
	{
		ANIMATION_SETS,
		MAX_ROOT_ROTATION_SPEED,
		INPUT_REFACTOR,
		ENTER_EXIT_EVENTS,
		ANIMATION_SPEED_MULTIPLIER,
		MASKS,
		END_GUARD,
		EVENTS_FIX,

		LAST
	};

public:
	ControllerResource(const Path& path, ResourceManager& resource_manager, IAllocator& allocator);
	~ControllerResource();

	ResourceType getType() const override { return TYPE; }

	void create() { onCreated(State::READY); }
	void destroy() { doUnload(); }
	void unload() override;
	bool load(u64 size, const u8* mem) override;
	ComponentInstance* createInstance(IAllocator& allocator) const;
	void serialize(OutputMemoryStream& blob);
	bool deserialize(InputMemoryStream& blob, int& version);
	IAllocator& getAllocator() const { return m_allocator; }
	void addAnimation(int set, u32 hash, Animation* animation);

	struct AnimSetEntry
	{
		int set;
		u32 hash;
		Animation* animation;
	};

	Array<AnimSetEntry> m_animation_set;
	Array<StaticString<32>> m_sets_names;
	Array<BoneMask> m_masks;
	InputDecl m_input_decl;
	Component* m_root;

	static const ResourceType TYPE;

private:
	void clearAnimationSets();

	IAllocator& m_allocator;
};


} // namespace Anim


} // namespace Lumix