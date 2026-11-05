#pragma once

// OpenXR output for the IW7 same-frame stereo path.
//
// The session runs on the game's own D3D11 device. Once per backend frame (at the pre-present point), the finished
// left/right eye images are copied into per-eye swapchain images and submitted as one projection layer; the UI layer
// goes out as a head-locked quad. The field of view declared to the compositor is the frustum the game rendered
// (tan half-angles from the view parms), so the image is not stretched by the compositor.
//
// Missing loader, runtime or headset leaves the game running flat on the monitor.
namespace vr_xr
{
	// HMD pose in the LOCAL reference space (OpenXR axes: x right, y up, -z forward; metres)
	struct head_pose
	{
		float q[4]; // x y z w
		float p[3];
	};

	struct eye_frame
	{
		ID3D11Texture2D* left;
		ID3D11Texture2D* right;
		ID3D11Texture2D* ui;   // premultiplied alpha, may be null
		float tan_half_x;      // what the game rendered (symmetric frustum)
		float tan_half_y;
		bool tagged;           // head = the pose the eyes were rendered with (else: the current located views)
		head_pose head;
		float half_ipd;        // metres, the eye offset the game rendered with
	};

	// Render thread, once per frame that has both eyes. Returns true when the frame went to the headset.
	bool submit(ID3D11Device* device, ID3D11DeviceContext* context, const eye_frame& frame);

	bool running();

	// Widest tan half-angles of the headset's eye frusta (symmetric). False until a session has located views.
	bool headset_tan(float& tan_x, float& tan_y);

	// Frontend thread: head pose predicted for when the frame being built now reaches the display.
	bool predict_head(head_pose& out);

	// Half the distance between the headset's eye positions (metres); 0.032 until measured.
	float half_ipd();

	// for the log: "<state> ..." one line
	const char* status();
}
