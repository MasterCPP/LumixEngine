#pragma once


#include "engine/array.h"
#include "engine/lumix.h"


namespace Lumix
{


class Animation;
struct IAllocator;
class OutputMemoryStream;
template<class Key> struct HashFunc;
template <typename K, typename V, typename H> class HashMap;


namespace Anim
{


using AnimSet = HashMap<u32, Animation*, HashFunc<u32>>;


struct RunningContext
{
	float time_delta;
	u8* input;
	IAllocator* allocator;
	struct ComponentInstance* current;
	struct Edge* edge;
	AnimSet* anim_set;
	OutputMemoryStream* event_stream;
	EntityRef controller;
};



struct InputDecl
{
	enum Type : int
	{
		// don't change order
		FLOAT,
		INT,
		BOOL,
		EMPTY
	};

	struct Constant
	{
		Type type = EMPTY;
		union
		{
			float f_value;
			int i_value;
			bool b_value;
		};
		StaticString<32> name;
	};

	struct Input
	{
		Type type = EMPTY;
		int offset;
		StaticString<32> name;
	};

	Input inputs[32];
	int inputs_count = 0;
	Constant constants[32];
	int constants_count = 0;


	static int getSize(Type type);

	int inputFromLinearIdx(int idx) const;
	int inputToLinearIdx(int idx) const;
	void removeInput(int index);
	void removeConstant(int index);
	void moveConstant(int old_idx, int new_idx);
	void moveInput(int old_idx, int new_idx);
	int addInput();
	int addConstant();
	int getInputsCount() const { return inputs_count; }
	int getSize() const;
	void recalculateOffsets();
	int getInputIdx(const char* name, int size) const;
	int getConstantIdx(const char* name, int size) const;
};


struct Condition
{
	enum class Error
	{
		NONE,
		UNKNOWN_IDENTIFIER,
		MISSING_LEFT_PARENTHESIS,
		MISSING_RIGHT_PARENTHESIS,
		UNEXPECTED_CHAR,
		OUT_OF_MEMORY,
		MISSING_BINARY_OPERAND,
		NOT_ENOUGH_PARAMETERS,
		INCORRECT_TYPE_ARGS,
		NO_RETURN_VALUE
	};

	static const char* errorToString(Error error);

	explicit Condition(IAllocator& allocator);

	bool operator()(RunningContext& rc);
	Error compile(const char* expression, InputDecl& decl);

	Array<u8> bytecode;
};


} // namespace Anim


} // namespace Lumix