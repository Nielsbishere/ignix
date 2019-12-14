#include <string>

//Nuklear Configuration

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_COMMAND_USERDATA
#define NK_ZERO_COMMAND_MEMORY

#define NK_PRIVATE
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT

#define NK_MEMCPY std::memcpy
#define NK_MEMSET std::memset

#define NK_IMPLEMENTATION

#include "../../Nuklear/nuklear.h"
#include "system/allocator.hpp"
#include "types/enum.hpp"
#include "input/input_device.hpp"
#include "input/mouse.hpp"
#include "input/keyboard.hpp"
#include "gui/gui.hpp"
#include "utils/math.hpp"

using namespace oic;

//Nuklear allocator

HashMap<void*, usz> sizes;

void *nkAlloc(nk_handle, void*, nk_size count) {
	u8 *allocation = oic::System::allocator()->allocArray(count);
	sizes[allocation] = count;
	return allocation;
}

void nkFree(nk_handle, u8 *old) {

	if (!old) return;

	auto it = sizes.find(old);

	if (it == sizes.end())
		oic::System::log()->fatal("Couldn't nkFree");

	oic::System::allocator()->freeArray(old, it->second);
	sizes.erase(it);
}

//Nuklear mappings

oicExposedEnum(
	NMouseButton, int,
	BUTTON_LEFT,
	BUTTON_MIDDLE,
	BUTTON_RIGHT
)

oicExposedEnum(
	NKey, int,
	KEY_SHIFT = NK_KEY_SHIFT,
	KEY_CTRL = NK_KEY_CTRL,
	KEY_DELETE = NK_KEY_DEL,
	KEY_ENTER = NK_KEY_ENTER,
	KEY_TAB = NK_KEY_TAB,
	KEY_BACKSPACE = NK_KEY_BACKSPACE,
	KEY_UP = NK_KEY_UP,
	KEY_DOWN = NK_KEY_DOWN,
	KEY_LEFT = NK_KEY_LEFT,
	KEY_RIGHT = NK_KEY_RIGHT
);

//GUI implementation for nuklear

namespace igx {

	struct GUI::Data {

		static constexpr usz MAX_MEMORY = 8_MiB;

		GPUBuffer ibo, vbo;
		Texture textureAtlas;
		PrimitiveBuffer primitiveBuffer;

		nk_context *ctx;
		nk_font *font;

		nk_allocator allocator{};
		nk_buffer drawCommands{};

		nk_draw_null_texture nullTexture;

		Buffer current, previous;
	};

	//Handling creation and deletion

	GUI::~GUI() {
		if (data) {
			nk_buffer_free(&data->drawCommands);
			oic::System::allocator()->free(data->ctx);
			oic::System::allocator()->free(data);
		}
	}
	
	void GUI::initData(Graphics &g) {

		//Allocate memory

		data = oic::System::allocator()->alloc<Data>();

		data->ctx = oic::System::allocator()->alloc<nk_context>();
		data->current.resize(Data::MAX_MEMORY);

		data->allocator.alloc = &nkAlloc;
		data->allocator.free = (nk_plugin_free)&nkFree;

		//Init font

		struct nk_font_atlas atlas{};
		nk_font_atlas_init(&atlas, &data->allocator);

		nk_font_atlas_begin(&atlas);

		data->font = nk_font_atlas_add_default(&atlas, 13 /* TODO: Pixel height */, nullptr);

		int width{}, height{};
		u8 *atlasData = (u8*) nk_font_atlas_bake(&atlas, &width, &height, NK_FONT_ATLAS_ALPHA8);

		Texture::Info info(
			Vec2u32{ u32(width), u32(height) }, GPUFormat::R8, GPUMemoryUsage::LOCAL, 1, 1
		);

		info.init({ Buffer(atlasData, atlasData + usz(width) * height) });

		data->textureAtlas = {
			g, NAME("Atlas texture"), info
		};

		DescriptorsSubresources resources;
		resources[0] = { sampler, data->textureAtlas };
		resources[1] = { resolution, 0 };

		descriptors = {
			g, NAME("Atlas descriptor"),
			Descriptors::Info(pipelineLayout, resources)
		};

		nk_font_atlas_end(&atlas, nk_handle_ptr(data->textureAtlas.get()), &data->nullTexture);

		//Init nk

		nk_init_fixed(data->ctx, data->current.data(), Data::MAX_MEMORY, &data->font->handle);

	}

	//Converting to vertex buffer

	void GUI::bakePrimitives(Graphics &g) {

		auto *ctx = data->ctx;

		if (data->drawCommands.allocated)
			nk_buffer_free(&data->drawCommands);

		static const struct nk_draw_vertex_layout_element vertLayout[] = {
			{ NK_VERTEX_POSITION, NK_FORMAT_FLOAT, vertexLayout[0].offset },
			{ NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, vertexLayout[1].offset },
			{ NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, vertexLayout[2].offset },
			{ NK_VERTEX_LAYOUT_END }
		};

		struct nk_convert_config cfg = {};
		cfg.shape_AA = NK_ANTI_ALIASING_ON;
		cfg.line_AA = NK_ANTI_ALIASING_ON;
		cfg.vertex_layout = vertLayout;
		cfg.vertex_size = vertexLayout.getStride();
		cfg.vertex_alignment = 4 /* Detect? */;
		cfg.circle_segment_count = 22;
		cfg.curve_segment_count = 22;
		cfg.arc_segment_count = 22;
		cfg.global_alpha = 1.0f;
		cfg.null = data->nullTexture;

		nk_buffer verts, idx;

		nk_buffer_init(&data->drawCommands, &data->allocator, NK_BUFFER_DEFAULT_INITIAL_SIZE);
		nk_buffer_init(&verts, &data->allocator, NK_BUFFER_DEFAULT_INITIAL_SIZE);
		nk_buffer_init(&idx, &data->allocator, NK_BUFFER_DEFAULT_INITIAL_SIZE);
		nk_convert(ctx, &data->drawCommands, &verts, &idx, &cfg);


		if (verts.needed) {

			bool recreatePbuffer = false;

			//Too many vertices (resize)
			if (!data->vbo.exists() || verts.needed > data->vbo->size()) {

				recreatePbuffer = true;
				data->vbo.release();

				usz newSize = Math::max(verts.needed * 2, 1_MiB);

				usz stride = vertexLayout.getStride();

				if (newSize % stride)
					newSize = (newSize / stride + 1) * stride;

				auto vboInfo = GPUBuffer::Info(newSize, GPUBufferType::VERTEX, GPUMemoryUsage::CPU_WRITE);
				std::memcpy(vboInfo.initData.data(), verts.memory.ptr, verts.needed);

				data->vbo = {
					g, NAME("NK VBO"),
					vboInfo
				};

			}
			
			//Update vbo
			else {
				std::memcpy(data->vbo->getBuffer(), verts.memory.ptr, verts.needed);
				data->vbo->flush(0, verts.needed);
			}

			//Too many indices (resize)
			if (!data->ibo.exists() || idx.needed > data->ibo->size()) {

				recreatePbuffer = true;
				data->ibo.release();

				usz newSize = Math::max(idx.needed * 2, 1_MiB);

				static constexpr usz stride = 2;

				if (newSize % stride)
					newSize = (newSize / stride + 1) * stride;

				auto iboInfo = GPUBuffer::Info(newSize, GPUBufferType::INDEX, GPUMemoryUsage::CPU_WRITE);
				std::memcpy(iboInfo.initData.data(), idx.memory.ptr, idx.needed);

				data->ibo = {
					g, NAME("NK IBO"),
					iboInfo
				};

			}

			//Update ibo
			else {
				std::memcpy(data->ibo->getBuffer(), idx.memory.ptr, idx.needed);
				data->ibo->flush(0, idx.needed);
			}

			//Recreate primitive buffer
			if (recreatePbuffer) {

				data->primitiveBuffer.release();

				data->primitiveBuffer = {
					g, NAME("Primitive buffer"),
					PrimitiveBuffer::Info(
						BufferLayout(data->vbo, vertexLayout),
						BufferLayout(data->ibo, BufferAttributes(GPUFormat::R16u))
					)
				};
			}

		} else {
			data->vbo.release();
			data->ibo.release();
			data->primitiveBuffer.release();
		}

		nk_buffer_free(&verts);
		nk_buffer_free(&idx);
	}

	//Draw commands

	void GUI::draw() {

		commands->add(BindPrimitiveBuffer(data->primitiveBuffer));

		const nk_draw_command *cmd {};
		nk_draw_index offset {};

		nk_draw_foreach(cmd, data->ctx, &data->drawCommands) {

			if (!cmd->elem_count) continue;

			auto r = cmd->clip_rect;

			commands->add(
				r.w == 16384 ? SetScissor() : SetScissor({ u32(r.w), u32(r.h) }, { i32(r.x), i32(r.y) }),
				DrawInstanced::indexed(cmd->elem_count, 1, offset)
			);

			offset += u16(cmd->elem_count);
		}
	}
	
	//Receive input

	bool GUI::onInputUpdate(const InputDevice *dvc, InputHandle ih, bool isActive) {

		if (dvc->getType() == InputDevice::Type::KEYBOARD) {

			String name = Key::nameById(ih);
			usz nkid = NKey::idByName(name);

			if (nkid != NKey::count) {
				couldRefresh = true;
				nk_input_key(data->ctx, nk_keys(NKey::values[nkid]), int(isActive));
				return true;
			}

		} else if (dvc->getType() == InputDevice::Type::MOUSE) {

			f64 x = dvc->getCurrentAxis(MouseAxis::AXIS_X);
			f64 y = dvc->getCurrentAxis(MouseAxis::AXIS_Y);

			//Buttons
			if (ih < MouseButton::count) {

				String name = MouseButton::nameById(ih);
				usz nkid = NMouseButton::idByName(name);

				if (nkid != MouseButton::count) {
					couldRefresh = true;
					nk_input_button(data->ctx, nk_buttons(NMouseButton::values[nkid]), int(x), int(y), int(isActive));
					return true;
				}

			}

			//Wheel or x/y
			else {

				usz axis = ih - MouseButton::count;

				if (axis == MouseAxis::AXIS_WHEEL) {
					couldRefresh = true;
					nk_input_scroll(data->ctx, nk_vec2(f32(dvc->getCurrentAxis(MouseAxis::AXIS_WHEEL)), 0));
					return true;
				}

				else if (axis == MouseAxis::AXIS_X || axis == MouseAxis::AXIS_Y) {
					couldRefresh = true;
					nk_input_motion(data->ctx, int(x), int(y));
					return true;
				}

			}
		}

		return false;
	}

	//Nuklear test

	bool GUI::prepareDrawData() {

		auto *ctx = data->ctx;

		//Clear previous and capture input

		nk_clear(ctx);
		nk_input_end(data->ctx);

		//Do render

		enum { EASY, NORMAL, HARD };
		static int op = EASY, active[3]{ 1, 0, 1 }, selected{};
		static float value = 0.6f;
		static usz test{};

		static c8 const * names[] = {
			"Large biome",
			"Small biome"
		};

		if (nk_begin(ctx, "Show", nk_rect(50, 50, 300, 350),
					 NK_WINDOW_BORDER|NK_WINDOW_SCALABLE|NK_WINDOW_MOVABLE|NK_WINDOW_TITLE)) {

			// fixed widget pixel width
			nk_layout_row_static(ctx, 30, 150, 1);
			if (nk_button_label(ctx, "Play"))
				oic::System::log()->debug("Hi");

			// fixed widget window ratio width
			nk_layout_row_dynamic(ctx, 30, 2);
			if (nk_option_label(ctx, "Easy", op == EASY)) op = EASY;
			if (nk_option_label(ctx, "Normal", op == NORMAL)) op = NORMAL;
			if (nk_option_label(ctx, "Hard", op == HARD)) op = HARD;

			nk_layout_row_dynamic(ctx, 30, 2);
			nk_checkbox_label(ctx, "Silver", active);
			nk_checkbox_label(ctx, "Bronze", active + 1);
			nk_checkbox_label(ctx, "Gold", active + 2);

			nk_layout_row_dynamic(ctx, 30, 2);
			nk_combobox(ctx, names, int(sizeof(names) / sizeof(names[0])), &selected, 30, nk_vec2(150, 200));

			// custom widget pixel width
			nk_layout_row_begin(ctx, NK_STATIC, 30, 2);
			{
				nk_layout_row_push(ctx, 50);
				nk_label(ctx, "Volume:", NK_TEXT_LEFT);
				nk_layout_row_push(ctx, 110);
				nk_slider_float(ctx, 0, &value, 1.0f, 0.1f);
				nk_progress(ctx, &test, 100, 1);
			}
			nk_layout_row_end(ctx);
		}

		nk_end(ctx);
		nk_input_begin(ctx);

		//Detect if different

		bool refresh = data->previous != data->current;
		data->previous = data->current;
		return refresh;
	}

}