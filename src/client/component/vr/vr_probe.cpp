#include <std_include.hpp>
#include "loader/component_loader.hpp"

#include "game/game.hpp"
#include "component/directx.hpp"

#include <utils/hook.hpp>
#include <utils/string.hpp>

#include <intrin.h>
#include <share.h>
#include <d3d11_1.h>

#include "vr_xr.hpp"

// IW7 VR: same-frame stereo by replaying the backend view once per eye, plus the diagnostics it was built with.
// See docs/TECHNICAL_NOTES.md for the engine map and the history of each mechanism ("stage N" below).
//
// What runs every frame once armed (first campaign scene render + replay_begin frames):
//   - R_RenderScene hook: headset FOV and head pose written into the refdef before the frontend consumes it;
//   - RB_DrawView hook: viewInfo snapshot, eye 1 (left) render, capture, restore, eye 2 (right) render, capture;
//   - after eye 2 the Display target is cleared so the UI renders onto its own layer;
//   - pre-present hook: eyes + UI go to OpenXR (vr_xr.cpp) and to the desktop as a side-by-side image.
// Diagnostics (only in a few detail frames) log to <working folder>/iw7-vr-probe.log: backend stages with thread
// and timing, view-parms rebuilds (0x140DE7E00), render-target binds, D3D11 draw counts per context and stage,
// per-draw comparisons between the eyes, and first-chance fault breadcrumbs.
// An earlier design (calling R_RenderScene twice per frame) crashed and was dropped; see the technical notes.

namespace vr_probe
{
	namespace
	{
		// iw7_ship.exe build 2715288 (SHA-256 a883ee28...).
		constexpr std::uintptr_t r_render_scene_addr = 0x140E357E0;     // frontend, main thread
		constexpr std::uintptr_t rb_frame_addr = 0x140E57390;           // backend frame (render thread)
		constexpr std::uintptr_t rb_draw_view_addr = 0x140E5C9C0;       // (ctx16*, viewInfo, backendData)
		constexpr std::uintptr_t rb_post_view_addr = 0x140E5C680;       // (ctx16*, viewInfo, backendData)
		constexpr std::uintptr_t rb_2d_addr = 0x140E51550;              // ()
		constexpr std::uintptr_t draw_job_addr = 0x140DE7590;           // job 0x3C "draw": {?, viewInfo, listIdx}
		constexpr std::uintptr_t view_parms_rebuild_addr = 0x140DE7E00; // (out, viewParms, offsets)
		constexpr std::uintptr_t rb_set_targets_addr = 0x140E3E070;     // (state, count, u16* colourIds, u16 depthId)
		constexpr std::uintptr_t rt_release_addr = 0x140E2B350;         // (u16 id): return a per-frame target to the pool
		constexpr std::uintptr_t draw_list_addr = 0x140E5A980;          // (ctx16*, listDesc*): walks code-surface lists
		constexpr std::uintptr_t rb_pre_present_addr = 0x140E56330;     // (flags u8, finalTargetId u16, u8, u8): copy to swap chain + present
		constexpr std::size_t view_info_size = 0x5BA0;
		constexpr std::uintptr_t frame_counter_addr = 0x148B9D27C;      // incremented by R_BeginFrame
		constexpr std::uintptr_t rt_table_addr = 0x149B3F238;           // entries 0x120 bytes, id = index + 1
		constexpr std::size_t rt_entry_size = 0x120;
		constexpr std::uint32_t rt_max_id = 600;

		struct hook_site
		{
			const char* name;
			std::uintptr_t addr;
			std::uint8_t bytes[8];
		};

		constexpr hook_site sites[] = {
			{"R_RenderScene", r_render_scene_addr, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48}},
			{"RB_Frame", rb_frame_addr, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x57}},
			{"RB_DrawView", rb_draw_view_addr, {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x41, 0x56, 0x41}},
			{"RB_PostView", rb_post_view_addr, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57}},
			{"RB_2D", rb_2d_addr, {0x48, 0x83, 0xEC, 0x58, 0x48, 0x8B, 0x05, 0xC5}},
			{"DrawJob", draw_job_addr, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}},
			{"ViewParmsRebuild", view_parms_rebuild_addr, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0x10}},
			{"SetTargets", rb_set_targets_addr, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C}},
			{"RT_Release", rt_release_addr, {0x66, 0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC}},
			{"DrawCodeList", draw_list_addr, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C}},
			{"RB_PrePresent", rb_pre_present_addr, {0xF6, 0xC1, 0x02, 0x74, 0x66, 0x48, 0x89, 0x5C}},
		};

		// windows, in frames after the first campaign scene render
		constexpr std::uint32_t d3d_hook_install = 1400; // installed from the main thread
		constexpr std::uint32_t detail_begin = 1500;
		constexpr std::uint32_t detail_end = 1503;

		// Stage 4 desktop stereo test: from replay_begin on, RB_DrawView runs twice per frame, once per eye (origin
		// shifted +-half IPD along the view's left axis), each result is copied out of 'Display', and the centre halves
		// of both eyes are written back side by side (left eye on the left: parallel free-viewing). Stays on until quit.
		// Frames replay_begin..+2 are logged in detail.
		bool replay_test_enabled = true;
		constexpr std::uint32_t replay_begin = 2000;
		constexpr std::uint32_t replay_end = 0xFFFFFFFF;
		constexpr std::uint32_t replay_detail_end = 2003;
		constexpr float half_ipd_units = 1.26f; // 64 mm IPD, 1 game unit = 1 inch

		// GfxViewParms inside viewInfo (current at +0x000, previous frame at +0x170)
		constexpr std::size_t vp_origin = 0x100;
		constexpr std::size_t vp_axis_left = 0x118; // axis[0] forward +0x10C, axis[1] left +0x118, axis[2] up +0x124
		constexpr std::size_t vp_previous = 0x170;

		// backend frame data: two static buffers 0x148F73100 / 0x149530380 (alternating per frame)
		constexpr std::size_t backend_data_size = 0x149530380 - 0x148F73100;
		constexpr std::uint32_t heartbeat_frames = 600;

		// GfxScene (iw7-mod structs.hpp): GfxCodeSurf (40 bytes) arrays codeEmissive[16384], surfs2[640], surfs3[1424],
		// surfs4[640], surfs5[640], 0x258E18 pad, then the five counts.
		constexpr std::uintptr_t gfx_scene_addr = 0x149BD1300;
		constexpr std::size_t gfx_scene_counts = 0x319898;
		constexpr std::size_t gfx_scene_diff_size = 0x320000;
		constexpr std::size_t max_lines = 60000;

		enum class stage : std::uint8_t
		{
			none,
			rb_frame,
			draw_view,
			post_view,
			two_d,
			draw_job,
			draw_view2,
			count,
		};

		const char* stage_name(const stage s)
		{
			switch (s)
			{
			case stage::rb_frame: return "rbframe";
			case stage::draw_view: return "drawview";
			case stage::post_view: return "postview";
			case stage::two_d: return "2d";
			case stage::draw_job: return "drawjob";
			case stage::draw_view2: return "drawview2";
			default: return "none";
			}
		}

		thread_local stage current_stage = stage::none;

		struct stage_scope
		{
			stage previous;

			explicit stage_scope(const stage s) : previous(current_stage)
			{
				current_stage = s;
			}

			~stage_scope()
			{
				current_stage = previous;
			}
		};

		utils::hook::detour r_render_scene_hook;
		utils::hook::detour rb_frame_hook;
		utils::hook::detour rb_draw_view_hook;
		utils::hook::detour rb_post_view_hook;
		utils::hook::detour rb_2d_hook;
		utils::hook::detour draw_job_hook;
		utils::hook::detour view_parms_rebuild_hook;
		utils::hook::detour rb_set_targets_hook;
		utils::hook::detour rt_release_hook;
		utils::hook::detour draw_list_hook;
		utils::hook::detour rb_pre_present_hook;
		std::uint8_t* current_view_info{};
		void* current_backend_data{};

		// replay bookkeeping (render thread only)
		enum class replay_phase : std::uint8_t
		{
			none,
			first,  // run 1: releases are held back
			second, // run 2: releases go through and are recorded
		};

		thread_local replay_phase current_replay_phase = replay_phase::none; // render thread only; other threads never hold
		std::vector<std::uint16_t> held_releases;
		std::vector<std::uint16_t> second_releases;
		std::vector<std::uint8_t> view_info_snapshot(view_info_size);

		utils::hook::detour d3d_draw_indexed_hook[2];
		utils::hook::detour d3d_draw_hook[2];
		utils::hook::detour d3d_draw_indexed_instanced_hook[2];
		utils::hook::detour d3d_draw_instanced_hook[2];
		utils::hook::detour d3d_execute_command_list_hook;
		utils::hook::detour d3d_finish_command_list_hook;

		std::mutex log_mutex;
		FILE* log_file{};
		std::size_t log_lines{};

		std::atomic<bool> armed{};
		std::atomic<std::uint32_t> first_frame{};
		std::atomic<bool> d3d_hooks_installed{};
		std::atomic<std::int64_t> rb_frame_start_qpc{};
		std::int64_t qpc_freq{};

		void log(const char* fmt, ...)
		{
			std::lock_guard _(log_mutex);
			if (!log_file || log_lines >= max_lines)
			{
				return;
			}

			va_list ap;
			va_start(ap, fmt);
			std::vfprintf(log_file, fmt, ap);
			va_end(ap);
			std::fputc('\n', log_file);
			std::fflush(log_file);

			if (++log_lines == max_lines)
			{
				std::fputs("[probe] line cap reached, logging stopped\n", log_file);
				std::fflush(log_file);
			}
		}

		std::uint32_t frame_now()
		{
			return *reinterpret_cast<volatile std::uint32_t*>(frame_counter_addr);
		}

		std::uint32_t rel_frame()
		{
			return armed ? frame_now() - first_frame : ~0u;
		}

		bool detail_active()
		{
			const auto rel = rel_frame();
			return (rel >= detail_begin && rel < detail_end) || (rel >= replay_begin && rel < replay_detail_end);
		}

		bool replay_active(const std::uint32_t rel)
		{
			return replay_test_enabled && rel >= replay_begin && rel < replay_end;
		}

		std::atomic<std::uint32_t> replay_frames{};

		// microseconds since the current backend frame started
		long long us_since_rb_frame()
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			const auto start = rb_frame_start_qpc.load();
			return start && qpc_freq ? (now.QuadPart - start) * 1000000 / qpc_freq : -1;
		}

		std::string stack_string(const int skip, const int count)
		{
			void* frames[8]{};
			const auto depth = RtlCaptureStackBackTrace(static_cast<DWORD>(skip + 1), static_cast<DWORD>(std::min(count, 8)), frames, nullptr);
			std::string result;
			for (USHORT i = 0; i < depth; ++i)
			{
				result += utils::string::va("%s%p", i ? "<" : "", frames[i]);
			}
			return result;
		}

		// ---- render-target names (SEH-guarded, no C++ objects in this function)

		const char* rt_name_raw(const std::uint32_t id)
		{
			__try
			{
				const auto* name = *reinterpret_cast<const char* const*>(rt_table_addr + (id - 1) * rt_entry_size + 0x78);
				if (!name)
				{
					return "(null)";
				}
				volatile char probe = name[0];
				(void)probe;
				return name;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return "(bad)";
			}
		}

		std::string describe_rt(const std::uint16_t raw_id)
		{
			const auto id = static_cast<std::uint32_t>(raw_id & 0x7FFF);
			if (!id)
			{
				return "0";
			}
			if (id > rt_max_id)
			{
				return utils::string::va("%u(?)", id);
			}
			return utils::string::va("%u'%s'", id, rt_name_raw(id));
		}

		// ---- D3D11 context classification and draw counting

		struct context_slot
		{
			std::atomic<void*> ctx{};
			std::atomic<int> type{-1}; // D3D11_DEVICE_CONTEXT_TYPE: 0 immediate, 1 deferred
			std::atomic<std::uint32_t> draws[static_cast<int>(stage::count)]{};
			std::atomic<std::uint32_t> thread_switches{};
			std::atomic<std::uint32_t> last_tid{};
		};

		constexpr std::size_t max_contexts = 64;
		context_slot contexts[max_contexts];
		std::atomic<std::uint32_t> context_overflow{};

		context_slot* find_context(ID3D11DeviceContext* ctx)
		{
			for (auto& slot : contexts)
			{
				auto* current = slot.ctx.load();
				if (current == ctx)
				{
					return &slot;
				}
				if (!current)
				{
					void* expected = nullptr;
					if (slot.ctx.compare_exchange_strong(expected, ctx) || expected == ctx)
					{
						if (slot.type == -1)
						{
							slot.type = static_cast<int>(ctx->GetType());
						}
						return &slot;
					}
				}
			}
			++context_overflow;
			return nullptr;
		}

		// ---- stage 4c: per-draw comparison of eye 1 and eye 2

		struct draw_record
		{
			std::uint8_t kind; // 0 DrawIndexed, 1 Draw, 2 DrawIndexedInstanced, 3 DrawInstanced
			std::uint32_t a, b, c, d, e;
			void* ib;
			std::uint32_t ib_offset;
			void* vb;
			std::uint32_t vb_offset;
			void* srv[4];
			void* ps;
			void* caller[3];
			// content copies in the staging buffer: VS cb0-3, PS cb0-3, vertices, indices (offset, size; size 0 = none)
			std::uint32_t seg_offset[10];
			std::uint32_t seg_size[10];
		};

		constexpr std::uintptr_t dynamic_draw_wrapper_ret = 0x140E4339F; // all draws that differed between the eyes went through it
		constexpr std::uint32_t staging_eye_bytes = 24u << 20;
		constexpr std::uint32_t cb_copy_max = 4096;
		constexpr std::uint32_t vb_copy_bytes = 512;
		constexpr std::uint32_t ib_copy_bytes = 64;
		ID3D11Buffer* content_staging{};
		ID3D11DeviceContext1* context1{};
		std::uint32_t staging_cursor[2]{};
		bool content_failed{};

		bool ensure_content_staging()
		{
			if (content_staging || content_failed)
			{
				return !content_failed;
			}
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = staging_eye_bytes * 2;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(dx::device->CreateBuffer(&desc, nullptr, &content_staging)) ||
				FAILED(dx::deviceContext->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1))))
			{
				log("[content] staging/context1 unavailable - content comparison off");
				content_failed = true;
				return false;
			}
			log("[content] staging buffer %u MB created", desc.ByteWidth >> 20);
			return true;
		}

		std::uint32_t buffer_size(ID3D11Buffer* buffer)
		{
			D3D11_BUFFER_DESC desc{};
			buffer->GetDesc(&desc);
			return desc.ByteWidth;
		}

		// copy [begin, begin+size) of a buffer into this eye's staging area; returns size copied (0 = skipped)
		std::uint32_t copy_segment(const int eye, ID3D11Buffer* source, std::uint32_t begin, std::uint32_t size, std::uint32_t& out_offset)
		{
			if (!source)
			{
				return 0;
			}
			const auto total = buffer_size(source);
			if (begin >= total)
			{
				return 0;
			}
			size = std::min(size, total - begin);
			if (staging_cursor[eye] + size > staging_eye_bytes)
			{
				return 0;
			}
			out_offset = static_cast<std::uint32_t>(eye) * staging_eye_bytes + staging_cursor[eye];
			const D3D11_BOX box{begin, 0, 0, begin + size, 1, 1};
			dx::deviceContext->CopySubresourceRegion(content_staging, 0, out_offset, 0, 0, source, 0, &box);
			staging_cursor[eye] += (size + 15) & ~15u;
			return size;
		}

		void capture_content(const int eye, draw_record& r, const std::uint32_t start_index, const DXGI_FORMAT index_format,
			ID3D11Buffer* ib, ID3D11Buffer* vb)
		{
			if (!ensure_content_staging())
			{
				return;
			}

			ID3D11Buffer* cbs[8]{};
			UINT first[8]{}, count[8]{};
			context1->VSGetConstantBuffers1(0, 4, cbs, first, count);
			context1->PSGetConstantBuffers1(0, 4, cbs + 4, first + 4, count + 4);
			for (int i = 0; i < 8; ++i)
			{
				if (cbs[i])
				{
					const auto begin = first[i] * 16;
					const auto size = count[i] ? std::min(count[i] * 16, cb_copy_max) : std::min(buffer_size(cbs[i]) - begin, cb_copy_max);
					r.seg_size[i] = copy_segment(eye, cbs[i], begin, size, r.seg_offset[i]);
					cbs[i]->Release();
				}
			}
			r.seg_size[8] = copy_segment(eye, vb, r.vb_offset, vb_copy_bytes, r.seg_offset[8]);
			const auto index_bytes = index_format == DXGI_FORMAT_R32_UINT ? 4u : 2u;
			r.seg_size[9] = copy_segment(eye, ib, r.ib_offset + start_index * index_bytes, ib_copy_bytes, r.seg_offset[9]);
		}

		std::vector<draw_record> eye_draws[2];

		void record_draw(ID3D11DeviceContext* ctx, const std::uint8_t kind, const std::uint32_t a, const std::uint32_t b,
			const std::uint32_t c, const std::uint32_t d, const std::uint32_t e)
		{
			const auto eye = current_stage == stage::draw_view ? 0 : current_stage == stage::draw_view2 ? 1 : -1;
			if (eye < 0 || !detail_active() || eye_draws[eye].size() >= 8000)
			{
				return;
			}

			draw_record r{};
			r.kind = kind;
			r.a = a;
			r.b = b;
			r.c = c;
			r.d = d;
			r.e = e;

			ID3D11Buffer* ib{};
			DXGI_FORMAT format{};
			UINT offset{};
			ctx->IAGetIndexBuffer(&ib, &format, &offset);
			r.ib = ib;
			r.ib_offset = offset;

			ID3D11Buffer* vb{};
			UINT stride{};
			ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
			r.vb = vb;
			r.vb_offset = offset;

			ID3D11ShaderResourceView* srv[4]{};
			ctx->PSGetShaderResources(0, 4, srv);
			for (int i = 0; i < 4; ++i)
			{
				r.srv[i] = srv[i];
				if (srv[i]) srv[i]->Release();
			}

			ID3D11PixelShader* ps{};
			ctx->PSGetShader(&ps, nullptr, nullptr);
			r.ps = ps;
			if (ps) ps->Release();

			RtlCaptureStackBackTrace(2, 3, r.caller, nullptr); // skip record_draw + draw stub
			if (reinterpret_cast<std::uintptr_t>(r.caller[0]) == dynamic_draw_wrapper_ret && (kind == 0 || kind == 2))
			{
				capture_content(eye, r, kind == 0 ? b : c, format, ib, vb); // start index: arg 2 of DrawIndexed, arg 3 of DrawIndexedInstanced
			}
			if (ib) ib->Release();
			if (vb) vb->Release();
			eye_draws[eye].push_back(r);
		}

		std::string describe_draw(const draw_record& r)
		{
			static const char* kinds[] = {"DrawIndexed", "Draw", "DrawIndexedInstanced", "DrawInstanced"};
			return utils::string::va("%s(%u,%u,%u,%u,%u) ib=%p+%u vb=%p+%u ps=%p srv=%p,%p,%p,%p from %p<%p<%p",
				kinds[r.kind], r.a, r.b, r.c, r.d, r.e, r.ib, r.ib_offset, r.vb, r.vb_offset, r.ps,
				r.srv[0], r.srv[1], r.srv[2], r.srv[3], r.caller[0], r.caller[1], r.caller[2]);
		}

		void compare_eye_draws(const std::uint32_t rel)
		{
			auto& e1 = eye_draws[0];
			auto& e2 = eye_draws[1];
			if (e1.empty() && e2.empty())
			{
				return;
			}

			std::size_t differ = 0, args = 0, ib = 0, vb = 0, srv = 0, ps = 0;
			std::size_t content_diff[10]{};
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (content_staging && FAILED(dx::deviceContext->Map(content_staging, 0, D3D11_MAP_READ, 0, &mapped)))
			{
				mapped.pData = nullptr;
			}
			const auto n = std::min(e1.size(), e2.size());
			for (std::size_t i = 0; i < n; ++i)
			{
				const auto& x = e1[i];
				const auto& y = e2[i];
				const bool d_args = x.kind != y.kind || x.a != y.a || x.b != y.b || x.c != y.c || x.d != y.d || x.e != y.e;
				const bool d_ib = x.ib != y.ib || x.ib_offset != y.ib_offset;
				const bool d_vb = x.vb != y.vb || x.vb_offset != y.vb_offset;
				const bool d_srv = std::memcmp(x.srv, y.srv, sizeof(x.srv)) != 0;
				const bool d_ps = x.ps != y.ps;
				if (!(d_args || d_ib || d_vb || d_srv || d_ps))
				{
					continue;
				}

				std::string content;
				if (content_staging && mapped.pData)
				{
					static const char* names[] = {"vs0", "vs1", "vs2", "vs3", "ps0", "ps1", "ps2", "ps3", "vtx", "idx"};
					const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
					for (int k = 0; k < 10; ++k)
					{
						if (!x.seg_size[k] && !y.seg_size[k])
						{
							continue;
						}
						const auto same = x.seg_size[k] == y.seg_size[k] &&
							std::memcmp(base + x.seg_offset[k], base + y.seg_offset[k], x.seg_size[k]) == 0;
						content += utils::string::va(" %s:%s", names[k], same ? "same" : "DIFF");
						if (!same && differ < 60 && k != 9)
						{
							const auto* fx = reinterpret_cast<const float*>(base + x.seg_offset[k]);
							const auto* fy = reinterpret_cast<const float*>(base + y.seg_offset[k]);
							const auto words = std::min<std::uint32_t>(std::min(x.seg_size[k], y.seg_size[k]) / 4, 4096 / 4);
							int shown = 0;
							for (std::uint32_t w = 0; w < words && shown < 6; ++w)
							{
								if (std::memcmp(fx + w, fy + w, 4) != 0)
								{
									content += utils::string::va(" [%s+%u %.4g->%.4g]", names[k], w * 4, fx[w], fy[w]);
									++shown;
								}
							}
						}
						if (!same)
						{
							++content_diff[k];
						}
					}
				}

				args += d_args;
				ib += d_ib;
				vb += d_vb;
				srv += d_srv;
				ps += d_ps;
				if (differ++ < 60)
				{
					log("[drawdiff] rel=%u #%zu%s%s%s%s%s content:%s", rel, i, d_args ? " ARGS" : "", d_ib ? " IB" : "", d_vb ? " VB" : "",
						d_srv ? " SRV" : "", d_ps ? " PS" : "", content.empty() ? " (not captured)" : content.data());
					log("    eye1 %s", describe_draw(x).data());
					log("    eye2 %s", describe_draw(y).data());
				}
			}

			log("[drawdiff] rel=%u eye1=%zu draws eye2=%zu draws, %zu differ (args %zu, ib %zu, vb %zu, srv %zu, ps %zu)",
				rel, e1.size(), e2.size(), differ, args, ib, vb, srv, ps);
			log("[content] differing draws with different CONTENT: vs0 %zu vs1 %zu vs2 %zu vs3 %zu ps0 %zu ps1 %zu ps2 %zu ps3 %zu vtx %zu idx %zu",
				content_diff[0], content_diff[1], content_diff[2], content_diff[3], content_diff[4], content_diff[5],
				content_diff[6], content_diff[7], content_diff[8], content_diff[9]);
			if (mapped.pData)
			{
				dx::deviceContext->Unmap(content_staging, 0);
			}
			staging_cursor[0] = staging_cursor[1] = 0;
			e1.clear();
			e2.clear();
		}

		std::uint32_t scene_count(const int i)
		{
			return *reinterpret_cast<const std::uint32_t*>(gfx_scene_addr + gfx_scene_counts + 4 * i);
		}

		ID3D11Texture2D* display_texture(); // defined with the stage-4 helpers below
		std::atomic<std::uint32_t> post_draws_display{}, post_draws_other{}, post_draws_none{};
		ID3D11Resource* post_other_target{};

		void classify_post_draw(ID3D11DeviceContext* ctx)
		{
			if (current_stage == stage::draw_view || current_stage == stage::draw_view2 || current_stage == stage::draw_job)
			{
				return;
			}
			ID3D11RenderTargetView* rtv{};
			ctx->OMGetRenderTargets(1, &rtv, nullptr);
			if (!rtv)
			{
				++post_draws_none;
				return;
			}
			ID3D11Resource* resource{};
			rtv->GetResource(&resource);
			rtv->Release();
			auto* display = display_texture();
			if (display && resource == display)
			{
				++post_draws_display;
			}
			else
			{
				++post_draws_other;
				post_other_target = resource;
			}
			if (display) display->Release();
			if (resource) resource->Release();
		}

		void count_draw(ID3D11DeviceContext* ctx)
		{
			if (!detail_active())
			{
				return;
			}
			classify_post_draw(ctx);

			if (auto* slot = find_context(ctx))
			{
				++slot->draws[static_cast<int>(current_stage)];
				const auto tid = GetCurrentThreadId();
				if (slot->last_tid.exchange(tid) != tid)
				{
					++slot->thread_switches;
				}
			}
		}

		void dump_and_reset_contexts(const std::uint32_t rel)
		{
			for (auto& slot : contexts)
			{
				auto* ctx = slot.ctx.load();
				if (!ctx)
				{
					continue;
				}

				std::string line = utils::string::va("[ctx] rel=%u ctx=%p type=%s lastTid=%u threadSwitches=%u draws:",
					rel, ctx, slot.type == 0 ? "IMMEDIATE" : "deferred", slot.last_tid.load(), slot.thread_switches.load());
				std::uint32_t total = 0;
				for (int s = 0; s < static_cast<int>(stage::count); ++s)
				{
					const auto n = slot.draws[s].exchange(0);
					total += n;
					if (n)
					{
						line += utils::string::va(" %s=%u", stage_name(static_cast<stage>(s)), n);
					}
				}
				slot.thread_switches = 0;
				if (total)
				{
					log("%s total=%u", line.data(), total);
				}
			}

			if (context_overflow)
			{
				log("[ctx] context table overflow x%u", context_overflow.exchange(0));
			}
		}

		void WINAPI draw_indexed_stub0(ID3D11DeviceContext* ctx, UINT a, UINT b, INT c)
		{
			count_draw(ctx);
			record_draw(ctx, 0, a, b, static_cast<std::uint32_t>(c), 0, 0);
			d3d_draw_indexed_hook[0].invoke<void>(ctx, a, b, c);
		}

		void WINAPI draw_indexed_stub1(ID3D11DeviceContext* ctx, UINT a, UINT b, INT c)
		{
			count_draw(ctx);
			record_draw(ctx, 0, a, b, static_cast<std::uint32_t>(c), 0, 0);
			d3d_draw_indexed_hook[1].invoke<void>(ctx, a, b, c);
		}

		void WINAPI draw_stub0(ID3D11DeviceContext* ctx, UINT a, UINT b)
		{
			count_draw(ctx);
			record_draw(ctx, 1, a, b, 0, 0, 0);
			d3d_draw_hook[0].invoke<void>(ctx, a, b);
		}

		void WINAPI draw_stub1(ID3D11DeviceContext* ctx, UINT a, UINT b)
		{
			count_draw(ctx);
			record_draw(ctx, 1, a, b, 0, 0, 0);
			d3d_draw_hook[1].invoke<void>(ctx, a, b);
		}

		void WINAPI draw_indexed_instanced_stub0(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, INT d, UINT e)
		{
			count_draw(ctx);
			record_draw(ctx, 2, a, b, c, static_cast<std::uint32_t>(d), e);
			d3d_draw_indexed_instanced_hook[0].invoke<void>(ctx, a, b, c, d, e);
		}

		void WINAPI draw_indexed_instanced_stub1(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, INT d, UINT e)
		{
			count_draw(ctx);
			record_draw(ctx, 2, a, b, c, static_cast<std::uint32_t>(d), e);
			d3d_draw_indexed_instanced_hook[1].invoke<void>(ctx, a, b, c, d, e);
		}

		void WINAPI draw_instanced_stub0(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, UINT d)
		{
			count_draw(ctx);
			record_draw(ctx, 3, a, b, c, d, 0);
			d3d_draw_instanced_hook[0].invoke<void>(ctx, a, b, c, d);
		}

		void WINAPI draw_instanced_stub1(ID3D11DeviceContext* ctx, UINT a, UINT b, UINT c, UINT d)
		{
			count_draw(ctx);
			record_draw(ctx, 3, a, b, c, d, 0);
			d3d_draw_instanced_hook[1].invoke<void>(ctx, a, b, c, d);
		}

		void WINAPI execute_command_list_stub(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore)
		{
			if (detail_active())
			{
				log("[d3d] ExecuteCommandList ctx=%p list=%p restore=%d tid=%u stage=%s t=%lldus stack=%s",
					ctx, list, restore, GetCurrentThreadId(), stage_name(current_stage), us_since_rb_frame(),
					stack_string(1, 4).data());
			}
			d3d_execute_command_list_hook.invoke<void>(ctx, list, restore);
		}

		HRESULT WINAPI finish_command_list_stub(ID3D11DeviceContext* ctx, BOOL restore, ID3D11CommandList** out)
		{
			const auto hr = d3d_finish_command_list_hook.invoke<HRESULT>(ctx, restore, out);
			if (detail_active())
			{
				log("[d3d] FinishCommandList ctx=%p list=%p tid=%u stage=%s t=%lldus stack=%s",
					ctx, out ? *out : nullptr, GetCurrentThreadId(), stage_name(current_stage), us_since_rb_frame(),
					stack_string(1, 4).data());
			}
			return hr;
		}

		void* vtable_entry(void* object, const int index)
		{
			return (*static_cast<void***>(object))[index];
		}

		void hook_vtable_entry(utils::hook::detour& hook, void* target, void* stub, const char* name)
		{
			hook.create(target, stub);
			hook.enable();
			log("[probe] d3d hook %s at %p", name, target);
		}

		// Immediate and deferred contexts may use different implementations; hook both when they differ.
		void install_d3d_hooks()
		{
			if (!dx::device || !dx::deviceContext)
			{
				log("[probe] d3d hooks skipped: no device yet");
				return;
			}

			ID3D11DeviceContext* deferred{};
			if (FAILED(dx::device->CreateDeferredContext(0, &deferred)) || !deferred)
			{
				log("[probe] d3d hooks: CreateDeferredContext failed");
				deferred = nullptr;
			}

			void* const objects[2] = {dx::deviceContext, deferred};
			void* done[8]{};
			std::size_t done_count = 0;
			const auto already = [&](void* fn)
			{
				for (std::size_t i = 0; i < done_count; ++i)
				{
					if (done[i] == fn)
					{
						return true;
					}
				}
				done[done_count++] = fn;
				return false;
			};

			for (int i = 0; i < 2; ++i)
			{
				if (!objects[i])
				{
					continue;
				}

				auto* fn = vtable_entry(objects[i], 12);
				if (!already(fn)) hook_vtable_entry(d3d_draw_indexed_hook[i], fn, i ? reinterpret_cast<void*>(draw_indexed_stub1) : reinterpret_cast<void*>(draw_indexed_stub0), "DrawIndexed");
				fn = vtable_entry(objects[i], 13);
				if (!already(fn)) hook_vtable_entry(d3d_draw_hook[i], fn, i ? reinterpret_cast<void*>(draw_stub1) : reinterpret_cast<void*>(draw_stub0), "Draw");
				fn = vtable_entry(objects[i], 20);
				if (!already(fn)) hook_vtable_entry(d3d_draw_indexed_instanced_hook[i], fn, i ? reinterpret_cast<void*>(draw_indexed_instanced_stub1) : reinterpret_cast<void*>(draw_indexed_instanced_stub0), "DrawIndexedInstanced");
				fn = vtable_entry(objects[i], 21);
				if (!already(fn)) hook_vtable_entry(d3d_draw_instanced_hook[i], fn, i ? reinterpret_cast<void*>(draw_instanced_stub1) : reinterpret_cast<void*>(draw_instanced_stub0), "DrawInstanced");
			}

			hook_vtable_entry(d3d_execute_command_list_hook, vtable_entry(dx::deviceContext, 58), reinterpret_cast<void*>(execute_command_list_stub), "ExecuteCommandList(immediate)");
			if (deferred)
			{
				hook_vtable_entry(d3d_finish_command_list_hook, vtable_entry(deferred, 114), reinterpret_cast<void*>(finish_command_list_stub), "FinishCommandList(deferred)");
				deferred->Release();
			}

			log("[probe] d3d hooks installed (immediate ctx %p)", dx::deviceContext);
		}

		// ---- game hooks
		// Every stub takes and forwards at least the four register arguments: a hook that declares fewer clobbers
		// the unused ones (observed crash: 0x140E5C680 reads r8 = backend data).

		// The game renders the headset's frustum: written into the refdef right before the frontend consumes it, so
		// everything downstream (culling, view parms +0x130/+0x134 that the declared FOV is read from) agrees.
		// what the backend last rendered (viewInfo+0x160/+0x164) and what the frontend asked for, for the vertical fit
		std::atomic<float> rendered_tan_x{}, rendered_tan_y{};
		float requested_tan_x{};
		float y_per_x{}; // >0 once the engine is seen deriving the vertical tan from the horizontal one

		void apply_headset_fov(game::refdef_t* refdef)
		{
			float tan_x{}, tan_y{};
			if (!refdef || !vr_xr::headset_tan(tan_x, tan_y))
			{
				return;
			}

			// Seen in headset testing: asking 1.376 x 1.428 filled the top of the view but not the bottom -> the vertical
			// tan the engine renders is not the one in the refdef. If the rendered pair keeps a fixed y/x ratio, widen x until y covers.
			const auto rx = rendered_tan_x.load();
			const auto ry = rendered_tan_y.load();
			if (requested_tan_x > 0.0f && std::fabs(rx - requested_tan_x) < 0.01f * requested_tan_x && ry < 0.99f * tan_y && !y_per_x)
			{
				y_per_x = ry / rx;
				log("[fov] engine renders vertical tan = %.4f * horizontal (asked %.4f x %.4f, got %.4f x %.4f); widening horizontal",
					y_per_x, requested_tan_x, tan_y, rx, ry);
			}
			if (y_per_x > 0.0f && y_per_x * tan_x < tan_y)
			{
				tan_x = tan_y / y_per_x;
			}

			static std::uint32_t logged{};
			if (logged++ % 3000 == 0)
			{
				log("[fov] refdef tan %.4f x %.4f -> asking %.4f x %.4f; last rendered %.4f x %.4f", refdef->view.tanHalfFovX,
					refdef->view.tanHalfFovY, tan_x, tan_y, rx, ry);
			}
			requested_tan_x = tan_x;
			refdef->view.tanHalfFovX = tan_x;
			refdef->view.tanHalfFovY = tan_y;
		}

		// ---- head tracking: HMD pose into the refdef camera; the pose is tagged so the submit declares what was rendered

		constexpr float inches_per_metre = 39.3701f;

		struct pose_tag
		{
			float forward[3];
			vr_xr::head_pose head;
		};
		std::mutex tag_mutex;
		pose_tag tags[8]{};
		std::uint32_t tag_next{};

		bool head_reference{};
		float reference_yaw{};
		float reference_pos[3]{};
		bool recenter_down{};
		float axis_left_sign{}; // +1: refdef axis[1] points left

		void to_iw(const float* xr, float* iw)
		{
			iw[0] = -xr[2];
			iw[1] = -xr[0];
			iw[2] = xr[1];
		}

		void unyaw(float* v)
		{
			const auto c = std::cos(reference_yaw), s = std::sin(reference_yaw);
			const auto x = v[0] * c + v[1] * s;
			const auto y = -v[0] * s + v[1] * c;
			v[0] = x;
			v[1] = y;
		}

		void quat_rotate(const float* q, const float* v, float* out)
		{
			const float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0])};
			out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
			out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
			out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
		}

		void apply_head(game::refdef_t* refdef)
		{
			vr_xr::head_pose head{};
			if (!refdef || !vr_xr::predict_head(head))
			{
				return;
			}
			auto& view = refdef->view;

			// head basis in game-camera terms (forward, left, up), position in inches
			static constexpr float xr_fwd[3] = {0, 0, -1}, xr_left[3] = {-1, 0, 0}, xr_up[3] = {0, 1, 0};
			float f[3], l[3], u[3], tmp[3], pos[3];
			quat_rotate(head.q, xr_fwd, tmp);
			to_iw(tmp, f);
			quat_rotate(head.q, xr_left, tmp);
			to_iw(tmp, l);
			quat_rotate(head.q, xr_up, tmp);
			to_iw(tmp, u);
			to_iw(head.p, pos);
			for (auto& c : pos)
			{
				c *= inches_per_metre;
			}

			const auto recenter = (GetAsyncKeyState(VK_NUMPAD5) & 0x8000) != 0;
			if (!head_reference || (recenter && !recenter_down))
			{
				reference_yaw = std::atan2(f[1], f[0]);
				std::memcpy(reference_pos, pos, sizeof(pos));
				head_reference = true;
				log("[head] recentred: yaw %.1f deg, position %.1f %.1f %.1f in", reference_yaw * 57.29578f, pos[0], pos[1], pos[2]);
			}
			recenter_down = recenter;

			for (int i = 0; i < 3; ++i)
			{
				pos[i] -= reference_pos[i];
			}
			unyaw(f);
			unyaw(l);
			unyaw(u);
			unyaw(pos);

			// With only the game's yaw in the camera, stick pitch moved the aim (and the gun, which hangs off the aim) out of
			// sight and use prompts could not be targeted (headset - Hinny that GOAT - test; inferred cause). The camera now carries the game's
			// full aim (yaw + pitch); the head rotates on top of it. Head position stays in the yaw-only frame (leaning never
			// tilts). Not yet confirmed in a headset.
			float body_fwd[3] = {view.axis[0][0], view.axis[0][1], 0.0f};
			const auto len = std::sqrt(body_fwd[0] * body_fwd[0] + body_fwd[1] * body_fwd[1]);
			if (len < 1e-3f)
			{
				return;
			}
			body_fwd[0] /= len;
			body_fwd[1] /= len;
			const float body_left[3] = {-body_fwd[1], body_fwd[0], 0.0f};
			if (!axis_left_sign)
			{
				const auto dot = view.axis[1][0] * body_left[0] + view.axis[1][1] * body_left[1];
				axis_left_sign = dot >= 0.0f ? 1.0f : -1.0f;
				log("[head] refdef axis[1] points %s (dot %.3f)", axis_left_sign > 0 ? "left" : "right", dot);
			}
			float aim[3][3];
			for (int i = 0; i < 3; ++i)
			{
				aim[0][i] = view.axis[0][i];
				aim[1][i] = view.axis[1][i] * axis_left_sign;
				aim[2][i] = view.axis[2][i];
			}
			auto rotate_world = [&](const float* v, float* out)
			{
				for (int i = 0; i < 3; ++i)
				{
					out[i] = v[0] * aim[0][i] + v[1] * aim[1][i] + v[2] * aim[2][i];
				}
			};
			auto move_world = [&](const float* v, float* out)
			{
				for (int i = 0; i < 3; ++i)
				{
					out[i] = v[0] * body_fwd[i] + v[1] * body_left[i] + (i == 2 ? v[2] : 0.0f);
				}
			};
			static std::uint32_t logged{};
			if (logged++ % 600 == 0)
			{
				log("[head] game aim pitch %.1f deg, head fwd (%.2f %.2f %.2f), offset %.1f %.1f %.1f in",
					std::asin(std::clamp(aim[0][2], -1.0f, 1.0f)) * -57.29578f, f[0], f[1], f[2], pos[0], pos[1], pos[2]);
			}
			float wf[3], wl[3], wu[3], wp[3];
			rotate_world(f, wf);
			rotate_world(l, wl);
			rotate_world(u, wu);
			move_world(pos, wp);
			for (int i = 0; i < 3; ++i)
			{
				view.axis[0][i] = wf[i];
				view.axis[1][i] = wl[i] * axis_left_sign;
				view.axis[2][i] = wu[i];
				view.org[i] += wp[i];
			}

			std::lock_guard _(tag_mutex);
			auto& tag = tags[tag_next++ % std::size(tags)];
			std::memcpy(tag.forward, wf, sizeof(wf));
			tag.head = head;
		}

		// backend: which tagged head pose produced this viewInfo (closest forward axis)
		bool find_tag(const std::uint8_t* view_info, vr_xr::head_pose& out, float& match)
		{
			const auto* fwd = reinterpret_cast<const float*>(view_info + vp_origin + 0x0C);
			std::lock_guard _(tag_mutex);
			auto best = -2.0f;
			for (const auto& tag : tags)
			{
				const auto dot = tag.forward[0] * fwd[0] + tag.forward[1] * fwd[1] + tag.forward[2] * fwd[2];
				if (dot > best)
				{
					best = dot;
					out = tag.head;
				}
			}
			match = best;
			return best > 0.9999f;
		}

		void r_render_scene_stub(void* refdef, const float scale, const int draw_type, void* flags, void* scene_def, void* a6)
		{
			if (!armed && game::Com_GameMode_GetActiveGameMode() == game::GAME_MODE_SP)
			{
				first_frame = frame_now();
				armed = true;
				log("[probe] armed at engine frame %u (first SP scene render)", first_frame.load());
			}

			const auto rel = rel_frame();
			if (rel == d3d_hook_install && !d3d_hooks_installed.exchange(true))
			{
				install_d3d_hooks();
			}

			if (detail_active())
			{
				log("[scene] rel=%u frame=%u tid=%u refdef=%p drawType=%d t=%lldus(since last RB frame start)",
					rel, frame_now(), GetCurrentThreadId(), refdef, draw_type, us_since_rb_frame());
			}

			apply_headset_fov(static_cast<game::refdef_t*>(refdef));
			apply_head(static_cast<game::refdef_t*>(refdef));
			r_render_scene_hook.invoke<void>(refdef, scale, draw_type, flags, scene_def, a6);
		}

		std::uint64_t rb_frame_stub(void* data, void* a2, void* a3, void* a4)
		{
			LARGE_INTEGER now{};
			QueryPerformanceCounter(&now);
			rb_frame_start_qpc = now.QuadPart;

			const auto rel = rel_frame();
			const auto detail = detail_active();
			if (detail)
			{
				log("[rb] rel=%u frame=%u RB_Frame enter tid=%u data=%p", rel, frame_now(), GetCurrentThreadId(), data);
			}

			std::uint64_t result;
			{
				stage_scope _(stage::rb_frame);
				result = rb_frame_hook.invoke<std::uint64_t>(data, a2, a3, a4);
			}

			if (detail)
			{
				log("[rb] rel=%u RB_Frame exit t=%lldus", rel, us_since_rb_frame());
				dump_and_reset_contexts(rel);
				compare_eye_draws(rel);
				log("[ui] rel=%u draws outside the 3D view: to Display %u, to other targets %u (last %p), no target %u", rel,
					post_draws_display.exchange(0), post_draws_other.exchange(0), post_other_target, post_draws_none.exchange(0));
			}

			if (replay_active(rel) || rel == replay_end)
			{
				const auto reason = dx::device ? dx::device->GetDeviceRemovedReason() : S_OK;
				if (reason != S_OK)
				{
					log("[replay] DEVICE REMOVED rel=%u reason=0x%08X", rel, static_cast<unsigned>(reason));
				}
				if (rel == replay_end && replay_frames)
				{
					log("[replay] end: %u frames replayed, device state 0x%08X", replay_frames.load(), static_cast<unsigned>(reason));
					replay_frames = 0;
				}
			}
			return result;
		}

		// ---- stage 4: per-eye view offset and desktop side-by-side

		void shift_eye(std::uint8_t* view_info, const float amount)
		{
			for (const auto base : {std::size_t{0}, vp_previous})
			{
				auto* origin = reinterpret_cast<float*>(view_info + base + vp_origin);
				const auto* left = reinterpret_cast<const float*>(view_info + base + vp_axis_left);
				for (int i = 0; i < 3; ++i)
				{
					origin[i] += left[i] * amount;
				}
			}
		}

		std::uint32_t display_rt_id{};
		ID3D11Texture2D* eye_textures[2]{};
		D3D11_TEXTURE2D_DESC eye_desc{};
		bool stereo_failed{};

		std::uint32_t find_rt_id(const char* name)
		{
			for (std::uint32_t id = 1; id <= rt_max_id; ++id)
			{
				if (std::strcmp(rt_name_raw(id), name) == 0)
				{
					return id;
				}
			}
			return 0;
		}

		void* rt_view_ptr(const std::uint32_t id, const std::size_t offset)
		{
			return *reinterpret_cast<void* const*>(rt_table_addr + (id - 1) * rt_entry_size + offset);
		}

		// returns an AddRef'd texture or nullptr
		ID3D11Texture2D* display_texture()
		{
			if (!display_rt_id || std::strcmp(rt_name_raw(display_rt_id), "Display") != 0)
			{
				display_rt_id = find_rt_id("Display");
				log("[stereo] 'Display' render target id = %u", display_rt_id);
				if (!display_rt_id)
				{
					return nullptr;
				}
			}

			auto* view = static_cast<ID3D11View*>(rt_view_ptr(display_rt_id, 0x80));
			if (!view)
			{
				view = static_cast<ID3D11View*>(rt_view_ptr(display_rt_id, 0x100));
			}
			if (!view)
			{
				return nullptr;
			}

			ID3D11Resource* resource{};
			view->GetResource(&resource);
			if (!resource)
			{
				return nullptr;
			}

			ID3D11Texture2D* texture{};
			resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
			resource->Release();
			return texture;
		}

		bool ensure_eye_textures(ID3D11Texture2D* display)
		{
			D3D11_TEXTURE2D_DESC desc{};
			display->GetDesc(&desc);
			if (eye_textures[0] && desc.Width == eye_desc.Width && desc.Height == eye_desc.Height && desc.Format == eye_desc.Format)
			{
				return true;
			}

			for (auto*& texture : eye_textures)
			{
				if (texture)
				{
					texture->Release();
					texture = nullptr;
				}
			}

			log("[stereo] Display %ux%u format=%u samples=%u bind=0x%X misc=0x%X mips=%u array=%u",
				desc.Width, desc.Height, desc.Format, desc.SampleDesc.Count, desc.BindFlags, desc.MiscFlags, desc.MipLevels, desc.ArraySize);

			eye_desc = desc;
			auto create = desc;
			create.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			create.MiscFlags = 0;
			create.CPUAccessFlags = 0;
			create.Usage = D3D11_USAGE_DEFAULT;
			create.MipLevels = 1;
			create.ArraySize = 1;
			for (auto*& texture : eye_textures)
			{
				const auto hr = dx::device->CreateTexture2D(&create, nullptr, &texture);
				if (FAILED(hr))
				{
					log("[stereo] CreateTexture2D failed 0x%08X - stereo capture disabled", static_cast<unsigned>(hr));
					stereo_failed = true;
					return false;
				}
			}
			log("[stereo] eye textures created");
			return true;
		}

		void capture_eye(const int eye)
		{
			if (stereo_failed || !dx::deviceContext)
			{
				return;
			}

			auto* display = display_texture();
			if (!display)
			{
				return;
			}
			if (ensure_eye_textures(display))
			{
				dx::deviceContext->CopySubresourceRegion(eye_textures[eye], 0, 0, 0, 0, display, 0, nullptr);
			}
			display->Release();
		}

		void compose_side_by_side()
		{
			if (stereo_failed || !dx::deviceContext || !eye_textures[0])
			{
				return;
			}

			auto* display = display_texture();
			if (!display)
			{
				return;
			}

			const auto w = eye_desc.Width;
			const auto h = eye_desc.Height;
			const D3D11_BOX centre{w / 4, 0, 0, w / 4 + w / 2, h, 1};
			dx::deviceContext->CopySubresourceRegion(display, 0, 0, 0, 0, eye_textures[0], 0, &centre);
			dx::deviceContext->CopySubresourceRegion(display, 0, w / 2, 0, 0, eye_textures[1], 0, &centre);
			display->Release();
		}

		// ---- stage 4b diagnostics: what does eye 1 change in the backend frame data?

		std::vector<std::uint8_t> backend_snapshot(backend_data_size);
		std::vector<std::uint8_t> scene_snapshot(gfx_scene_diff_size);

		void log_range_diff(const char* name, const std::uint8_t* before, const std::uint8_t* after, const std::size_t size)
		{
			std::size_t ranges = 0, changed = 0, i = 0;
			while (i < size)
			{
				if (before[i] == after[i])
				{
					++i;
					continue;
				}
				const auto start = i;
				auto last = i;
				while (i < size && i - last <= 16)
				{
					if (before[i] != after[i])
					{
						last = i;
					}
					++i;
				}
				changed += last - start + 1;
				if (ranges++ < 60)
				{
					const auto o = start & ~std::size_t{3};
					log("[%s] +0x%06X len=%zu [+%X] %08X->%08X", name, static_cast<unsigned>(start), last - start + 1,
						static_cast<unsigned>(o), *reinterpret_cast<const std::uint32_t*>(before + o),
						*reinterpret_cast<const std::uint32_t*>(after + o));
				}
			}
			log("[%s] eye 1 changed %zu ranges, %zu bytes (may include concurrent frontend writes)", name, ranges, changed);
		}

		void log_backend_diff(const std::uint8_t* before, const std::uint8_t* after, const void* base)
		{
			std::size_t ranges = 0;
			std::size_t changed_bytes = 0;
			std::size_t i = 0;
			while (i < backend_data_size)
			{
				if (before[i] == after[i])
				{
					++i;
					continue;
				}

				// coalesce: a range ends after 16 equal bytes
				const auto start = i;
				auto last = i;
				while (i < backend_data_size && i - last <= 16)
				{
					if (before[i] != after[i])
					{
						last = i;
					}
					++i;
				}
				const auto length = last - start + 1;
				changed_bytes += length;

				if (ranges < 300)
				{
					std::string values;
					const auto aligned = start & ~std::size_t{3};
					for (auto o = aligned; o <= last && o + 4 <= backend_data_size && o < aligned + 32; o += 4)
					{
						values += utils::string::va(" [+%X] %08X->%08X", static_cast<unsigned>(o),
							*reinterpret_cast<const std::uint32_t*>(before + o), *reinterpret_cast<const std::uint32_t*>(after + o));
					}
					log("[diff] data=%p +0x%06X len=%zu%s", base, static_cast<unsigned>(start), length, values.data());
				}
				++ranges;
			}
			log("[diff] eye 1 changed %zu ranges, %zu bytes of backend data", ranges, changed_bytes);
		}

		// ---- crash breadcrumbs (the game's minidump can be truncated when the driver faults)

		std::atomic<std::uint32_t> last_rel_seen{};
		std::atomic<std::uint32_t> replayed_total{};
		std::atomic<std::uint32_t> faults_logged{};

		LONG CALLBACK crash_breadcrumb(EXCEPTION_POINTERS* info)
		{
			const auto code = info->ExceptionRecord->ExceptionCode;
			if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW)
			{
				return EXCEPTION_CONTINUE_SEARCH;
			}
			// first-chance: handled faults (the mod's guarded reads, the game's __try) also land here, so log a few
			if (faults_logged++ >= 8)
			{
				return EXCEPTION_CONTINUE_SEARCH;
			}

			HMODULE module{};
			char module_name[MAX_PATH]{};
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCSTR>(info->ExceptionRecord->ExceptionAddress), &module))
			{
				GetModuleFileNameA(module, module_name, sizeof(module_name));
			}

			// no allocation-heavy work here: plain fprintf under the log lock
			if (log_file && log_mutex.try_lock())
			{
				std::fprintf(log_file, "[fault] (first-chance; the last one before the log ends is the crash) code=%08X addr=%p module=%s+0x%llX tid=%u stage=%s phase=%d rel=%u replayed=%u\n",
					static_cast<unsigned>(code), info->ExceptionRecord->ExceptionAddress, module_name,
					module ? static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress) - reinterpret_cast<std::uintptr_t>(module)) : 0ull,
					GetCurrentThreadId(), stage_name(current_stage), static_cast<int>(current_replay_phase),
					last_rel_seen.load(), replayed_total.load());
				void* frames[16]{};
				const auto depth = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
				std::fprintf(log_file, "[fault] handler stack:");
				for (USHORT i = 0; i < depth; ++i)
				{
					std::fprintf(log_file, " %p", frames[i]);
				}
				std::fprintf(log_file, "\n[crash] rip context: rsp=%p rip=%p\n", reinterpret_cast<void*>(info->ContextRecord->Rsp),
					reinterpret_cast<void*>(info->ContextRecord->Rip));
				std::fflush(log_file);
				log_mutex.unlock();
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}

		struct list_kind
		{
			int ptr;
			int count;
			int entry_size; // 0: count is used unscaled by 0x140E5A810
		};

		constexpr list_kind list_layout[] = {
			{0x08, 0x00, 4}, {0x18, 0x10, 0}, {0x30, 0x28, 0}, {0x48, 0x40, 0}, {0x60, 0x58, 0},
			{0x70, 0x78, 16}, {0x80, 0x88, 40}, {0x90, 0x98, 0xE8}, {0xA0, 0xA8, 32}, {0xB8, 0xC0, 4}, {0xC8, 0xD0, 4},
		};
		constexpr int list_kinds = static_cast<int>(std::size(list_layout));
		constexpr int code_surf_list = 6; // index into list_layout: GfxCodeSurf, 40 bytes
		constexpr std::size_t list_desc_first = 0x2698;
		constexpr std::size_t list_desc_stride = 0x100;
		constexpr int list_desc_count = 19; // 0x2698 .. 0x3898

		// eye 2 reads private copies of the code-surface arrays and their index data, taken before eye 1 runs
		constexpr std::size_t private_arena_size = 32u << 20;
		std::uint8_t* private_arena{}; // VirtualAlloc'd on first use (a 32 MB namespace-scope vector exhausts the compiler)
		std::size_t private_used{};
		void* private_code_surfs[list_desc_count]{};
		std::uint32_t private_stats_surfs{}, private_stats_indices{}, private_overflow{};

		void* private_copy(const void* source, const std::size_t bytes)
		{
			if (!source || !bytes)
			{
				return nullptr;
			}
			if (!private_arena)
			{
				private_arena = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, private_arena_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
				if (!private_arena)
				{
					++private_overflow;
					return nullptr;
				}
			}
			const auto aligned = (private_used + 15) & ~std::size_t{15};
			if (aligned + bytes > private_arena_size)
			{
				++private_overflow;
				return nullptr;
			}
			auto* dest = private_arena + aligned;
			std::memcpy(dest, source, bytes);
			private_used = aligned + bytes;
			return dest;
		}

		// GfxCodeSurf (iw7-mod structs.hpp): +0x08 triCount u32, +0x10 indices u16*
		void copy_code_surf_lists(std::uint8_t* view_info)
		{
			private_used = 0;
			private_stats_surfs = private_stats_indices = 0;
			for (int d = 0; d < list_desc_count; ++d)
			{
				auto* desc = view_info + list_desc_first + d * list_desc_stride;
				const auto& k = list_layout[code_surf_list];
				const auto count = *reinterpret_cast<std::uint32_t*>(desc + k.count);
				const auto* surfs = *reinterpret_cast<std::uint8_t**>(desc + k.ptr);
				private_code_surfs[d] = nullptr;
				if (!count || !surfs || count > 65536)
				{
					continue;
				}

				auto* copy = static_cast<std::uint8_t*>(private_copy(surfs, std::size_t{count} * 40));
				if (!copy)
				{
					continue;
				}
				for (std::uint32_t i = 0; i < count; ++i)
				{
					auto* surf = copy + i * 40;
					const auto tris = *reinterpret_cast<std::uint32_t*>(surf + 0x08);
					auto*& indices = *reinterpret_cast<std::uint16_t**>(surf + 0x10);
					if (indices && tris && tris < 1u << 20)
					{
						if (auto* index_copy = private_copy(indices, std::size_t{tris} * 3 * sizeof(std::uint16_t)))
						{
							indices = static_cast<std::uint16_t*>(index_copy);
							private_stats_indices += tris * 3;
						}
					}
				}
				private_code_surfs[d] = copy;
				private_stats_surfs += count;
			}
		}

		void use_private_code_surf_lists(std::uint8_t* view_info)
		{
			for (int d = 0; d < list_desc_count; ++d)
			{
				if (private_code_surfs[d])
				{
					*reinterpret_cast<void**>(view_info + list_desc_first + d * list_desc_stride + list_layout[code_surf_list].ptr) =
						private_code_surfs[d];
				}
			}
		}

		const char* memory_state(const void* p)
		{
			if (!p)
			{
				return "null";
			}
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(p, &mbi, sizeof(mbi)))
			{
				return "query-failed";
			}
			if (mbi.State != MEM_COMMIT)
			{
				return mbi.State == MEM_FREE ? "FREE" : "RESERVED";
			}
			return (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ? "NOACCESS" : "ok";
		}

		std::string classify(const void* p)
		{
			const auto a = reinterpret_cast<std::uintptr_t>(p);
			const auto vi = reinterpret_cast<std::uintptr_t>(current_view_info);
			const auto bd = reinterpret_cast<std::uintptr_t>(current_backend_data);
			if (vi && a >= vi && a < vi + view_info_size) return utils::string::va("viewInfo+0x%llX", static_cast<unsigned long long>(a - vi));
			if (bd && a >= bd && a < bd + backend_data_size) return utils::string::va("backendData+0x%llX", static_cast<unsigned long long>(a - bd));
			if (a >= 0x140000000 && a < 0x14C301400) return utils::string::va("image+0x%llX", static_cast<unsigned long long>(a - 0x140000000));
			return "heap/stack";
		}

		std::uint64_t draw_list_stub(void* ctx16, std::uint8_t* list, void* a3, void* a4)
		{
			if (detail_active() && list && (current_stage == stage::draw_view || current_stage == stage::draw_view2))
			{
				// list layout from 0x140E5A810: {ptr offset, count offset, entry size (0 = count used unscaled)}
				std::string line = utils::string::va("[list] %s desc=%p (%s)", current_stage == stage::draw_view ? "eye1" : "eye2",
					list, classify(list).data());
				for (int i = 0; i < list_kinds; ++i)
				{
					const auto& k = list_layout[i];
					const auto count = *reinterpret_cast<std::uint32_t*>(list + k.count);
					const auto* ptr = *reinterpret_cast<void**>(list + k.ptr);
					if (count || ptr)
					{
						line += utils::string::va(" L%d=%u@%p[%s,%s]", i + 1, count, ptr, classify(ptr).data(), memory_state(ptr));
					}
				}
				log("%s", line.data());
			}
			return draw_list_hook.invoke<std::uint64_t>(ctx16, list, a3, a4);
		}

		// ---- stage 5: UI layer + per-eye composite
		//
		// After eye 2 the Display target is cleared to transparent; everything the game draws afterwards (post-view
		// pass, 2D, HUD, menus) lands on it alone. Just before - Hinny that GOAT - the final copy + present (0x140E56330), Display holds
		// the UI layer: it is copied out and Display is redrawn as side-by-side full frames, eye + UI per half.

		ID3D11Texture2D* ui_texture{};
		ID3D11ShaderResourceView* eye_srv[2]{};
		ID3D11ShaderResourceView* ui_srv{};
		ID3D11VertexShader* composite_vs{};
		ID3D11PixelShader* composite_ps{};
		ID3D11SamplerState* composite_sampler{};
		ID3D11Buffer* composite_cb{};
		ID3DDeviceContextState* composite_state{};
		bool composite_failed{};
		bool frame_has_eyes{};
		float eye_tan_x{}, eye_tan_y{};
		bool eye_tagged{};
		float game_fov_x{}, game_fov_y{};
		vr_xr::head_pose eye_head{};
		float eye_half_ipd_m{0.032f};
		std::uint32_t xr_frames{};
		float ui_shift_uv = 0.0f; // horizontal UI offset per eye (0 = UI at screen depth)

		constexpr const char* composite_hlsl = R"(
cbuffer params : register(b0) { float ui_shift; float3 pad; };
Texture2D eye_left : register(t0);
Texture2D eye_right : register(t1);
Texture2D ui_layer : register(t2);
SamplerState linear_clamp : register(s0);

void vs(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0)
{
	uv = float2((id << 1) & 2, id & 2);
	pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
	bool right = uv.x >= 0.5;
	float2 eye_uv = float2(frac(uv.x * 2), uv.y);
	float4 scene = right ? eye_right.Sample(linear_clamp, eye_uv) : eye_left.Sample(linear_clamp, eye_uv);
	float4 ui = ui_layer.Sample(linear_clamp, eye_uv + float2(right ? -ui_shift : ui_shift, 0));
	return float4(scene.rgb * (1 - ui.a) + ui.rgb, 1);
}
)";

		using d3d_compile_fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
			UINT, UINT, ID3DBlob**, ID3DBlob**);

		ID3DBlob* compile(const char* entry, const char* target)
		{
			static d3d_compile_fn d3d_compile = []
			{
				auto* module = LoadLibraryA("d3dcompiler_47.dll");
				return module ? reinterpret_cast<d3d_compile_fn>(GetProcAddress(module, "D3DCompile")) : nullptr;
			}();
			if (!d3d_compile)
			{
				log("[ui] d3dcompiler_47.dll / D3DCompile unavailable");
				return nullptr;
			}
			ID3DBlob* code{};
			ID3DBlob* errors{};
			const auto hr = d3d_compile(composite_hlsl, std::strlen(composite_hlsl), "vr_composite", nullptr, nullptr, entry, target,
				0, 0, &code, &errors);
			if (FAILED(hr))
			{
				log("[ui] shader %s failed: %s", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
			}
			if (errors) errors->Release();
			return code;
		}

		bool ensure_compositor()
		{
			if (composite_failed)
			{
				return false;
			}
			if (composite_ps)
			{
				return true;
			}

			composite_failed = true; // until everything below succeeds
			auto* vs_code = compile("vs", "vs_5_0");
			auto* ps_code = compile("ps", "ps_5_0");
			if (!vs_code || !ps_code)
			{
				return false;
			}
			auto ok = SUCCEEDED(dx::device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &composite_vs)) &&
				SUCCEEDED(dx::device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &composite_ps));
			vs_code->Release();
			ps_code->Release();

			D3D11_SAMPLER_DESC sampler{};
			sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			sampler.MaxLOD = D3D11_FLOAT32_MAX;
			ok = ok && SUCCEEDED(dx::device->CreateSamplerState(&sampler, &composite_sampler));

			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = 16;
			cb.Usage = D3D11_USAGE_DEFAULT;
			cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			ok = ok && SUCCEEDED(dx::device->CreateBuffer(&cb, nullptr, &composite_cb));

			ID3D11Device1* device1{};
			ok = ok && SUCCEEDED(dx::device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1)));
			if (device1)
			{
				const auto level = dx::device->GetFeatureLevel();
				ok = ok && SUCCEEDED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1),
					nullptr, &composite_state));
				device1->Release();
			}

			if (!ok)
			{
				log("[ui] compositor setup failed");
				return false;
			}
			composite_failed = false;
			log("[ui] compositor ready");
			return true;
		}

		bool ensure_ui_resources()
		{
			if (!eye_textures[0] || !eye_textures[1])
			{
				return false;
			}
			if (ui_texture)
			{
				return true;
			}
			auto desc = eye_desc;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = 0;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = eye_desc.Format;
			srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srv.Texture2D.MipLevels = 1;
			if (FAILED(dx::device->CreateTexture2D(&desc, nullptr, &ui_texture)) ||
				FAILED(dx::device->CreateShaderResourceView(ui_texture, &srv, &ui_srv)) ||
				FAILED(dx::device->CreateShaderResourceView(eye_textures[0], &srv, &eye_srv[0])) ||
				FAILED(dx::device->CreateShaderResourceView(eye_textures[1], &srv, &eye_srv[1])))
			{
				log("[ui] UI/eye views failed (format %u)", eye_desc.Format);
				composite_failed = true;
				return false;
			}
			log("[ui] UI layer + eye views created");
			return true;
		}

		ID3D11RenderTargetView* display_rtv_for_write(ID3D11Texture2D* display)
		{
			static ID3D11RenderTargetView* rtv{};
			static ID3D11Texture2D* for_texture{};
			if (rtv && for_texture == display)
			{
				return rtv;
			}
			if (rtv)
			{
				rtv->Release();
				rtv = nullptr;
			}
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			desc.Format = eye_desc.Format;
			desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			if (FAILED(dx::device->CreateRenderTargetView(display, &desc, &rtv)))
			{
				log("[ui] Display RTV failed");
				return nullptr;
			}
			for_texture = display;
			return rtv;
		}

		// after eye 2: start the UI layer
		void begin_ui_layer()
		{
			auto* display = display_texture();
			if (!display)
			{
				return;
			}
			if (auto* rtv = display_rtv_for_write(display))
			{
				const float transparent[4]{0, 0, 0, 0};
				dx::deviceContext->ClearRenderTargetView(rtv, transparent);
				frame_has_eyes = true;
			}
			display->Release();
		}

		std::uint32_t ui_alpha_logged{};

		void log_ui_alpha_once(const std::uint32_t rel)
		{
			if (ui_alpha_logged >= 3 || !detail_active())
			{
				return;
			}
			++ui_alpha_logged;
			// sample the UI layer on the CPU: fraction of pixels with colour, and with alpha
			D3D11_TEXTURE2D_DESC desc = eye_desc;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags = 0;
			ID3D11Texture2D* staging{};
			if (FAILED(dx::device->CreateTexture2D(&desc, nullptr, &staging)))
			{
				return;
			}
			dx::deviceContext->CopyResource(staging, ui_texture);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(dx::deviceContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
			{
				std::size_t coloured = 0, with_alpha = 0, colour_no_alpha = 0, total = 0;
				for (UINT y = 0; y < desc.Height; y += 4)
				{
					const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch;
					for (UINT x = 0; x < desc.Width; x += 4, ++total)
					{
						const auto* px = row + x * 4;
						const bool colour = px[0] || px[1] || px[2];
						coloured += colour;
						with_alpha += px[3] != 0;
						colour_no_alpha += colour && !px[3];
					}
				}
				dx::deviceContext->Unmap(staging, 0);
				log("[ui] rel=%u UI layer: %.2f%% pixels coloured, %.2f%% with alpha, %.2f%% coloured without alpha",
					rel, 100.0 * coloured / total, 100.0 * with_alpha / total, 100.0 * colour_no_alpha / total);
			}
			staging->Release();
		}

		// just before the final copy + present: Display = side by side (eye + UI) per half
		void composite_eyes_and_ui()
		{
			if (!frame_has_eyes)
			{
				return;
			}
			frame_has_eyes = false;
			if (!ensure_compositor() || !ensure_ui_resources())
			{
				return;
			}
			auto* display = display_texture();
			if (!display)
			{
				return;
			}
			auto* rtv = display_rtv_for_write(display);
			if (!rtv)
			{
				display->Release();
				return;
			}

			dx::deviceContext->CopyResource(ui_texture, display);
			log_ui_alpha_once(rel_frame());

			ID3DDeviceContextState* previous{};
			context1->SwapDeviceContextState(composite_state, &previous);

			// headset first, inside the compositor's own context state: whatever the runtime does to the context in xrEndFrame
			// stays out of the game's pipeline state
			if (vr_xr::submit(dx::device, dx::deviceContext, {eye_textures[0], eye_textures[1], ui_texture, eye_tan_x, eye_tan_y, eye_tagged, eye_head, eye_half_ipd_m}))
			{
				if (++xr_frames % 600 == 1)
				{
					log("[xr] %s", vr_xr::status());
				}
			}

			const float params[4]{ui_shift_uv, 0, 0, 0};
			dx::deviceContext->UpdateSubresource(composite_cb, 0, nullptr, params, 0, 0);
			const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(eye_desc.Width), static_cast<float>(eye_desc.Height), 0, 1};
			dx::deviceContext->RSSetViewports(1, &viewport);
			dx::deviceContext->OMSetRenderTargets(1, &rtv, nullptr);
			dx::deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			dx::deviceContext->VSSetShader(composite_vs, nullptr, 0);
			dx::deviceContext->PSSetShader(composite_ps, nullptr, 0);
			ID3D11ShaderResourceView* srvs[3]{eye_srv[0], eye_srv[1], ui_srv};
			dx::deviceContext->PSSetShaderResources(0, 3, srvs);
			dx::deviceContext->PSSetSamplers(0, 1, &composite_sampler);
			dx::deviceContext->PSSetConstantBuffers(0, 1, &composite_cb);
			dx::deviceContext->Draw(3, 0);

			ID3D11ShaderResourceView* none[3]{};
			dx::deviceContext->PSSetShaderResources(0, 3, none);
			context1->SwapDeviceContextState(previous, nullptr);
			if (previous) previous->Release();
			display->Release();
		}

		std::uint64_t rb_pre_present_stub(void* a1, void* a2, void* a3, void* a4)
		{
			if (frame_has_eyes && dx::deviceContext && (context1 || SUCCEEDED(dx::deviceContext->QueryInterface(
				__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&context1)))))
			{
				composite_eyes_and_ui();
			}
			frame_has_eyes = false;
			return rb_pre_present_hook.invoke<std::uint64_t>(a1, a2, a3, a4);
		}

		std::uint64_t rt_release_stub(const std::uint16_t id, void* a2, void* a3, void* a4)
		{
			if (current_replay_phase == replay_phase::first)
			{
				held_releases.push_back(id);
				return 0;
			}
			if (current_replay_phase == replay_phase::second)
			{
				second_releases.push_back(id);
			}
			return rt_release_hook.invoke<std::uint64_t>(id, a2, a3, a4);
		}

		std::string ids_string(const std::vector<std::uint16_t>& ids)
		{
			std::string result;
			for (const auto id : ids)
			{
				result += " " + describe_rt(id);
			}
			return result;
		}

		std::uint64_t rb_draw_view_stub(void* ctx16, std::uint8_t* view_info, void* backend_data, void* a4)
		{
			const auto detail = detail_active();
			const auto rel = rel_frame();
			const auto replay = replay_active(rel) && view_info;
			current_view_info = view_info;
			current_backend_data = backend_data;
			if (replay)
			{
				// The projection is built from viewParms +0x130/+0x134 (= the refdef tan written by apply_headset_fov);
				// +0x160/+0x164 keep the game's own fov (0.7645 x 0.4778, ADS 0.4368) and are NOT what is rendered.
				// Declare +0x130/+0x134.
				eye_tan_x = *reinterpret_cast<const float*>(view_info + 0x130);
				eye_tan_y = *reinterpret_cast<const float*>(view_info + 0x134);
				// Weapon was invisible in the headset. +0x160/+0x164 still hold the game's own fov (0.7645 x 0.4778, ADS
				// smaller); the weapon (depth-hack) pass is believed to project - Hinny that GOAT - with it, so the gun would land magnified at the
				// bottom edge of the headset frustum. Give it the scene's tan pair, current and previous parms, while the
				// headset fov drives the camera. Not yet confirmed in a headset.
				if (vr_xr::running())
				{
					for (const auto base : {std::size_t{0}, vp_previous})
					{
						auto* fov = reinterpret_cast<float*>(view_info + base + 0x60 + 0x100);
						if (base == 0)
						{
							game_fov_x = fov[0];
							game_fov_y = fov[1];
						}
						fov[0] = eye_tan_x;
						fov[1] = eye_tan_y;
					}
				}
				rendered_tan_x = eye_tan_x;
				rendered_tan_y = eye_tan_y;
				float match{};
				eye_tagged = find_tag(view_info, eye_head, match);
				eye_half_ipd_m = vr_xr::running() ? vr_xr::half_ipd() : half_ipd_units / inches_per_metre;
				static std::uint32_t logged{};
				if (logged++ % 600 == 0)
				{
					log("[head] rendered tan %.4f x %.4f (game fov +0x160 was %.4f x %.4f, now %.4f x %.4f), tag %s (match %.6f), half ipd %.1f mm",
						eye_tan_x, eye_tan_y, game_fov_x, game_fov_y, *reinterpret_cast<const float*>(view_info + 0x160),
						*reinterpret_cast<const float*>(view_info + 0x164), eye_tagged ? "found" : "MISSING", match, eye_half_ipd_m * 1000.0f);
				}
			}

			if (detail)
			{
				log("[rb] RB_DrawView enter tid=%u ctx16=%p viewInfo=%p data=%p seq(2008)=%04X t=%lldus",
					GetCurrentThreadId(), ctx16, view_info, backend_data,
					view_info ? *reinterpret_cast<std::uint16_t*>(view_info + 0x2008) : 0, us_since_rb_frame());
			}

			if (replay)
			{
				// RB_DrawView consumes its view: it releases the per-frame targets (0x140E2B350) and zeroes their ids in
				// viewInfo (replaying without this crashed). Run 2 must start from the same viewInfo, with the targets still allocated.
				std::memcpy(view_info_snapshot.data(), view_info, view_info_size);
				copy_code_surf_lists(view_info);
				held_releases.clear();
				second_releases.clear();
				current_replay_phase = replay_phase::first;
				shift_eye(view_info, +eye_half_ipd_m * inches_per_metre); // run 1 = left eye
				if (detail && backend_data)
				{
					std::memcpy(backend_snapshot.data(), backend_data, backend_data_size);
					std::memcpy(scene_snapshot.data(), reinterpret_cast<const void*>(gfx_scene_addr), gfx_scene_diff_size);
					log("[scene] before eye 1: codeSurf counts %u %u %u %u %u", scene_count(0), scene_count(1), scene_count(2),
						scene_count(3), scene_count(4));
				}
			}

			std::uint64_t result;
			{
				stage_scope _(stage::draw_view);
				result = rb_draw_view_hook.invoke<std::uint64_t>(ctx16, view_info, backend_data, a4);
			}

			if (detail)
			{
				log("[rb] RB_DrawView exit t=%lldus", us_since_rb_frame());
			}

			if (replay)
			{
				last_rel_seen = rel;
				const auto total = ++replayed_total;
				if (total % heartbeat_frames == 0)
				{
					const auto reason = dx::device ? dx::device->GetDeviceRemovedReason() : S_OK;
					log("[heartbeat] rel=%u replayed=%u device=0x%08X private: %u surfs %u indices %zu KB overflow %u", rel, total,
						static_cast<unsigned>(reason), private_stats_surfs, private_stats_indices, private_used >> 10, private_overflow);
				}
				const auto n = replay_frames++;
				if (n == 0)
				{
					log("[replay] begin: desktop stereo - RB_DrawView once per eye (half IPD %.2f units), side-by-side on Display", half_ipd_units);
				}

				if (detail && backend_data)
				{
					log_backend_diff(backend_snapshot.data(), static_cast<const std::uint8_t*>(backend_data), backend_data);
					log("[scene] after eye 1: codeSurf counts %u %u %u %u %u", scene_count(0), scene_count(1), scene_count(2),
						scene_count(3), scene_count(4));
					log_range_diff("gfxscene", scene_snapshot.data(), reinterpret_cast<const std::uint8_t*>(gfx_scene_addr),
						gfx_scene_diff_size);
				}
				capture_eye(0);
				{
					const auto used_x = *reinterpret_cast<const float*>(view_info + 0x130);
					const auto used_y = *reinterpret_cast<const float*>(view_info + 0x134);
					if (used_x != eye_tan_x || used_y != eye_tan_y)
					{
						static std::uint32_t changed{};
						if (changed++ % 600 == 0)
						{
							log("[fov] RB_DrawView changed the parms tan %.4f x %.4f -> %.4f x %.4f; declaring the used pair", eye_tan_x,
								eye_tan_y, used_x, used_y);
						}
						eye_tan_x = used_x;
						eye_tan_y = used_y;
					}
				}
				std::memcpy(view_info, view_info_snapshot.data(), view_info_size);
				use_private_code_surf_lists(view_info); // eye 2 reads the copies eye 1's inputs were taken from
				shift_eye(view_info, -eye_half_ipd_m * inches_per_metre); // run 2 = right eye
				current_replay_phase = replay_phase::second;

				if (detail || n == 0)
				{
					log("[replay] run 1 held %zu releases:%s", held_releases.size(), ids_string(held_releases).data());
					log("[replay] left eye captured; viewInfo restored and shifted to the right eye. second RB_DrawView enter t=%lldus",
						us_since_rb_frame());
					log("[private] eye 2 uses private code-surface lists: %u surfaces, %u indices, %zu KB, overflow %u",
						private_stats_surfs, private_stats_indices, private_used >> 10, private_overflow);
				}

				{
					stage_scope _(stage::draw_view2);
					rb_draw_view_hook.invoke<std::uint64_t>(ctx16, view_info, backend_data, a4);
				}

				current_replay_phase = replay_phase::none;
				capture_eye(1);
				begin_ui_layer(); // UI drawn after this goes to a transparent Display; composited before present

				// release run-1-only targets: held minus what run 2 released (the frame-level targets)
				std::vector<std::uint16_t> extra;
				auto remaining = second_releases;
				for (const auto id : held_releases)
				{
					const auto it = std::find_if(remaining.begin(), remaining.end(), [&](const std::uint16_t other)
					{
						return (other & 0x7FFF) == (id & 0x7FFF);
					});
					if (it != remaining.end())
					{
						remaining.erase(it);
					}
					else
					{
						extra.push_back(id);
					}
				}

				for (const auto id : extra)
				{
					rt_release_hook.invoke<std::uint64_t>(id, nullptr, nullptr, nullptr);
				}

				if (detail || n == 0)
				{
					log("[replay] second RB_DrawView exit t=%lldus (replayed frame #%u). run 2 released %zu:%s",
						us_since_rb_frame(), n + 1, second_releases.size(), ids_string(second_releases).data());
					log("[replay] released run-1-only targets: %zu:%s; run-2-only releases: %zu", extra.size(),
						ids_string(extra).data(), remaining.size());
				}
			}
			return result;
		}

		std::uint64_t rb_post_view_stub(void* ctx16, std::uint8_t* view_info, void* backend_data, void* a4)
		{
			const auto detail = detail_active();
			if (detail)
			{
				log("[rb] RB_PostView enter tid=%u viewInfo=%p t=%lldus", GetCurrentThreadId(), view_info, us_since_rb_frame());
			}

			std::uint64_t result;
			{
				stage_scope _(stage::post_view);
				result = rb_post_view_hook.invoke<std::uint64_t>(ctx16, view_info, backend_data, a4);
			}

			if (detail)
			{
				log("[rb] RB_PostView exit t=%lldus", us_since_rb_frame());
			}
			return result;
		}

		std::uint64_t rb_2d_stub(void* a1, void* a2, void* a3, void* a4)
		{
			const auto detail = detail_active();
			if (detail)
			{
				log("[rb] RB_2D enter tid=%u t=%lldus", GetCurrentThreadId(), us_since_rb_frame());
			}

			std::uint64_t result;
			{
				stage_scope _(stage::two_d);
				result = rb_2d_hook.invoke<std::uint64_t>(a1, a2, a3, a4);
			}

			if (detail)
			{
				log("[rb] RB_2D exit t=%lldus", us_since_rb_frame());
			}
			return result;
		}

		std::uint64_t draw_job_stub(std::uint8_t* job, void* a2, void* a3, void* a4)
		{
			const auto detail = detail_active();
			std::uint32_t list_index = 0;
			if (detail && job)
			{
				list_index = *reinterpret_cast<std::uint32_t*>(job + 0x10);
				log("[job] draw enter tid=%u job=%p a0(viewInfo?)=%p a8=%p list=%u(0x%X) t=%lldus",
					GetCurrentThreadId(), job, *reinterpret_cast<void**>(job), *reinterpret_cast<void**>(job + 8),
					list_index, list_index, us_since_rb_frame());
			}

			std::uint64_t result;
			{
				stage_scope _(stage::draw_job);
				result = draw_job_hook.invoke<std::uint64_t>(job, a2, a3, a4);
			}

			if (detail && job)
			{
				log("[job] draw exit tid=%u list=%u t=%lldus", GetCurrentThreadId(), list_index, us_since_rb_frame());
			}
			return result;
		}

		std::uint64_t view_parms_rebuild_stub(void* out, std::uint8_t* parms, void* offsets, void* a4)
		{
			if (detail_active())
			{
				log("[vp] rebuild tid=%u stage=%s src=%p offsets=%p out=%p src+100=(%.2f %.2f %.2f) stack=%s",
					GetCurrentThreadId(), stage_name(current_stage), parms, offsets, out,
					parms ? *reinterpret_cast<float*>(parms + 0x100) : 0.f, parms ? *reinterpret_cast<float*>(parms + 0x104) : 0.f,
					parms ? *reinterpret_cast<float*>(parms + 0x108) : 0.f, stack_string(1, 3).data());
			}
			return view_parms_rebuild_hook.invoke<std::uint64_t>(out, parms, offsets, a4);
		}

		void rb_set_targets_stub(std::uint8_t* state, const std::uint32_t count, const std::uint16_t* colour_ids, const std::uint16_t depth_id)
		{
			if (detail_active())
			{
				auto* ctx = state ? *reinterpret_cast<ID3D11DeviceContext**>(state + 0x70) : nullptr;
				const auto* slot = ctx ? find_context(ctx) : nullptr;
				std::string line = utils::string::va("[bind] tid=%u stage=%s ctx=%p(%s) t=%lldus count=%u",
					GetCurrentThreadId(), stage_name(current_stage), ctx,
					!slot ? "?" : slot->type == 0 ? "imm" : "def", us_since_rb_frame(), count);
				for (std::uint32_t i = 0; i < count && i < 8 && colour_ids; ++i)
				{
					line += " c" + std::to_string(i) + "=" + describe_rt(colour_ids[i]);
				}
				line += " depth=" + describe_rt(depth_id);
				line += " stack=" + stack_string(1, 4);
				log("%s", line.data());
			}

			rb_set_targets_hook.invoke<void>(state, count, colour_ids, depth_id);
		}
	}

	void log_external(const char* fmt, ...)
	{
		char buffer[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, ap);
		va_end(ap);
		log("%s", buffer);
	}

	class component final : public component_interface
	{
	public:
		void post_unpack() override
		{
			if (game::environment::is_dedi())
			{
				return;
			}

			LARGE_INTEGER freq{};
			QueryPerformanceFrequency(&freq);
			qpc_freq = freq.QuadPart;

			log_file = _fsopen("iw7-vr-probe.log", "w", _SH_DENYWR); // readable while the game runs
			log("[probe] iw7-vr probe stage 4. d3d hooks at rel %u, detail frames %u-%u, RB_DrawView replay %s rel %u-%u",
				d3d_hook_install, detail_begin, detail_end - 1, replay_test_enabled ? "ON" : "off", replay_begin, replay_end);

			auto all_ok = true;
			for (const auto& site : sites)
			{
				const auto ok = std::memcmp(reinterpret_cast<const void*>(site.addr), site.bytes, sizeof(site.bytes)) == 0;
				all_ok &= ok;
				log("[probe] prologue %-16s %p %s", site.name, reinterpret_cast<void*>(site.addr), ok ? "ok" : "MISMATCH");
			}

			if (!all_ok)
			{
				log("[probe] prologue mismatch - wrong exe build or code still encrypted; no hooks installed");
				return;
			}

			r_render_scene_hook.create(r_render_scene_addr, r_render_scene_stub);
			rb_frame_hook.create(rb_frame_addr, rb_frame_stub);
			rb_draw_view_hook.create(rb_draw_view_addr, rb_draw_view_stub);
			rb_post_view_hook.create(rb_post_view_addr, rb_post_view_stub);
			rb_2d_hook.create(rb_2d_addr, rb_2d_stub);
			draw_job_hook.create(draw_job_addr, draw_job_stub);
			view_parms_rebuild_hook.create(view_parms_rebuild_addr, view_parms_rebuild_stub);
			rb_set_targets_hook.create(rb_set_targets_addr, rb_set_targets_stub);
			rt_release_hook.create(rt_release_addr, rt_release_stub);
			draw_list_hook.create(draw_list_addr, draw_list_stub);
			rb_pre_present_hook.create(rb_pre_present_addr, rb_pre_present_stub);
			AddVectoredExceptionHandler(1, crash_breadcrumb);
			log("[probe] game hooks installed, crash breadcrumbs on");
		}

		void pre_destroy() override
		{
			log("[probe] shutting down");
			std::lock_guard _(log_mutex);
			if (log_file)
			{
				std::fclose(log_file);
				log_file = nullptr;
			}
		}
	};
}

REGISTER_COMPONENT(vr_probe::component)
