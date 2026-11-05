#include <std_include.hpp>

#include <d3d11.h>
#include <dxgi.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include "component/vr/third_party/openxr/openxr.h"
#include "component/vr/third_party/openxr/openxr_platform.h"

#include "vr_xr.hpp"

#include <utils/string.hpp>

#include <atomic>
#include <algorithm>

namespace vr_probe
{
	void log_external(const char* fmt, ...); // vr_probe.cpp
}

namespace vr_xr
{
	namespace
	{
#define XR_FUNCTIONS(X)                       \
	X(xrEnumerateInstanceExtensionProperties) \
	X(xrCreateInstance)                       \
	X(xrDestroyInstance)                      \
	X(xrGetSystem)                            \
	X(xrGetInstanceProperties)                \
	X(xrGetD3D11GraphicsRequirementsKHR)      \
	X(xrCreateSession)                        \
	X(xrDestroySession)                       \
	X(xrBeginSession)                         \
	X(xrEndSession)                           \
	X(xrPollEvent)                            \
	X(xrCreateReferenceSpace)                 \
	X(xrEnumerateViewConfigurationViews)      \
	X(xrEnumerateSwapchainFormats)            \
	X(xrCreateSwapchain)                      \
	X(xrDestroySwapchain)                     \
	X(xrEnumerateSwapchainImages)             \
	X(xrAcquireSwapchainImage)                \
	X(xrWaitSwapchainImage)                   \
	X(xrReleaseSwapchainImage)                \
	X(xrWaitFrame)                            \
	X(xrBeginFrame)                           \
	X(xrEndFrame)                             \
	X(xrLocateViews)                          \
	X(xrLocateSpace)

		PFN_xrGetInstanceProcAddr get_proc{};
#define DECLARE(name) PFN_##name p_##name{};
		XR_FUNCTIONS(DECLARE)
#undef DECLARE

		enum class state
		{
			idle,       // nothing tried yet
			no_runtime, // loader/instance/system unavailable; retried later
			ready,      // instance + system, session not created
			session,    // session exists
			failed,     // permanent (adapter mismatch, bad format)
		};

		state current = state::idle;
		XrInstance instance{XR_NULL_HANDLE};
		XrSystemId system{XR_NULL_SYSTEM_ID};
		XrSession session{XR_NULL_HANDLE};
		XrSpace local_space{XR_NULL_HANDLE};
		XrSpace view_space{XR_NULL_HANDLE};
		XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
		bool session_running{};
		std::uint32_t retry_countdown{};
		std::uint32_t frames_submitted{};
		std::uint32_t failures{};
		char status_line[256] = "idle";

		struct chain
		{
			XrSwapchain handle{XR_NULL_HANDLE};
			std::vector<ID3D11Texture2D*> images;
			std::uint32_t width{}, height{};
		};

		chain eye_chain[2];
		chain ui_chain;
		std::int64_t swapchain_format{};

		bool ok(const XrResult result, const char* what)
		{
			if (XR_SUCCEEDED(result))
			{
				return true;
			}
			if (failures++ < 40)
			{
				vr_probe::log_external("[xr] %s failed: %d", what, static_cast<int>(result));
			}
			return false;
		}

		bool load_functions()
		{
			if (get_proc)
			{
				return true;
			}
			// loader next to the exe (game folder) by full path: the bare name is not found there (the process restricts
			// the DLL search path), then whatever the normal search finds
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			std::wstring full = path;
			full = full.substr(0, full.find_last_of(L"\\/") + 1) + L"openxr_loader.dll";
			auto* loader = LoadLibraryExW(full.data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			const auto full_error = GetLastError();
			if (!loader)
			{
				loader = LoadLibraryW(L"openxr_loader.dll");
			}
			if (!loader)
			{
				vr_probe::log_external("[xr] openxr_loader.dll not loaded (%ls: error %lu; search path: error %lu) - running flat",
					full.data(), full_error, GetLastError());
				return false;
			}
			vr_probe::log_external("[xr] loader loaded from %ls", full.data());
			get_proc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(loader, "xrGetInstanceProcAddr"));
			if (!get_proc)
			{
				vr_probe::log_external("[xr] xrGetInstanceProcAddr missing in the loader");
				return false;
			}
			get_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
				reinterpret_cast<PFN_xrVoidFunction*>(&p_xrEnumerateInstanceExtensionProperties));
			get_proc(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&p_xrCreateInstance));
			return p_xrCreateInstance && p_xrEnumerateInstanceExtensionProperties;
		}

		bool create_instance()
		{
			std::uint32_t count = 0;
			p_xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
			std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
			p_xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
			const auto has_d3d11 = std::any_of(extensions.begin(), extensions.end(), [](const XrExtensionProperties& e)
			{
				return std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
			});
			if (!has_d3d11)
			{
				vr_probe::log_external("[xr] runtime has no XR_KHR_D3D11_enable");
				return false;
			}

			const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
			XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
			strncpy_s(info.applicationInfo.applicationName, "IW7 VR", _TRUNCATE);
			strncpy_s(info.applicationInfo.engineName, "IW7", _TRUNCATE);
			info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
			info.enabledExtensionCount = 1;
			info.enabledExtensionNames = enabled;
			if (!ok(p_xrCreateInstance(&info, &instance), "xrCreateInstance"))
			{
				return false;
			}

#define RESOLVE(name) get_proc(instance, #name, reinterpret_cast<PFN_xrVoidFunction*>(&p_##name));
			XR_FUNCTIONS(RESOLVE)
#undef RESOLVE

			XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
			if (p_xrGetInstanceProperties && XR_SUCCEEDED(p_xrGetInstanceProperties(instance, &properties)))
			{
				vr_probe::log_external("[xr] runtime: %s %u.%u.%u", properties.runtimeName, XR_VERSION_MAJOR(properties.runtimeVersion),
					XR_VERSION_MINOR(properties.runtimeVersion), XR_VERSION_PATCH(properties.runtimeVersion));
			}
			return true;
		}

		bool get_system()
		{
			XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
			info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			const auto result = p_xrGetSystem(instance, &info, &system);
			if (XR_FAILED(result))
			{
				vr_probe::log_external("[xr] no headset yet (xrGetSystem %d) - retrying", static_cast<int>(result));
				return false;
			}
			return true;
		}

		bool adapter_matches(ID3D11Device* device)
		{
			XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
			if (!ok(p_xrGetD3D11GraphicsRequirementsKHR(instance, system, &requirements), "xrGetD3D11GraphicsRequirementsKHR"))
			{
				return false;
			}

			IDXGIDevice* dxgi_device{};
			IDXGIAdapter* adapter{};
			DXGI_ADAPTER_DESC desc{};
			if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) &&
				SUCCEEDED(dxgi_device->GetAdapter(&adapter)))
			{
				adapter->GetDesc(&desc);
			}
			if (adapter) adapter->Release();
			if (dxgi_device) dxgi_device->Release();

			const auto same = desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart &&
				desc.AdapterLuid.HighPart == requirements.adapterLuid.HighPart;
			if (!same)
			{
				vr_probe::log_external("[xr] the game's GPU is not the headset's GPU - OpenXR disabled");
			}
			return same;
		}

		bool create_session(ID3D11Device* device)
		{
			XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
			binding.device = device;
			XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
			info.next = &binding;
			info.systemId = system;
			if (!ok(p_xrCreateSession(instance, &info, &session), "xrCreateSession"))
			{
				return false;
			}

			XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
			space.poseInReferenceSpace.orientation.w = 1.0f;
			space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
			ok(p_xrCreateReferenceSpace(session, &space, &local_space), "xrCreateReferenceSpace(LOCAL)");
			space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
			ok(p_xrCreateReferenceSpace(session, &space, &view_space), "xrCreateReferenceSpace(VIEW)");

			std::uint32_t count = 0;
			p_xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
			std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
			p_xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data());
			if (count >= 2)
			{
				vr_probe::log_external("[xr] session created. recommended eye image %ux%u", views[0].recommendedImageRectWidth,
					views[0].recommendedImageRectHeight);
			}
			return true;
		}

		// the game writes gamma-encoded values into an 8-bit UNORM target: an sRGB swapchain makes the compositor
		// read them as what they are. Copies between UNORM and UNORM_SRGB of the same family are raw.
		bool pick_format()
		{
			std::uint32_t count = 0;
			p_xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
			std::vector<std::int64_t> formats(count);
			p_xrEnumerateSwapchainFormats(session, count, &count, formats.data());
			for (const auto wanted : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM})
			{
				if (std::find(formats.begin(), formats.end(), static_cast<std::int64_t>(wanted)) != formats.end())
				{
					swapchain_format = wanted;
					vr_probe::log_external("[xr] swapchain format %lld", swapchain_format);
					return true;
				}
			}
			vr_probe::log_external("[xr] runtime offers no R8G8B8A8 swapchain format");
			return false;
		}

		bool create_chain(chain& target, const std::uint32_t width, const std::uint32_t height)
		{
			XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
			info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
			info.format = swapchain_format;
			info.sampleCount = 1;
			info.width = width;
			info.height = height;
			info.faceCount = 1;
			info.arraySize = 1;
			info.mipCount = 1;
			if (!ok(p_xrCreateSwapchain(session, &info, &target.handle), "xrCreateSwapchain"))
			{
				return false;
			}
			std::uint32_t count = 0;
			p_xrEnumerateSwapchainImages(target.handle, 0, &count, nullptr);
			std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
			p_xrEnumerateSwapchainImages(target.handle, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
			target.images.clear();
			for (const auto& image : images)
			{
				target.images.push_back(image.texture);
			}
			target.width = width;
			target.height = height;
			return true;
		}

		void destroy_chains()
		{
			for (auto* c : {&eye_chain[0], &eye_chain[1], &ui_chain})
			{
				if (c->handle)
				{
					p_xrDestroySwapchain(c->handle);
				}
				*c = {};
			}
		}

		void poll_events()
		{
			XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
			while (p_xrPollEvent(instance, &event) == XR_SUCCESS)
			{
				if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				{
					const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
					session_state = changed.state;
					vr_probe::log_external("[xr] session state %d", static_cast<int>(session_state));
					if (session_state == XR_SESSION_STATE_READY && !session_running)
					{
						XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
						begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						session_running = ok(p_xrBeginSession(session, &begin), "xrBeginSession");
					}
					else if (session_state == XR_SESSION_STATE_STOPPING && session_running)
					{
						p_xrEndSession(session);
						session_running = false;
					}
					else if (session_state == XR_SESSION_STATE_EXITING || session_state == XR_SESSION_STATE_LOSS_PENDING)
					{
						destroy_chains();
						p_xrDestroySession(session);
						session = XR_NULL_HANDLE;
						session_running = false;
						current = state::ready; // recreate later
						vr_probe::log_external("[xr] session ended (%d) - will retry", static_cast<int>(session_state));
						return;
					}
				}
				else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
				{
					vr_probe::log_external("[xr] instance loss pending");
				}
				event = {XR_TYPE_EVENT_DATA_BUFFER};
			}
		}

		bool copy_into(chain& target, ID3D11DeviceContext* context, ID3D11Texture2D* source)
		{
			XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			std::uint32_t index = 0;
			if (!ok(p_xrAcquireSwapchainImage(target.handle, &acquire, &index), "xrAcquireSwapchainImage"))
			{
				return false;
			}
			XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			wait.timeout = 100000000; // 100 ms
			const auto waited = ok(p_xrWaitSwapchainImage(target.handle, &wait), "xrWaitSwapchainImage");
			if (waited && source && index < target.images.size())
			{
				context->CopyResource(target.images[index], source);
			}
			XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			p_xrReleaseSwapchainImage(target.handle, &release);
			return waited;
		}

		bool try_start(ID3D11Device* device)
		{
			if (current == state::failed)
			{
				return false;
			}
			if (current == state::no_runtime && retry_countdown--)
			{
				return false;
			}
			if (!instance)
			{
				if (!load_functions() || !create_instance())
				{
					current = state::no_runtime;
					retry_countdown = 3000;
					return false;
				}
			}
			if (system == XR_NULL_SYSTEM_ID && !get_system())
			{
				current = state::no_runtime;
				retry_countdown = 600;
				return false;
			}
			if (!adapter_matches(device))
			{
				current = state::failed;
				return false;
			}
			if (!create_session(device) || !pick_format())
			{
				current = state::failed;
				return false;
			}
			current = state::session;
			return true;
		}
	}

	namespace
	{
		// widest tan half-angles over both eyes, as the headset reports them; 0 until the first located views
		std::atomic<float> headset_tan_x{}, headset_tan_y{};
		std::atomic<float> measured_half_ipd{0.032f};
		std::atomic<XrTime> last_display_time{};
		std::atomic<XrDuration> display_period{};

		void rotate(const float* q, const float* v, float* out)
		{
			// v + 2w(q x v) + 2 q x (q x v)
			const float t[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]), 2.0f * (q[0] * v[1] - q[1] * v[0])};
			out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
			out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
			out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
		}
	}

	float half_ipd()
	{
		return measured_half_ipd.load();
	}

	bool predict_head(head_pose& out)
	{
		const auto when = last_display_time.load();
		if (!session_running || !p_xrLocateSpace || !when)
		{
			return false;
		}
		// the frame being built now is presented about one frame after the one the render thread waited for last
		XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
		if (XR_FAILED(p_xrLocateSpace(view_space, local_space, when + display_period.load(), &location)) ||
			!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		{
			return false;
		}
		const auto& pose = location.pose;
		out.q[0] = pose.orientation.x;
		out.q[1] = pose.orientation.y;
		out.q[2] = pose.orientation.z;
		out.q[3] = pose.orientation.w;
		const bool position_valid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
		out.p[0] = position_valid ? pose.position.x : 0.0f;
		out.p[1] = position_valid ? pose.position.y : 0.0f;
		out.p[2] = position_valid ? pose.position.z : 0.0f;
		return true;
	}

	bool headset_tan(float& tan_x, float& tan_y)
	{
		tan_x = headset_tan_x.load();
		tan_y = headset_tan_y.load();
		return session_running && tan_x > 0.1f && tan_y > 0.1f;
	}

	bool submit(ID3D11Device* device, ID3D11DeviceContext* context, const eye_frame& frame)
	{
		if (!device || !context || !frame.left || !frame.right)
		{
			return false;
		}
		if (current != state::session && !try_start(device))
		{
			return false;
		}

		poll_events();
		if (!session || !session_running)
		{
			std::snprintf(status_line, sizeof(status_line), "session waiting (state %d)", static_cast<int>(session_state));
			return false;
		}

		D3D11_TEXTURE2D_DESC desc{};
		frame.left->GetDesc(&desc);
		if (eye_chain[0].width != desc.Width || eye_chain[0].height != desc.Height)
		{
			destroy_chains();
			if (!create_chain(eye_chain[0], desc.Width, desc.Height) || !create_chain(eye_chain[1], desc.Width, desc.Height) ||
				!create_chain(ui_chain, desc.Width, desc.Height))
			{
				current = state::failed;
				return false;
			}
			vr_probe::log_external("[xr] swapchains %ux%u created (eyes + UI)", desc.Width, desc.Height);
		}

		XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
		XrFrameState frame_state{XR_TYPE_FRAME_STATE};
		if (!ok(p_xrWaitFrame(session, &wait_info, &frame_state), "xrWaitFrame"))
		{
			return false;
		}
		last_display_time = frame_state.predictedDisplayTime;
		display_period = frame_state.predictedDisplayPeriod;
		XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
		if (!ok(p_xrBeginFrame(session, &begin_info), "xrBeginFrame"))
		{
			return false;
		}

		XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime = frame_state.predictedDisplayTime;
		locate.space = local_space;
		XrViewState view_state{XR_TYPE_VIEW_STATE};
		XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
		std::uint32_t view_count = 0;
		const auto located = XR_SUCCEEDED(p_xrLocateViews(session, &locate, &view_state, 2, &view_count, views)) && view_count == 2 &&
			(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

		if (located)
		{
			float tx = 0.0f, ty = 0.0f;
			for (const auto& v : views)
			{
				tx = std::max({tx, std::tan(-v.fov.angleLeft), std::tan(v.fov.angleRight)});
				ty = std::max({ty, std::tan(v.fov.angleUp), std::tan(-v.fov.angleDown)});
			}
			if (headset_tan_x.exchange(tx) == 0.0f)
			{
				vr_probe::log_external("[xr] headset fov: L(%.3f %.3f %.3f %.3f) R(%.3f %.3f %.3f %.3f) rad -> symmetric tan %.4f x %.4f",
					views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown, views[1].fov.angleLeft,
					views[1].fov.angleRight, views[1].fov.angleUp, views[1].fov.angleDown, tx, ty);
			}
			headset_tan_y = ty;
			const auto& a = views[0].pose.position;
			const auto& b = views[1].pose.position;
			const auto ipd = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
			if (ipd > 0.04f && ipd < 0.09f)
			{
				measured_half_ipd = 0.5f * ipd;
			}
		}

		std::uint32_t layer_count = 0;
		const XrCompositionLayerBaseHeader* layers[2]{};
		XrCompositionLayerProjectionView projection_views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		XrCompositionLayerQuad ui_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

		if (frame_state.shouldRender && located)
		{
			const auto copied = copy_into(eye_chain[0], context, frame.left) && copy_into(eye_chain[1], context, frame.right);
			const auto ui_copied = frame.ui && copy_into(ui_chain, context, frame.ui);

			// declared FOV = rendered frustum (symmetric, the game's tan half-angles); pose = the head pose the frame was
			// rendered with when it is tagged, else the currently located views (the image then follows the head)
			const auto half_x = std::atan(frame.tan_half_x);
			const auto half_y = std::atan(frame.tan_half_y);
			for (int eye = 0; eye < 2; ++eye)
			{
				auto& view = projection_views[eye];
				if (frame.tagged)
				{
					// pose-tagged: the head pose the game rendered this frame with, each eye offset along the head's x axis
					const float offset[3] = {eye == 0 ? -frame.half_ipd : frame.half_ipd, 0.0f, 0.0f};
					float world[3];
					rotate(frame.head.q, offset, world);
					view.pose.orientation = {frame.head.q[0], frame.head.q[1], frame.head.q[2], frame.head.q[3]};
					view.pose.position = {frame.head.p[0] + world[0], frame.head.p[1] + world[1], frame.head.p[2] + world[2]};
				}
				else
				{
					view.pose = views[eye].pose;
				}
				view.fov.angleLeft = -half_x;
				view.fov.angleRight = half_x;
				view.fov.angleUp = half_y;
				view.fov.angleDown = -half_y;
				view.subImage.swapchain = eye_chain[eye].handle;
				view.subImage.imageRect.extent = {static_cast<std::int32_t>(eye_chain[eye].width), static_cast<std::int32_t>(eye_chain[eye].height)};
			}
			if (copied)
			{
				projection.space = local_space;
				projection.viewCount = 2;
				projection.views = projection_views;
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
			}
			if (ui_copied)
			{
				// head-locked, 1.5 m ahead; premultiplied alpha
				constexpr float distance = 1.5f;
				constexpr float scale = 0.7f;
				ui_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
				ui_quad.space = view_space;
				ui_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				ui_quad.subImage.swapchain = ui_chain.handle;
				ui_quad.subImage.imageRect.extent = {static_cast<std::int32_t>(ui_chain.width), static_cast<std::int32_t>(ui_chain.height)};
				ui_quad.pose.orientation.w = 1.0f;
				ui_quad.pose.position.z = -distance;
				// fixed size (the scene fov now follows the headset); keep the UI texture's aspect
				constexpr float ui_tan_x = 0.75f;
				ui_quad.size.width = 2.0f * distance * ui_tan_x * scale;
				ui_quad.size.height = ui_quad.size.width * static_cast<float>(ui_chain.height) / static_cast<float>(ui_chain.width);
				layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&ui_quad);
			}
		}

		XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
		end_info.displayTime = frame_state.predictedDisplayTime;
		end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		end_info.layerCount = layer_count;
		end_info.layers = layers;
		const auto ended = ok(p_xrEndFrame(session, &end_info), "xrEndFrame");
		if (ended && layer_count)
		{
			if (frames_submitted++ == 0)
			{
				vr_probe::log_external("[xr] first frame submitted: fov tan %.4f x %.4f, %u layers", frame.tan_half_x, frame.tan_half_y, layer_count);
			}
		}
		std::snprintf(status_line, sizeof(status_line), "running, %u frames submitted, %u errors", frames_submitted, failures);
		return ended && layer_count;
	}

	bool running()
	{
		return session_running;
	}

	const char* status()
	{
		return status_line;
	}
}
