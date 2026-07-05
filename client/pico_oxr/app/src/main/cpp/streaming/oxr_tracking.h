#pragma once

#include "pico_tracking.h"
#include "wivrn_client_pico.h"

struct wivrn_session_pico;

class oxr_tracker : public pico_native_tracker
{
public:
	void set_head_pose_from_xr(const XrPosef & pose);
};
