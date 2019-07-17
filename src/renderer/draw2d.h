#pragma once

#include "engine/array.h"
#include "engine/math.h"


namespace Lumix
{

namespace ffr { struct TextureHandle; }

#pragma pack(1)
struct Color {
	Color(u32 v) { 
		r = u8(v & 0xff);
		g = u8((v >> 8) & 0xff);
		b = u8((v >> 16) & 0xff);
		a = u8((v >> 24) & 0xff);
	}

	Color(u8 r, u8 g, u8 b, u8 a) : r(r), g(g), b(b), a(a) {}

	u8 r;
	u8 g;
	u8 b;
	u8 a;
};
#pragma pack()

struct Font;

struct Draw2D {
	struct Cmd {
		ffr::TextureHandle* texture;
		u32 indices_count;
		u32 index_offset;
		Vec2 clip_pos;
		Vec2 clip_size;
	};

	struct Vertex {
		Vec2 pos;
		Vec2 uv;
		Color color; 
	};

	Draw2D(IAllocator& allocator);

	void clear(Vec2 atlas_size);
	void pushClipRect(const Vec2& from, const Vec2& to);
	void popClipRect();
	void addLine(const Vec2& from, const Vec2& to, Color color, float width);
	void addRect(const Vec2& from, const Vec2& to, Color color, float width);
	void addRectFilled(const Vec2& from, const Vec2& to, Color color);
	void addText(const Font& font, const Vec2& pos, Color color, const char* text);
	void addImage(ffr::TextureHandle* tex, const Vec2& from, const Vec2& to, const Vec2& uv0, const Vec2& uv1);
	const Array<Vertex>& getVertices() const { return m_vertices; }
	const Array<u32>& getIndices() const { return m_indices; }
	const Array<Cmd>& getCmds() const { return m_cmds; }

private:
	struct Rect {
		Vec2 from;
		Vec2 to;
	};

	IAllocator& m_allocator;
	Vec2 m_atlas_size;
	Array<Cmd> m_cmds;
	Array<u32> m_indices;
	Array<Vertex> m_vertices;
	Array<Rect> m_clip_queue;
};

} //namespace Lumix